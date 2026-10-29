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
    g.drawText("CLIPPER  /  LIVE SPECTRAL RESPONSE", 26, 44, 400, 20, juce::Justification::left);

    const juce::Rectangle<float> graph(32.0f, 94.0f, 656.0f, 210.0f);
    g.setColour(juce::Colour(0xff101114));
    g.fillRoundedRectangle(graph, 7.0f);

    // Log-frequency FFT display, 20 Hz to Nyquist (maximum 20 kHz).
    const float sr = static_cast<float>(processor.getSpectrumSampleRate());
    const float topFrequency = juce::jmin(20000.0f, sr * 0.5f);
    const auto frequencyToX = [&](float frequency)
    {
        return graph.getX() + graph.getWidth() *
            (std::log(frequency / 20.0f) / std::log(topFrequency / 20.0f));
    };
    g.setFont(10.0f);
    for (float freq : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        if (freq > topFrequency) continue;
        const float x = frequencyToX(freq);
        g.setColour(juce::Colour(0xff303238));
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
        g.setColour(juce::Colour(0xff8b9099));
        g.drawText(freq >= 1000.0f ? juce::String(freq / 1000.0f, 0) + "k" : juce::String(freq, 0),
                   juce::roundToInt(x - 16.0f), 308, 34, 14, juce::Justification::centred);
    }
    for (int db = -72; db <= 0; db += 12)
    {
        const float y = graph.getY() + graph.getHeight() * (-static_cast<float>(db) / 72.0f);
        g.setColour(juce::Colour(0xff303238));
        g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        g.setColour(juce::Colour(0xff8b9099));
        g.drawText(juce::String(db), 37, juce::roundToInt(y) + 1, 30, 12, juce::Justification::left);
    }
    const float midStart = frequencyToX(100.0f);
    const float midEnd = frequencyToX(1000.0f);
    g.setColour(juce::Colour(0x174cacc7));
    g.fillRect(midStart, graph.getY(), midEnd - midStart, graph.getHeight());

    const auto dbToY = [&](float db)
    {
        return graph.getY() + graph.getHeight() *
            (1.0f - (juce::jlimit(-72.0f, 0.0f, db) + 72.0f) / 72.0f);
    };
    juce::Path inputLine, outputLine, inputFill, reductionFill;
    const int points = DPC_vstAudioProcessor::scopeSize;
    for (int i = 0; i < points; ++i)
    {
        const float frequency = 20.0f * std::pow(topFrequency / 20.0f,
            static_cast<float>(i) / static_cast<float>(points - 1));
        const float x = frequencyToX(frequency);
        const float inputY = dbToY(inputSpectrum[(size_t)i]);
        const float outputY = dbToY(outputSpectrum[(size_t)i]);
        if (i == 0)
        {
            inputLine.startNewSubPath(x, inputY);
            outputLine.startNewSubPath(x, outputY);
            inputFill.startNewSubPath(x, graph.getBottom());
        }
        else
        {
            inputLine.lineTo(x, inputY);
            outputLine.lineTo(x, outputY);
        }
        inputFill.lineTo(x, inputY);
        // Orange shading only where the actual processed spectrum is lower.
        if (outputY > inputY + 0.25f)
        {
            g.setColour(juce::Colour(0x44ef8e36));
            g.drawVerticalLine(juce::roundToInt(x), inputY, outputY);
        }
    }
    inputFill.lineTo(graph.getRight(), graph.getBottom());
    inputFill.closeSubPath();
    g.setColour(juce::Colour(0x397e8998));
    g.fillPath(inputFill);
    g.setColour(juce::Colour(0xff8793a3));
    g.strokePath(inputLine, juce::PathStrokeType(1.3f));
    g.setColour(accent);
    g.strokePath(outputLine, juce::PathStrokeType(2.1f));
    g.setFont(11.0f);
    g.setColour(juce::Colour(0xffa1a8b2));
    g.drawText("INPUT", 40, 98, 60, 16, juce::Justification::left);
    g.setColour(accent);
    g.drawText("PROCESSED", 104, 98, 110, 16, juce::Justification::left);
    g.setColour(juce::Colour(0xffb8b8b8));
    g.drawText("SPECTRAL REDUCTION  /  REAL AUDIO FFT", 36, 330, 360, 20, juce::Justification::left);
    g.setColour(juce::Colour(0xff858a92));
    g.setFont(10.0f);
    g.drawText("Orange shading = measured reduction; spectral boost and harmonics remain visible.",
               36, 351, 640, 18, juce::Justification::left);
}

void DPC_vstAudioProcessorEditor::timerCallback()
{
    updateSpectrum();
    repaint();
}

void DPC_vstAudioProcessorEditor::updateSpectrum()
{
    constexpr int n = DPC_vstAudioProcessor::fftSize;
    const int writePosition = processor.getSpectrumWritePosition();
    const float sampleRate = static_cast<float>(processor.getSpectrumSampleRate());
    const float highestFrequency = juce::jmin(20000.0f, sampleRate * 0.5f);
    if (sampleRate <= 0.0f || highestFrequency <= 20.0f) return;
    for (int i = 0; i < n; ++i)
    {
        const int idx = (writePosition + i) % n;
        const float hann = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi
                                                  * static_cast<float>(i) / static_cast<float>(n - 1));
        fftInput[(size_t)i] = processor.getSpectrumInput(idx) * hann;
        fftOutput[(size_t)i] = processor.getSpectrumOutput(idx) * hann;
        fftInput[(size_t)(i + n)] = 0.0f;
        fftOutput[(size_t)(i + n)] = 0.0f;
    }
    fft.performFrequencyOnlyForwardTransform(fftInput.data());
    fft.performFrequencyOnlyForwardTransform(fftOutput.data());
    const float normalization = 4.0f / static_cast<float>(n);
    for (int i = 0; i < DPC_vstAudioProcessor::scopeSize; ++i)
    {
        const float position = static_cast<float>(i) / static_cast<float>(DPC_vstAudioProcessor::scopeSize - 1);
        const float frequency = 20.0f * std::pow(highestFrequency / 20.0f, position);
        const float bin = frequency * static_cast<float>(n) / sampleRate;
        const int lower = juce::jlimit(1, n / 2 - 2, static_cast<int>(bin));
        const float fraction = juce::jlimit(0.0f, 1.0f, bin - static_cast<float>(lower));
        const auto magnitude = [lower, fraction](const auto& data)
        {
            return data[(size_t)lower] + fraction * (data[(size_t)(lower + 1)] - data[(size_t)lower]);
        };
        const float inputDb = juce::Decibels::gainToDecibels(
            juce::jmax(1.0e-8f, normalization * magnitude(fftInput)));
        const float outputDb = juce::Decibels::gainToDecibels(
            juce::jmax(1.0e-8f, normalization * magnitude(fftOutput)));
        // A single smoothing constant keeps both overlays responsive and readable.
        inputSpectrum[(size_t)i] += 0.28f * (inputDb - inputSpectrum[(size_t)i]);
        outputSpectrum[(size_t)i] += 0.28f * (outputDb - outputSpectrum[(size_t)i]);
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
