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

void DPC_vstAudioProcessor::prepareToPlay(double sampleRate, int)
{
    outputGain.reset(sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(
        parameters.getRawParameterValue("OUTPUT")->load()));
}

void DPC_vstAudioProcessor::releaseResources() {}

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
    const float mix = parameters.getRawParameterValue("MIX")->load();
    const float outputDb = parameters.getRawParameterValue("OUTPUT")->load();

    // CLIP is the actual output ceiling. Normalise before the curve so each
    // mode uses the same nominal ceiling, rather than clipping at 0 dBFS.
    const float threshold = juce::Decibels::decibelsToGain(clipDb);
    const float ironNorm = std::tanh(1.08f);
    const float heatNorm = std::tanh(1.8f);
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outputDb));

    for (int ch = channels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, samples);

    // Advance the output smoother once per frame, keeping stereo channels linked.
    for (int i = 0; i < samples; ++i)
    {
        const float gain = outputGain.getNextValue();
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            const float dry = data[i];
            const float driven = dry / juce::jmax(threshold, 1.0e-6f);
            float normalisedWet = 0.0f;

            if (mode == 0) // CORE: hard, symmetric ceiling
            {
                normalisedWet = juce::jlimit(-1.0f, 1.0f, driven);
            }
            else if (mode == 1) // IRON: mild asymmetric saturation
            {
                const float asym = driven + 0.08f * driven * driven;
                normalisedWet = std::tanh(asym) / ironNorm;
            }
            else // HEAT: denser, more aggressive soft saturation
            {
                normalisedWet = std::tanh(driven * 1.8f) / heatNorm;
            }

            // DETAIL currently crossfades from the selected curve toward a
            // softer curve. It is a tonal control, not yet a measured transient
            // reconstruction algorithm; see README limitations.
            const float soft = std::tanh(driven * 0.7f) / std::tanh(0.7f);
            normalisedWet = normalisedWet * (1.0f - detail) + soft * detail;
            const float wet = juce::jlimit(-1.0f, 1.0f, normalisedWet) * threshold;
            data[i] = (dry + (wet - dry) * mix) * gain;
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DPC_vstAudioProcessor();
}
