# Native file-chooser lifecycle regression tests

The macOS PresetTests target opens the product's real Import and Export menus,
then verifies native NSOpenPanel/NSSavePanel lifetime for editor ancestor-hide,
detach, destruction, and hide-then-immediate-destruction (eight cases per product). It does not replace the chooser
with a fake or call an artificial successful import/export callback.

Each case requires a visible, correctly typed native panel and a live JUCE modal
before the owner transition. Afterwards the native panel must be hidden, its JUCE
delegate cleared, and the JUCE modal destroyed. Exact processor state, preset
selection, and every file/directory in the temporary fixture must remain unchanged.
The reopened editor must accept a real Save As/Cancel interaction and parameter
button clicks.

On macOS, JUCE destroys a native `FileChooser` by leaving its modal state,
removing its peer, and closing the AppKit panel. On hide or detach, PresetBar
therefore invalidates the callback and removes the chooser from its active slot
synchronously, but defers native destruction to its next message-thread timer
event. This avoids re-entering the `ComponentMovementWatcher` notification that
caused cancellation without posting a second owner-specific callback object.
The deferred chooser remains owned by PresetBar. On destruction, its existing
timer is stopped and member ordering unregisters the watcher before active or
deferred choosers are destroyed. Thus no callback or native object survives
into a possible VST3 module unload.

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
its configured build directory. CTest registers all eight cases separately for
precise diagnostics and also runs all eight sequentially in one process. The
sequential test is required: it detects stale AppKit modal state that process
isolation would hide. Native UI is kept out of the normal unfiltered PresetTests
invocation; the existing reentrancy-only and lifecycle-only modes remain
unchanged.

To reproduce one isolated case manually, also set
`WHYKIKI_PRESET_TEST_NATIVE_CASE` to an operation (`import` or `export`) plus
one of `ancestor-hide`, `detach`, `destroy`, or `hide-then-destroy`. The last
case intentionally performs no message-loop turn between hiding the owner and
destroying it, so a queued native chooser cannot outlive the plug-in owner.

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
