// Offline harness: runs the real DPC_vstAudioProcessor on synthetic signals.
// Not a unit-test framework; prints measurements and returns non-zero on failure.
#include "../Source/PluginProcessor.h"
#include <cstdio>
#include <random>

static void setParam(DPC_vstAudioProcessor& p, const char* id, float plainValue)
{
    auto* prm = p.parameters.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1(plainValue));
}

// Runs `signal` (stereo, identical L/R) through the plugin, returns output.
static std::vector<float> run(DPC_vstAudioProcessor& p, const std::vector<float>& in)
{
    const int block = 512;
    std::vector<float> out(in.size());
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < in.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t>((size_t) block, in.size() - pos);
        juce::AudioBuffer<float> buf(2, n);
        for (int i = 0; i < n; ++i) { buf.setSample(0, i, in[pos + (size_t) i]); buf.setSample(1, i, in[pos + (size_t) i]); }
        p.processBlock(buf, midi);
        for (int i = 0; i < n; ++i) out[pos + (size_t) i] = buf.getSample(0, i);
    }
    return out;
}

static float peak(const std::vector<float>& v, size_t from)
{
    float m = 0; for (size_t i = from; i < v.size(); ++i) m = std::max(m, std::abs(v[i])); return m;
}


// Peak/T of an IDEAL hard clipper (clip at T on the 4x signal) passed through the same
// polyphase-IIR oversampler used by the plugin. Anything above 1.0 here is inter-sample
// overshoot of the decimation filter, not a clipper defect, so the plugin is judged against it.
static float idealClipperOvershoot(const std::vector<float>& x, float T)
{
    juce::dsp::Oversampling<float> os { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };
    os.initProcessing(512); os.reset();
    float pk = 0;
    for (size_t pos = 0; pos < x.size(); pos += 512)
    {
        const int n = (int) std::min<size_t>(512, x.size() - pos);
        juce::AudioBuffer<float> b(2, n);
        for (int i = 0; i < n; ++i) { b.setSample(0, i, x[pos + (size_t) i]); b.setSample(1, i, x[pos + (size_t) i]); }
        juce::dsp::AudioBlock<float> blk(b);
        auto w = os.processSamplesUp(blk);
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < w.getNumSamples(); ++i)
                w.setSample(c, i, juce::jlimit(-T, T, w.getSample(c, i)));
        os.processSamplesDown(blk);
        if (pos > 4800) for (int i = 0; i < n; ++i) pk = std::max(pk, std::abs(b.getSample(0, i)));
    }
    return pk / T;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double fs = 48000.0;
    const size_t N = (size_t) fs * 2;
    int failures = 0;

    // broadband test signal: pink-ish noise, peak-normalised to 0.9
    std::mt19937 rng(1); std::normal_distribution<float> nd;
    std::vector<float> noise(N); float b0 = 0, b1 = 0, b2 = 0;
    for (auto& s : noise) { const float w = nd(rng); b0 = 0.99765f*b0 + w*0.0990460f; b1 = 0.96300f*b1 + w*0.2965164f; b2 = 0.57000f*b2 + w*1.0526913f; s = b0 + b1 + b2 + w*0.1848f; }
    const float pk = peak(noise, 0); for (auto& s : noise) s *= 0.9f / pk;

    const float refOvershoot = idealClipperOvershoot(noise, juce::Decibels::decibelsToGain(-12.0f));
    std::printf("[reference] ideal hard clipper + same oversampler: peak/T = %.3f\n", refOvershoot);
    const char* modeNames[] = { "CORE", "IRON", "HEAT" };
    for (int mode = 0; mode < 3; ++mode)
    {
        for (float detail : { 0.0f, 1.0f })
        {
            DPC_vstAudioProcessor p;
            p.setPlayConfigDetails(2, 2, fs, 512);
            p.prepareToPlay(fs, 512);
            setParam(p, "MODE", (float) mode); setParam(p, "CLIP", 12.0f);
            setParam(p, "DETAIL", detail); setParam(p, "MIX", 1.0f); setParam(p, "OUTPUT", 0.0f);
            const float T = juce::Decibels::decibelsToGain(-12.0f);
            const float ceiling = T * (1.0f + detail) * refOvershoot;   // explicit ceiling x decimation-filter overshoot
            const auto out = run(p, noise);
            const float outPeak = peak(out, 4800);
            const bool ok = outPeak <= ceiling * 1.02f;   // 2 % tolerance for DC-blocker ringing
            std::printf("[ceiling] %s detail=%.0f  peak/T=%.3f  allowed<=%.2f (=(1+detail) x ideal-clipper overshoot)  %s\n",
                        modeNames[mode], detail, outPeak / T, ceiling / T, ok ? "OK" : "FAIL");
            if (! ok) ++failures;
        }
    }

    // small-signal gain: -40 dBFS sine, CLIP=6 dB, DETAIL=0 -> must be ~0 dB in every mode
    for (int mode = 0; mode < 3; ++mode)
    {
        for (float hz : { 100.0f, 1000.0f, 8000.0f })
        {
            DPC_vstAudioProcessor p;
            p.setPlayConfigDetails(2, 2, fs, 512);
            p.prepareToPlay(fs, 512);
            setParam(p, "MODE", (float) mode); setParam(p, "CLIP", 6.0f);
            setParam(p, "DETAIL", 0.0f); setParam(p, "MIX", 1.0f); setParam(p, "OUTPUT", 0.0f);
            std::vector<float> sine(N);
            const float a = juce::Decibels::decibelsToGain(-40.0f);
            for (size_t i = 0; i < N; ++i) sine[i] = a * std::sin(juce::MathConstants<float>::twoPi * hz * (float) i / (float) fs);
            const auto out = run(p, sine);
            double e = 0; size_t c = 0; for (size_t i = N/2; i < N; ++i) { e += (double) out[i]*out[i]; ++c; }
            const double rmsOut = std::sqrt(e / (double) c), rmsIn = a / std::sqrt(2.0);
            const double db = 20.0 * std::log10(rmsOut / rmsIn);
            const bool ok = std::abs(db) < 0.5;
            std::printf("[gain@-40dBFS] %s %5.0f Hz  %+.2f dB  %s\n", modeNames[mode], hz, db, ok ? "OK" : "FAIL");
            if (! ok) ++failures;
        }
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
