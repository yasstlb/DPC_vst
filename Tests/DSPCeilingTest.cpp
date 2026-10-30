#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

// Wet-only tests at 48 kHz, MIX=1, OUTPUT=0 dB.
// STRICT_CEILING is OFF by default because host-rate hard clipping aliases.
// These are sample peaks, NOT reconstructed true peaks.
static void setParameter(DPC_vstAudioProcessor& p, const char* id, float normalized)
{
    p.parameters.getParameter(id)->setValueNotifyingHost(normalized);
}
static std::unique_ptr<DPC_vstAudioProcessor> makeProcessor(int mode, float detail, bool strict)
{
    auto p = std::make_unique<DPC_vstAudioProcessor>();
    setParameter(*p, "MODE", static_cast<float>(mode) / 2.0f);
    setParameter(*p, "CLIP", 12.0f / 18.0f);
    setParameter(*p, "DETAIL", detail);
    setParameter(*p, "MIX", 1.0f);
    setParameter(*p, "OUTPUT", 24.0f / 36.0f);
    setParameter(*p, "STRICT_CEILING", strict ? 1.0f : 0.0f);
    p->setPlayConfigDetails(2, 2, 48000.0, 512);
    p->prepareToPlay(48000.0, 512);
    return p;
}
static bool amplitudeSweep()
{
    constexpr int n = 512;
    const float threshold = juce::Decibels::decibelsToGain(-12.0f);
    for (int mode = 0; mode < 3; ++mode)
        for (float detail : { 0.0f, 1.0f })
            for (bool strict : { false, true })
            {
                auto p = makeProcessor(mode, detail, strict);
                juce::AudioBuffer<float> audio(2, n);
                juce::MidiBuffer midi;
                float peak = 0.0f;
                for (int block = 0; block < 128; ++block)
                {
                    const float amplitude = threshold * 8.0f * static_cast<float>(block) / 127.0f;
                    for (int i = 0; i < n; ++i)
                    {
                        const float phase = static_cast<float>(block * n + i);
                        audio.setSample(0, i, amplitude * std::sin(phase * 0.31f));
                        audio.setSample(1, i, amplitude * std::sin(phase * 0.047f));
                    }
                    p->processBlock(audio, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i)
                        {
                            const float sample = audio.getSample(ch, i);
                            if (!std::isfinite(sample)) return false;
                            peak = juce::jmax(peak, std::abs(sample));
                        }
                }
                const float ceiling = threshold * (1.0f + detail);
                // Strict mode tests the contractual sample ceiling.
                // Default mode allows IIR reconstruction overshoot; 2.0x
                // is a safety regression guard, NOT a promised ceiling.
                const float guard = strict ? ceiling + 0.0002f
                                           : ceiling * 2.0f + 0.0002f;
                std::printf("mode=%d DETAIL=%.1f strict=%d peak=%.6f guard=%.6f\n",
                            mode, detail, strict ? 1 : 0, peak, guard);
                if (peak > guard) return false;
            }
    return true;
}
static bool smallSignalGain()
{
    constexpr int n = 512;
    constexpr int blocks = 96;
    for (int mode = 0; mode < 3; ++mode)
    {
        auto p = makeProcessor(mode, 0.0f, false);
        juce::AudioBuffer<float> audio(2, n);
        juce::MidiBuffer midi;
        double inputPower = 0.0, outputPower = 0.0;
        for (int block = 0; block < blocks; ++block)
        {
            for (int i = 0; i < n; ++i)
            {
                const double input = 0.01 * std::sin(juce::MathConstants<double>::twoPi
                    * 1000.0 * (block * n + i) / 48000.0);
                audio.setSample(0, i, static_cast<float>(input));
                audio.setSample(1, i, static_cast<float>(input));
                if (block > 20) inputPower += input * input;
            }
            p->processBlock(audio, midi);
            if (block > 20)
                for (int i = 0; i < n; ++i)
                    outputPower += static_cast<double>(audio.getSample(0, i))
                                 * static_cast<double>(audio.getSample(0, i));
        }
        const double gainDb = 10.0 * std::log10(outputPower / inputPower);
        std::printf("small signal mode=%d gain=%.3f dB\n", mode, gainDb);
        if (!std::isfinite(gainDb) || std::abs(gainDb) > 0.30) return false;
    }
    return true;
}
static double spectralSpurEnergy(const std::vector<float>& signal, int fundamental)
{
    constexpr int n = 16384;
    juce::dsp::FFT transform(14);
    std::vector<float> bins(n * 2, 0.0f);
    for (int i = 0; i < n; ++i) bins[static_cast<size_t>(i)] = signal[static_cast<size_t>(i)];
    transform.performFrequencyOnlyForwardTransform(bins.data());
    double total = 0.0;
    for (int k = 3; k < n / 2 - 3; ++k)
    {
        // Exclude the fundamental and expected true harmonics; remaining
        // energy is a conservative alias/noise indicator.
        bool harmonic = false;
        for (int h = 1; h <= 3; ++h)
        {
            const int target = h * fundamental;
            if (target < n / 2 && std::abs(k - target) <= 3) harmonic = true;
        }
        if (!harmonic)
        {
            const double mag = bins[static_cast<size_t>(k)];
            total += mag * mag;
        }
    }
    return total;
}
static bool aliasingRegression()
{
    constexpr int n = 16384, blockSize = 512, frequencyBin = 1811;
    std::array<double, 2> aliasEnergy {};
    for (int variant = 0; variant < 2; ++variant)
    {
        auto p = makeProcessor(0, 0.0f, variant == 1);
        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> measured(n);
        // Warm up filters with a continuous coherent tone.
        for (int frame = 0; frame < n * 2; frame += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float t = static_cast<float>(frame + i);
                const float x = 1.0f * std::sin(juce::MathConstants<float>::twoPi
                    * static_cast<float>(frequencyBin) * t / static_cast<float>(n));
                block.setSample(0, i, x);
                block.setSample(1, i, x);
            }
            p->processBlock(block, midi);
            if (frame >= n)
                for (int i = 0; i < blockSize; ++i)
                    measured[static_cast<size_t>(frame - n + i)] = block.getSample(0, i);
        }
        aliasEnergy[static_cast<size_t>(variant)] = spectralSpurEnergy(measured, frequencyBin);
    }
    const double improvement = 10.0 * std::log10((aliasEnergy[1] + 1.0e-18)
                                                 / (aliasEnergy[0] + 1.0e-18));
    std::printf("alias delta strict-vs-default: %.2f dB\n", improvement);
    // Regression must show default mode is substantially cleaner than
    // the optional hard clamp on this challenging above-threshold tone.
    return std::isfinite(improvement) && improvement > 3.0;
}
int main()
{
    if (!amplitudeSweep()) { std::fprintf(stderr, "Amplitude sweep failed\n"); return 1; }
    if (!smallSignalGain()) { std::fprintf(stderr, "Small-signal gain failed\n"); return 1; }
    if (!aliasingRegression()) { std::fprintf(stderr, "Aliasing regression failed\n"); return 1; }
    return 0;
}
