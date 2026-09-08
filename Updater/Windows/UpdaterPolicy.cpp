#include "UpdaterPolicy.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <set>

namespace wk::windows_updater
{
namespace
{
std::optional<std::uint32_t> parsePart(std::string_view part)
{
    if (part.empty() || (part.size() > 1 && part.front() == '0'))
        return std::nullopt;

    std::uint32_t value{};
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
    if (error != std::errc{} || end != part.data() + part.size())
        return std::nullopt;
    return value;
}

bool isAsciiAlphaNumeric(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9');
}

bool isUpperHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
}

bool isLowerHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool equalAsciiInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), [] (unsigned char a, unsigned char b)
    {
        const auto upper = [] (unsigned char value)
        {
            return static_cast<unsigned char>(value >= 'a' && value <= 'z'
                                                   ? value - ('a' - 'A') : value);
        };
        return upper(a) == upper(b);
    });
}

std::optional<std::uint8_t> hexNibble(char value)
{
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
    return std::nullopt;
}
}

std::optional<SemVersion> parseVersion(std::string_view text)
{
    const auto first = text.find('.');
    const auto second = first == std::string_view::npos
                      ? std::string_view::npos : text.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos
        || text.find('.', second + 1) != std::string_view::npos)
        return std::nullopt;

    const auto major = parsePart(text.substr(0, first));
    const auto minor = parsePart(text.substr(first + 1, second - first - 1));
    const auto patch = parsePart(text.substr(second + 1));
    if (! major || ! minor || ! patch)
        return std::nullopt;
    // Windows Installer's ProductVersion fields are 8, 8 and 16 bits. Reject a
    // tag here, before selecting/downloading an asset, if no matching MSI can exist.
    if (*major > 255 || *minor > 255 || *patch > 65535)
        return std::nullopt;
    return SemVersion { *major, *minor, *patch };
}

std::optional<SemVersion> parseStableTag(std::string_view text)
{
    if (text.size() < 2 || text.front() != 'v')
        return std::nullopt;
    return parseVersion(text.substr(1));
}

bool isStrictlyNewer(const SemVersion& candidate, const SemVersion& installed)
{
    if (candidate.major != installed.major) return candidate.major > installed.major;
    if (candidate.minor != installed.minor) return candidate.minor > installed.minor;
    return candidate.patch > installed.patch;
}

std::string toString(const SemVersion& version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor)
         + "." + std::to_string(version.patch);
}

bool isSafeRepositoryComponent(std::string_view value)
{
    if (value.empty() || value.size() > 100 || value.front() == '.' || value.back() == '.')
        return false;
    return std::all_of(value.begin(), value.end(), [] (char c)
    {
        return isAsciiAlphaNumeric(c) || c == '-' || c == '_' || c == '.';
    });
}

