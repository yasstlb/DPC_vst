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

    // Enlarged clip-history view: grey input and amber processed signal.
    // This is a block snapshot, not a scrolling history or FFT spectrum.
    const juce::Rectangle<float> scope(28.0f, 88.0f, 664.0f, 108.0f);
    g.setColour(juce::Colour(0xff101114));
    g.fillRoundedRectangle(scope, 6.0f);
    const float clipDb = processor.parameters.getRawParameterValue("CLIP")->load();
    const float threshold = juce::Decibels::decibelsToGain(-clipDb);
    const float scale = scope.getHeight() * 0.44f;
    const float centre = scope.getCentreY();
    const float topLimit = centre - threshold * scale;
    const float bottomLimit = centre + threshold * scale;
    g.setColour(juce::Colour(0xff30343a));
    g.drawHorizontalLine(juce::roundToInt(centre), scope.getX(), scope.getRight());
    g.setColour(juce::Colour(0x44e0a14b));
    g.fillRect(scope.getX(), topLimit - 1.0f, scope.getWidth(), 2.0f);
    g.fillRect(scope.getX(), bottomLimit - 1.0f, scope.getWidth(), 2.0f);

    juce::Path inputFill, outputLine;
    inputFill.startNewSubPath(scope.getX(), centre);
    for (int i = 0; i < DPC_vstAudioProcessor::scopeSize; ++i)
    {
        const float x = scope.getX() + scope.getWidth() * static_cast<float>(i)
            / static_cast<float>(DPC_vstAudioProcessor::scopeSize - 1);
        const float input = juce::jlimit(-1.0f, 1.0f, processor.getInputScopeSample(i));
        const float output = juce::jlimit(-1.0f, 1.0f, processor.getOutputScopeSample(i));
        const float inY = centre - input * scale;
        const float outY = centre - output * scale;
        inputFill.lineTo(x, inY);
        if (i == 0) outputLine.startNewSubPath(x, outY);
        else outputLine.lineTo(x, outY);
        // Highlight only input excursions beyond the actual base clip threshold.
        if (std::abs(input) > threshold)
        {
            g.setColour(juce::Colour(0x88ff9c3d));
            const float limitY = input > 0.0f ? topLimit : bottomLimit;
            g.drawVerticalLine(juce::roundToInt(x), juce::jmin(inY, limitY), juce::jmax(inY, limitY));
        }
    }
    inputFill.lineTo(scope.getRight(), centre);
    inputFill.closeSubPath();
    g.setColour(juce::Colour(0x667f8793));
    g.fillPath(inputFill);
    g.setColour(accent);
    g.strokePath(outputLine, juce::PathStrokeType(1.7f));
    g.setFont(10.0f);
    g.setColour(juce::Colour(0xffffc47a));
    g.drawText("CLIP THRESHOLD  -" + juce::String(clipDb, 1) + " dB",
               405, 91, 275, 16, juce::Justification::right);
    // The three meters display measured per-band gain reduction from the DSP.
    const juce::Rectangle<float> transientView(28.0f, 204.0f, 664.0f, 34.0f);
    g.setColour(juce::Colour(0xff202226));
    g.fillRoundedRectangle(transientView, 4.0f);
    const char* names[3] = { "<100 Hz", "100-1000 Hz", ">1 kHz" };
    for (int b = 0; b < 3; ++b)
    {
        const float x = transientView.getX() + 6.0f + b * 219.0f;
        const float reduction = juce::jlimit(0.0f, 1.0f, processor.getBandDuck(b));
        g.setColour(juce::Colour(0xff363a40));
        g.fillRoundedRectangle(x, 223.0f, 120.0f, 6.0f, 2.0f);
        g.setColour(b == 1 ? accent : juce::Colour(0xff70c7a0));
        g.fillRoundedRectangle(x, 223.0f, 120.0f * reduction, 6.0f, 2.0f);
        g.setFont(10.0f);
        g.drawText(juce::String(names[b]) + "  -" + juce::String(reduction * 100.0f, 0) + "%",
                   juce::roundToInt(x + 123.0f), 217, 92, 16, juce::Justification::left);
    }
    g.setColour(juce::Colour(0xff9ba1aa));
    g.setFont(10.0f);
    g.drawText("LIVE FREQUENCY-BAND DUCKING", 36, 205, 400, 14, juce::Justification::left);

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
    g.drawText("ATTACK RESPONSE  " + juce::String(processor.getTransientActivity() * 100.0f, 0) + "%",
               30, metricsY + 24, 260, 22, juce::Justification::left);
    g.setFont(11.0f);
    g.setColour(juce::Colour(0xff777b82));
    g.drawText("Grey: input  |  Amber: output  |  Orange spikes: above base threshold  |  Bars: band ducking",
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
