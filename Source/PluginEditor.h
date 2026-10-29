#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class DPC_vstAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit DPC_vstAudioProcessorEditor(DPC_vstAudioProcessor&);
    ~DPC_vstAudioProcessorEditor() override = default;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { repaint(); }
    DPC_vstAudioProcessor& processor;
    juce::ComboBox modeBox;
    juce::Slider clipSlider, detailSlider, mixSlider, outputSlider;
    juce::Label titleLabel, modeLabel, clipLabel, detailLabel, mixLabel, outputLabel;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<ComboAttachment> modeAttachment;
    std::unique_ptr<SliderAttachment> clipAttachment, detailAttachment, mixAttachment, outputAttachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DPC_vstAudioProcessorEditor)
};