bool isCanonicalGuid(std::string_view value)
{
    if (value.size() != 36)
        return false;
    constexpr std::array<std::size_t, 4> dashes { 8, 13, 18, 23 };
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (std::find(dashes.begin(), dashes.end(), index) != dashes.end())
        {
            if (value[index] != '-') return false;
            continue;
        }
        const auto c = value[index];
        if (! ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool isSha256Hex(std::string_view value)
{
    if (value.size() != 64)
        return false;
    return std::all_of(value.begin(), value.end(), [] (char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool isGitCommitHex(std::string_view value)
{
    if (value.size() != 40)
        return false;
    return std::all_of(value.begin(), value.end(), [] (char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool isP256PublicKeyXYHex(std::string_view value)
{
    return value.size() == 128
        && std::all_of(value.begin(), value.end(), isUpperHex);
}

std::optional<std::array<std::uint8_t, 64>> decodeP256P1363SignatureHex(
    std::string_view value)
{
    constexpr std::string_view order =
        "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551";
    constexpr std::string_view halfOrder =
        "7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A8";
    constexpr std::string_view zero =
        "0000000000000000000000000000000000000000000000000000000000000000";
    if (value.size() != 128
        || ! std::all_of(value.begin(), value.end(), isUpperHex))
        return std::nullopt;
    const auto r = value.substr(0, 64);
    const auto s = value.substr(64, 64);
    // Fixed-width uppercase hex makes lexical and numeric order identical.
    // Low-S removes ECDSA's otherwise equivalent (r, n-s) representation.
    if (r == zero || r >= order || s == zero || s > halfOrder)
        return std::nullopt;
    std::array<std::uint8_t, 64> result{};
    for (std::size_t index{}; index < result.size(); ++index)
    {
        const auto high = hexNibble(value[index * 2]);
        const auto low = hexNibble(value[index * 2 + 1]);
        if (! high || ! low)
            return std::nullopt;
        result[index] = static_cast<std::uint8_t>((*high << 4) | *low);
    }
    return result;
}

std::string encodeP256P1363SignatureHex(const std::array<std::uint8_t, 64>& value)
{
    constexpr char alphabet[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 2);
    for (const auto byte : value)
    {
        result.push_back(alphabet[byte >> 4]);
        result.push_back(alphabet[byte & 0x0f]);
    }
    return result;
}

std::optional<std::uint64_t> parseCanonicalPositiveUint64(std::string_view value)
{
    if (value.empty() || value.front() == '0')
        return std::nullopt;
    std::uint64_t parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0)
        return std::nullopt;
    return parsed;
}

std::optional<std::string> digestHex(std::string_view githubDigest)
{
    constexpr std::string_view prefix = "sha256:";
    if (! githubDigest.starts_with(prefix))
        return std::nullopt;
    auto value = githubDigest.substr(prefix.size());
    if (! isSha256Hex(value))
        return std::nullopt;
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [] (unsigned char c)
    {
        return static_cast<char>(c >= 'a' && c <= 'f' ? c - ('a' - 'A') : c);
    });
    return result;
}

bool isAllowedHttpsHost(std::string_view host)
{
    constexpr std::array allowed {
        std::string_view { "api.github.com" },
        std::string_view { "github.com" },
        std::string_view { "objects.githubusercontent.com" },
        std::string_view { "release-assets.githubusercontent.com" },
        std::string_view { "github-releases.githubusercontent.com" }
    };
    return std::find(allowed.begin(), allowed.end(), host) != allowed.end();
}

bool isCanonicalReleaseGatePipeName(std::string_view value)
{
    constexpr std::string_view prefix = "WhykikiAudio.UpdaterReleaseGate.";
    return value.size() == prefix.size() + 32 && value.starts_with(prefix)
        && std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       value.end(), isUpperHex);
}

bool isReleaseGateExpiryValid(std::uint64_t expiresAtUnixSeconds,
                              std::uint64_t nowUnixSeconds)
{
    return nowUnixSeconds != 0 && expiresAtUnixSeconds > nowUnixSeconds
        && expiresAtUnixSeconds - nowUnixSeconds <= releaseGateMaximumTtlSeconds;
}

std::optional<std::string> canonicalReleaseGateAuthorization(
    const ReleaseGateAuthorizationFields& fields)
{
    const auto target = parseStableTag(fields.tag);
    if (! isSafeRepositoryComponent(fields.owner)
        || ! isSafeRepositoryComponent(fields.repository)
        || ! isSafeRepositoryComponent(fields.product)
        || fields.releaseId == 0 || fields.parentProcessId == 0
        || fields.parentProcessCreatedAtFiletime == 0 || fields.expiresAtUnixSeconds == 0
        || ! target || fields.tag != "v" + toString(*target)
        || ! isStrictlyNewer(*target, fields.installedVersion)
        || fields.sourceCommit.size() != 40
        || ! std::all_of(fields.sourceCommit.begin(), fields.sourceCommit.end(), isLowerHex)
        || fields.challenge.size() != 64
        || ! std::all_of(fields.challenge.begin(), fields.challenge.end(), isUpperHex)
        || ! isCanonicalReleaseGatePipeName(fields.responsePipe))
        return std::nullopt;

    // Every value above is a tightly bounded ASCII token that cannot contain
    // '=' or LF.  The fixed field order, canonical decimal/version forms and
    // final LF therefore define one unambiguous byte string for every request.
    return std::string("domain=whykiki.windows-updater-release-gate-authorization\n")
        + "schemaVersion=1\n"
        + "repository=" + fields.owner + "/" + fields.repository + "\n"
        + "product=" + fields.product + "\n"
        + "architecture=" + architectureAssetSuffix(fields.architecture) + "\n"
        + "installedVersion=" + toString(fields.installedVersion) + "\n"
        + "releaseId=" + std::to_string(fields.releaseId) + "\n"
        + "tag=" + fields.tag + "\n"
        + "sourceCommit=" + fields.sourceCommit + "\n"
        + "challenge=" + fields.challenge + "\n"
        + "responsePipe=" + fields.responsePipe + "\n"
        + "parentProcessId=" + std::to_string(fields.parentProcessId) + "\n"
        + "parentProcessCreatedAtFiletime="
        + std::to_string(fields.parentProcessCreatedAtFiletime) + "\n"
        + "expiresAtUnixSeconds=" + std::to_string(fields.expiresAtUnixSeconds) + "\n";
}

std::string architectureAssetSuffix(Architecture architecture)
{
    return architecture == Architecture::x64 ? "x64" : "arm64ec";
}

std::string architectureBundleDirectory(Architecture architecture)
{
    return architecture == Architecture::x64 ? "x86_64-win" : "arm64ec-win";
}

std::string expectedAssetName(std::string_view product,
                              const SemVersion& version,
                              Architecture architecture)
{
    return std::string(product) + "-" + toString(version) + "-Windows-"
         + architectureAssetSuffix(architecture) + ".msi";
}

std::string expectedAssetUrl(std::string_view owner,
                             std::string_view repository,
                             std::string_view product,
                             const SemVersion& version,
                             Architecture architecture)
{
    const auto tag = "v" + toString(version);
    return "https://github.com/" + std::string(owner) + "/" + std::string(repository)
         + "/releases/download/" + tag + "/"
         + expectedAssetName(product, version, architecture);
}

std::string releasesApiUrl(std::string_view owner, std::string_view repository)
{
    return "https://api.github.com/repos/" + std::string(owner) + "/"
         + std::string(repository) + "/releases?per_page=100";
}

std::string releaseByIdApiUrl(std::string_view owner,
                              std::string_view repository,
                              std::uint64_t releaseId)
{
    return "https://api.github.com/repos/" + std::string(owner) + "/"
         + std::string(repository) + "/releases/" + std::to_string(releaseId);
}

bool isForbiddenMsiSideEffectTable(std::string_view table)
{
    constexpr std::array forbidden {
        std::string_view { "CustomAction" }, std::string_view { "Binary" },
        std::string_view { "MsiEmbeddedUI" }, std::string_view { "MsiEmbeddedChainer" },
        std::string_view { "AppSearch" }, std::string_view { "CompLocator" },
        std::string_view { "RegLocator" }, std::string_view { "IniLocator" },
        std::string_view { "DrLocator" }, std::string_view { "Signature" },
        std::string_view { "Control" }, std::string_view { "ControlEvent" },
        std::string_view { "ServiceInstall" }, std::string_view { "ServiceControl" },
        std::string_view { "MsiServiceConfig" },
        std::string_view { "MsiServiceConfigFailureActions" },
        std::string_view { "Registry" }, std::string_view { "RemoveRegistry" },
        std::string_view { "SelfReg" }, std::string_view { "TypeLib" },
        std::string_view { "Class" }, std::string_view { "ProgId" },
        std::string_view { "Extension" }, std::string_view { "MIME" },
        std::string_view { "AppId" }, std::string_view { "ODBCDataSource" },
        std::string_view { "ODBCDriver" }, std::string_view { "ODBCTranslator" },
        std::string_view { "IniFile" }, std::string_view { "RemoveIniFile" },
        std::string_view { "Environment" }, std::string_view { "RemoveFile" },
        // MoveFile is the table.  MoveFiles is a standard action and is a no-op
        // when this table is empty, so banning that action would reject harmless
        // standard sequencing without strengthening this table-level contract.
        std::string_view { "MoveFile" }, std::string_view { "DuplicateFile" },
        std::string_view { "CreateFolder" }, std::string_view { "Shortcut" },
        std::string_view { "ReserveCost" }, std::string_view { "BindImage" },
        std::string_view { "Font" }, std::string_view { "IsolatedComponent" },
        std::string_view { "MsiAssembly" }, std::string_view { "MsiAssemblyName" },
        std::string_view { "PublishComponent" }, std::string_view { "Complus" },
        std::string_view { "Verb" }, std::string_view { "ODBCAttribute" },
        std::string_view { "LockPermissions" }, std::string_view { "MsiLockPermissionsEx" },
        std::string_view { "Permission" }, std::string_view { "PermissionEx" },
        std::string_view { "Patch" }, std::string_view { "PatchPackage" },
        std::string_view { "MsiPatchCertificate" },
        std::string_view { "SFPCatalog" }
    };
    return std::find(forbidden.begin(), forbidden.end(), table) != forbidden.end();
}

bool isForbiddenMsiSequenceAction(std::string_view action)
{
    constexpr std::array forbidden {
        std::string_view { "ForceReboot" }, std::string_view { "ScheduleReboot" },
        std::string_view { "DisableRollback" }
    };
    return std::find(forbidden.begin(), forbidden.end(), action) != forbidden.end();
}

bool isForbiddenMsiProperty(std::string_view property)
{
    constexpr std::array forbidden {
        std::string_view { "DISABLEROLLBACK" }, std::string_view { "TARGETDIR" },
        std::string_view { "ROOTDRIVE" }, std::string_view { "TRANSFORMS" },
        std::string_view { "TRANSFORMSATSOURCE" }, std::string_view { "TRANSFORMSSECURE" }
    };
    return std::any_of(forbidden.begin(), forbidden.end(), [property] (const auto candidate)
    {
        return equalAsciiInsensitive(property, candidate);
    });
}

bool hasMsiDirectoryPropertyOverride(
    const std::map<std::string, std::string>& properties,
    const std::map<std::string, std::string>& directoryParents)
{
    for (const auto& [property, ignoredValue] : properties)
    {
        (void) ignoredValue;
        for (const auto& [directory, ignoredParent] : directoryParents)
        {
            (void) ignoredParent;
            if (equalAsciiInsensitive(property, directory)) return true;
        }
    }
    return false;
}

bool isPredefinedMsiPathProperty(std::string_view property)
{
    // Windows Installer initializes these properties to absolute directories or
    // paths independently of the authored Directory_Parent graph.  A payload
    // Directory row must not reuse one of these identifiers below INSTALLFOLDER.
    constexpr std::array predefined {
        std::string_view { "OriginalDatabase" }, std::string_view { "ParentOriginalDatabase" },
        std::string_view { "SourceDir" }, std::string_view { "TARGETDIR" },
        std::string_view { "ROOTDRIVE" }, std::string_view { "CCP_DRIVE" },
        std::string_view { "PrimaryVolumePath" }, std::string_view { "MsiLogFileLocation" },
        std::string_view { "AdminToolsFolder" }, std::string_view { "AppDataFolder" },
        std::string_view { "CommonAppDataFolder" }, std::string_view { "CommonFilesFolder" },
        std::string_view { "CommonFiles64Folder" }, std::string_view { "CommonFiles6432Folder" },
        std::string_view { "DesktopFolder" }, std::string_view { "FavoritesFolder" },
        std::string_view { "FontsFolder" }, std::string_view { "LocalAppDataFolder" },
        std::string_view { "MyPicturesFolder" }, std::string_view { "NetHoodFolder" },
        std::string_view { "PersonalFolder" }, std::string_view { "PrintHoodFolder" },
        std::string_view { "ProgramFilesFolder" }, std::string_view { "ProgramFiles64Folder" },
        std::string_view { "ProgramFiles6432Folder" }, std::string_view { "PerUserProgramFilesFolder" },
        std::string_view { "ProgramMenuFolder" },
        std::string_view { "RecentFolder" }, std::string_view { "SendToFolder" },
        std::string_view { "StartMenuFolder" }, std::string_view { "StartupFolder" },
        std::string_view { "System16Folder" }, std::string_view { "SystemFolder" },
        std::string_view { "System64Folder" }, std::string_view { "System6432Folder" },
        std::string_view { "TempFolder" }, std::string_view { "TemplateFolder" },
        std::string_view { "WindowsFolder" }, std::string_view { "WindowsVolume" }
    };
    return std::any_of(predefined.begin(), predefined.end(), [property] (const auto candidate)
    {
        return equalAsciiInsensitive(property, candidate);
    });
}

bool msiDirectoryIdentifiersAreSafe(
    const std::map<std::string, std::string>& directoryParents,
    std::string_view expectedSystemFolderAnchor)
{
    for (const auto& [identifier, ignoredParent] : directoryParents)
    {
        (void) ignoredParent;
        if (identifier != "TARGETDIR" && identifier != expectedSystemFolderAnchor
            && isPredefinedMsiPathProperty(identifier))
            return false;
    }
    return true;
}

bool hasExactUpgradeContract(const std::vector<MsiUpgradeRow>& rows,
                             std::string_view currentUpgradeCode,
                             std::string_view otherUpgradeCode,
                             const SemVersion& targetVersion)
{
    if (rows.size() != 3 || currentUpgradeCode == otherUpgradeCode)
        return false;
    constexpr int migrateFeatures = 0x001;
    constexpr int onlyDetect = 0x002;
    constexpr int minimumInclusive = 0x100;
    const auto version = toString(targetVersion);
    std::set<std::string> actions;
    std::size_t currentRows{};
    std::size_t otherRows{};
    for (const auto& row : rows)
    {
        // WiX 6.0.2 emits a NULL Remove column for all three rows generated by
        // this package.  Accepting "ALL" here would silently widen an otherwise
        // exact contract and permit a differently authored upgrade policy.
        if (! row.remove.empty()) return false;
        if (! actions.insert(row.actionProperty).second) return false;
        if (row.upgradeCode == otherUpgradeCode)
        {
            ++otherRows;
            if (row.versionMin != "0.0.0" || ! row.versionMax.empty() || ! row.language.empty()
                || row.attributes != (onlyDetect | minimumInclusive)
                || row.actionProperty != "OTHERARCHITECTUREDETECTED")
                return false;
            continue;
        }
        if (row.upgradeCode != currentUpgradeCode) return false;
        ++currentRows;
        if (row.actionProperty == "WIX_UPGRADE_DETECTED")
        {
            if (! row.versionMin.empty() || row.versionMax != version || row.language != "1033"
                || row.attributes != migrateFeatures)
                return false;
        }
        else if (row.actionProperty == "WIX_DOWNGRADE_DETECTED")
        {
            if (row.versionMin != version || ! row.versionMax.empty() || row.language != "1033"
                || row.attributes != onlyDetect)
                return false;
        }
        else
        {
            return false;
        }
    }
    return currentRows == 2 && otherRows == 1
        && actions == std::set<std::string> {
            "OTHERARCHITECTUREDETECTED", "WIX_DOWNGRADE_DETECTED", "WIX_UPGRADE_DETECTED" };
}

bool hasExactLaunchConditions(const std::vector<std::string>& rawConditions)
{
    std::set<std::string> conditions;
    for (auto condition : rawConditions)
    {
        std::transform(condition.begin(), condition.end(), condition.begin(), [] (unsigned char c)
        {
            return static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
        });
        condition.erase(std::remove_if(condition.begin(), condition.end(), [] (unsigned char c)
        {
            return c == ' ' || c == '\t' || c == '(' || c == ')';
        }), condition.end());
        if (! conditions.insert(std::move(condition)).second) return false;
    }
    return conditions == std::set<std::string> {
        "INSTALLEDORNOTOTHERARCHITECTUREDETECTED",
        "NOTWIX_DOWNGRADE_DETECTED" };
}

bool componentDirectoriesAreInsideInstallFolder(
    const std::map<std::string, std::string>& directoryParents,
    const std::vector<std::string>& componentDirectories)
{
    const auto root = directoryParents.find("TARGETDIR");
    if (componentDirectories.empty() || root == directoryParents.end() || ! root->second.empty())
        return false;
    // Validate the complete graph, not only the paths used by current
    // components.  A disconnected cyclic or dangling Directory subtree is not
    // part of the narrowly authored package and therefore fails closed.
    for (const auto& [identifier, ignoredParent] : directoryParents)
    {
        (void) ignoredParent;
        auto directory = identifier;
        std::set<std::string> visited;
        while (directory != "TARGETDIR")
        {
            if (directory.empty()) return false;
            if (! visited.insert(directory).second) return false;
            const auto parent = directoryParents.find(directory);
            if (parent == directoryParents.end()) return false;
            directory = parent->second;
        }
    }
    for (auto directory : componentDirectories)
    {
        std::set<std::string> visited;
        while (directory != "INSTALLFOLDER")
        {
            const auto parent = directoryParents.find(directory);
            if (! visited.insert(directory).second || parent == directoryParents.end()) return false;
            directory = parent->second;
        }
    }
    return true;
}
}
