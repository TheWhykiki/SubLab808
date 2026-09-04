# Preset QA – SubLab808 1.4.0

Local Release verification on 2026-09-04:

- SubLab808Smoke, SubLab808BundleLoad and SubLab808Presets: all passed.
- 64 complete factory recipes; unique names and recipes even when output and seed are excluded.
- All 8 legacy names, indices and values preserved.
- 192 rendered scenarios (64 presets × 3 MIDI pitches/velocities), 48 kHz; all finite, audible, peak ≤ 1.001.
- Observed peak range: 0.401785–0.738917. 64 different combined audio fingerprints.
- User save/load, overwrite, rename, delete, import/export and favourites passed.
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
