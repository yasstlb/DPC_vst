#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <memory>

// Tests the processed WET path: MIX=100%, OUTPUT=0 dB, CLIP=12 dB.
// This is a sample-peak ceiling, not an inter-sample true-peak guarantee.
// CLIP=0 deliberately bypasses clipping and is outside this ceiling contract.
int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr float clipDb = 12.0f;
    const float threshold = juce::Decibels::decibelsToGain(-clipDb);
    for (int mode = 0; mode < 3; ++mode)
    {
        for (const float detail : { 0.0f, 1.0f })
        {
            DPC_vstAudioProcessor p;
            p.parameters.getParameter("MODE")->setValueNotifyingHost(static_cast<float>(mode) / 2.0f);
            p.parameters.getParameter("CLIP")->setValueNotifyingHost(clipDb / 18.0f);
            p.parameters.getParameter("DETAIL")->setValueNotifyingHost(detail);
            p.parameters.getParameter("MIX")->setValueNotifyingHost(1.0f);
            p.parameters.getParameter("OUTPUT")->setValueNotifyingHost(24.0f / 36.0f);
            p.setPlayConfigDetails(2, 2, sampleRate, blockSize);
            p.prepareToPlay(sampleRate, blockSize);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> audio(2, blockSize);
            const float bound = threshold * (1.0f + detail);
            float measured = 0.0f;
            // Sweep from silence up to 8x clipping threshold, with
            // phase/frequency changes to exercise both transient and sustain.
            for (int block = 0; block < 128; ++block)
            {
                const float amplitude = threshold * 8.0f * static_cast<float>(block) / 127.0f;
                for (int i = 0; i < blockSize; ++i)
                {
                    const float phase = static_cast<float>(block * blockSize + i);
                    audio.setSample(0, i, amplitude * std::sin(phase * 0.31f));
                    audio.setSample(1, i, amplitude * std::sin(phase * 0.047f));
                }
                p.processBlock(audio, midi);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float sample = audio.getSample(ch, i);
                        if (!std::isfinite(sample) || std::abs(sample) > bound + 0.0002f)
                        {
                            std::fprintf(stderr, "FAIL mode=%d DETAIL=%.1f block=%d sample=%g bound=%g\\n",
                                mode, detail, block, static_cast<double>(sample), static_cast<double>(bound));
                            return 1;
                        }
                        measured = juce::jmax(measured, std::abs(sample));
                    }
            }
            std::printf("PASS mode=%d DETAIL=%.1f peak=%.6f ceiling=%.6f\\n",
                        mode, detail, measured, bound);
            p.releaseResources();
        }
    }
    return 0;
}
