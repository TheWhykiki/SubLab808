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
   where macOS requires it. Keep DAWs closed and the updater open until it reports
   **Update installiert**. This is a guided native installation, not unattended
   privileged installation. The updater never quits a DAW or enters a password.
5. The updater verifies the system package receipt, version, every payload file
   and code signature. Only then does it move an unchanged older user-level VST3
   into a unique backup under
   `~/Library/Application Support/Whykiki Audio/Update Backups/SubLab808/`.
   This prevents the old user installation from shadowing the new system version.
   Preset libraries and DAW project files are untouched. Restart/rescan the DAW.

An aborted Installer does not count as success. Close the updater and restart it
from the plugin to retry a canceled installation. If verification fails after
installation, inspect the displayed error; the old user copy remains unless it
was already successfully archived. No automatic system-wide rollback is claimed.
Do not restart another host during installation: macOS does not provide this
helper with an atomic lock preventing other applications from loading a VST3.
Other users' sessions may not be fully visible to the unprivileged process check.

## Release contract

- Repository: `TheWhykiki/SubLab808`; HTTPS GitHub Releases API, no embedded token.
- Stable tags: `vMAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH`; numeric comparison;
  no prereleases, draft releases or downgrade of either standard installation.
- Exactly one asset named `SubLab808-MAJOR.MINOR.PATCH-macOS-arm64.pkg`,
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
- Plugin identity/version, expected Mach-O architectures and the complete code
  signature are checked. Architecture inspection does not require end-user Xcode.
- Trust rests on the fixed GitHub repository over TLS, its asset digest and macOS
  package trust. There is currently no separate pinned Developer Team ID or
  independently signed update feed. Compromise of the release account remains a
  distribution risk; protect release permissions accordingly.

Only installations in `/Library/Audio/Plug-Ins/VST3` and
`~/Library/Audio/Plug-Ins/VST3` are supported. Arbitrary symlinked installation paths
are rejected. Existing custom locations need a normal installer migration.
Only macOS is supported; non-macOS test builds disable the update button.

## Build and publish preparation

CMake builds and embeds `Contents/Helpers/SubLab808Updater.app` before the final
VST3 signing pass. Helper source changes also relink, re-embed and re-sign the
VST3. Local builds use ad-hoc signing; this does not establish distribution trust.
The current source version is 1.4.0; bump `project(... VERSION ...)` for a new
public release, build the bootstrap release containing the helper, then publish
future strictly newer releases for in-plugin updates.

Run `bash scripts/package-release.sh` on macOS with CMake, Python and Xcode tools.
It builds a fresh source snapshot, checks generated presets, runs CTest, checks
both the plugin and embedded helper, and validates ZIP/PKG extraction through a
VST3 test host. It writes a new candidate directory below `dist/` with checksums
and source/test evidence; existing candidates are never overwritten. SubLab's
pinned JUCE dependency is fetched by CMake; ReverseLab needs its JUCE submodule.
This command neither installs locally nor publishes to GitHub.

For trusted distribution, provide the existing real certificate names via
`SUBLAB808_APPLICATION_IDENTITY` and `SUBLAB808_INSTALLER_IDENTITY`, and an existing
keychain notary profile via `SUBLAB808_NOTARY_PROFILE`. The nested helper is signed
before the enclosing VST3. A successful notarization is stapled and validated.
No credential or identity is bundled in the updater. Without these settings the
pipeline produces a local test candidate which the updater will refuse to install.
Upload the verified `.pkg` to the corresponding stable GitHub release; check its
API digest and test the downloaded signed artifact before announcing availability.

As inspected on 2026-09-04, SubLab808 had no public release and ReverseLab's latest
release was the older unsigned 1.0.4. Those assets do not enable this workflow.
Users of builds without the helper need the first updater-enabled release through
a normal installation once. No installed plugin is changed by building/testing.

## Verification and remaining distribution gate

`python3 -B scripts/test-updater.py` exercises version/product/URL/digest policies,
Mach-O parsing, corrupt/truncated downloads, package metadata, symlink rejection,
backup preservation and rejection of a real unsigned local PKG. It does not open
Installer, access live releases or modify an installed plugin. CTest also runs
`SubLab808UpdaterPolicy` and the release-pipeline contract tests; their macOS signing
and notarization paths use fake tools. Bundle-load tests exercise the built VST3.

Before the first public updater release, perform a full signed/notarized N-to-N+1
installation on a test Mac: user-only, system-only and duplicate installations;
DAW still open and reopened; canceled download/Installer; offline/rate-limited API;
invalid trust/hash; user copy changed during install; success, backup and rescan.
Verify both supported CPU architectures where applicable. Those privileged and
live-release scenarios require real release artifacts and are not established by
local ad-hoc builds or mock packaging tests.

Release references: [GitHub Releases API](https://docs.github.com/en/rest/releases/releases),
[Apple notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).
