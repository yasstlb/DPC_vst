#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

class DPC_vstAudioProcessor final : public juce::AudioProcessor
{
public:
    DPC_vstAudioProcessor();
    ~DPC_vstAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;

    static constexpr int scopeSize = 128;
    static constexpr int fftSize = 2048;
    int getSpectrumWritePosition() const { return spectrumPublished.load(std::memory_order_acquire); }
    float getSpectrumInput(int index) const { return spectrumInput[(size_t)index].load(std::memory_order_relaxed); }
    float getSpectrumOutput(int index) const { return spectrumOutput[(size_t)index].load(std::memory_order_relaxed); }
    double getSpectrumSampleRate() const { return spectrumSampleRate.load(std::memory_order_relaxed); }
    float getInputScopeSample(int i) const { return inputScope[(size_t)i].load(std::memory_order_relaxed); }
    float getOutputScopeSample(int i) const { return outputScope[(size_t)i].load(std::memory_order_relaxed); }
    float getTransientScopeSample(int i) const { return transientScope[(size_t)i].load(std::memory_order_relaxed); }
    float getInputPeak() const { return inputPeak.load(std::memory_order_relaxed); }
    float getOutputPeak() const { return outputPeak.load(std::memory_order_relaxed); }
    float getClipActivity() const { return clipActivity.load(std::memory_order_relaxed); }
    float getTransientActivity() const { return transientActivity.load(std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> clipThreshold;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix;
    juce::dsp::Oversampling<float> oversampling { 2, 2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> dryDelay { 8192 };
    int oversamplingLatency = 0;
    juce::AudioBuffer<float> dryBuffer;

    // Broadband transient detector; spectral analysis is performed on the UI thread.
    float fastEnvelope = 0.0f;
    float slowEnvelope = 0.0f;
    float dcInput[2] = { 0.0f, 0.0f };
    float dcOutput[2] = { 0.0f, 0.0f };
    float dcBlockerCoeff = 0.0f;
    float fastAttackCoeff = 0.0f;
    float fastReleaseCoeff = 0.0f;
    float slowAttackCoeff = 0.0f;
    float slowReleaseCoeff = 0.0f;

    // Continuous audio capture for UI-thread FFT; no FFT work on audio thread.
    std::array<std::atomic<float>, fftSize> spectrumInput {};
    std::array<std::atomic<float>, fftSize> spectrumOutput {};
    std::atomic<int> spectrumPublished { 0 };
    std::atomic<double> spectrumSampleRate { 48000.0 };
    int spectrumWriter = 0;
    // Lock-free telemetry for the editor. Audio thread is the sole writer.
    std::array<std::atomic<float>, scopeSize> inputScope {};
    std::array<std::atomic<float>, scopeSize> outputScope {};
    std::array<std::atomic<float>, scopeSize> transientScope {};
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    std::atomic<float> clipActivity { 0.0f }, transientActivity { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DPC_vstAudioProcessor)
};
