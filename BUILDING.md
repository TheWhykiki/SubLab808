# Building SubLab808

## Requirements

- macOS 11 or newer
- Apple Silicon Mac
- Xcode command-line tools
- CMake 3.25 or newer
- JUCE 8.0.15 (downloaded automatically when no local checkout is found)

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

`SubLab808Smoke` verifies audio generation, every factory preset, state restoration, Gate mode, Pitch Bend, editor construction, and UI rendering. The rendered preview is written to `build/SubLab808-preview.png`.
