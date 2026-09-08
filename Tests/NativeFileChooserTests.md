# Native file-chooser lifecycle regression tests

The macOS PresetTests target opens the product's real Import and Export menus,
then verifies native NSOpenPanel/NSSavePanel lifetime for editor ancestor-hide,
detach, destruction, and hide-then-immediate-destruction (eight cases per product). It does not replace the chooser
with a fake or call an artificial successful import/export callback.

Each case requires a visible, correctly typed native panel and a live JUCE modal
before the owner transition. The interception also proves that the product retained
its module before AppKit copied the completion block. Afterwards the native panel must be hidden, its JUCE
delegate cleared, removed from `NSApp.windows`, and the JUCE modal destroyed. The
test-only observer marks safe owner retirement only after all four conditions hold.
At that boundary the callback must either have entered and returned exactly once,
or remain entirely unentered. AppKit does not promise to release a modeless
completion block immediately after a programmatic close, so block release is
recorded only as a diagnostic. A process-wide counter records any completion that
enters after owner retirement; such an entry is legal only if it returns exactly
once without changing product state or files. The next same-process case and wrap-around
sentinel additionally detect leaked observer state. Exact processor state, preset selection,
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
modal teardown. JUCE's modeless handler captures a `SafePointer` to that native
component, so a later AppKit invocation cannot reach the destroyed chooser. The
harness nevertheless records any such late entry while the process continues.
Before launching a native chooser, the product resolves its own Mach-O image and
acquires one ref-counted `RTLD_NOLOAD` handle that is intentionally kept until
process exit. If that fails, Import/Export stops without registering another
asynchronous UI callback. This keeps the block's code mapped even if a host drops
its VST3 handle. A separate unloadable-module test proves this behavior with an
unpinned negative control. The panel harness does not claim that every queued
AppKit event has been drained; exact-host acceptance still covers host behavior.

The bridge observes only the test process's own NSApp windows. It installs a
test-process-only observer around `NSSavePanel.beginWithCompletionHandler`, calls
the original implementation and JUCE handler exactly as supplied, and records
module retention at entry, normal return, final wrapper-block release, safe owner retirement, and any
late entry without closing or confirming the panel itself. A callback-local retain
keeps the observer valid if the JUCE handler synchronously releases AppKit's last
block owner. It stores
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
JUCE dispatch loop. The harness does not assume that AppKit invokes, discards, or
releases a modeless panel completion after a programmatic close. Instead it marks
the verified JUCE/AppKit owner-retirement boundary atomically and keeps the late-
entry sentinel active across subsequent editor interaction and chooser sessions.
Completed processors and their final state/file snapshots are retained and audited
on every later coordinator turn, so a late callback cannot hide in the final fence.
Before its first order-in, the
synthetic `Preset UI Tests` host window also disables AppKit's automatic order
animation; otherwise that short-lived console-only window can leave a display-link
worker running after `main()` exits. Native
file-panel animations remain enabled. A callback itself can still block inside
AppKit, so CTest remains the hard process watchdog. The lifetime guard is product
code; the AppKit interception remains test-only and relaxes none of the assertions.

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

The normal, non-sanitized macOS CTest matrices also run `MacModulePinPinned` and
`MacModulePinUnpinned` in separate
processes against a deliberately unloadable module. Sixteen threads race the
first pin and must observe one cached status; after the simulated host handle is
closed, pinned code remains callable while the unpinned control is absent. These
two controls are omitted under sanitizers because an instrumented DSO may be made
non-unloadable by the sanitizer runtime, which would invalidate the oracle.

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
They do not replace unload and UI acceptance of the signed VST3 in every supported
Cubase/REAPER version.
