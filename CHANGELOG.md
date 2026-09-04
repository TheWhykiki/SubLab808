# Changelog

## 1.4.0 (unreleased)

- Expand the factory bank from 8 to 64 distinct, fully specified bass recipes; preserve all legacy program indices and values.
- Add a searchable preset browser, categories, favourites and previous/next navigation.
- Add persistent user presets with Save/Save As, rename, delete, validated import/export and conflict-safe atomic writes.
- Preserve user preset identity, baseline and unsaved edits in DAW state; protect edited sounds during browser navigation.
- Add persistence, invalid-file, UI and complete-bank audio tests to CI.

## Unreleased

- Corrected the maximum host-reported tail to 47 s, matching the exponential 4 s decay time constant.
- The oscillator now stops exactly at the envelope silence threshold; worst-case output is silent by the reported tail.
- Repeated and more than 16 simultaneous MIDI notes now retain correct last-note priority without lost Note Off state.
- MIDI note ownership, pitch bend, All Notes Off and All Sound Off are isolated correctly per channel.
- Reset All Controllers (CC121) now recentres Pitch Bend only on its MIDI channel.
- Live Tune and per-channel Pitch Bend changes use a short ramp to avoid zipper artefacts.
- Last-note ordering now updates in constant time and covers all 2,048 channel/note pairs.
- Pitch bend remains attached to the sounding voice throughout its Gate release.
- One Shot notes no longer retune or enter release in response to Note Off / All Notes Off.
- Factory program names remain stable for host caches; the editor continues to show modified sounds as Custom.
- Program changes and state restores are serialized; re-entrant host echoes are bounded without resuming stale listener notifications.
- Project saves and the audio thread retain the last complete parameter set while a preset or state is being published, preventing hybrid snapshots and transient hybrid DSP settings.
- Preset/state transactions revalidate the final parameter values before showing an unmodified factory name.
- Legacy states without a modified flag are classified by their actual factory parameter values; invalid state roots are ignored.
- Restored editor dimensions now update an editor that is already open.
- Editor dimensions are published as one atomic snapshot so worker-thread state restores cannot mix width and height.
- Parameter smoothing now continues while the synth is silent, so the next note starts with current automated values.
- GitHub Actions are pinned to immutable commit revisions.
- CI archives and reloads the downloadable VST3 so executable permissions and bundle integrity are verified.
- The loaded VST3 wrapper is rendered through its reported maximum tail, not only checked for metadata.
- The completed VST3 is re-signed after JUCE writes its module metadata, keeping the final resource seal valid.
- Final signing defaults to ad hoc but accepts a Developer ID identity for release builds.
- CI now fails unless the built VST3 contains exactly the intended ARM64 architecture.
- Host-test failures report invalid MIDI capability and plugin names explicitly; removed the unsafe SIGABRT handler.
- UI snapshot validation encodes in memory instead of leaving generated files in the checkout.
- Tune now acts live on a held note (previously only at note-on); note-on behaviour is unchanged.
- Factory presets use named fields instead of a positional value array; a test checks every preset value against its parameter range.
- All Notes Off respects One Shot like a note-off.
- Editor: "808" is positioned from the measured title width; the header shows the version.
- Legato: a new note played while another is held with Glide active now slides without retriggering the envelope, phase and click (previously every slide clicked from a phase reset).
- Idle voices stop rendering once envelope, click and filter have decayed.
- Program changes from the editor are reported through the host's dedicated program-change notification.
- Parameter identifiers carry version hints (VST3 IDs unchanged).
- Fixed the VST3 host test overflowing the wrapper's scratch buffers by processing a 2048-sample block after preparing for 512 (intermittent silence or abort in CI); it now renders in host-sized blocks.
- Tests: legato click detector, sample-rate invariance of the envelope, staged diagnostics in the VST3 host test, AddressSanitizer/UBSan CI job.
- CI: builds with the documented Unix Makefiles generator, caches JUCE, uploads the VST3 artefact.

## 1.3.1

- Added an automated VST3 bundle scan and host-instantiation test.
- Added MIDI-driven audio rendering through the built plugin bundle.
- Revalidated the clean macOS 11 ARM64 release and Cubase metadata.

## 1.3.0

- Smoothed automated DSP parameters to prevent zipper noise.
- Added a protected final output stage and reliable MIDI All Sound Off handling.
- Added held-note priority for monophonic playing and reset Pitch Bend on panic.
- Made click timing independent of project sample rate.
- Added Custom preset state, complete One Shot preset data, and editor-size persistence.
- Fixed compact header layout and added accessible names, descriptions, and tooltips.
- Corrected the release build target to macOS 11 and pinned JUCE to an exact commit.
- Expanded behavioral audio tests and release verification.

## 1.2.0

- Added One Shot and Gate playback modes.
- Added adjustable Gate release and velocity response.
- Added MIDI Pitch Bend support with a ±2-semitone range.
- Expanded the interface to twelve consistently styled controls.
- Extended smoke tests to cover Gate mode and Pitch Bend.
- Added reproducible JUCE dependency resolution and build documentation.

## 1.1.0

- Added eight factory presets and a preset selector.
- Exposed factory sounds as Cubase programs.
- Persisted the selected factory program in plugin state.
- Added automated audio validation for every factory preset.

## 1.0.0

- Initial Apple Silicon VST3 release.
- Monophonic 808 synthesis, pitch envelope, glide, drive, tone and output controls.
- Scalable custom interface and output meter.
