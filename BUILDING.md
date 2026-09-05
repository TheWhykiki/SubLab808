# Building SubLab808

## Requirements

- macOS 11 or newer on an Apple Silicon or Intel Mac; Windows 10 or newer for x64, or Windows 11 on Arm for ARM64EC
- Xcode command-line tools on macOS; Visual Studio 2022 17.3 or a newer supported MSVC with Desktop C++, a Windows 11 SDK and ARM64/ARM64EC tools on Windows
- CMake 3.25 or newer
- JUCE 8.0.15 at commit `91ad83ae34a81e0833b1a2b0866f54846370ae53` (downloaded automatically when no local checkout is supplied)

## Configure and build on macOS

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SubLab808Tests SubLab808_VST3 SubLab808HostTests SubLab808PresetTests SubLab808WindowsUpdaterPolicyTests -j 4
ctest --test-dir build --output-on-failure
```

To use an existing JUCE checkout:

```sh
cmake -S . -B build -DSUBLAB808_JUCE_PATH=/absolute/path/to/JUCE
```

The VST3 bundle is created at:

`build/SubLab808_artefacts/Release/VST3/SubLab808.vst3`

The macOS VST3 is a universal binary containing native `arm64` and `x86_64`
slices. Both slices have a macOS 11 deployment target. CI builds that universal
bundle on both `macos-15` (Arm) and `macos-15-intel`, then runs the VST3 host,
render and state-recall checks natively on each processor architecture.

## Configure and build on Windows

Windows uses separate build trees for the two supported VST3 ABIs:

```powershell
cmake -S . -B build-windows-x64 -A x64
cmake --build build-windows-x64 --config Release --target SubLab808Tests SubLab808_VST3 SubLab808HostTests SubLab808PresetTests SubLab808WindowsUpdaterPolicyTests SubLab808WindowsUpdaterSelfTests --parallel
ctest --test-dir build-windows-x64 -C Release --output-on-failure

cmake -S . -B build-windows-arm64ec -A ARM64EC
cmake --build build-windows-arm64ec --config Release --target SubLab808Tests SubLab808_VST3 SubLab808HostTests SubLab808PresetTests SubLab808WindowsUpdaterPolicyTests SubLab808WindowsUpdaterSelfTests --parallel
ctest --test-dir build-windows-arm64ec -C Release --output-on-failure
```

The resulting VST3 binaries are below `Contents/x86_64-win` and
`Contents/arm64ec-win`, respectively. ARM64EC requires Windows 11 and is the
Windows-on-Arm ABI for x64/ARM64EC plug-in hosts; classic ARM64 binaries cannot
be loaded into those processes. Configure ARM64EC on a native Windows-on-Arm
machine. An x64-hosted ARM64EC cross-build is rejected because the pinned JUCE
VST3 manifest helper must execute for the target ABI during the build.

The CI runs the JUCE VST3 scan, instantiation, render and state-recall host test
natively on both Windows architectures. This includes the non-UI smoke tests and
deterministic preset persistence subset; the desktop-dialog suite remains a
separate interactive test. CI uses the explicit GitHub-hosted Windows 11 Arm +
Visual Studio 2026 image for ARM64EC. These checks do not replace release
acceptance in Cubase on physical Windows x64 and Arm systems.

The default Windows build compiles the complete updater in a non-installing
self-test mode, but does not embed it or enable the editor button. A production
build must be configured with the real SHA-256 fingerprint of its Authenticode
distribution certificate:

```powershell
cmake -S . -B build-windows-x64 -A x64 `
  -DSUBLAB808_WINDOWS_UPDATER_SIGNER_SHA256=<64-hex-certificate-fingerprint>
cmake --build build-windows-x64 --config Release --target SubLab808_VST3 --parallel
```

This embeds `Contents\Helpers\SubLab808Updater.exe`. The MSI pipeline signs the
staged helper and every PE payload before signing the MSI; see
[WINDOWS_UPDATER.md](WINDOWS_UPDATER.md) and
[WINDOWS_INSTALLER.md](WINDOWS_INSTALLER.md). Never substitute a dummy pin in a
distributed build.

## Local installation

