# Building SubLab808

## Requirements

- macOS 11 or newer
- Apple Silicon Mac
- Xcode command-line tools
- CMake 3.25 or newer
- JUCE 8.0.15 at commit `91ad83ae34a81e0833b1a2b0866f54846370ae53` (downloaded automatically when no local checkout is supplied)

## Configure and build

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SubLab808Tests SubLab808_VST3 -j 4
ctest --test-dir build --output-on-failure
```

To use an existing JUCE checkout:

```sh
cmake -S . -B build -DSUBLAB808_JUCE_PATH=/absolute/path/to/JUCE
```

The VST3 bundle is created at:

`build/SubLab808_artefacts/Release/VST3/SubLab808.vst3`

## Local installation

Copy the bundle to `~/Library/Audio/Plug-Ins/VST3/`, then restart Cubase or rescan plugins in Cubase's Plug-in Manager.

## Tests

`SubLab808Smoke` verifies audio generation, every factory preset, parameter/state restoration, Gate mode, Pitch Bend, MIDI panic, output bounds, editor construction, and UI rendering. The rendered preview is written to `build/SubLab808-preview.png`.

Release builds must be created from a clean build directory. Verify the compatibility target with:

```sh
otool -l build/SubLab808_artefacts/Release/VST3/SubLab808.vst3/Contents/MacOS/SubLab808 | grep -A3 LC_BUILD_VERSION
```

Public distribution additionally requires an Apple Developer ID Application certificate and notarization. Local ad-hoc signing is suitable only for the current machine.
