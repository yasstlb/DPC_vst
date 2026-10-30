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
    spectrumWriter = 0;
    spectrumPublished.store(0, std::memory_order_relaxed);
    spectrumSampleRate.store(sampleRate, std::memory_order_relaxed);
    for (auto& v : spectrumInput) v.store(0.0f, std::memory_order_relaxed);
    for (auto& v : spectrumOutput) v.store(0.0f, std::memory_order_relaxed);

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
    detailSplitCoeff[0] = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi
                                       * 180.0f / static_cast<float>(processingRate));
    detailSplitCoeff[1] = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi
                                       * 2800.0f / static_cast<float>(processingRate));
    detailEnvelopeFastAttack = envelopeCoeff(0.0015f);
    detailEnvelopeFastRelease = envelopeCoeff(0.035f);
    detailEnvelopeSlowAttack = envelopeCoeff(0.030f);
    detailEnvelopeSlowRelease = envelopeCoeff(0.170f);
    for (auto& s : detailSplit) for (auto& v : s) v = 0.0f;
    for (auto& v : detailFast) v = 0.0f;
    for (auto& v : detailSlow) v = 0.0f;
    for (auto& v : detailProtect) v = 0.0f;
    const auto pole = [processingRate](float hz)
    {
        return 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * hz
                              / static_cast<float>(processingRate));
    };
    ironMemoryCoeff = pole(1800.0f);
    ironLowCoeff = pole(120.0f);
    heatLowCoeff = pole(350.0f);
    heatAirCoeff = pole(10500.0f);
    for (int ch = 0; ch < 2; ++ch)
    {
        for (auto& state : ironMemory[ch]) state = 0.0f;
        for (auto& state : ironLow[ch]) state = 0.0f;
        for (auto& state : heatLow[ch]) state = 0.0f;
        for (auto& state : heatAir[ch]) state = 0.0f;
    }
    dcBlockerCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * 20.0f
                              / static_cast<float>(processingRate));
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
    float blockInputPeak = 0.0f, blockOutputPeak = 0.0f, blockClipActivity = 0.0f, blockTransientActivity = 0.0f;
    float clippingStrengthSum = 0.0f;
    int clippingFrames = 0;
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

    // CORE has unity small-signal slope. IRON and HEAT apply ONLY the
    // nonlinear residual, avoiding their former small-signal amplification
    // and removing the linear coloration of their internal filters.
    const auto shapeSample = [this, mode, clipAmount](float input, float threshold, int ch, int region)
    {
        if (clipAmount <= 0.0001f) return input;
        const float safeThreshold = juce::jmax(threshold, 1.0e-5f);
        const float x = input / safeThreshold;
        if (mode == 0)
            return juce::jlimit(-1.0f, 1.0f, x) * safeThreshold;

        if (mode == 1)
        {
            ironLow[ch][region] += ironLowCoeff * (x - ironLow[ch][region]);
            const float excitation = x + 0.11f * ironLow[ch][region];
            const float rate = ironMemoryCoeff / (1.0f + 0.7f * std::abs(excitation));
            ironMemory[ch][region] += rate * (excitation - ironMemory[ch][region]);
            const float flux = excitation - 0.10f * ironMemory[ch][region];
            const float asym = flux + 0.075f * flux * flux / (1.0f + std::abs(flux));
            const float nonlinearResidual = std::tanh(1.35f * asym) / 1.35f - flux;
            return juce::jlimit(-1.0f, 1.0f, x + nonlinearResidual) * safeThreshold;
        }

        heatLow[ch][region] += heatLowCoeff * (x - heatLow[ch][region]);
        const float emphasized = x + 0.18f * (x - heatLow[ch][region]);
        const float stageOne = std::tanh(1.45f * emphasized) / 1.45f;
        const float stageTwo = std::tanh(1.75f * stageOne) / 1.75f;
        const float nonlinearResidual = stageTwo - emphasized;
        heatAir[ch][region] += heatAirCoeff * (nonlinearResidual - heatAir[ch][region]);
        return juce::jlimit(-1.0f, 1.0f,
            x + 0.78f * nonlinearResidual + 0.22f * heatAir[ch][region]) * safeThreshold;
    };

    // Complementary split: low + mid + high = input. The actual nonlinear
    // output is generated separately for each region; transient regions
    // crossfade toward their *unprocessed* signals under DETAIL.
    const auto separateRegions = [this](float x, int ch, float (&regions)[detailRegions])
    {
        detailSplit[ch][0] += detailSplitCoeff[0] * (x - detailSplit[ch][0]);
        detailSplit[ch][1] += detailSplitCoeff[1] * (x - detailSplit[ch][1]);
        regions[0] = detailSplit[ch][0];
        regions[1] = detailSplit[ch][1] - detailSplit[ch][0];
        regions[2] = x - detailSplit[ch][1];
    };
    const auto updateProtection = [this, detail](const float (&levels)[detailRegions],
                                                  float (&protection)[detailRegions])
    {
        float greatestAttack = 0.0f;
        for (int region = 0; region < detailRegions; ++region)
        {
            const float value = levels[region];
            const float fastCoeff = value > detailFast[region]
                ? detailEnvelopeFastAttack : detailEnvelopeFastRelease;
            const float slowCoeff = value > detailSlow[region]
                ? detailEnvelopeSlowAttack : detailEnvelopeSlowRelease;
            detailFast[region] = fastCoeff * detailFast[region] + (1.0f - fastCoeff) * value;
            detailSlow[region] = slowCoeff * detailSlow[region] + (1.0f - slowCoeff) * value;
            const float attack = juce::jlimit(0.0f, 1.0f,
                (detailFast[region] - detailSlow[region])
                / (detailFast[region] + 1.0e-5f));
            // Attack-only bypass. Higher DETAIL keeps more of the original
            // transient in that region while leaving sustained content clipped.
            const float target = detail * attack;
            detailProtect[region] += 0.14f * (target - detailProtect[region]);
            protection[region] = detailProtect[region];
            greatestAttack = juce::jmax(greatestAttack, attack);
        }
        return greatestAttack;
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
        for (size_t i = 0; i < wetBlock.getNumSamples(); ++i)
        {
            const float threshold = clipThreshold.getNextValue();
            float regions[2][detailRegions] {};
            float levels[detailRegions] {};
            for (int ch = 0; ch < 2; ++ch)
            {
                separateRegions(wetBlock.getSample(static_cast<size_t>(ch), i), ch, regions[ch]);
                for (int r = 0; r < detailRegions; ++r)
                    levels[r] = juce::jmax(levels[r], std::abs(regions[ch][r]));
            }
            float protection[detailRegions] {};
            const float attack = updateProtection(levels, protection);
            blockTransientActivity = juce::jmax(blockTransientActivity, attack);
            const int scopeBin = juce::jmin(scopeSize - 1,
                static_cast<int>(i * scopeSize / juce::jmax<size_t>(1, wetBlock.getNumSamples())));
            transientScope[(size_t)scopeBin].store(attack, std::memory_order_relaxed);
            for (int r = 0; r < detailRegions; ++r)
            {
                const float excess = juce::jmax(0.0f, levels[r] - threshold);
                clippingStrengthSum += (1.0f - protection[r])
                    * juce::jlimit(0.0f, 1.0f, excess / juce::jmax(levels[r], 1.0e-6f));
                ++clippingFrames;
            }
            for (int ch = 0; ch < 2; ++ch)
            {
                float sum = 0.0f;
                for (int r = 0; r < detailRegions; ++r)
                {
                    const float dry = regions[ch][r];
                    const float wet = shapeSample(dry, threshold, ch, r);
                    sum += wet + protection[r] * (dry - wet);
                }
                auto* data = wetBlock.getChannelPointer(static_cast<size_t>(ch));
                data[i] = removeDc(sum, ch);
            }
        }
        oversampling.processSamplesDown(block);
    }
    else
    {
        for (int i = 0; i < samples; ++i)
        {
            const float threshold = clipThreshold.getNextValue();
            float regions[detailRegions] {};
            separateRegions(buffer.getSample(0, i), 0, regions);
            float levels[detailRegions] {};
            for (int r = 0; r < detailRegions; ++r)
                levels[r] = std::abs(regions[r]);
            float protection[detailRegions] {};
            const float attack = updateProtection(levels, protection);
            blockTransientActivity = juce::jmax(blockTransientActivity, attack);
            const int scopeBin = juce::jmin(scopeSize - 1, i * scopeSize / juce::jmax(1, samples));
            transientScope[(size_t)scopeBin].store(attack, std::memory_order_relaxed);
            float sum = 0.0f;
            for (int r = 0; r < detailRegions; ++r)
            {
                const float excess = juce::jmax(0.0f, levels[r] - threshold);
                clippingStrengthSum += (1.0f - protection[r])
                    * juce::jlimit(0.0f, 1.0f, excess / juce::jmax(levels[r], 1.0e-6f));
                ++clippingFrames;
                const float wet = shapeSample(regions[r], threshold, 0, r);
                sum += wet + protection[r] * (regions[r] - wet);
            }
            buffer.setSample(0, i, removeDc(sum, 0));
        }
    }

    // Publish the wet signal BEFORE MIX and OUTPUT, so the spectral visualization
    // measures the nonlinear processor rather than user gain compensation.
    for (int i = 0; i < samples; ++i)
    {
        float wetSum = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            wetSum += buffer.getSample(ch, i);
        const int ringIndex = (spectrumWriter + i) % fftSize;
        spectrumOutput[(size_t)ringIndex].store(
            wetSum / static_cast<float>(juce::jmax(1, channels)),
            std::memory_order_relaxed);
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
    // Publish input samples on the same rolling timeline as the wet-only tap.
    for (int i = 0; i < samples; ++i)
    {
        float in = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
        {
            in += dryBuffer.getSample(ch, i);
        }
        const float invChannels = 1.0f / static_cast<float>(juce::jmax(1, channels));
        spectrumInput[(size_t)spectrumWriter].store(in * invChannels, std::memory_order_relaxed);
        spectrumWriter = (spectrumWriter + 1) % fftSize;
    }
    spectrumPublished.store(spectrumWriter, std::memory_order_release);
    inputPeak.store(blockInputPeak, std::memory_order_relaxed);
    outputPeak.store(blockOutputPeak, std::memory_order_relaxed);
    blockClipActivity = (clipAmount > 0.0001f && clippingFrames > 0)
        ? clippingStrengthSum / static_cast<float>(clippingFrames) : 0.0f;
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
