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
         + std::string(repository) + "/releases/latest";
}

bool isForbiddenMsiSideEffectTable(std::string_view table)
{
    constexpr std::array forbidden {
        std::string_view { "CustomAction" }, std::string_view { "Binary" },
        std::string_view { "ServiceInstall" }, std::string_view { "ServiceControl" },
        std::string_view { "Registry" }, std::string_view { "RemoveRegistry" },
        std::string_view { "SelfReg" }, std::string_view { "TypeLib" },
        std::string_view { "Class" }, std::string_view { "ProgId" },
        std::string_view { "Extension" }, std::string_view { "MIME" },
        std::string_view { "AppId" }, std::string_view { "ODBCDataSource" },
        std::string_view { "ODBCDriver" }, std::string_view { "ODBCTranslator" },
        std::string_view { "IniFile" }, std::string_view { "RemoveIniFile" },
        std::string_view { "Environment" }, std::string_view { "RemoveFile" },
        std::string_view { "MoveFiles" }, std::string_view { "DuplicateFile" },
        std::string_view { "CreateFolder" }, std::string_view { "Shortcut" },
        std::string_view { "ReserveCost" }, std::string_view { "BindImage" },
        std::string_view { "Font" }, std::string_view { "IsolatedComponent" },
        std::string_view { "MsiAssembly" }, std::string_view { "MsiAssemblyName" },
        std::string_view { "PublishComponent" }, std::string_view { "Complus" },
        std::string_view { "Verb" }, std::string_view { "ODBCAttribute" },
        std::string_view { "LockPermissions" }, std::string_view { "MsiLockPermissionsEx" },
        std::string_view { "Patch" }, std::string_view { "PatchPackage" },
        std::string_view { "SFPCatalog" }
    };
    return std::find(forbidden.begin(), forbidden.end(), table) != forbidden.end();
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
        "INSTALLEDORNOTWIX_DOWNGRADE_DETECTED" };
}

bool componentDirectoriesAreInsideInstallFolder(
    const std::map<std::string, std::string>& directoryParents,
    const std::vector<std::string>& componentDirectories)
{
    if (componentDirectories.empty()) return false;
    // Validate the complete graph, not only the paths used by current
    // components.  A disconnected cyclic or dangling Directory subtree is not
    // part of the narrowly authored package and therefore fails closed.
    for (const auto& [identifier, ignoredParent] : directoryParents)
    {
        (void) ignoredParent;
        auto directory = identifier;
        std::set<std::string> visited;
        while (! directory.empty())
        {
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
