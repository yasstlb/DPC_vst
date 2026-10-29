#include "PluginEditor.h"

DPC_vstAudioProcessorEditor::DPC_vstAudioProcessorEditor(DPC_vstAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    titleLabel.setText("DPC", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(30.0f, juce::Font::bold));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    modeLabel.setText("MODE", juce::dontSendNotification);
    clipLabel.setText("CLIP AMOUNT", juce::dontSendNotification);
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

    setSize(720, 500);
    startTimerHz(30);
}

void DPC_vstAudioProcessorEditor::paint(juce::Graphics& g)
{
    const auto bg = juce::Colour(0xff111214);
    const auto panel = juce::Colour(0xff25272b);
    const auto accent = juce::Colour(0xffe0a14b);
    g.fillAll(bg);
    g.setColour(panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 12.0f);
    g.setColour(accent);
    g.fillRoundedRectangle(24.0f, 76.0f, 672.0f, 2.0f, 1.0f);
    g.setColour(juce::Colour(0xff777b82));
    g.setFont(12.0f);
    g.drawText("CLIPPER  /  LIVE DSP VIEW", 26, 44, 400, 20, juce::Justification::left);

    const juce::Rectangle<float> scope(28.0f, 92.0f, 664.0f, 104.0f);
    g.setColour(juce::Colour(0xff17191c));
    g.fillRoundedRectangle(scope, 7.0f);
    g.setColour(juce::Colour(0xff34383d));
    for (int i = 1; i < 4; ++i)
        g.drawHorizontalLine(juce::roundToInt(scope.getY() + scope.getHeight() * i / 4.0f), scope.getX(), scope.getRight());
    g.drawHorizontalLine(juce::roundToInt(scope.getCentreY()), scope.getX(), scope.getRight());

    auto drawWave = [&](bool input)
    {
        juce::Path path;
        const float mid = scope.getCentreY();
        const float scale = scope.getHeight() * 0.43f;
        for (int i = 0; i < DPC_vstAudioProcessor::scopeSize; ++i)
        {
            const float value = input ? processor.getInputScopeSample(i) : processor.getOutputScopeSample(i);
            const float x = scope.getX() + scope.getWidth() * (float)i / (DPC_vstAudioProcessor::scopeSize - 1);
            const float y = mid - juce::jlimit(-1.0f, 1.0f, value) * scale;
            if (i == 0) path.startNewSubPath(x, y); else path.lineTo(x, y);
        }
        g.setColour(input ? juce::Colour(0xff7c8796) : accent);
        g.strokePath(path, juce::PathStrokeType(input ? 1.2f : 1.8f));
    };
    drawWave(true);
    drawWave(false);

    // A separate strip plots the detector's per-sample transient score.
    const juce::Rectangle<float> transientView(28.0f, 204.0f, 664.0f, 34.0f);
    g.setColour(juce::Colour(0xff202226));
    g.fillRoundedRectangle(transientView, 4.0f);
    juce::Path transientPath;
    for (int i = 0; i < DPC_vstAudioProcessor::scopeSize; ++i)
    {
        const float value = juce::jlimit(0.0f, 1.0f, processor.getTransientScopeSample(i));
        const float x = transientView.getX() + transientView.getWidth() * (float)i / (DPC_vstAudioProcessor::scopeSize - 1);
        const float y = transientView.getBottom() - value * (transientView.getHeight() - 4.0f);
        if (i == 0) transientPath.startNewSubPath(x, y); else transientPath.lineTo(x, y);
    }
    g.setColour(juce::Colour(0xff70c7a0));
    g.strokePath(transientPath, juce::PathStrokeType(1.5f));
    g.setFont(10.0f);
    g.drawText("TRANSIENT DETECTOR  /  0–100%", 36, 206, 230, 14, juce::Justification::left);

    g.setFont(11.0f);
    g.setColour(juce::Colour(0xff9ba1aa));
    g.drawText("INPUT", 38, 98, 60, 16, juce::Justification::left);
    g.setColour(accent);
    g.drawText("OUTPUT", 98, 98, 70, 16, juce::Justification::left);

    const float inDb = processor.getInputPeak() > 1.0e-6f ? juce::Decibels::gainToDecibels(processor.getInputPeak()) : -120.0f;
    const float outDb = processor.getOutputPeak() > 1.0e-6f ? juce::Decibels::gainToDecibels(processor.getOutputPeak()) : -120.0f;
    const auto metricsY = 250;
    g.setColour(juce::Colour(0xffb8b8b8));
    g.setFont(13.0f);
    g.drawText("IN PEAK  " + juce::String(inDb, 1) + " dBFS", 30, metricsY, 160, 22, juce::Justification::left);
    g.drawText("OUT PEAK  " + juce::String(outDb, 1) + " dBFS", 205, metricsY, 180, 22, juce::Justification::left);
    g.drawText("CLIP ACTIVITY  " + juce::String(processor.getClipActivity() > 0.0f ? "ACTIVE" : "IDLE"),
               405, metricsY, 180, 22, juce::Justification::left);
    g.drawText("TRANSIENT DETECTOR  " + juce::String(processor.getTransientActivity() * 100.0f, 0) + "%",
               30, metricsY + 24, 260, 22, juce::Justification::left);
    g.setFont(11.0f);
    g.setColour(juce::Colour(0xff777b82));
    g.drawText("Grey = input  |  Amber = processed output  |  Clip activity is a threshold-crossing indicator",
               30, metricsY + 48, 650, 18, juce::Justification::left);
}

void DPC_vstAudioProcessorEditor::resized()
{
    titleLabel.setBounds(24, 14, 160, 34);
    modeLabel.setBounds(28, 326, 150, 20);
    modeBox.setBounds(28, 350, 150, 28);

    const int x[] = { 190, 350, 510 };
    juce::Label* labels[] = { &clipLabel, &detailLabel, &mixLabel };
    juce::Slider* sliders[] = { &clipSlider, &detailSlider, &mixSlider };
    for (int i = 0; i < 3; ++i)
    {
        labels[i]->setBounds(x[i], 326, 130, 20);
        sliders[i]->setBounds(x[i], 346, 120, 130);
    }
    outputLabel.setBounds(28, 392, 100, 20);
    outputSlider.setBounds(20, 405, 130, 80);
}
