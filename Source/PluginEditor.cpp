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
    spectralDelta.fill(0.0f);
    foldShape.fill(0.0f);
    startTimerHz(30);
}

void DPC_vstAudioProcessorEditor::paint(juce::Graphics& g)
{
    const auto bg = juce::Colour(0xff111214);
    const auto panel = juce::Colour(0xff25272b);
    const auto accent = juce::Colour(0xffff5ca8);
    g.fillAll(bg);
    g.setColour(panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(12.0f), 12.0f);
    g.setColour(accent);
    g.fillRoundedRectangle(24.0f, 76.0f, 672.0f, 2.0f, 1.0f);
    g.setColour(juce::Colour(0xff777b82));
    g.setFont(12.0f);
    g.drawText("CLIPPER  /  SPECTRAL TRANSFORMATION", 26, 44, 420, 20, juce::Justification::left);


    const juce::Rectangle<float> graph(32.0f, 94.0f, 656.0f, 210.0f);
    g.setColour(juce::Colour(0xff0c101a));
    g.fillRoundedRectangle(graph, 7.0f);
    g.saveState();
    g.reduceClipRegion(graph.toNearestInt());
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = graph.getX() + graph.getWidth() * std::log(hz / 20.0f) / std::log(1000.0f);
        g.setColour(juce::Colour(0xff242a39));
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
    }
    for (int db = -72; db <= 0; db += 12)
    {
        const float y = graph.getBottom() - graph.getHeight() * (db + 90.0f) / 90.0f;
        g.setColour(juce::Colour(0xff242a39));
        g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
    }
    juce::Path inputArea, outputLine, foldLine;
    const float glow = juce::jlimit(0.0f, 1.0f, clipGlow * 3.0f);
    for (int i = 0; i < spectrumPoints; ++i)
    {
        const float x = graph.getX() + graph.getWidth() * static_cast<float>(i) / (spectrumPoints - 1);
        const auto position = [&graph](float db)
        {
            return graph.getBottom() - graph.getHeight() * juce::jlimit(0.0f, 1.0f, (db + 90.0f) / 90.0f);
        };
        const float inY = position(inputSpectrum[(size_t)i]);
        const float outY = position(outputSpectrum[(size_t)i]);
        // Expressive spectral folding gesture driven by measured FFT movement.
        // This is intentionally NOT a map of affected clipper frequencies.
        const float foldY = graph.getY() + 18.0f + graph.getHeight() * 0.46f * foldShape[(size_t)i];
        if (i == 0)
        {
            inputArea.startNewSubPath(x, graph.getBottom());
            inputArea.lineTo(x, inY);
            outputLine.startNewSubPath(x, outY);
            foldLine.startNewSubPath(x, foldY);
        }
        else
        {
            inputArea.lineTo(x, inY);
            outputLine.lineTo(x, outY);
            foldLine.lineTo(x, foldY);
        }
    }
    inputArea.lineTo(graph.getRight(), graph.getBottom());
    inputArea.closeSubPath();
    // Measured clip intensity controls colour/brightness, never an invented frequency mask.
    g.setColour(juce::Colour(0xff3578ff).withAlpha(0.20f + 0.36f * glow));
    g.fillPath(inputArea);
    g.setColour(juce::Colour(0xff489cf7).withAlpha(0.36f + 0.54f * glow));
    g.strokePath(outputLine, juce::PathStrokeType(1.3f + 0.65f * glow));
    g.setColour(accent.withAlpha(0.12f + 0.24f * glow));
    g.strokePath(foldLine, juce::PathStrokeType(5.0f));
    g.setColour(accent.withAlpha(0.75f + 0.25f * glow));
    g.strokePath(foldLine, juce::PathStrokeType(1.65f));
    // Compact peak-clipping activity meter, sourced from the audio-thread detector.
    g.setColour(juce::Colour(0xff252c3a));
    g.fillRoundedRectangle(graph.getRight() - 91.0f, graph.getY() + 15.0f, 72.0f, 4.0f, 2.0f);
    g.setColour(accent.withAlpha(0.40f + 0.60f * glow));
    g.fillRoundedRectangle(graph.getRight() - 91.0f, graph.getY() + 15.0f,
                           72.0f * glow, 4.0f, 2.0f);
    g.restoreState();
    g.setColour(juce::Colour(0xffa3a9ba));
    g.setFont(10.0f);
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = graph.getX() + graph.getWidth() * std::log(hz / 20.0f) / std::log(1000.0f);
        g.drawText(hz >= 1000.0f ? juce::String(hz / 1000.0f, 0) + "k" : juce::String(hz, 0),
                   juce::roundToInt(x - 17.0f), 308, 34, 14, juce::Justification::centred);
    }
    g.setColour(juce::Colour(0xffd2d6dc));
    g.setFont(12.0f);
    g.drawText("LIVE SPECTRAL TRANSFORMATION", 42, 101, 330, 20, juce::Justification::left);
    g.setColour(juce::Colour(0xff949ba5));
    g.setFont(10.0f);
    g.drawText("BLUE: input   /   CYAN: output   /   PINK: spectral motion   /   BRIGHTNESS: clipping", 36, 330, 645, 18, juce::Justification::left);
    g.drawText("Expressive fold gesture follows spectral attacks; glow tracks measured clip intensity.", 36, 351, 645, 18, juce::Justification::left);
}

