# Native file-chooser lifecycle regression tests

The macOS PresetTests target opens the product's real Import and Export menus,
then verifies native NSOpenPanel/NSSavePanel lifetime for editor ancestor-hide,
detach, and destruction (six cases per product). It does not replace the chooser
with a fake or call an artificial successful import/export callback.

Each case requires a visible, correctly typed native panel and a live JUCE modal
before the owner transition. Afterwards the native panel must be hidden, its JUCE
delegate cleared, and the JUCE modal destroyed. Exact processor state, preset
selection, and every file/directory in the temporary fixture must remain unchanged.
The reopened editor must accept a real Save As/Cancel interaction and parameter
button clicks.

The bridge observes only the test process's own NSApp windows. Panel retention is
test-only: it permits safe observation after JUCE closes/releases the panel. The
retained Cocoa object itself is not a deallocation or leak-freedom test. The
fixture redirects the panel to an isolated temporary directory containing an input
preset and an initially nonexistent export destination; it never confirms a file
operation. No DAW, installed bundle, or user preset library is modified.

## Running

Build the product's PresetTests target, then set
`WHYKIKI_PRESET_TEST_NATIVE_ONLY=1` when launching its executable. For example:

```sh
cmake --build build-review --target SubLab808PresetTests -j 2
env WHYKIKI_PRESET_TEST_NATIVE_ONLY=1 \
  ./build-review/SubLab808PresetTests_artefacts/Release/SubLab808PresetTests
```

For ReverseLab, use the equivalent `ReverseLabPresetTests` target/executable and
its configured build directory. A normal, unfiltered macOS PresetTests run also
includes all six cases alongside the existing lifecycle/persistence/audio tests.
The existing reentrancy-only and lifecycle-only modes remain unchanged.

An active macOS desktop session is required. Run native UI suites serially.
The console test's bridge completes NSApplication launch and activates only its
own process; missing activation, display, or native-panel availability is a setup
failure, not evidence of a plugin defect. A requested native-only run on another
platform fails explicitly instead of accepting a non-native fallback.

The .mm bridge intentionally uses JUCE's existing CXX toolchain. Do not enable a
separate OBJCXX language just for this file: that can reclassify every JUCE .mm file
and bypass sanitizer options supplied through CMAKE_CXX_FLAGS.

## Limits

These cases test JUCE owner hide/detach/destruction with genuine native panels.
They do **not** establish behavior when a particular DAW only hides an NSWindow
without changing the JUCE owner hierarchy. Cubase/REAPER acceptance on the exact
delivered bundle remains a separate requirement. They also do not exercise actual
file selection/confirmation or an externally queued native successful callback.
