# Native macOS updates

The **Updates...** button in SubLab808's VST3 editor opens a separate native
updater. Checking is on demand; the processor and audio callback do no network,
filesystem, installer or updater work. The helper is copied outside the loaded
VST3 before launch so it survives closing the DAW and replacing the plugin.

## User flow

1. Click **Updates...**. The helper checks this product's latest public stable
   GitHub release and shows the installed and available versions.
2. Click **Update laden & installieren** to download and verify the package.
3. Save projects and completely quit the calling DAW and other plugin hosts.
   Click **Installieren**. The helper checks that no process still has either
   standard plugin binary open and opens the verified package in macOS Installer.
4. Complete the normal Installer prompts, including administrator authentication
   where macOS requires it. Keep DAWs closed until the updater reports
   **Update installiert**. Closing its window keeps verification running; reopen
   the window from the Dock. This is a guided native installation, not unattended
   privileged installation. The updater never quits a DAW or enters a password.
5. The updater verifies the system package receipt, version, every payload file
   and code signature. Only then does it move an unchanged older user-level VST3
   into a unique backup under
   `~/Library/Application Support/Whykiki Audio/Update Backups/SubLab808/`.
   This prevents the old user installation from shadowing the new system version.
   Preset libraries and DAW project files are untouched. Restart/rescan the DAW.

An aborted Installer does not count as success. Once Installer exits, the updater
pauses polling and offers **Installation fortsetzen**. It first checks whether the
installation actually completed, then rechecks DAW usage and the package hash
before reopening Installer. An already-running Installer is brought forward;
a second installation is never intentionally opened. If a canceled wizard leaves
Installer running, quit Installer first. After 15 minutes automatic polling pauses
with an explicit incomplete state; this is not treated as proof of cancellation.
A payload mismatch after Installer exits offers an explicit repair; a migration
failure retains the old copy and pending job for another verification attempt.

Before the first Installer handoff, a private transaction record, package and
signed copy of the updater are saved in
`~/Library/Application Support/Whykiki Audio/Updates/SubLab808/<transaction-id>/`.
The pending record is stored as `pending.json` in the product's Updates directory.
Normal window closure keeps the process and product lock alive. Explicitly quitting
offers **Später fortsetzen** and displays the saved recovery app's path. Reopen
through the plugin's Updates button, or open that saved `SubLab808Updater.app`
directly without a DAW. Direct launch without a pending job cannot start an update.
No login item or background launch service is installed.

Resumption reloads and validates the product-relative record and rechecks package
trust, hash and contents before verifying the installation. A missing/corrupt
package can be downloaded again with the original pinned release digest. The backup
location is stable for the transaction: if the process stopped after moving the
user copy but before recording completion, matching backup bytes complete recovery
without another move. A newly created or modified user copy is preserved and
requires resolution. The record is removed only after verification and migration
succeed. Failed or deferred jobs retain their files. No automatic system-wide
rollback is claimed.
Do not restart another host during installation: macOS does not provide this
helper with an atomic lock preventing other applications from loading a VST3.
Other users' sessions may not be fully visible to the unprivileged process check.

## Release contract

- Repository: `TheWhykiki/SubLab808`; HTTPS GitHub Releases API, no embedded token.
- The UI distinguishes no published release, the current public release, and a
  development build newer than that release. Only newer releases are offered.
- Stable tags: `vMAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH`; numeric comparison;
  no prereleases, draft releases or downgrade of either standard installation.
- Exactly one asset named `SubLab808-MAJOR.MINOR.PATCH-macOS-universal.pkg`,
  in the matching release and repository. Maximum download size: 128 MiB.
- GitHub must provide the asset's `sha256:` digest. The complete downloaded bytes
  must match both the advertised size and digest before any installation.
- Only HTTPS redirects to GitHub's release asset hosts are accepted. Metadata is
  bounded to 1 MiB; downloads have size limits, cancellation and timeouts.
- The package must pass `pkgutil --check-signature` and macOS
  `spctl --assess --type install`. Distribution requires Developer ID Installer
  signing and Apple notarization. Local unsigned packages are deliberately rejected.
- One component package, identifier `audio.whykiki.sublab808.pkg`,
  exact release version, install-location `/`, non-relocatable, no installer
  scripts and no payload outside `Library/Audio/Plug-Ins/VST3/SubLab808.vst3`.
  Packaging sets strict bundle identity, version checking and complete replacement.
- Plugin identity/version, exactly the `arm64` and `x86_64` Mach-O slices, their
  macOS 11.0 deployment targets and the complete code signature are checked.
  Architecture inspection does not require end-user Xcode.
- Trust rests on the fixed GitHub repository over TLS, its asset digest and macOS
  package trust. There is currently no separate pinned Developer Team ID or
  independently signed update feed. Compromise of the release account remains a
  distribution risk; protect release permissions accordingly.

