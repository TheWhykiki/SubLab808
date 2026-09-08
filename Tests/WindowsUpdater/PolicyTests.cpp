#include "UpdaterPolicy.h"

#include <cstdlib>
#include <array>
#include <iostream>
#include <limits>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using namespace wk::windows_updater;

    const auto installed = parseVersion("1.4.0");
    const auto newer = parseStableTag("v1.4.1");
    require(installed && newer && isStrictlyNewer(*newer, *installed), "strict stable upgrade");
    require(! parseVersion("01.4.0") && ! parseVersion("1.4") && ! parseVersion("1.4.0.1"),
            "canonical three-part versions only");
    require(parseVersion("255.255.65535") && ! parseVersion("256.0.0")
                && ! parseVersion("0.256.0") && ! parseVersion("0.0.65536"),
            "Windows Installer ProductVersion bounds enforced before download");
    require(! parseStableTag("1.4.1") && ! parseStableTag("v1.4.1-rc1"),
            "stable tags require exact vMAJOR.MINOR.PATCH");
    require(! isStrictlyNewer(*installed, *installed)
                && ! isStrictlyNewer(*parseVersion("1.3.99"), *installed),
            "equal versions and downgrades rejected");

    require(isSafeRepositoryComponent("TheWhykiki") && isSafeRepositoryComponent("SubLab808"),
            "safe repository components");
    require(! isSafeRepositoryComponent("../owner") && ! isSafeRepositoryComponent("owner/repo"),
            "repository path injection rejected");
    require(expectedAssetName("SubLab808", *newer, Architecture::x64)
                == "SubLab808-1.4.1-Windows-x64.msi",
            "exact x64 asset name");
    require(expectedAssetName("SubLab808", *newer, Architecture::arm64ec)
                == "SubLab808-1.4.1-Windows-arm64ec.msi",
            "exact ARM64EC asset name");
    require(expectedAssetUrl("TheWhykiki", "SubLab808", "SubLab808", *newer, Architecture::x64)
                == "https://github.com/TheWhykiki/SubLab808/releases/download/v1.4.1/"
                   "SubLab808-1.4.1-Windows-x64.msi",
            "exact repository, tag and asset URL");
    require(releasesApiUrl("TheWhykiki", "SubLab808")
                == "https://api.github.com/repos/TheWhykiki/SubLab808/releases?per_page=100",
            "exact bounded releases API URL");
    require(releaseByIdApiUrl("TheWhykiki", "SubLab808", 123456789)
                == "https://api.github.com/repos/TheWhykiki/SubLab808/releases/123456789",
            "exact release-by-ID API URL");
    require(isGitCommitHex("0123456789abcdef0123456789abcdef01234567")
                && ! isGitCommitHex("0123456789abcdef0123456789abcdef0123456g"),
            "exact 40-hex source commit");
    require(parseCanonicalPositiveUint64("1") == std::uint64_t { 1 }
                && parseCanonicalPositiveUint64("18446744073709551615")
                    == std::numeric_limits<std::uint64_t>::max()
                && ! parseCanonicalPositiveUint64("0")
                && ! parseCanonicalPositiveUint64("01")
                && ! parseCanonicalPositiveUint64("18446744073709551616"),
            "canonical positive release ID");

    const std::string testPublicKey =
        "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296"
        "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5";
    require(isP256PublicKeyXYHex(testPublicKey)
                && ! isP256PublicKeyXYHex(testPublicKey.substr(2))
                && ! isP256PublicKeyXYHex(std::string(128, 'a')),
            "P-256 public key is canonical uppercase X||Y hex");
    std::array<std::uint8_t, 64> signatureBytes{};
    for (std::size_t index{}; index < signatureBytes.size(); ++index)
        signatureBytes[index] = static_cast<std::uint8_t>(index);
    const auto signatureHex = encodeP256P1363SignatureHex(signatureBytes);
    const std::string derSignatureHex =
        "30440220"
        "0000000000000000000000000000000000000000000000000000000000000001"
        "0220"
        "0000000000000000000000000000000000000000000000000000000000000001";
    require(signatureHex.size() == 128
                && decodeP256P1363SignatureHex(signatureHex) == signatureBytes
                && ! decodeP256P1363SignatureHex(signatureHex.substr(2))
                && ! decodeP256P1363SignatureHex(derSignatureHex)
                && ! decodeP256P1363SignatureHex(std::string(128, 'a'))
                && ! decodeP256P1363SignatureHex(
                    std::string(64, '0') + signatureHex.substr(64))
                && ! decodeP256P1363SignatureHex(
                    signatureHex.substr(0, 64) + std::string(64, '0'))
                && ! decodeP256P1363SignatureHex(
                    signatureHex.substr(0, 64)
                    + "7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A9"),
            "P-256 P1363 signatures use exactly 64 uppercase-hex bytes and canonical low-S");

    ReleaseGateAuthorizationFields authorization {
        "TheWhykiki", "SubLab808", "SubLab808", Architecture::x64,
        *installed, 123456789, "v1.4.1",
        "0123456789abcdef0123456789abcdef01234567",
        std::string(64, 'A'),
        "WhykikiAudio.UpdaterReleaseGate.0123456789ABCDEF0123456789ABCDEF",
        42, 133444736000000000, 1700000300
    };
    const auto canonicalAuthorization = canonicalReleaseGateAuthorization(authorization);
    const std::string expectedAuthorization =
        "domain=whykiki.windows-updater-release-gate-authorization\n"
        "schemaVersion=1\n"
        "repository=TheWhykiki/SubLab808\n"
        "product=SubLab808\n"
        "architecture=x64\n"
        "installedVersion=1.4.0\n"
        "releaseId=123456789\n"
        "tag=v1.4.1\n"
        "sourceCommit=0123456789abcdef0123456789abcdef01234567\n"
        "challenge=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
        "responsePipe=WhykikiAudio.UpdaterReleaseGate.0123456789ABCDEF0123456789ABCDEF\n"
        "parentProcessId=42\n"
        "parentProcessCreatedAtFiletime=133444736000000000\n"
        "expiresAtUnixSeconds=1700000300\n";
    require(canonicalAuthorization && *canonicalAuthorization == expectedAuthorization,
            "release-gate authorization has one fixed versioned LF encoding");
    require(isCanonicalReleaseGatePipeName(authorization.responsePipe)
                && ! isCanonicalReleaseGatePipeName(
                    "WhykikiAudio.UpdaterReleaseGate.0123456789abcdef0123456789abcdef")
                && ! isCanonicalReleaseGatePipeName("\\\\.\\pipe\\attacker"),
            "release-gate pipe name is a canonical bare current-user token");
    require(isReleaseGateExpiryValid(1300, 1000)
                && ! isReleaseGateExpiryValid(1000, 1000)
                && ! isReleaseGateExpiryValid(1301, 1000)
                && ! isReleaseGateExpiryValid(1, 0),
            "release-gate expiry is future-only and bounded to five minutes");

    const auto originalCanonical = *canonicalAuthorization;
    const auto requireMutationChangesAuthorization = [&] (const ReleaseGateAuthorizationFields& mutation)
    {
        const auto changed = canonicalReleaseGateAuthorization(mutation);
        require(changed && *changed != originalCanonical,
                "every accepted authorization-field mutation changes signed bytes");
    };
    auto mutation = authorization;
    mutation.owner = "OtherOwner";
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.repository = "OtherRepository";
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.product = "OtherProduct";
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.architecture = Architecture::arm64ec;
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.installedVersion = *parseVersion("1.3.9");
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    ++mutation.releaseId;
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.tag = "v1.4.2";
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.sourceCommit = std::string(40, 'f');
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.challenge = std::string(64, 'B');
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    mutation.responsePipe =
        "WhykikiAudio.UpdaterReleaseGate.FEDCBA9876543210FEDCBA9876543210";
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    ++mutation.parentProcessId;
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    ++mutation.parentProcessCreatedAtFiletime;
    requireMutationChangesAuthorization(mutation);
    mutation = authorization;
    ++mutation.expiresAtUnixSeconds;
    requireMutationChangesAuthorization(mutation);

    const std::array<void (*) (ReleaseGateAuthorizationFields&), 6> invalidMutations {
        [] (ReleaseGateAuthorizationFields& value) { value.sourceCommit[0] = 'A'; },
        [] (ReleaseGateAuthorizationFields& value) { value.challenge[0] = 'a'; },
        [] (ReleaseGateAuthorizationFields& value) { value.responsePipe.back() = 'a'; },
        [] (ReleaseGateAuthorizationFields& value) { value.parentProcessId = 0; },
        [] (ReleaseGateAuthorizationFields& value) { value.parentProcessCreatedAtFiletime = 0; },
        [] (ReleaseGateAuthorizationFields& value) { value.expiresAtUnixSeconds = 0; }
    };
    for (const auto invalid : invalidMutations)
    {
        mutation = authorization;
        invalid(mutation);
        require(! canonicalReleaseGateAuthorization(mutation),
                "non-canonical release-gate authorization field rejected");
    }

    const std::string digest(64, 'a');
    const auto normalized = digestHex("sha256:" + digest);
    require(normalized && *normalized == std::string(64, 'A'), "GitHub sha256 digest normalized");
    require(! digestHex("sha512:" + digest) && ! digestHex("sha256:abcd"),
            "missing or malformed digest rejected");
    require(isAllowedHttpsHost("api.github.com")
                && isAllowedHttpsHost("release-assets.githubusercontent.com"),
            "required GitHub hosts allowed");
    require(! isAllowedHttpsHost("api.github.com.attacker.invalid")
                && ! isAllowedHttpsHost("raw.githubusercontent.com"),
            "redirect host matching is exact");
    require(isCanonicalGuid("DB0CABBA-9411-5738-8A43-98D900748C58")
                && ! isCanonicalGuid("{DB0CABBA-9411-5738-8A43-98D900748C58}"),
            "canonical UpgradeCode format");

    const std::string currentCode = "DB0CABBA-9411-5738-8A43-98D900748C58";
    const std::string otherCode = "8494E96B-8735-5AB6-8E20-D1BF667DADD9";
    const std::vector<MsiUpgradeRow> upgrades {
        { currentCode, "", "1.4.1", "1033", 0x001, "", "WIX_UPGRADE_DETECTED" },
        { currentCode, "1.4.1", "", "1033", 0x002, "", "WIX_DOWNGRADE_DETECTED" },
        { otherCode, "0.0.0", "", "", 0x102, "", "OTHERARCHITECTUREDETECTED" }
    };
    require(hasExactUpgradeContract(upgrades, currentCode, otherCode, *newer),
            "exact WiX 6.0.2 three-row upgrade contract");
    for (std::size_t row = 0; row < upgrades.size(); ++row)
    {
        auto mutant = upgrades;
        mutant[row].attributes ^= 0x200;
        require(! hasExactUpgradeContract(mutant, currentCode, otherCode, *newer),
                "upgrade attribute mutant rejected");
    }
    auto foreignUpgrade = upgrades;
    foreignUpgrade[2].upgradeCode = currentCode;
    require(! hasExactUpgradeContract(foreignUpgrade, currentCode, otherCode, *newer),
            "foreign/duplicate architecture upgrade rows rejected");
    auto removeAll = upgrades;
    removeAll[0].remove = "ALL";
    require(! hasExactUpgradeContract(removeAll, currentCode, otherCode, *newer),
            "non-NULL WiX Upgrade Remove policy rejected");
    require(hasExactLaunchConditions({ "NOT WIX_DOWNGRADE_DETECTED",
                                       "(Installed OR NOT OTHERARCHITECTUREDETECTED)" }),
            "exact launch conditions accepted");
    require(! hasExactLaunchConditions({ "Installed OR NOT WIX_DOWNGRADE_DETECTED",
                                         "Installed OR NOT OTHERARCHITECTUREDETECTED" }),
            "legacy WiX downgrade condition rejected");
    require(! hasExactLaunchConditions({ "1 OR NOT WIX_DOWNGRADE_DETECTED",
                                         "Installed OR NOT OTHERARCHITECTUREDETECTED" }),
            "bypassed downgrade condition rejected");
    for (const auto table : { "CustomAction", "MsiEmbeddedUI", "MsiEmbeddedChainer",
                              "AppSearch", "RegLocator", "Control", "ControlEvent",
                              "ServiceInstall", "MsiServiceConfig",
                              "MsiServiceConfigFailureActions", "MoveFile",
                              "Permission", "PermissionEx", "MsiPatchCertificate" })
        require(isForbiddenMsiSideEffectTable(table),
                "dangerous MSI side-effect table mutation rejected");
    require(! isForbiddenMsiSideEffectTable("File"),
            "dangerous MSI side-effect table policy");
    for (const auto action : { "ForceReboot", "ScheduleReboot", "DisableRollback" })
        require(isForbiddenMsiSequenceAction(action),
                "dangerous MSI sequence action mutation rejected");
    require(! isForbiddenMsiSequenceAction("InstallFiles")
                && ! isForbiddenMsiSequenceAction("MoveFiles")
                && ! isForbiddenMsiSequenceAction("MsiConfigureServices"),
            "table-dependent MSI standard actions remain allowed");
    for (const auto property : { "DISABLEROLLBACK", "TARGETDIR", "ROOTDRIVE",
                                 "TRANSFORMS", "TRANSFORMSATSOURCE", "TRANSFORMSSECURE" })
        require(isForbiddenMsiProperty(property),
                "dangerous MSI control Property mutation rejected");
    require(isForbiddenMsiProperty("targetdir")
                && ! isForbiddenMsiProperty("ProductName"),
            "MSI control Property comparison is case-insensitive");

    const std::map<std::string, std::string> directories {
        { "Contents", "INSTALLFOLDER" }, { "Resources", "Contents" },
        { "Binary", "Contents" }, { "INSTALLFOLDER", "VST3Folder" },
        { "VST3Folder", "CommonFiles64Folder" }, { "CommonFiles64Folder", "TARGETDIR" },
        { "TARGETDIR", "" }
    };
    const std::map<std::string, std::string> safeProperties {
        { "ProductName", "Example" }, { "Manufacturer", "Example" }
    };
    require(! hasMsiDirectoryPropertyOverride(safeProperties, directories),
            "unrelated MSI properties do not override Directory identifiers");
    auto directoryOverride = safeProperties;
    directoryOverride["installfolder"] = "C:\\Windows";
    require(hasMsiDirectoryPropertyOverride(directoryOverride, directories),
            "case-insensitive MSI Directory Property override rejected");
    require(isPredefinedMsiPathProperty("WindowsFolder")
                && isPredefinedMsiPathProperty("sourcedir")
                && ! isPredefinedMsiPathProperty("Contents")
                && msiDirectoryIdentifiersAreSafe(directories, "CommonFiles64Folder"),
            "predefined MSI path Property identifiers are classified");
    auto engineRedirect = directories;
    engineRedirect["WindowsFolder"] = "INSTALLFOLDER";
    require(! msiDirectoryIdentifiersAreSafe(engineRedirect, "CommonFiles64Folder"),
            "engine-defined WindowsFolder child escape rejected");
    require(componentDirectoriesAreInsideInstallFolder(directories, { "Resources", "Binary" }),
            "all payload components descend from INSTALLFOLDER");
    require(! componentDirectoriesAreInsideInstallFolder(directories, { "Resources", "WindowsFolder" }),
            "component escaping to WindowsFolder rejected");
    auto cycle = directories;
    cycle["A"] = "B";
    cycle["B"] = "A";
    require(! componentDirectoriesAreInsideInstallFolder(cycle, { "Resources", "Binary" }),
            "disconnected directory cycle rejected");
    auto dangling = directories;
    dangling["Unused"] = "MissingParent";
    require(! componentDirectoriesAreInsideInstallFolder(dangling, { "Resources", "Binary" }),
            "dangling directory parent rejected");
    auto extraRoot = directories;
    extraRoot["OtherRoot"] = "";
    require(! componentDirectoriesAreInsideInstallFolder(extraRoot, { "Resources", "Binary" }),
            "non-TARGETDIR Directory root rejected");

    std::cout << "PASS: Windows updater portable policy\n";
    return 0;
}
