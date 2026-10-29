#include "PluginEditor.h"

DPC_vstAudioProcessorEditor::DPC_vstAudioProcessorEditor(DPC_vstAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    titleLabel.setText("DPC", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(30.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    modeLabel.setText("MODE", juce::dontSendNotification);
    clipLabel.setText("CLIP", juce::dontSendNotification);
    detailLabel.setText("DETAIL", juce::dontSendNotification);
    mixLabel.setText("MIX", juce::dontSendNotification);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);

    for (auto* label : { &modeLabel, &clipLabel, &detailLabel, &mixLabel, &outputLabel })
    {
        label->setFont(juce::Font(12.0f, juce::Font::bold));
        label->setColour(juce::Label::textColourId, juce::Colour(0xffb8b8b8));
        addAndMakeVisible(*label);
    }

    modeBox.addItemList({ "CORE", "IRON", "HEAT" }, 1);
    addAndMakeVisible(modeBox);
    modeAttachment = std::make_unique<ComboAttachment>(processor.parameters, "MODE", modeBox);

    for (auto* slider : { &clipSlider, &detailSlider, &mixSlider, &outputSlider })
    {
        slider->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 20);
        slider->setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffe0a14b));
        slider->setColour(juce::Slider::thumbColourId, juce::Colour(0xffffcf83));
        addAndMakeVisible(*slider);
    }

    clipAttachment = std::make_unique<SliderAttachment>(processor.parameters, "CLIP", clipSlider);
    detailAttachment = std::make_unique<SliderAttachment>(processor.parameters, "DETAIL", detailSlider);
    mixAttachment = std::make_unique<SliderAttachment>(processor.parameters, "MIX", mixSlider);
    outputAttachment = std::make_unique<SliderAttachment>(processor.parameters, "OUTPUT", outputSlider);

    setSize(620, 360);
}

void DPC_vstAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff111214));
    g.setColour(juce::Colour(0xff25272b));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 12.0f);
    g.setColour(juce::Colour(0xffe0a14b));
    g.fillRoundedRectangle(24.0f, 76.0f, 572.0f, 2.0f, 1.0f);
    g.setColour(juce::Colour(0xff777b82));
    g.setFont(12.0f);
    g.drawText("DETAIL-PRESERVING CLIPPER  /  V0.1", 26, 44, 400, 20, juce::Justification::left);
}

void DPC_vstAudioProcessorEditor::resized()
{
    titleLabel.setBounds(24, 14, 160, 34);
    modeLabel.setBounds(28, 88, 150, 20);
    modeBox.setBounds(28, 112, 150, 28);

    const int x[] = { 28, 178, 328, 478 };
    juce::Label* labels[] = { &clipLabel, &detailLabel, &mixLabel, &outputLabel };
    juce::Slider* sliders[] = { &clipSlider, &detailSlider, &mixSlider, &outputSlider };
    for (int i = 0; i < 4; ++i)
    {
        labels[i]->setBounds(x[i], 166, 120, 20);
        sliders[i]->setBounds(x[i], 184, 118, 140);
    }
}
