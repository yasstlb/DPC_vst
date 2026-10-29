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
        juce::NormalisableRange<float>(0.0f, 18.0f, 0.01f), 6.0f, " dB"));
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

    const double processingRate = sampleRate * (stereo ? 4.0 : 1.0);
    clipThreshold.reset(processingRate, 0.005);
    clipThreshold.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        -parameters.getRawParameterValue("CLIP")->load()));

    const auto envelopeCoeff = [processingRate](float timeSeconds)
    {
        return std::exp(-1.0f / (static_cast<float>(processingRate) * timeSeconds));
    };
    fastAttackCoeff = envelopeCoeff(0.0015f);
    fastReleaseCoeff = envelopeCoeff(0.040f);
    slowAttackCoeff = envelopeCoeff(0.025f);
    slowReleaseCoeff = envelopeCoeff(0.180f);
    fastEnvelope = 0.0f;
    slowEnvelope = 0.0f;
    dcBlockerCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * 20.0f
                              / static_cast<float>(sampleRate));
    for (int ch = 0; ch < 2; ++ch)
    {
        dcInput[ch] = 0.0f;
        dcOutput[ch] = 0.0f;
    }
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
    const float clipAmount = parameters.getRawParameterValue("CLIP")->load();
    const float outputDb = parameters.getRawParameterValue("OUTPUT")->load();
    const float thresholdTarget = juce::Decibels::decibelsToGain(
        -parameters.getRawParameterValue("CLIP")->load());
    const float mixTarget = parameters.getRawParameterValue("MIX")->load();
    const float ironNorm = std::tanh(1.08f);
    const float heatNorm = std::tanh(1.8f);
    float blockInputPeak = 0.0f, blockOutputPeak = 0.0f, blockClipActivity = 0.0f, blockTransientActivity = 0.0f;
    // Resample each host block to a fixed-width scope, including short blocks.
    for (int bin = 0; bin < scopeSize; ++bin)
    {
        const int begin = bin * samples / scopeSize;
        const int end = juce::jmax(begin + 1, (bin + 1) * samples / scopeSize);
        float peak = 0.0f;
        for (int i = begin; i < juce::jmin(end, samples); ++i)
        {
            float sample = 0.0f;
            for (int ch = 0; ch < channels; ++ch) sample += buffer.getSample(ch, i);
            sample /= static_cast<float>(juce::jmax(1, channels));
            peak = juce::jmax(peak, std::abs(sample));
        }
        const int center = juce::jlimit(0, samples - 1, (begin + end) / 2);
        float sample = 0.0f;
        for (int ch = 0; ch < channels; ++ch) sample += buffer.getSample(ch, center);
        sample /= static_cast<float>(juce::jmax(1, channels));
        inputScope[(size_t)bin].store(sample, std::memory_order_relaxed);
        blockInputPeak = juce::jmax(blockInputPeak, peak);
    }
    for (auto& value : transientScope) value.store(0.0f, std::memory_order_relaxed);

    clipThreshold.setTargetValue(thresholdTarget);
    wetMix.setTargetValue(mixTarget);
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outputDb));

    for (int ch = channels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, samples);

    if (dryBuffer.getNumSamples() < samples)
        dryBuffer.setSize(2, samples, false, false, true);
    for (int ch = 0; ch < channels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, samples);

    // DETAIL is a transient-preservation control, not a blend between waveshapers.
    // Fast/slow envelope divergence raises the effective threshold briefly on attacks.
    const auto shapeSample = [mode, ironNorm, heatNorm, clipAmount](float input, float threshold)
    {
        if (clipAmount <= 0.0001f) return input;
        const float safeThreshold = juce::jmax(threshold, 1.0e-6f);
        const float driven = input / safeThreshold;
        float y = 0.0f;

        if (mode == 0)
            y = juce::jlimit(-1.0f, 1.0f, driven);
        else if (mode == 1)
            y = std::tanh(driven + 0.08f * driven * driven) / ironNorm;
        else
            y = std::tanh(driven * 1.8f) / heatNorm;

        return juce::jlimit(-1.0f, 1.0f, y) * safeThreshold;
    };

    int transientScopeBin = 0;
    const auto getTransientThreshold = [this, detail, samples, &blockTransientActivity, &transientScopeBin](float linkedLevel, float baseThreshold)
    {
        const float fastCoeff = linkedLevel > fastEnvelope ? fastAttackCoeff : fastReleaseCoeff;
        const float slowCoeff = linkedLevel > slowEnvelope ? slowAttackCoeff : slowReleaseCoeff;
        fastEnvelope = fastCoeff * fastEnvelope + (1.0f - fastCoeff) * linkedLevel;
        slowEnvelope = slowCoeff * slowEnvelope + (1.0f - slowCoeff) * linkedLevel;

        const float transient = juce::jlimit(0.0f, 1.0f,
            (fastEnvelope - slowEnvelope) / (fastEnvelope + 1.0e-5f));
        // DETAIL scales the transient threshold lift up to 2x at full transient level.
        blockTransientActivity = juce::jmax(blockTransientActivity, transient);
        const int bin = juce::jmin(scopeSize - 1, transientScopeBin++ * scopeSize / juce::jmax(1, samples * (getTotalNumInputChannels() == 2 ? 4 : 1)));
        transientScope[(size_t)bin].store(transient, std::memory_order_relaxed);
        const float thresholdLift = 1.0f + detail * transient;
        return baseThreshold * thresholdLift;
    };

    const auto removeDc = [this](float input, int channel)
    {
        const float output = input - dcInput[channel] + dcBlockerCoeff * dcOutput[channel];
        dcInput[channel] = input;
        dcOutput[channel] = output;
        return output;
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
            const float linkedLevel = juce::jmax(
                std::abs(wetBlock.getSample(0, i)),
                std::abs(wetBlock.getSample(1, i)));
            const float transientAwareThreshold = getTransientThreshold(linkedLevel, threshold);
            if (linkedLevel > transientAwareThreshold) blockClipActivity = 1.0f;
            for (size_t ch = 0; ch < wetBlock.getNumChannels(); ++ch)
            {
                auto* data = wetBlock.getChannelPointer(ch);
                data[i] = removeDc(shapeSample(data[i], transientAwareThreshold), static_cast<int>(ch));
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
            float linkedLevel = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                linkedLevel = juce::jmax(linkedLevel, std::abs(buffer.getSample(ch, i)));
            const float transientAwareThreshold = getTransientThreshold(linkedLevel, threshold);
            if (linkedLevel > transientAwareThreshold) blockClipActivity = 1.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float input = buffer.getSample(ch, i);
                buffer.setSample(ch, i, removeDc(shapeSample(input, transientAwareThreshold), ch));
            }
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

            const float outputSample = mixed * gain;
            buffer.setSample(ch, i, outputSample);
            blockOutputPeak = juce::jmax(blockOutputPeak, std::abs(outputSample));
        }
    }

    // Store post-processing waveform samples for the editor without locks.
    for (int bin = 0; bin < scopeSize; ++bin)
    {
        const int sampleIndex = juce::jlimit(0, samples - 1, (bin * samples + samples / 2) / scopeSize);
        float sample = 0.0f;
        for (int ch = 0; ch < channels; ++ch) sample += buffer.getSample(ch, sampleIndex);
        sample /= static_cast<float>(juce::jmax(1, channels));
        outputScope[(size_t)bin].store(sample, std::memory_order_relaxed);
    }
    inputPeak.store(blockInputPeak, std::memory_order_relaxed);
    outputPeak.store(blockOutputPeak, std::memory_order_relaxed);
    clipActivity.store(blockClipActivity, std::memory_order_relaxed);
    transientActivity.store(blockTransientActivity, std::memory_order_relaxed);
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
