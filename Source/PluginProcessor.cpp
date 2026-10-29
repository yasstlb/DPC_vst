#include "PluginProcessor.h"
#include "PluginEditor.h"

DPC_vstAudioProcessor::DPC_vstAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout DPC_vstAudioProcessor::createParameterLayout()
{
    using APF = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<Choice>("MODE", "Mode",
        juce::StringArray { "CORE", "IRON", "HEAT" }, 0));
    layout.add(std::make_unique<APF>("CLIP", "Clip",
        juce::NormalisableRange<float>(-18.0f, 0.0f, 0.01f), -6.0f, " dB"));
    layout.add(std::make_unique<APF>("DETAIL", "Detail",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.5f));
    layout.add(std::make_unique<APF>("MIX", "Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
    layout.add(std::make_unique<APF>("OUTPUT", "Output",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.01f), 0.0f, " dB"));
    return layout;
}

void DPC_vstAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const bool stereo = getTotalNumInputChannels() == 2;

    outputGain.reset(sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("OUTPUT")->load()));

    clipThreshold.reset(sampleRate * (stereo ? 4.0 : 1.0), 0.005);
    clipThreshold.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("CLIP")->load()));
    wetMix.reset(sampleRate, 0.005);
    wetMix.setCurrentAndTargetValue(parameters.getRawParameterValue("MIX")->load());

    dryBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
    oversampling.initProcessing(static_cast<size_t>(juce::jmax(1, samplesPerBlock)));
    oversampling.reset();
    oversamplingLatency = stereo ? juce::roundToInt(oversampling.getLatencyInSamples()) : 0;

    dryDelay.setMaximumDelayInSamples(8191);
    dryDelay.prepare({ sampleRate, static_cast<juce::uint32>(juce::jmax(1, samplesPerBlock)), 2 });
    dryDelay.setDelay(static_cast<float>(oversamplingLatency));
    setLatencySamples(oversamplingLatency);
    dryDelay.reset();
}

void DPC_vstAudioProcessor::releaseResources()
{
    oversampling.reset();
    dryDelay.reset();
}

bool DPC_vstAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& input = layouts.getMainInputChannelSet();
    const auto& output = layouts.getMainOutputChannelSet();
    return input == output && (output == juce::AudioChannelSet::mono()
                            || output == juce::AudioChannelSet::stereo());
}

void DPC_vstAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = getTotalNumInputChannels();
    const int samples = buffer.getNumSamples();
    const int mode = static_cast<int>(parameters.getRawParameterValue("MODE")->load());
    const float detail = parameters.getRawParameterValue("DETAIL")->load();
    const float outputDb = parameters.getRawParameterValue("OUTPUT")->load();
    const float thresholdTarget = juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("CLIP")->load());
    const float mixTarget = parameters.getRawParameterValue("MIX")->load();
    const float ironNorm = std::tanh(1.08f);
    const float heatNorm = std::tanh(1.8f);

    clipThreshold.setTargetValue(thresholdTarget);
    wetMix.setTargetValue(mixTarget);
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outputDb));

    for (int ch = channels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, samples);

    if (dryBuffer.getNumSamples() < samples)
        dryBuffer.setSize(2, samples, false, false, true);
    for (int ch = 0; ch < channels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, samples);

    const auto shapeSample = [mode, detail, ironNorm, heatNorm](float input, float threshold)
    {
        const float safeThreshold = juce::jmax(threshold, 1.0e-6f);
        const float driven = input / safeThreshold;
        float y = 0.0f;

        if (mode == 0)
            y = juce::jlimit(-1.0f, 1.0f, driven);
        else if (mode == 1)
            y = std::tanh(driven + 0.08f * driven * driven) / ironNorm;
        else
            y = std::tanh(driven * 1.8f) / heatNorm;

        const float detailCurve = std::tanh(driven * 0.7f) / std::tanh(0.7f);
        y = y * (1.0f - detail) + detailCurve * detail;
        return juce::jlimit(-1.0f, 1.0f, y) * safeThreshold;
    };

    if (channels == 2)
    {
        auto block = juce::dsp::AudioBlock<float>(buffer);
        auto wetBlock = oversampling.processSamplesUp(block);

        // Advance the shared threshold ramp once per oversampled frame, not once
        // per channel, so left and right remain under identical gain conditions.
        for (size_t i = 0; i < wetBlock.getNumSamples(); ++i)
        {
            const float threshold = clipThreshold.getNextValue();
            for (size_t ch = 0; ch < wetBlock.getNumChannels(); ++ch)
            {
                auto* data = wetBlock.getChannelPointer(ch);
                data[i] = shapeSample(data[i], threshold);
            }
        }
        oversampling.processSamplesDown(block);
    }
    else
    {
        // Mono uses the same shaping curve at the native sample rate. No
        // oversampling latency is reported for this path.
        for (int i = 0; i < samples; ++i)
        {
            const float threshold = clipThreshold.getNextValue();
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample(ch, i, shapeSample(buffer.getSample(ch, i), threshold));
        }
    }

    for (int i = 0; i < samples; ++i)
    {
        const float gain = outputGain.getNextValue();
        const float wet = wetMix.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
        {
            const float wetSample = buffer.getSample(ch, i);
            float mixed = wetSample;

            if (channels == 2)
            {
                const float drySample = dryBuffer.getSample(ch, i);
                const float delayedDry = dryDelay.popSample(ch);
                dryDelay.pushSample(ch, drySample);
                mixed = delayedDry + (wetSample - delayedDry) * wet;
            }
            else
            {
                const float drySample = dryBuffer.getSample(ch, i);
                mixed = drySample + (wetSample - drySample) * wet;
            }

            buffer.setSample(ch, i, mixed * gain);
        }
    }
}

void DPC_vstAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void DPC_vstAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* DPC_vstAudioProcessor::createEditor()
{
    return new DPC_vstAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DPC_vstAudioProcessor();
}
