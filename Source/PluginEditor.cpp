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
    g.drawText("CLIPPER  /  LIVE SPECTRAL REDUCTION", 26, 44, 400, 20, juce::Justification::left);

    // One continuous spectral reduction curve. 0 dB = no attenuation.
    // The FFT ratio is measured, never synthesized from knob positions.
    const juce::Rectangle<float> graph(32.0f, 94.0f, 656.0f, 210.0f);
    g.setColour(juce::Colour(0xff101114));
    g.fillRoundedRectangle(graph, 7.0f);
    const float sampleRate = static_cast<float>(processor.getSpectrumSampleRate());
    const float maxHz = juce::jmax(21.0f, juce::jmin(20000.0f, sampleRate * 0.5f));
    const auto freqX = [&](float hz)
    {
        return graph.getX() + graph.getWidth() * std::log(hz / 20.0f) / std::log(maxHz / 20.0f);
    };
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        if (hz > maxHz) continue;
        const float x = freqX(hz);
        g.setColour(juce::Colour(0xff30343a));
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
        g.setColour(juce::Colour(0xff9aa0a8));
        g.setFont(10.0f);
        g.drawText(hz >= 1000.0f ? juce::String(hz / 1000.0f, 0) + "k" : juce::String(hz, 0),
                   juce::roundToInt(x - 18.0f), 308, 36, 14, juce::Justification::centred);
    }
    for (int reduction = 0; reduction <= 24; reduction += 6)
    {
        const float y = graph.getY() + graph.getHeight() * static_cast<float>(reduction) / 24.0f;
        g.setColour(reduction == 0 ? juce::Colour(0xff8e724d) : juce::Colour(0xff30343a));
        g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        g.setColour(juce::Colour(0xff9aa0a8));
        g.drawText(reduction == 0 ? "0 dB" : "-" + juce::String(reduction),
                   36, juce::roundToInt(y) + 2, 42, 13, juce::Justification::left);
    }
    g.setColour(juce::Colour(0x154b9bb8));
    g.fillRect(freqX(100.0f), graph.getY(), freqX(1000.0f) - freqX(100.0f), graph.getHeight());
    juce::Path reductionCurve, reductionArea;
    for (int i = 0; i < DPC_vstAudioProcessor::scopeSize; ++i)
    {
        const float fraction = static_cast<float>(i) / static_cast<float>(DPC_vstAudioProcessor::scopeSize - 1);
        const float x = graph.getX() + fraction * graph.getWidth();
        // Negative differences indicate attenuation. Increases from harmonics
        // or output gain are displayed at 0 dB rather than as fake reduction.
        const float attenuation = juce::jlimit(0.0f, 24.0f,
            inputSpectrum[(size_t)i] - outputSpectrum[(size_t)i]);
        const float y = graph.getY() + graph.getHeight() * attenuation / 24.0f;
        if (i == 0)
        {
            reductionCurve.startNewSubPath(x, y);
            reductionArea.startNewSubPath(x, graph.getY());
        }
        else reductionCurve.lineTo(x, y);
        reductionArea.lineTo(x, y);
    }
    reductionArea.lineTo(graph.getRight(), graph.getY());
    reductionArea.closeSubPath();
    g.setColour(juce::Colour(0x55e0a14b));
    g.fillPath(reductionArea);
    g.setColour(accent);
    g.strokePath(reductionCurve, juce::PathStrokeType(2.2f));
    g.setColour(juce::Colour(0xffd2d6dc));
    g.setFont(12.0f);
    g.drawText("LIVE SPECTRAL ATTENUATION", 42, 101, 290, 20, juce::Justification::left);
    g.setFont(10.0f);
    g.setColour(juce::Colour(0xff949ba5));
    g.drawText("0 dB = no reduction  |  Downward = more reduction  |  2048-point FFT",
               36, 330, 640, 18, juce::Justification::left);
    g.drawText("CLIP + DETAIL affect the measured response; output gain and mix also affect the reading.",
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
