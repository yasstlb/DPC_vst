# Offline DSP harness

`dsp_harness.cpp` runs the real `DPC_vstAudioProcessor` on synthetic signals and prints measurements
(non-zero exit code on failure). It is not wired into CMake/CI yet.

What it checks
- **Ceiling**: peak after the plugin must not exceed `(1 + DETAIL) x` the peak of an *ideal* hard clipper
  passed through the same 4x polyphase-IIR oversampler. Inter-sample overshoot of the decimation filter
  (~+3.5 dB on heavily clipped noise) is a property of the oversampler, not of the clipper, so it is
  measured and used as the reference instead of being ignored.
- **Small-signal gain**: a -40 dBFS sine must come out within +/-0.5 dB in CORE, IRON and HEAT.

Quick Linux build (needs JUCE 8.0.6 modules compiled alongside, see `tools/` notes in the PR description).

`tools/ceiling_model.py` is a pure-Python model of the residual architecture (`c = shape(x)`, `r = x - c`,
`out = c + sum_k protect_k * r_k`); it validates the *design*, not the C++.
