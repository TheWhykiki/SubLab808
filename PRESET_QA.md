# Preset QA – SubLab808 1.4.0

Local Release verification on 2026-09-04:

- SubLab808Smoke, SubLab808BundleLoad and SubLab808Presets: all passed.
- 64 complete factory recipes; unique names and recipes even when output and seed are excluded.
- All 8 legacy names, indices and values preserved.
- 192 rendered scenarios (64 presets × 3 MIDI pitches/velocities), 48 kHz; all finite, audible, peak ≤ 1.001.
- Observed peak range: 0.401785–0.738917. 64 different combined audio fingerprints.
- User save/load, overwrite, rename, delete, import/export and favourites passed.
- A stale Rename dialog cannot rename a different preset after a host-state change.
- UI tests wait for queued callbacks using a message barrier, rather than relying on a fixed delay.
- Invalid/foreign/future-version files, duplicate names including umlauts, missing values, invalid numbers, write failures and stale-instance saves rejected without changing the sound/library.
- DAW recall restores all values, user name and unsaved edits even without the external preset file.
- A real desktop test window exercises Save As, Save, dirty-navigation Cancel/Discard, browser and search.
- Minimum/default/maximum editor sizes and browser/save-dialog snapshots visually inspected.
- ARM64 VST3, embedded version 1.4.0, strict ad-hoc signature verification passed.
- CI configurations updated; remote CI has not been run for this local change.

Audio checks establish technical behaviour under the defined fixtures; they do not replace musical listening in an arrangement.
The separate acceptance-review changes are integrated and released by the coordinated review task. This preset change does not publish or replace the installed plugin.

Run `SubLab808PresetTests <absolute-output-directory>` to retain the audio CSV and PNG previews.
The interactive part requires a desktop display. Tests use a temporary library and do not change real user presets.

## Cross-platform build follow-up (2026-09-05)

- A clean local macOS build produced exactly `arm64` and `x86_64` slices, both with a macOS 11.0 deployment target and an intact strict ad-hoc signature.
- Smoke, listener-lock, VST3 load/render/state recall and the non-UI preset save/restore subset passed natively on Apple Silicon, including the ZIP extraction roundtrip with identical binary and `moduleinfo.json` hashes.
- Native Intel-macOS, Windows x64 and Windows ARM64EC jobs are configured in CI; those remote runners and physical Cubase acceptance were not run by this local follow-up.

## Follow-up: preset file and dialog safety

- Managed exports (including directory symlinks) are rejected without changing stored bytes. Ordinary exports, including UTF-8 file paths, remain supported.
- Load, Save and Rename reject mismatched file identities; Import can recover the file under a fresh identity.
- Pending Discard and Save As actions preserve a sound changed by the DAW while the dialog was open. Export uses the same sound guard.
- Error alerts use an explicit asynchronous callback, avoiding nested modal loops when JUCE permits modal dispatch.
- Regression UI checks wait for the requested alert across queue turns.
- The complete local preset suites pass for both products; DSP and host checks remain green. No DSP, parameter or build configuration changed in this follow-up.
