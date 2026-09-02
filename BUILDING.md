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
cmake --build build --target SubLab808Tests SubLab808_VST3 SubLab808HostTests -j 4
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

`SubLab808Smoke` verifies audio generation, every factory preset, parameter/state restoration, Gate and One Shot modes, channel-isolated MIDI and Pitch Bend, tail behavior, output bounds, editor construction, size restoration, and in-memory UI rendering. `SubLab808BundleLoad` scans and instantiates the built VST3 through JUCE's VST3 host implementation, then renders MIDI-driven audio through the bundle through its reported maximum tail.

## Control-thread contract

Program changes and state restores are synchronous and serialized. Independent control threads may call them concurrently, and state capture remains available while a change is being published. A synchronous JUCE parameter or host callback must not start and join a worker that calls `setCurrentProgram()` or `setStateInformation()` back on the same processor: JUCE holds its own parameter-listener lock for the duration of such callbacks, so that cross-thread wait cycle is invalid regardless of the plugin's control-state serialization.

Release builds must be created from a clean build directory. Verify the compatibility target with:

```sh
otool -l build/SubLab808_artefacts/Release/VST3/SubLab808.vst3/Contents/MacOS/SubLab808 | grep -A3 LC_BUILD_VERSION
```

The completed local bundle is ad-hoc signed by default. To apply an available Developer ID Application certificate as the final build step, configure with:

```sh
cmake -S . -B build -DSUBLAB808_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
```

Public distribution additionally requires notarization. Local ad-hoc signing is suitable only for testing on the current machine.