On macOS, copy the bundle to `~/Library/Audio/Plug-Ins/VST3/`. On Windows, use
the matching reviewed MSI; a development bundle may instead be copied to
`C:\Program Files\Common Files\VST3\` with administrator permission. Then restart
Cubase or rescan plug-ins in its Plug-in Manager.

## Tests

`SubLab808Smoke` verifies audio generation, every factory preset, parameter/state restoration, Gate and One Shot modes, channel-isolated MIDI and Pitch Bend, tail behavior, output bounds, editor construction, size restoration, and in-memory UI rendering. `SubLab808BundleLoad` scans and instantiates the built VST3 through JUCE's VST3 host implementation, then renders MIDI-driven audio through the bundle through its reported maximum tail. It also saves 13 non-default parameters (including Click), destroys the reference instance, restores a fresh VST3 instance, and compares 96,000 stereo frames sample-exactly. Validator negative controls reject a missing restore, a one-ULP audio change and non-finite values.

`SubLab808Presets` tests the factory bank, file safety and real preset UI. Its 39 editor-lifecycle/host-callback cases use temporary libraries and a desktop display; use an isolated desktop session when running GUI tests. These include ordinary Save-As-then-Load and a newer restore which must cancel the stale follow-up without undoing the saved file. On macOS, eight additional Import/Export cases verify real native file panels: hide/detach/destroy and hide-then-immediate-destroy close the panel, clear its delegate and destroy its JUCE modal without changing fixture files or processor state; the replacement editor remains interactive. A non-native fallback is not accepted as native coverage.

For a focused diagnostic run, set `WHYKIKI_PRESET_TEST_LIFECYCLE_ONLY=1` (39 UI cases), `WHYKIKI_PRESET_TEST_REENTRANCY_ONLY=1` (8 synchronous host-callback destruction cases plus 2 Save-then-Load cases), or `WHYKIKI_PRESET_TEST_NATIVE_ONLY=1` (macOS-only file-panel cases) on the `SubLab808PresetTests` executable. `WHYKIKI_PRESET_TEST_NATIVE_CASE=import-detach`, for example, selects one native case and requires native-only mode. CTest runs all eight native cases in fresh, serial processes with 60-second deadlines and also runs all eight sequentially in one process with a 480-second deadline to expose leaked AppKit modal state. The native test bridge is not linked into the VST3 and does not substitute for Cubase/REAPER acceptance of host-specific window-closing behavior.

`WHYKIKI_PRESET_TEST_DIRTY_ONLY=1` checks Factory/User dirty status against the actual ranged parameters while APVTS notifications deliberately lag, then verifies that real notifications bring the raw cache back into agreement. These cases also run in the full suite. Do not combine this focused mode with another: contradictory requests fail as a test-setup error.

`WHYKIKI_PRESET_TEST_SAVE_RESTORE_ONLY=1` runs the dirty guards and 14 deterministic persistence/control-state groups without opening an editor. They cover a restore after a real file commit, same-ID and ABA changes, coherent capture, restore from the host notification, and bounded capture contention before any write. Scheduling hooks are compiled into the preset-test target only, not the plugin. Do not combine this focused mode with another.

`SubLab808ListenerLock` runs the genuine held-JUCE-listener-lock regression independently with a 20-second CTest timeout; it is also exercised by the ordinary smoke binary. Smoke/bundle tests have 300-second limits, the non-native full preset test 600 seconds and each native panel case 60 seconds. A timeout is a failed test, not a successful bounded operation.

## Control-thread contract

Independent program and state setters commit synchronously under a short control gate. That gate updates the actual ranged parameters, program identity, user selection, dimensions and reset marker without invoking parameter listeners, host callbacks or editor methods. State capture uses this complete committed state, including when called from the first notification of a change. APVTS raw/UI caches may lag until notifications are delivered; they are not the authority for DSP, persistence or the preset library's dirty status.

Notifications run separately and never write an older callback argument back into a parameter. A genuine nested state restore commits immediately and supersedes older queued program requests. Reentrant program requests from program callbacks retain FIFO ordering; program callbacks generated by a state restore retain the existing echo-suppression behavior. A genuinely later request made after a nested restore is not discarded as an earlier request. Each drain processes at most 128 notification generations/no-op queue entries; remaining finite work continues through AsyncUpdater instead of being silently dropped.

The audio callback uses a single-attempt, sequence-validated parameter/reset packet and never waits for the control gate. The sequence and reset marker use the same sequentially consistent ordering as the pinned standard JUCE parameter atomics, with lock-free atomic types required by the build. This covers transaction commits, not a promise that unrelated individual host-automation writes form one atomic operation. State restore resets the Click noise sequence through the coherent packet without changing the existing voice-continuation behavior.

Editor size is applied on the message thread; a worker restore publishes its dimensions immediately for state capture and schedules the visible update. The broader JUCE overlap tests do not change VST3's UI-thread state-access contract or replace actual Cubase/REAPER tests. Arbitrary cross-thread waits inside external listeners can still introduce their own lock cycles.

Release builds must be created from a clean build directory. Verify the compatibility target with:

```sh
binary="build/SubLab808_artefacts/Release/VST3/SubLab808.vst3/Contents/MacOS/SubLab808"
test "$(lipo -archs "$binary" | tr ' ' '\n' | LC_ALL=C sort | xargs)" = "arm64 x86_64"
for architecture in arm64 x86_64; do
  otool -arch "$architecture" -l "$binary" | grep -A3 LC_BUILD_VERSION | grep "minos 11.0"
done
```

The completed local bundle is ad-hoc signed by default. To apply an available Developer ID Application certificate as the final build step, configure with:

```sh
cmake -S . -B build -DSUBLAB808_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
```

Public distribution additionally requires notarization. Local ad-hoc signing is suitable only for testing on the current machine.
The build embeds and signs `SubLab808Updater.app` before signing the enclosing
VST3 root. Signing never uses recursive `--deep`; the completed bundle is instead
verified recursively with `codesign --verify --deep --strict`.