void DPC_vstAudioProcessorEditor::timerCallback()
{
    updateSpectrum();
    repaint();
}

void DPC_vstAudioProcessorEditor::updateSpectrum()
{
    const int writePosition = processor.getSpectrumWritePosition();
    const double sampleRate = processor.getSpectrumSampleRate();
    if (sampleRate <= 0.0) return;
    clipGlow += 0.22f * (processor.getClipActivity() - clipGlow);
    transientGlow += 0.22f * (processor.getTransientActivity() - transientGlow);
    fftInput.fill(0.0f);
    fftOutput.fill(0.0f);
    constexpr int n = DPC_vstAudioProcessor::fftSize;
    for (int i = 0; i < n; ++i)
    {
        const int index = (writePosition + i) % n;
        const float window = 0.5f - 0.5f * std::cos(
            juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(n - 1));
        fftInput[(size_t)i] = processor.getSpectrumInput(index) * window;
        fftOutput[(size_t)i] = processor.getSpectrumOutput(index) * window;
    }
    fft.performFrequencyOnlyForwardTransform(fftInput.data());
    fft.performFrequencyOnlyForwardTransform(fftOutput.data());
    for (int i = 0; i < spectrumPoints; ++i)
    {
        const float f = static_cast<float>(i) / static_cast<float>(spectrumPoints - 1);
        const float hz = 20.0f * std::pow(1000.0f, f);
        const float bin = hz * static_cast<float>(n) / static_cast<float>(sampleRate);
        const int lo = juce::jlimit(1, n / 2 - 1, static_cast<int>(bin));
        const int hi = lo + 1;
        const float t = juce::jlimit(0.0f, 1.0f, bin - static_cast<float>(lo));
        const float in = juce::jmap(t, fftInput[(size_t)lo], fftInput[(size_t)hi]);
        const float out = juce::jmap(t, fftOutput[(size_t)lo], fftOutput[(size_t)hi]);
        // Hann coherent gain is approximately N/4 for real-input FFT magnitudes.
        const float inputDb = juce::Decibels::gainToDecibels(in / (n * 0.25f), -90.0f);
        const float outputDb = juce::Decibels::gainToDecibels(out / (n * 0.25f), -90.0f);
        const float delta = inputDb > -72.0f ? outputDb - inputDb : 0.0f;
        inputSpectrum[(size_t)i] += 0.32f * (inputDb - inputSpectrum[(size_t)i]);
        outputSpectrum[(size_t)i] += 0.32f * (outputDb - outputSpectrum[(size_t)i]);
        spectralDelta[(size_t)i] += 0.28f * (delta - spectralDelta[(size_t)i]);
        // Rising spectral energy creates localized fold gestures; steady tones settle.
        // No free-running animation: movement derives from successive FFT frames.
        const float onset = juce::jlimit(0.0f, 1.0f,
            (inputDb - inputSpectrum[(size_t)i] + 1.5f) / 17.0f);
        const float energy = juce::jlimit(0.0f, 1.0f, (inputDb + 72.0f) / 58.0f);
        const float targetFold = energy * (0.28f + 0.72f * onset)
            * (0.40f + 0.60f * transientGlow);
        foldShape[(size_t)i] += 0.24f * (targetFold - foldShape[(size_t)i]);
    }
}

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
