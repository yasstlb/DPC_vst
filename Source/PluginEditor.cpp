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
    inputSpectrum.fill(-72.0f);
    outputSpectrum.fill(-72.0f);
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
    g.drawText("CLIPPER  /  LIVE TRANSIENT FOLDBACK", 26, 44, 400, 20, juce::Justification::left);

    // Direct measured three-band DSP foldback; no before/after FFT comparison.
    const juce::Rectangle<float> graph(32.0f, 94.0f, 656.0f, 210.0f);
    g.setColour(juce::Colour(0xff101114));
    g.fillRoundedRectangle(graph, 7.0f);
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = graph.getX() + graph.getWidth() * std::log(hz / 20.0f) / std::log(1000.0f);
        g.setColour(juce::Colour(0xff30343a));
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
        g.setColour(juce::Colour(0xff9aa0a8));
        g.setFont(10.0f);
        g.drawText(hz >= 1000.0f ? juce::String(hz / 1000.0f, 0) + "k" : juce::String(hz, 0),
                   juce::roundToInt(x - 18.0f), 308, 36, 14, juce::Justification::centred);
    }
    for (int db = 0; db <= 12; db += 3)
    {
        const float y = graph.getY() + graph.getHeight() * static_cast<float>(db) / 12.0f;
        g.setColour(juce::Colour(0xff30343a));
        g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        g.setColour(juce::Colour(0xff9aa0a8));
        g.drawText(db == 0 ? "0 dB" : "-" + juce::String(db), 36, juce::roundToInt(y) + 2, 42, 13, juce::Justification::left);
    }
    std::array<float, 24> gains {};
    for (int band = 0; band < 24; ++band)
        gains[(size_t)band] = 1.0f;
    juce::Path curve, area;
    constexpr int count = 192;
    for (int i = 0; i < count; ++i)
    {
        const float fraction = static_cast<float>(i) / static_cast<float>(count - 1);
        const float hz = 20.0f * std::pow(1000.0f, fraction);
        // Interpolate between 24 log-spaced DSP band measurements.
        const float bandPosition = juce::jlimit(0.0f,
            static_cast<float>(24 - 1),
            std::log(hz / 20.0f) / std::log(1000.0f) *
                static_cast<float>(24) - 0.5f);
        const int lower = juce::jlimit(0, 24 - 1,
            static_cast<int>(bandPosition));
        const int upper = juce::jmin(24 - 1, lower + 1);
        const float t = bandPosition - static_cast<float>(lower);
        const float gain = gains[(size_t)lower] + t * (gains[(size_t)upper] - gains[(size_t)lower]);
        const float reduction = juce::jlimit(0.0f, 12.0f,
            -juce::Decibels::gainToDecibels(juce::jmax(1.0e-5f, gain)));
        const float x = graph.getX() + fraction * graph.getWidth();
        const float y = graph.getY() + graph.getHeight() * reduction / 12.0f;
        if (i == 0)
        {
            curve.startNewSubPath(x, y);
            area.startNewSubPath(x, graph.getY());
        }
        else curve.lineTo(x, y);
        area.lineTo(x, y);
    }
    area.lineTo(graph.getRight(), graph.getY());
    area.closeSubPath();
    g.setColour(juce::Colour(0x55e0a14b));
    g.fillPath(area);
    g.setColour(accent);
    g.strokePath(curve, juce::PathStrokeType(2.4f));
    g.setColour(juce::Colour(0xffd2d6dc));
    g.setFont(12.0f);
    g.drawText("LIVE TRANSIENT FOLDBACK", 42, 101, 320, 20, juce::Justification::left);
    g.setColour(juce::Colour(0xff949ba5));
    g.setFont(10.0f);
    g.drawText("0 dB = preserved  |  Downward = more DSP foldback  |  CLIP and DETAIL", 36, 330, 640, 18, juce::Justification::left);
    g.drawText("Curve interpolates 24 real DSP bands across 20 Hz - 20 kHz.",
               36, 351, 640, 18, juce::Justification::left);
}

void DPC_vstAudioProcessorEditor::timerCallback()
{
    updateSpectrum();
    repaint();
}

void DPC_vstAudioProcessorEditor::updateSpectrum() {}

void DPC_vstAudioProcessorEditor::resized()
{
    titleLabel.setBounds(24, 14, 160, 34);
    modeLabel.setBounds(28, 378, 150, 20);
    modeBox.setBounds(28, 402, 150, 28);

    const int x[] = { 190, 350, 510 };
    juce::Label* labels[] = { &clipLabel, &detailLabel, &mixLabel };
    juce::Slider* sliders[] = { &clipSlider, &detailSlider, &mixSlider };
    for (int i = 0; i < 3; ++i)
    {
        labels[i]->setBounds(x[i], 378, 130, 20);
        sliders[i]->setBounds(x[i], 398, 120, 90);
    }
    outputLabel.setBounds(28, 441, 100, 18);
    outputSlider.setBounds(20, 455, 130, 40);
}
