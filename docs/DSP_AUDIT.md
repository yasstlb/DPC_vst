# DSP Audit — DPC Detail-Preserving Clipper

**Audited revision:** `f6bf10905550599d1e5b0eaa1f67be65452533b5`  
**Scope:** source-level review of `Source/PluginProcessor.cpp` and `Source/PluginProcessor.h`. This is not a listening test or a measured performance benchmark.

## Changes already present

- Stereo transient detection uses the maximum absolute level across L/R, so the transient threshold lift is shared.
- The clip threshold smoother advances once per oversampled frame in stereo, rather than once per channel.
- DETAIL raises the threshold according to fast/slow envelope divergence, capped at a factor of 2 (approximately +6 dB).
- A first-order 20 Hz DC blocker is applied after the shaping stage to control bias from the asymmetric IRON curve.
- The workflow builds the Windows VST3 target.

## Findings and risks

### 1. Transient detector and threshold

The detector uses fast and slow envelope followers, with attack/release constants of 1.5/40 ms and 25/180 ms. The transient measure is `clamp((fast - slow) / (fast + 1e-5), 0, 1)`; DETAIL scales the resulting threshold lift.

- The intended attack emphasis is plausible, but no objective transient-preservation test currently measures attack peak, crest factor, or decay against a reference.
- The detector runs at the oversampled rate in stereo but at native rate in mono. Since the coefficients are computed using a processing rate that is 4× the host rate only when stereo, mono and stereo can have different detector time behaviour. This needs correction or explicit justification.
- The code's comments say the threshold lift is capped at +6 dB; the actual formula is `1 + detail * transient`, which does cap at 2× when DETAIL and transient are both 1.

### 2. Stereo behaviour

The threshold is peak-linked across the two channels, which avoids separate detector modulation between left and right. The DC blocker state remains per-channel, as expected.

- No test currently verifies identical gain modulation or image stability for correlated stereo signals.
- The mono branch shares the detector logic, but its sample-rate mismatch versus the stereo oversampled branch should be tested.

### 3. DC offset

The blocker is a conventional one-pole high-pass recurrence with its pole set to 20 Hz. It should suppress steady DC, but it also changes very-low-frequency content and can produce a settling transient after abrupt state changes.

- No test currently measures residual DC after a sustained asymmetric signal or checks bass-band magnitude/phase impact.
- Test with silence after a high-level IRON signal to check the tail and state recovery.

### 4. Clipping ceiling and modes

- CORE is a hard clamp in the driven domain, then scales back by the threshold. Its nominal wet output is bounded by the threshold before output gain.
- IRON adds a quadratic asymmetric term before `tanh`, then normalizes by `tanh(1.08)`. Its final output is additionally clamped to the threshold range.
- HEAT uses a stronger `tanh` curve and normalization, with the same final clamp.
- OUTPUT gain is applied after the clipper/mix, so final plugin output can exceed the clipping threshold by design when OUTPUT is positive.
- DETAIL's effect is threshold lift rather than a parallel transient reconstruction. It can reduce clipping on attacks, but does not guarantee preservation of the original transient waveform.

### 5. Dry/wet alignment

The stereo dry signal is delayed by the oversampling latency before mixing. Mono bypasses oversampling and uses an undelayed dry signal.

- There is no automated impulse test proving sample alignment or checking for comb filtering at intermediate MIX settings.
- Validate latency reporting and the actual oversampler delay using an impulse through the plugin in a host or an offline harness.

### 6. CPU and real-time safety

- The stereo path uses 4× oversampling and processes samples in a nested frame/channel loop.
- Per-sample work includes envelope updates, shaping, DC blocking, and dry delay handling. No benchmark currently measures CPU use at common sample rates/block sizes.
- The processing path appears to avoid dynamic allocation inside the main per-sample loop; however, `dryBuffer.setSize` can allocate if a host sends a block larger than the prepared size. Consider a documented maximum-block strategy or safe preallocation if strict real-time behaviour is required.

## Missing automated DSP tests

These are not implemented by this audit document; they remain required work:

1. **Clipping ceiling:** sweep input amplitude and assert wet output stays within the threshold for all modes (with OUTPUT at 0 dB).
2. **Silence:** process zero input after reset and assert finite, zero output with no DC or NaNs.
3. **Impulse response:** test latency, peak position, and dry/wet alignment.
4. **Transient preservation:** compare attack peak/crest factor with DETAIL at 0 and 1 against a defined reference and input sequence.
5. **Dry/wet:** assert MIX=0 reproduces the dry signal with the expected latency and MIX=1 equals the wet path.
6. **DC rejection:** feed asymmetric IRON input, measure mean output after settling, then verify state recovery on silence.
7. **Stereo consistency:** feed identical L/R signals and verify output channels remain identical; test one-channel-only transients for linked detector behaviour.
8. **Mono/stereo timing:** compare detector response in equivalent mono and stereo test cases.

## Audit conclusion

The recent changes address stereo-linked transient detection and asymmetric DC bias at source level. The build has been green for the audited commit, but that only establishes compilation. Until the tests above exist and run, clipping behaviour, alignment, transient preservation, DC rejection, and real-time performance remain unverified experimentally.
