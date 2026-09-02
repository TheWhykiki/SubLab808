# Changelog

## Unreleased

- Corrected the maximum host-reported tail to 47 s, matching the exponential 4 s decay time constant.
- The oscillator now stops exactly at the envelope silence threshold; worst-case output is silent by the reported tail.
- Repeated and more than 16 simultaneous MIDI notes now retain correct last-note priority without lost Note Off state.
- MIDI note ownership, pitch bend, All Notes Off and All Sound Off are isolated correctly per channel.
- Pitch bend remains attached to the sounding voice throughout its Gate release.
- One Shot notes no longer retune or enter release in response to Note Off / All Notes Off.
- Factory program names remain stable for host caches; the editor continues to show modified sounds as Custom.
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
- Held-note tracking grows to 32 notes and forgets the oldest instead of the newest when full; All Notes Off respects One Shot like a note-off.
- Editor: "808" is positioned from the measured title width; the header shows the version.
- Legato: a new note played while another is held with Glide active now slides without retriggering the envelope, phase and click (previously every slide clicked from a phase reset).
- Idle voices stop rendering once envelope, click and filter have decayed.
- Program changes from the editor are reported to the host; tail length reduced to 8 s.
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
