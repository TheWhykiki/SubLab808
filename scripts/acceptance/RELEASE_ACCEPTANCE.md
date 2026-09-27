# Physical release acceptance (receipt schema 2)

A signed candidate is accepted only after the exact immutable release has been
tested in Cubase and REAPER on the four physical systems. Build/test output and
an audio comparison report do not establish that these host actions occurred.

| Delivery artifact | Physical CPU | Host process | Hosts |
|---|---|---|---|
| Windows x64 MSI | x86_64 | x86_64 | Cubase, REAPER |
| Windows ARM64EC MSI | arm64 | x86_64 or arm64ec; record actual mode | Cubase, REAPER |
| macOS Universal PKG | x86_64 | x86_64 | Cubase, REAPER |
| macOS Universal PKG | arm64 | arm64, native | Cubase, REAPER |
| macOS Universal ZIP | x86_64 | x86_64 | Cubase, REAPER |
| macOS Universal ZIP | arm64 | arm64, native | Cubase, REAPER |

These are twelve mandatory checks per plugin. A Rosetta process on Apple Silicon
does not substitute for either a physical Intel system or native Apple Silicon
acceptance. Windows ARM64EC evidence records the actual supported host ABI.

Download all nine assets by the candidate's exact release ID. Check the GitHub
asset digests and SHA256SUMS, then validate the source ZIP. Keep the three
`*.evidence.json` files unchanged. The receipt validator binds their exact bytes
to immutable release digests and then uses their payload inventory to verify the
SHA-256 of the loaded plugin binary.

For every matrix row and host:

1. Close hosts before installation/replacement. Retain any prior installation for
   recovery. Leave UAC, Gatekeeper and notarization enforcement enabled.
2. Install the stated MSI/PKG or copy the stated ZIP VST3, rescan and instantiate.
   Record the exact loaded binary path, file SHA-256, product version and source
   commit. Record physical CPU and actual host-process architecture separately.
   A bundle directory hash or a hash of a different installed copy is not valid.
3. Verify GUI rendering/scaling, audio, automation playback and editing, project
   save/reopen, factory presets, user preset Save/Save As, Import/Export and
   two simultaneous instances. Verify dirty preset state and canceled dialogs.
4. Use the original 48-kHz/120-BPM MIDI/audio fixtures and before/after recall
   protocol in the [shared DAW protocol](https://github.com/TheWhykiki/ReverseLab/blob/fix/acceptance-regressions/scripts/acceptance/DAW_ACCEPTANCE.md).
   Preserve host identity and the exact original WAV evidence. The analysis tool's
   Cubase filenames must not be used to imply that a REAPER export came from Cubase.
5. On the Apple Silicon reference system, listen to all 64 factory presets of
   this plugin separately in Cubase and REAPER. On the remaining systems, listen
   to the eight documented focus presets. Record preset-by-preset results.
6. For SubLab808 verify Note-Off, Glide, pitch bend, Click state and decay/tail.
   For ReverseLab verify Freeze capture/release/recapture, seed, tempo changes,
   and recall from a fresh input buffer. A saved Freeze parameter does not persist
   recorded audio.
7. Exercise clean removal; on Windows also confirm downgrade and wrong-architecture
   package rejection. Do not mark a check passed until all its cases pass.

Copy `release/physical-daw-receipt.example.json` and replace every placeholder.
The initial result is `not-run`; only performed passing checks become `pass`.
Supply the actual UTC time after candidate publication, non-secret machine alias,
tester, DAW and OS version, loaded binary path and SHA-256. The four `artifacts`
digests come from GitHub; `loadedVst3Sha256` comes from the loaded binary and must
equal the payload hash in the matching immutable evidence file.

Under the owner-approved sole-owner policy, `TheWhykiki` submits the complete
JSON alone as the `physical-daw-release` approval comment. The environment
requires exactly the repository owner by immutable user ID and login, with
`prevent_self_review=false`, no administrator bypass and protected branches only.
The owner may also be the workflow or rerun initiator. This is owner attestation,
not independent QA; it does not reduce or waive any physical test. Signing approvals
for `release-signing` can coexist in the run; exactly one review must approve only
the physical environment. Mixed or duplicate physical approvals are rejected.

The separate candidate-specific Stable/Latest owner approval in
[`docs/PRODUCTION_RELEASE.md`](../../docs/PRODUCTION_RELEASE.md) is also mandatory;
an environment approval alone is not publication permission.

Retain projects, exports, preset lists, host screenshots and test notes with the
receipt. This is authenticated human attestation, not a machine proof of DAW use.
Never fill a receipt from expected results or a previous build.