Only installations in `/Library/Audio/Plug-Ins/VST3` and
`~/Library/Audio/Plug-Ins/VST3` are supported. Arbitrary symlinked installation paths
are rejected. Existing custom locations need a normal installer migration.
This document and helper cover only macOS. Windows uses the separate native
[updater contract](WINDOWS_UPDATER.md) and [MSI contract](WINDOWS_INSTALLER.md).
Unsupported platforms disable the update button.

## Build and publish preparation

CMake builds and embeds `Contents/Helpers/SubLab808Updater.app` before the final
VST3 signing pass, signs that nested helper first, then signs the VST3 root
without recursive `--deep` signing. Recursive strict verification follows.
Helper source changes also relink, re-embed and re-sign the VST3. Local builds
use ad-hoc signing; this does not establish distribution trust.
The current source version is 1.4.0; bump `project(... VERSION ...)` for a new
public release, build the bootstrap release containing the helper, then publish
future strictly newer releases for in-plugin updates.

Run `bash scripts/package-release.sh` on macOS with CMake, Python and Xcode tools.
It builds a fresh source snapshot, checks generated presets, runs CTest, checks
both universal plugin/updater slices, and validates ZIP/PKG extraction through a
VST3 test host. The package is named
`SubLab808-MAJOR.MINOR.PATCH-macOS-universal.pkg`; the companion bundle archive
is `SubLab808-MAJOR.MINOR.PATCH-macOS-universal-VST3.zip`. It writes a new
candidate directory below `dist/` with checksums
and source/test evidence; existing candidates are never overwritten. SubLab's
pinned JUCE dependency is fetched by CMake; ReverseLab needs its JUCE submodule.
This command neither installs locally nor publishes to GitHub.

For trusted distribution, provide the existing real certificate names via
`SUBLAB808_APPLICATION_IDENTITY` and `SUBLAB808_INSTALLER_IDENTITY`, and an existing
keychain notary profile via `SUBLAB808_NOTARY_PROFILE`. Set
`SUBLAB808_NOTARY_KEYCHAIN` to the absolute, existing regular-file path of the
same keychain; the pipeline passes it explicitly to both `notarytool submit` and
`notarytool log` and rejects profile-only/default-keychain fallback. The nested helper is signed
before the enclosing VST3. A successful submission must yield a valid job UUID
and a matching full Accepted/zero-status Apple log without error issues; both
tickets and final Gatekeeper assessments are validated and retained as evidence.
No credential or identity is bundled in the updater. Without these settings the
pipeline produces a local test candidate which the updater will refuse to install.
Upload the verified `.pkg` to the corresponding stable GitHub release; check its
API digest and test the downloaded signed artifact before announcing availability.

As inspected on 2026-09-04, SubLab808 had no public release and ReverseLab's latest
release was the older unsigned 1.0.4. Those assets do not enable this workflow.
Users of builds without the helper need the first updater-enabled release through
a normal installation once. No installed plugin is changed by building/testing.

## Verification and remaining distribution gate

`python3 -B scripts/test-updater.py` compiles every production Swift component
(including the actual AppKit controller and HTTPClient; only the executable entry
point is excluded) and runs three suites:

- 23 policy tests: versions, product/URL/digest/universal-Mach-O validation, package metadata,
  real unsigned-package rejection, symlinks, backup preservation and interrupted
  archive recovery, including a new user copy appearing after the original rename.
- 16 controller/lifecycle tests: window closure, explicit defer, fresh-controller
  resume, cancellation, retry, critical-operation quit protection, failed handoff,
  DAW use, deadline, corrupted downloads, payload repair, migration failure and
  invalid/symlinked journals. System installation operations use explicit fixtures;
  journal and download fixture I/O is real, confined to temporary directories.
- 10 HTTP/controller tests: actual URLSession delegates with URLProtocol transport
  fixtures for responses, size bounds, offline errors, cancellation, redirects,
  malformed releases and version/no-release UI states.

These tests open no window or Installer and do not modify installed plugins.
CTest includes `SubLab808UpdaterPolicy` and the existing release-pipeline contract
tests; signing/notarization in the latter use fake tools. Bundle-load tests exercise
the built VST3. A live GitHub download/trust probe is separate from offline tests.

Before the first public updater release, perform a full signed/notarized N-to-N+1
installation on a test Mac: user-only, system-only and duplicate installations;
DAW still open and reopened; canceled download/Installer; offline/rate-limited API;
invalid trust/hash; user copy changed during install; success, backup and rescan.
Verify the universal package natively on both supported CPU architectures. Those
privileged and live-release scenarios require real release artifacts and are not
established by local ad-hoc builds or mock packaging tests.

Release references: [GitHub Releases API](https://docs.github.com/en/rest/releases/releases),
[Apple notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).
