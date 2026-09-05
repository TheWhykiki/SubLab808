# Native Windows updater contract

`Updater/Windows` is a separate, visible, on-demand updater executable shared by
SubLab808 and ReverseLab. It is not a service, scheduled task, startup item or
audio-thread component. The plugin may launch it only from an explicit **Updates...**
button. The executable copies itself to its private operation directory before
doing any download or installation work, so closing the DAW cannot unload the
running updater from a VST3 bundle that Windows Installer may replace.

## Release identity

The build supplies the product-specific values; the C++ implementation stays
byte-identical between repositories.

| Product | Repository | Current version source | Asset names |
| --- | --- | --- | --- |
| SubLab808 | `TheWhykiki/SubLab808` | CMake project version | `SubLab808-VERSION-Windows-x64.msi`, `SubLab808-VERSION-Windows-arm64ec.msi` |
| ReverseLab | `TheWhykiki/ReverseLab` | CMake project version | `ReverseLab-VERSION-Windows-x64.msi`, `ReverseLab-VERSION-Windows-arm64ec.msi` |

Only `/repos/OWNER/REPOSITORY/releases/latest` is queried. A release must be
published, non-draft and non-prerelease; its tag must be exactly
`vMAJOR.MINOR.PATCH`. Versions use Windows Installer's bounds (major/minor
0–255, patch 0–65535) and must be strictly newer than both the invoking build
and any system VST3 already present. Exactly one architecture-specific asset is
accepted. Its browser URL must be the exact repository/tag/name URL, its size
must be positive and at most 256 MiB, and GitHub must publish a valid
`sha256:` digest.

WinHTTP follows at most five redirects, manually, over HTTPS port 443. Only the
exact hosts `api.github.com`, `github.com`, `objects.githubusercontent.com`,
`release-assets.githubusercontent.com` and `github-releases.githubusercontent.com`
are accepted. Metadata is capped at 1 MiB, each operation has connect/read and
total deadlines, and no GitHub token or user credential is stored or sent.

## Trust and installation

The plugin-side launcher verifies the bundled helper before `CreateProcessW`
while holding its non-reparse file handle against replacement. `WinVerifyTrust`
checks the signed bytes, timestamp and locally available certificate chain with
revocation disabled for this one UI-thread gate; URL retrieval is cache-only.
Any local trust error or mismatch with the plugin's exact SHA-256 leaf pin blocks
launch. This avoids rejecting a valid first-run/offline system merely because it
has no cached CRL/OCSP response. The standalone updater then performs online
whole-chain revocation checking for the helper before handoff and for the MSI
before installation, so unknown or revoked distribution trust still fails closed
outside the DAW process.

Before elevation, the complete MSI is size/hash checked, held against write or
replacement, accepted by `WinVerifyTrust`, and bound to the distribution
certificate's pinned SHA-256 thumbprint. The updater then opens the MSI database
read-only and verifies exact product, manufacturer, version, architecture,
ProductCode/UpgradeCodes, per-machine scope and downgrade/other-architecture
rules. The WiX 6.0.2 Upgrade table must contain exactly the three reviewed rows,
including NULL `Remove` fields and their exact bounds, languages and attributes.
Every file must have a safe leaf name and belong to a 64-bit component whose
Directory ancestry is part of a complete cycle-free graph ending at the one
product `INSTALLFOLDER`. External cabinets, custom/binary/script
actions, services, registry writes, path-moving/removal/duplication tables,
shortcuts, permission tables and forced reboot/rollback-disabling actions are
rejected.

The verified MSI is administratively extracted through the trusted System32
`msiexec.exe` into a current-user-only operation Temp directory. Reparse points,
alternate data streams, case-colliding paths, executable scripts and foreign
files outside the one VST3 payload are rejected. `moduleinfo.json` must identify
the exact product, vendor and version. Every PE inside the bundle must have the
expected x64 or ARM64EC/ARM64X form and the pinned Authenticode signer. The
updater records a complete path/size/SHA-256 tree fingerprint.

After an explicit reminder to save work and close Cubase, REAPER and all other
plugin hosts, the updater starts a normal visible `msiexec /i` with UAC. It does
not terminate a DAW, automate a password prompt or perform a silent install.
Exit codes 0 and 3010 are accepted. Success is shown only after the system VST3
has again passed identity, version, architecture, signature and exact full-tree
comparison with the administrative image. Presets and DAW projects are outside
the MSI payload and are never modified.

Each operation has an unpredictable GUID directory, restrictive ACL, exclusive
process/operation locks and an atomically replaced JSON journal. A canceled or
offline phase can be selected for a later retry. Old partial downloads are never
treated as verified; every resumed phase repeats the relevant hash, signature,
MSI and payload checks. A current/older release is recorded as the terminal
`no-update` phase, so it is reported as an informational result and is not later
offered as a resumable failed installation.

## Build integration contract

Do not invent a signer identity or UpgradeCode. Read the two fixed architecture
UpgradeCodes from the repository's reviewed Windows installer configuration.
The production target must receive all of these definitions:

