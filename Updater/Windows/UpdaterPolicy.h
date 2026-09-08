#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wk::windows_updater
{
enum class Architecture
{
    x64,
    arm64ec
};

struct SemVersion
{
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};

    friend constexpr bool operator== (const SemVersion&, const SemVersion&) = default;
};

std::optional<SemVersion> parseVersion(std::string_view text);
std::optional<SemVersion> parseStableTag(std::string_view text);
bool isStrictlyNewer(const SemVersion& candidate, const SemVersion& installed);
std::string toString(const SemVersion& version);

bool isSafeRepositoryComponent(std::string_view value);
bool isCanonicalGuid(std::string_view value);
bool isSha256Hex(std::string_view value);
bool isGitCommitHex(std::string_view value);
bool isP256PublicKeyXYHex(std::string_view value);
std::optional<std::array<std::uint8_t, 64>> decodeP256P1363SignatureHex(
    std::string_view value);
std::string encodeP256P1363SignatureHex(const std::array<std::uint8_t, 64>& value);
std::optional<std::uint64_t> parseCanonicalPositiveUint64(std::string_view value);
std::optional<std::string> digestHex(std::string_view githubDigest);
bool isAllowedHttpsHost(std::string_view lowerCaseHost);

inline constexpr std::uint64_t releaseGateMaximumTtlSeconds = 5u * 60u;

struct ReleaseGateAuthorizationFields
{
    std::string owner;
    std::string repository;
    std::string product;
    Architecture architecture{};
    SemVersion installedVersion;
    std::uint64_t releaseId{};
    std::string tag;
    std::string sourceCommit;
    std::string challenge;
    std::string responsePipe;
    std::uint32_t parentProcessId{};
    std::uint64_t parentProcessCreatedAtFiletime{};
    std::uint64_t expiresAtUnixSeconds{};
};

bool isCanonicalReleaseGatePipeName(std::string_view value);
bool isReleaseGateExpiryValid(std::uint64_t expiresAtUnixSeconds,
                              std::uint64_t nowUnixSeconds);
std::optional<std::string> canonicalReleaseGateAuthorization(
    const ReleaseGateAuthorizationFields& fields);

std::string architectureAssetSuffix(Architecture architecture);
std::string architectureBundleDirectory(Architecture architecture);
std::string expectedAssetName(std::string_view product,
                              const SemVersion& version,
                              Architecture architecture);
std::string expectedAssetUrl(std::string_view owner,
                             std::string_view repository,
                             std::string_view product,
                             const SemVersion& version,
                             Architecture architecture);
std::string releasesApiUrl(std::string_view owner, std::string_view repository);
std::string releaseByIdApiUrl(std::string_view owner,
                              std::string_view repository,
                              std::uint64_t releaseId);

struct MsiUpgradeRow
{
    std::string upgradeCode;
    std::string versionMin;
    std::string versionMax;
    std::string language;
    int attributes{};
    std::string remove;
    std::string actionProperty;
};

bool isForbiddenMsiSideEffectTable(std::string_view table);
bool hasExactUpgradeContract(const std::vector<MsiUpgradeRow>& rows,
                             std::string_view currentUpgradeCode,
                             std::string_view otherUpgradeCode,
                             const SemVersion& targetVersion);
bool hasExactLaunchConditions(const std::vector<std::string>& conditions);
bool componentDirectoriesAreInsideInstallFolder(
    const std::map<std::string, std::string>& directoryParents,
    const std::vector<std::string>& componentDirectories);
}
