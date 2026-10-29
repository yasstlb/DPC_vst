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
    outputGain.reset(sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("OUTPUT")->load()));

    clipThreshold.reset(sampleRate, 0.005);
    clipThreshold.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("CLIP")->load()));
    wetMix.reset(sampleRate, 0.005);
    wetMix.setCurrentAndTargetValue(parameters.getRawParameterValue("MIX")->load());

    dryBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
    oversampling.initProcessing(static_cast<size_t>(juce::jmax(1, samplesPerBlock)));
    oversampling.reset();
    oversamplingLatency = juce::roundToInt(oversampling.getLatencyInSamples());

    dryDelay.setMaximumDelayInSamples(8191);
    dryDelay.prepare({ sampleRate, static_cast<juce::uint32>(juce::jmax(1, samplesPerBlock)), 2 });
    dryDelay.setDelay(static_cast<float>(oversamplingLatency));
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
    const float clipDb = parameters.getRawParameterValue("CLIP")->load();
    const float detail = parameters.getRawParameterValue("DETAIL")->load();
    const float mixTarget = parameters.getRawParameterValue("MIX")->load();
    const float outputDb = parameters.getRawParameterValue("OUTPUT")->load();
    const float thresholdTarget = juce::Decibels::decibelsToGain(clipDb);
    const float ironNorm = std::tanh(1.08f);
    const float heatNorm = std::tanh(1.8f);

    clipThreshold.setTargetValue(thresholdTarget);
    wetMix.setTargetValue(mixTarget);
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outputDb));

    for (int ch = channels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, samples);

    dryBuffer.setSize(2, samples, false, false, true);
    for (int ch = 0; ch < channels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, samples);

    // Oversampling is used for stereo; mono hosts retain the same DSP curve
    // without passing a one-channel block to the two-channel oversampler.
    if (channels == 2)
    {
        auto block = juce::dsp::AudioBlock<float>(buffer);
        auto wetBlock = oversampling.processSamplesUp(block);
        for (size_t ch = 0; ch < wetBlock.getNumChannels(); ++ch)
        {
            auto* data = wetBlock.getChannelPointer(ch);
            for (size_t i = 0; i < wetBlock.getNumSamples(); ++i)
            {
                const float threshold = juce::jmax(clipThreshold.getCurrentValue(), 1.0e-6f);
                const float driven = data[i] / threshold;
                float y = 0.0f;
                if (mode == 0)
                    y = juce::jlimit(-1.0f, 1.0f, driven);
                else if (mode == 1)
                    y = std::tanh(driven + 0.08f * driven * driven) / ironNorm;
                else
                    y = std::tanh(driven * 1.8f) / heatNorm;

                const float detailCurve = std::tanh(driven * 0.7f) / std::tanh(0.7f);
                y = y * (1.0f - detail) + detailCurve * detail;
                data[i] = juce::jlimit(-1.0f, 1.0f, y) * threshold;
                clipThreshold.getNextValue();
            }
        }
        oversampling.processSamplesDown(block);
    }

    // Delay the dry path by the oversampler latency, then mix at base rate.
    // The delay is still applied in mono mode for consistent reported latency.
    for (int i = 0; i < samples; ++i)
    {
        const float gain = outputGain.getNextValue();
        const float wet = wetMix.getNextValue();
        for (int ch = 0; ch < channels; ++ch)
        {
            const float wetSample = buffer.getSample(ch, i);
            const float drySample = dryBuffer.getSample(ch, i);
            const float delayedDry = dryDelay.popSample(ch);
            dryDelay.pushSample(ch, drySample);
            const float mixed = channels == 2
                ? (delayedDry + (wetSample - delayedDry) * wet)
                : wetSample;
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