```cmake
add_executable(${PROJECT_NAME}WindowsUpdater WIN32
    Updater/Windows/main.cpp
    Updater/Windows/Updater.manifest
    Updater/Windows/WindowsUpdater.cpp
    Updater/Windows/UpdaterPolicy.cpp)
target_include_directories(${PROJECT_NAME}WindowsUpdater PRIVATE Updater/Windows)
target_compile_features(${PROJECT_NAME}WindowsUpdater PRIVATE cxx_std_20)
target_compile_definitions(${PROJECT_NAME}WindowsUpdater PRIVATE
    WK_WINDOWS_UPDATER_PRODUCT="${PROJECT_NAME}"
    WK_WINDOWS_UPDATER_VERSION="${PROJECT_VERSION}"
    WK_WINDOWS_UPDATER_MANUFACTURER="Whykiki Audio"
    WK_WINDOWS_UPDATER_GITHUB_OWNER="TheWhykiki"
    WK_WINDOWS_UPDATER_GITHUB_REPOSITORY="${PROJECT_NAME}"
    WK_WINDOWS_UPDATER_UPGRADE_CODE="${CURRENT_ARCH_UPGRADE_CODE}"
    WK_WINDOWS_UPDATER_OTHER_UPGRADE_CODE="${OTHER_ARCH_UPGRADE_CODE}"
    WK_WINDOWS_UPDATER_SIGNER_SHA256="${DISTRIBUTION_SIGNER_SHA256}"
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(${PROJECT_NAME}WindowsUpdater PRIVATE juce::juce_core
    bcrypt comctl32 crypt32 msi shell32 winhttp wintrust advapi32 ole32
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
```

`cmake/Updater.cmake` reads the product and both UpgradeCodes directly from
`Installer/Windows/package-config.json`. The target architecture is the same
Visual Studio `-A x64` or `-A ARM64EC` as its VST3 and MSI. With an empty
`SUBLAB808_WINDOWS_UPDATER_SIGNER_SHA256` or
`REVERSELAB_WINDOWS_UPDATER_SIGNER_SHA256`, CMake deliberately omits the
production helper and leaves the editor button disabled. A supplied value must
be exactly 64 hexadecimal characters; the production source independently
fails compilation without the matching macro. This is the SHA-256 certificate
fingerprint, not a file digest. The MSI pipeline must Authenticode-sign the
embedded EXE with that certificate before signing the enclosing MSI.

Before signing or packaging a staged production helper, the MSI packager executes
its pure build-contract gate. The argument order and spellings are intentionally
fixed; the packager creates a cryptographically random 32-byte challenge and a
separate random private named-pipe endpoint for every invocation:

```text
ProductUpdater.exe --validate-build-contract --challenge 64-HEX --response-pipe WhykikiAudio.UpdaterBuildContract.32-HEX --parent-process-id DECIMAL-PID --product Product --version 1.2.3 --manufacturer "Whykiki Audio" --github-owner TheWhykiki --github-repository Product --architecture x64 --upgrade-code CURRENT-GUID --other-upgrade-code OTHER-GUID --signer-sha256 64-HEX-SHA256
```

Use `arm64ec` (lowercase) for Windows on Arm. The WIN32-subsystem helper does not
depend on a console or stdout. It connects only to the supplied pipe, verifies
that its server is the named parent process, and writes one canonical ASCII/UTF-8
JSON record ending in a single LF. The record contains schema and schema version,
the exact fresh challenge, server PID, `buildMode=production`,
`compileOnly=false`, and every compiled identity field: product, version,
manufacturer, GitHub owner/repository, architecture, both UpgradeCodes and the
certificate SHA-256 pin.

The packager owns a one-instance `CurrentUserOnly` byte-mode pipe and uses the OS
pipe metadata to require that the connected client PID is exactly the updater
process it just started. It applies one 30-second deadline to
connection/read/process exit and reads at most the exact expected record size
plus one byte. It accepts only byte-for-byte equality (no
BOM, alternate encoding, whitespace or trailing data) together with exit code
0. Console text and exit code alone carry no authority. This path performs no
dialog, ordinary file access, network request or elevation. Test-mode and
`WK_WINDOWS_UPDATER_COMPILE_ONLY=1` binaries reject the contract before opening
the pipe, so neither can be embedded or packaged as a production helper.

Windows CI also builds the explicit `ProductWindowsUpdaterLauncherShape` target.
It is an `EXCLUDE_FROM_ALL` executable, not an object-only compile check: the
target compiles the real `Source/UpdaterLauncher.cpp`, links it with JUCE,
`crypt32` and `wintrust`, and uses a minimal consumer that passes an intentionally
invalid product name. Even if run accidentally, it therefore stops before bundle
discovery, signature verification or process creation. Its output name ends in
`UpdaterLauncherLinkShape-UNSIGNED-NOT-FOR-DISTRIBUTION`; it must never be
embedded, packaged, signed as a release asset or uploaded.

CI builds a separate test target with `WK_WINDOWS_UPDATER_TEST_MODE=1` and the
reviewed product/repository/UpgradeCode configuration. That mode can run policy and Windows
fixture hooks but `launchMsi` rejects before opening a file, calling
`ShellExecuteExW` or requesting elevation. It must never be packaged. Compile
`Tests/WindowsUpdater/WindowsUpdaterTests.cpp` with the implementation for this
Windows-only hook, and compile `Tests/WindowsUpdater/PolicyTests.cpp` with
`UpdaterPolicy.cpp` on every platform.

CTest registers both the portable policy binary and the static source contract.
The latter is also runnable directly:

```text
python -m unittest -v scripts/tests/test_windows_updater.py
```

Before release, Windows x64 and native Windows-on-Arm must compile the real EXE
with `/W4`, run both test targets, sign an N+1 MSI and exercise download,
cancel/offline/resume, UAC cancellation, DAW-still-open guidance, 0/3010, wrong
digest/signer/architecture/UpgradeCode, malicious MSI tables/reparse points and
post-install tampering. Local source/static tests do not claim that privileged
signed installation acceptance.

The gated GitHub Actions path that performs the two signed builds and publishes
both MSI assets together is specified in [WINDOWS_RELEASE.md](WINDOWS_RELEASE.md).
