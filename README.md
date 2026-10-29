# DPC — Detail-Preserving Clipper

DPC is a JUCE-based VST3 clipper project. The current branch is an early development scaffold, not a release-ready audio processor.

## Modes and controls

- **CORE** — hard-clipping starting point.
- **IRON** — asymmetric soft-saturation starting point.
- **HEAT** — stronger soft-saturation starting point.
- **CLIP** — threshold/drive control in dB.
- **DETAIL** — experimental blend control; its behavior is not yet a validated transient-preservation algorithm.
- **MIX** — dry/wet blend.
- **OUTPUT** — output gain.

## Build

Requires CMake 3.22+, a C++17 compiler, and a working internet connection for JUCE FetchContent.

### Windows (Visual Studio)

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target DPC_vst_VST3
```

The VST3 target is expected under `build/DPC_vst_artefacts/Release/VST3` (exact path can vary by generator).

### macOS

```sh
cmake -S . -B build -G Xcode
cmake --build build --config Release --target DPC_vst_VST3 DPC_vst_AU
```

## Current limitations

This scaffold does **not yet** include the full intended feature set. In particular, production-quality oversampling/anti-aliasing, explicit latency-compensated dry/wet mixing, a tested detail-preservation design, input/output spectrum analysis, automated DSP tests, and listening validation remain to be implemented. No build or audio-quality claim should be inferred until CI and local/plugin-host tests pass.

## CI

GitHub Actions attempts to configure and compile the Windows VST3 target on pushes to `main` and `feat/**`, pull requests, and manual dispatch.
