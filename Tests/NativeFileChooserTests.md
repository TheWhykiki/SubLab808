# Native file-chooser lifecycle regression tests

The macOS PresetTests target opens the product's real Import and Export menus,
then verifies native NSOpenPanel/NSSavePanel lifetime for editor ancestor-hide,
detach, destruction, and hide-then-immediate-destruction (eight cases per product). It does not replace the chooser
with a fake or call an artificial successful import/export callback.

Each case requires a visible, correctly typed native panel and a live JUCE modal
before the owner transition. Afterwards the native panel must be hidden, its JUCE
delegate cleared, removed from `NSApp.windows`, and the JUCE modal destroyed. The
test-only observer also requires JUCE's real `beginWithCompletionHandler` block to
enter and return exactly once before the editor is reopened; panel disappearance
alone is not treated as completed teardown. The next same-process case and wrap-around
sentinel additionally detect leaked session state. Exact processor state, preset selection,
and every file/directory in the temporary fixture must remain unchanged.
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
deferred choosers are destroyed. Before the editor is reopened, the harness
returns repeatedly to the real top-level app loop and proves panel, delegate and
modal teardown. It deliberately does not claim that every queued AppKit NSEvent
has been drained, or that a host may dynamically unload the VST3 module in the
same call stack before AppKit has retired its completion handler; exact-host
acceptance must cover that stronger boundary.

The bridge observes only the test process's own NSApp windows. It installs a
test-process-only observer around `NSSavePanel.beginWithCompletionHandler`, calls
the original implementation and JUCE handler exactly as supplied, and records
entry and normal return without closing or confirming the panel itself. It stores
the panel's opaque identity and re-resolves it through the live window list on every
inspection; it deliberately does not retain the panel because JUCE's close-release
is part of the lifecycle under test. The fixture redirects the panel to an isolated
temporary directory containing an input preset and an initially nonexistent export
destination; it never confirms a file operation. No DAW, installed bundle, or user
preset library is modified.

The console target completes `NSApplication` launch once and then runs one genuine
top-level `MessageManager`/`NSApplication` loop for the complete native suite. A
timer-driven state machine performs at most one bounded action per callback and
returns after every asynchronous boundary: activation, menu dismissal, panel
presentation, owner transition, panel retirement, editor reopen and control probe.
It never calls `CFRunLoopRunInMode`, manually sends an `NSEvent`, or enters a nested
JUCE dispatch loop. AppKit therefore retires each modeless panel completion in its
normal application loop before the next session starts. Before its first order-in, the
synthetic `Preset UI Tests` host window also disables AppKit's automatic order
animation; otherwise that short-lived console-only window can leave a display-link
worker running after `main()` exits. Native
file-panel animations remain enabled. A callback itself can still block inside
AppKit, so CTest remains the hard process watchdog. This changes no product code
and relaxes none of the lifecycle assertions above.

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
precise diagnostics and also runs all eight sequentially in one process. That
run then repeats the first import/ancestor-hide case as a wrap-around sentinel,
proving that the final export/hide-then-destroy transition cannot poison the
next native session. The sequential test is required: it detects stale AppKit
modal state that process isolation would hide. Native UI is kept out of the
normal unfiltered PresetTests invocation; the existing reentrancy-only and
lifecycle-only modes remain unchanged.

The coordinator has an absolute 45-second isolated / 450-second sequential
deadline so a responsive failure reports its case and phase before CTest's
60-second / 480-second process watchdog. CTest remains the fallback for a callback
that blocks the message thread completely.

To reproduce one isolated case manually, also set
`WHYKIKI_PRESET_TEST_NATIVE_CASE` to an operation (`import` or `export`) plus
one of `ancestor-hide`, `detach`, `destroy`, or `hide-then-destroy`. The last
case intentionally performs no message-loop turn between hiding the owner and
destroying it, exercising member teardown before the queued timer can run.

An active macOS desktop session is required. Run native UI suites serially.
The console test's bridge completes NSApplication launch and activates only its
own process; missing activation, display, or native-panel availability is a setup
failure, not evidence of a plugin defect. A requested native-only run on another
platform fails explicitly instead of accepting a non-native fallback.

The project already enables Objective-C++ for its updater. Sanitizer builds must
therefore pass matching `CMAKE_OBJCXX_FLAGS`; `CMAKE_CXX_FLAGS` alone does not
instrument this `.mm` bridge or the MRC/block lifetime that it observes.

## Limits

These cases test JUCE owner hide/detach/destruction with genuine native panels.
They do **not** establish behavior when a particular DAW only hides an NSWindow
without changing the JUCE owner hierarchy. Cubase/REAPER acceptance on the exact
delivered bundle remains a separate requirement. They also do not exercise actual
file selection/confirmation or an externally queued native successful callback.
