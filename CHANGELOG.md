# Changelog

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
