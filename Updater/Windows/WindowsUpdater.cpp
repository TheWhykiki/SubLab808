#define NOMINMAX 1
#include "WindowsUpdater.h"
#include "UpdaterPolicy.h"

#include <juce_core/juce_core.h>

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <msi.h>
#include <msiquery.h>
#include <propidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "msi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")

#ifndef WK_WINDOWS_UPDATER_PRODUCT
#error "WK_WINDOWS_UPDATER_PRODUCT must identify the exact product"
#endif
#ifndef WK_WINDOWS_UPDATER_VERSION
#error "WK_WINDOWS_UPDATER_VERSION must be MAJOR.MINOR.PATCH"
#endif
#ifndef WK_WINDOWS_UPDATER_MANUFACTURER
#error "WK_WINDOWS_UPDATER_MANUFACTURER must identify the MSI publisher"
#endif
#ifndef WK_WINDOWS_UPDATER_GITHUB_OWNER
#error "WK_WINDOWS_UPDATER_GITHUB_OWNER must identify the exact GitHub owner"
#endif
#ifndef WK_WINDOWS_UPDATER_GITHUB_REPOSITORY
#error "WK_WINDOWS_UPDATER_GITHUB_REPOSITORY must identify the exact GitHub repository"
#endif
#ifndef WK_WINDOWS_UPDATER_UPGRADE_CODE
#error "WK_WINDOWS_UPDATER_UPGRADE_CODE must be the target architecture's fixed UpgradeCode"
#endif
#ifndef WK_WINDOWS_UPDATER_OTHER_UPGRADE_CODE
#error "WK_WINDOWS_UPDATER_OTHER_UPGRADE_CODE must be the other architecture's fixed UpgradeCode"
#endif
#ifndef WK_WINDOWS_UPDATER_TEST_MODE
#define WK_WINDOWS_UPDATER_TEST_MODE 0
#endif
#ifndef WK_WINDOWS_UPDATER_COMPILE_ONLY
#define WK_WINDOWS_UPDATER_COMPILE_ONLY 0
#endif

#if ! WK_WINDOWS_UPDATER_TEST_MODE && ! defined(WK_WINDOWS_UPDATER_SIGNER_SHA256)
#error "Production updater builds require WK_WINDOWS_UPDATER_SIGNER_SHA256"
#endif
#ifndef WK_WINDOWS_UPDATER_SIGNER_SHA256
#define WK_WINDOWS_UPDATER_SIGNER_SHA256 ""
#endif
#ifndef WK_WINDOWS_UPDATER_NEXT_SIGNER_SHA256
#define WK_WINDOWS_UPDATER_NEXT_SIGNER_SHA256 ""
#endif
#ifndef WK_WINDOWS_UPDATER_RELEASE_GATE_PUBLIC_KEY_XY
#error "Updater builds require WK_WINDOWS_UPDATER_RELEASE_GATE_PUBLIC_KEY_XY"
#endif
#ifndef WK_WINDOWS_UPDATER_RELEASE_GATE_NEXT_PUBLIC_KEY_XY
#define WK_WINDOWS_UPDATER_RELEASE_GATE_NEXT_PUBLIC_KEY_XY ""
#endif

#if defined(_M_ARM64EC)
#define WK_WINDOWS_UPDATER_ARCHITECTURE_ARM64EC 1
#elif defined(_M_X64)
#define WK_WINDOWS_UPDATER_ARCHITECTURE_X64 1
#else
#error "The updater must be built natively for x64 or ARM64EC"
#endif

namespace wk::windows_updater
{
namespace
{
using Path = std::filesystem::path;
constexpr bool kTestMode = WK_WINDOWS_UPDATER_TEST_MODE != 0;
constexpr bool kCompileOnly = WK_WINDOWS_UPDATER_COMPILE_ONLY != 0;
constexpr std::uint64_t kMaximumMetadataBytes = 1024u * 1024u;
constexpr std::uint64_t kMaximumMsiBytes = 256u * 1024u * 1024u;
constexpr std::uint64_t kMaximumManifestBytes = 1024u * 1024u;
constexpr DWORD kMaximumRedirects = 5;
constexpr wchar_t kGithubApiHeaders[] = L"Accept: application/vnd.github+json\r\n"
                                         L"X-GitHub-Api-Version: 2026-03-10\r\n";
// SummaryInformation property identifiers from [MS-OLEPS] section 2.18.2.
// Keep these local: the Windows SDK does not expose the PIDSI_* aliases in
// every supported header configuration.
constexpr UINT kMsiSummaryTemplate = 7u;
constexpr UINT kMsiSummaryRevisionNumber = 9u;

#if defined(WK_WINDOWS_UPDATER_ARCHITECTURE_ARM64EC)
constexpr Architecture kArchitecture = Architecture::arm64ec;
#else
constexpr Architecture kArchitecture = Architecture::x64;
#endif

constexpr std::string_view kProduct = WK_WINDOWS_UPDATER_PRODUCT;
constexpr std::string_view kInstalledVersion = WK_WINDOWS_UPDATER_VERSION;
constexpr std::string_view kManufacturer = WK_WINDOWS_UPDATER_MANUFACTURER;
constexpr std::string_view kOwner = WK_WINDOWS_UPDATER_GITHUB_OWNER;
constexpr std::string_view kRepository = WK_WINDOWS_UPDATER_GITHUB_REPOSITORY;
constexpr std::string_view kUpgradeCode = WK_WINDOWS_UPDATER_UPGRADE_CODE;
constexpr std::string_view kOtherUpgradeCode = WK_WINDOWS_UPDATER_OTHER_UPGRADE_CODE;
constexpr std::string_view kCurrentSignerSha256 = WK_WINDOWS_UPDATER_SIGNER_SHA256;
constexpr std::string_view kNextSignerSha256 = WK_WINDOWS_UPDATER_NEXT_SIGNER_SHA256;
constexpr std::string_view kReleaseGatePublicKeyXY =
    WK_WINDOWS_UPDATER_RELEASE_GATE_PUBLIC_KEY_XY;
constexpr std::string_view kReleaseGateNextPublicKeyXY =
    WK_WINDOWS_UPDATER_RELEASE_GATE_NEXT_PUBLIC_KEY_XY;

constexpr bool isHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr bool isUpperHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

constexpr bool pinsEqualInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto upper = [] (char c) constexpr
        {
            return c >= 'a' && c <= 'f' ? static_cast<char>(c - ('a' - 'A')) : c;
        };
        if (upper(left[index]) != upper(right[index])) return false;
    }
    return true;
}

constexpr bool compileTimePinsAreValid()
{
    if ((! kTestMode || ! kCurrentSignerSha256.empty())
        && (kCurrentSignerSha256.size() != 64
            || ! std::all_of(kCurrentSignerSha256.begin(), kCurrentSignerSha256.end(), isHex)))
        return false;
    if (! kNextSignerSha256.empty()
        && (kNextSignerSha256.size() != 64
            || ! std::all_of(kNextSignerSha256.begin(), kNextSignerSha256.end(), isHex)))
        return false;
    if (! kNextSignerSha256.empty()
        && pinsEqualInsensitive(kCurrentSignerSha256, kNextSignerSha256))
        return false;
    if (kReleaseGatePublicKeyXY.size() != 128
        || ! std::all_of(kReleaseGatePublicKeyXY.begin(),
                         kReleaseGatePublicKeyXY.end(), isUpperHex))
        return false;
    if (! kReleaseGateNextPublicKeyXY.empty()
        && (kReleaseGateNextPublicKeyXY.size() != 128
            || ! std::all_of(kReleaseGateNextPublicKeyXY.begin(),
                             kReleaseGateNextPublicKeyXY.end(), isUpperHex)))
        return false;
    return kReleaseGateNextPublicKeyXY.empty()
        || kReleaseGatePublicKeyXY != kReleaseGateNextPublicKeyXY;
}

static_assert(compileTimePinsAreValid(),
              "Updater signer pins and release-gate P-256 public keys are malformed or duplicated");

class Failure final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& message)
{
    throw Failure(message);
}

void require(bool condition, const std::string& message)
{
    if (! condition) fail(message);
}

std::string winError(const char* context, DWORD code = GetLastError())
{
    wchar_t* buffer = nullptr;
    const auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                          | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0,
                                      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = count != 0 && buffer != nullptr ? std::wstring(buffer, count) : L"unknown error";
    if (buffer != nullptr) LocalFree(buffer);
    while (! text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();

    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 0 ? static_cast<std::size_t>(bytes) : 0, '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            utf8.data(), bytes, nullptr, nullptr);
    return std::string(context) + " (" + std::to_string(code) + "): " + utf8;
}

std::wstring widen(std::string_view text)
{
    if (text.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    require(count > 0, winError("Invalid UTF-8"));
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), result.data(), count) == count,
            winError("UTF-8 conversion failed"));
    return result;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(count > 0, winError("Invalid UTF-16"));
    std::string result(static_cast<std::size_t>(count), '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), result.data(), count, nullptr, nullptr) == count,
            winError("UTF-16 conversion failed"));
    return result;
}

std::string upperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char c)
    {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
    });
    return value;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char c)
    {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return value;
}

std::wstring lowerAscii(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [] (wchar_t c)
    {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    });
    return value;
}

bool equalInsensitive(std::wstring_view left, std::wstring_view right)
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

struct OrdinalInsensitiveLess
{
    bool operator() (const std::wstring& left, const std::wstring& right) const
    {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                    right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) { reset(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    bool valid() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value; }
    HANDLE release() { return std::exchange(value, INVALID_HANDLE_VALUE); }
    void reset(HANDLE replacement = INVALID_HANDLE_VALUE)
    {
        if (valid()) CloseHandle(value);
        value = replacement;
    }
private:
    HANDLE value = INVALID_HANDLE_VALUE;
};

class InternetHandle
{
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value != nullptr) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return value; }
    bool valid() const { return value != nullptr; }
private:
    HINTERNET value{};
};

class FindHandle
{
public:
    explicit FindHandle(HANDLE handle) : value(handle) {}
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    HANDLE get() const { return value; }
private:
    HANDLE value = INVALID_HANDLE_VALUE;
};

class MsiHandle
{
public:
    MsiHandle() = default;
    explicit MsiHandle(MSIHANDLE handle) : value(handle) {}
    ~MsiHandle() { if (value != 0) MsiCloseHandle(value); }
    MsiHandle(const MsiHandle&) = delete;
    MsiHandle& operator=(const MsiHandle&) = delete;
    MSIHANDLE get() const { return value; }
private:
    MSIHANDLE value{};
};

class CertificateStore
{
public:
    explicit CertificateStore(HCERTSTORE handle) : value(handle) {}
    ~CertificateStore() { if (value != nullptr) CertCloseStore(value, 0); }
    CertificateStore(const CertificateStore&) = delete;
    CertificateStore& operator=(const CertificateStore&) = delete;
    HCERTSTORE get() const { return value; }
private:
    HCERTSTORE value{};
};

class CryptMessage
{
public:
    explicit CryptMessage(HCRYPTMSG handle) : value(handle) {}
    ~CryptMessage() { if (value != nullptr) CryptMsgClose(value); }
    CryptMessage(const CryptMessage&) = delete;
    CryptMessage& operator=(const CryptMessage&) = delete;
    HCRYPTMSG get() const { return value; }
private:
    HCRYPTMSG value{};
};

class CertificateContext
{
public:
    explicit CertificateContext(PCCERT_CONTEXT context) : value(context) {}
    ~CertificateContext() { if (value != nullptr) CertFreeCertificateContext(value); }
    CertificateContext(const CertificateContext&) = delete;
    CertificateContext& operator=(const CertificateContext&) = delete;
    PCCERT_CONTEXT get() const { return value; }
private:
    PCCERT_CONTEXT value{};
};

class Sha256
{
public:
    Sha256()
    {
        require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                "BCryptOpenAlgorithmProvider failed");
        DWORD bytes{};
        DWORD returned{};
        require(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                  reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes), &returned, 0) >= 0,
                "BCryptGetProperty failed");
        object.resize(bytes);
        require(BCryptCreateHash(algorithm, &hash, object.data(), bytes, nullptr, 0, 0) >= 0,
                "BCryptCreateHash failed");
    }
    ~Sha256()
    {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const void* data, std::size_t bytes)
    {
        require(! finished && bytes <= std::numeric_limits<ULONG>::max(), "Invalid SHA-256 input");
        require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                               static_cast<ULONG>(bytes), 0) >= 0,
                "BCryptHashData failed");
    }
    std::array<unsigned char, 32> finishBytes()
    {
        require(! finished, "SHA-256 was already finalized");
        std::array<unsigned char, 32> digest{};
        require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
                "BCryptFinishHash failed");
        finished = true;
        return digest;
    }
    std::string finish()
    {
        const auto digest = finishBytes();
        constexpr char alphabet[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(64);
        for (const auto byte : digest)
        {
            result.push_back(alphabet[byte >> 4]);
            result.push_back(alphabet[byte & 0x0f]);
        }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<unsigned char> object;
    bool finished{};
};

class BCryptAlgorithmHandle
{
public:
    BCryptAlgorithmHandle()
    {
        require(BCryptOpenAlgorithmProvider(&value, BCRYPT_ECDSA_P256_ALGORITHM,
                                            nullptr, 0) >= 0,
                "Cannot open the Windows P-256 provider");
    }
    ~BCryptAlgorithmHandle()
    {
        if (value != nullptr) BCryptCloseAlgorithmProvider(value, 0);
    }
    BCryptAlgorithmHandle(const BCryptAlgorithmHandle&) = delete;
    BCryptAlgorithmHandle& operator=(const BCryptAlgorithmHandle&) = delete;
    BCRYPT_ALG_HANDLE get() const { return value; }
private:
    BCRYPT_ALG_HANDLE value{};
};

class BCryptKeyHandle
{
public:
    BCryptKeyHandle() = default;
    ~BCryptKeyHandle() { if (value != nullptr) BCryptDestroyKey(value); }
    BCryptKeyHandle(const BCryptKeyHandle&) = delete;
    BCryptKeyHandle& operator=(const BCryptKeyHandle&) = delete;
    BCRYPT_KEY_HANDLE get() const { return value; }
    BCRYPT_KEY_HANDLE* put()
    {
        require(value == nullptr, "CNG key handle was already initialized");
        return &value;
    }
private:
    BCRYPT_KEY_HANDLE value{};
};

template <std::size_t ByteCount>
std::array<unsigned char, ByteCount> decodeUpperHex(std::string_view value)
{
    require(value.size() == ByteCount * 2,
            "Hex value does not contain the required fixed number of bytes");
    const auto nibble = [] (char character) -> std::optional<unsigned char>
    {
        if (character >= '0' && character <= '9')
            return static_cast<unsigned char>(character - '0');
        if (character >= 'A' && character <= 'F')
            return static_cast<unsigned char>(character - 'A' + 10);
        return std::nullopt;
    };
    std::array<unsigned char, ByteCount> result{};
    for (std::size_t index{}; index < result.size(); ++index)
    {
        const auto high = nibble(value[index * 2]);
        const auto low = nibble(value[index * 2 + 1]);
        require(high && low, "P-256 value is not canonical uppercase hexadecimal");
        result[index] = static_cast<unsigned char>((*high << 4) | *low);
    }
    return result;
}

class P256PublicKey
{
public:
    explicit P256PublicKey(std::string_view xyHex)
    {
        const auto coordinates = decodeUpperHex<64>(xyHex);
        struct PublicBlob
        {
            BCRYPT_ECCKEY_BLOB header;
            std::array<unsigned char, 64> xy;
        } blob { { BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32 }, coordinates };
        static_assert(sizeof(PublicBlob) == sizeof(BCRYPT_ECCKEY_BLOB) + 64);
        require(BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                    key.put(), reinterpret_cast<PUCHAR>(&blob),
                                    static_cast<ULONG>(sizeof(blob)), 0) >= 0,
                "Configured release-gate P-256 public key is not a valid curve point");
    }

    bool verifies(const std::array<unsigned char, 32>& digest,
                  const std::array<std::uint8_t, 64>& signature) const
    {
        return BCryptVerifySignature(key.get(), nullptr,
                                     const_cast<PUCHAR>(digest.data()),
                                     static_cast<ULONG>(digest.size()),
                                     const_cast<PUCHAR>(signature.data()),
                                     static_cast<ULONG>(signature.size()), 0) >= 0;
    }
private:
    BCryptAlgorithmHandle algorithm;
    BCryptKeyHandle key;
};

bool p256PublicKeyIsOnCurve(std::string_view xyHex) noexcept
{
    try
    {
        if (! isP256PublicKeyXYHex(xyHex)) return false;
        const P256PublicKey key(xyHex);
        (void) key;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::array<unsigned char, 32> sha256Bytes(std::string_view value)
{
    Sha256 hash;
    hash.add(value.data(), value.size());
    return hash.finishBytes();
}

std::uint64_t currentUnixSeconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    require(seconds > 0, "System UTC clock is outside the supported range");
    return static_cast<std::uint64_t>(seconds);
}

std::uint64_t fileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

std::pair<std::uint64_t, std::string> hashFile(const Path& path)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(file.valid(), winError("Cannot open file for hashing"));
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.get(), &size) && size.QuadPart >= 0, winError("Cannot read file size"));
    Sha256 hash;
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;)
    {
        DWORD read{};
        require(ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr),
                winError("Cannot hash file"));
        if (read == 0) break;
        hash.add(buffer.data(), read);
    }
    return { static_cast<std::uint64_t>(size.QuadPart), hash.finish() };
}

bool constantTimeEqual(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    unsigned char difference{};
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    return difference == 0;
}

void writeAll(HANDLE file, const void* data, std::size_t bytes)
{
    const auto* current = static_cast<const unsigned char*>(data);
    while (bytes != 0)
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 1024u * 1024u));
        DWORD written{};
        require(WriteFile(file, current, chunk, &written, nullptr) && written == chunk,
                winError("Cannot write file"));
        current += written;
        bytes -= written;
    }
}

std::vector<unsigned char> readSmallFile(const Path& path, std::uint64_t maximum)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(file.valid(), winError("Cannot open file"));
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.get(), &size) && size.QuadPart >= 0
                && static_cast<std::uint64_t>(size.QuadPart) <= maximum,
            "File is empty, invalid or exceeds its policy limit");
    std::vector<unsigned char> result(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset{};
    while (offset < result.size())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(result.size() - offset,
                                                                    1024u * 1024u));
        DWORD read{};
        require(ReadFile(file.get(), result.data() + offset, chunk, &read, nullptr) && read != 0,
                winError("Cannot read file"));
        offset += read;
    }
    return result;
}

std::string fileUtf8(const Path& path, std::uint64_t maximum)
{
    const auto bytes = readSmallFile(path, maximum);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void ensureNotReparsePoint(const Path& path, const char* description)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES, winError(description));
    require((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
            std::string(description) + " is a reparse point");
}

std::wstring currentUserSid()
{
    Handle token;
    HANDLE raw{};
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), winError("OpenProcessToken failed"));
    token.reset(raw);
    DWORD bytes{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0,
            winError("Cannot size token information"));
    std::vector<unsigned char> storage(bytes);
    require(GetTokenInformation(token.get(), TokenUser, storage.data(), bytes, &bytes),
            winError("Cannot read token information"));
    wchar_t* sid{};
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid),
            winError("Cannot convert user SID"));
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

struct LocalMemory
{
    HLOCAL value{};
    ~LocalMemory() { if (value != nullptr) LocalFree(value); }
};

void createPrivateDirectory(const Path& path, bool mustBeNew)
{
    const auto sddl = L"D:P(A;;FA;;;" + currentUserSid()
                    + L")(A;;FA;;;SY)(A;;FA;;;BA)";
    LocalMemory descriptor;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr),
            winError("Cannot create private security descriptor"));
    SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE };
    if (! CreateDirectoryW(path.c_str(), &attributes))
    {
        const auto error = GetLastError();
        require(! mustBeNew && error == ERROR_ALREADY_EXISTS, winError("Cannot create private directory", error));
    }
    ensureNotReparsePoint(path, "Private directory");
    require((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0,
            "Private operation path is not a directory");
}

Handle lockDirectoryAgainstReplacement(const Path& path, const char* description)
{
    ensureNotReparsePoint(path, description);
    // FILE_READ_ATTRIBUTES alone is exempt from Windows share-mode checks and
    // therefore would not make the missing FILE_SHARE_DELETE an actual lock.
    // FILE_LIST_DIRECTORY participates in those checks for directory handles.
    Handle directory(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(directory.valid(), winError(description));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    require(GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                         &attributes, sizeof(attributes)), winError(description));
    require((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
            std::string(description) + " is not a regular directory");
    return directory;
}

Path localOperationsRoot()
{
    PWSTR raw{};
    require(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw) == S_OK,
            "LocalAppData is unavailable");
    Path current(raw);
    CoTaskMemFree(raw);
    ensureNotReparsePoint(current, "LocalAppData");
    for (const auto& part : { widen("Whykiki Audio"), widen(kProduct), std::wstring(L"Updater"),
                              std::wstring(L"Operations") })
    {
        current /= part;
        createPrivateDirectory(current, false);
    }
    return current;
}

std::string newOperationId()
{
    GUID guid{};
    require(CoCreateGuid(&guid) == S_OK, "Cannot create updater operation ID");
    wchar_t text[40]{};
    require(StringFromGUID2(guid, text, static_cast<int>(std::size(text))) > 0,
            "Cannot format updater operation ID");
    std::wstring value(text);
    require(value.size() == 38 && value.front() == L'{' && value.back() == L'}',
            "Unexpected operation ID format");
    return upperAscii(narrow(std::wstring_view(value).substr(1, 36)));
}

bool isOperationId(std::string_view value)
{
    return isCanonicalGuid(value);
}

void atomicWrite(const Path& destination, std::string_view content)
{
    require(content.size() <= 64u * 1024u, "Journal is too large");
    if (std::filesystem::exists(destination))
        ensureNotReparsePoint(destination, "Existing journal");
    const auto temporary = destination.parent_path()
        / (L"journal." + std::to_wstring(GetCurrentProcessId()) + L"."
           + std::to_wstring(GetTickCount64()) + L".tmp");
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    require(file.valid(), winError("Cannot create journal temporary file"));
    writeAll(file.get(), content.data(), content.size());
    require(FlushFileBuffers(file.get()), winError("Cannot flush journal"));
    file.reset();
    if (! MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const auto error = GetLastError();
        DeleteFileW(temporary.c_str());
        fail(winError("Cannot atomically replace journal", error));
    }
}

enum class Phase { created, noUpdate, metadata, downloaded, extracted, installed, verified };

enum class OperationMode { normal, releaseGate };

struct ReleaseGateIdentity
{
    std::uint64_t releaseId{};
    std::string tag;
    std::string sourceCommit;

    friend bool operator==(const ReleaseGateIdentity&, const ReleaseGateIdentity&) = default;
};

struct ReleaseGateInvocation
{
    ReleaseGateIdentity identity;
    std::string challenge;
    std::string responsePipe;
    DWORD parentProcessId{};
    std::uint64_t parentProcessCreatedAtFiletime{};
    std::uint64_t expiresAtUnixSeconds{};
    std::string authorizationSignatureP1363;
};

struct Invocation
{
    std::optional<std::string> resumeId;
    std::optional<ReleaseGateInvocation> releaseGate;
};

const char* phaseName(Phase phase)
{
    switch (phase)
    {
        case Phase::created: return "created";
        case Phase::noUpdate: return "no-update";
        case Phase::metadata: return "metadata";
        case Phase::downloaded: return "downloaded";
        case Phase::extracted: return "extracted";
        case Phase::installed: return "installed";
        case Phase::verified: return "verified";
    }
    return "invalid";
}

std::optional<Phase> parsePhase(std::string_view text)
{
    for (const auto phase : { Phase::created, Phase::noUpdate, Phase::metadata, Phase::downloaded,
                              Phase::extracted, Phase::installed, Phase::verified })
        if (text == phaseName(phase)) return phase;
    return std::nullopt;
}

struct Journal
{
    std::string operationId;
    std::string writerVersion;
    OperationMode mode = OperationMode::normal;
    std::optional<ReleaseGateIdentity> releaseGate;
    Phase phase = Phase::created;
    std::string targetVersion;
    std::string assetUrl;
    std::string digest;
    std::uint64_t size{};
    std::string lastError;
};

juce::var requiredProperty(juce::DynamicObject& object, const char* name)
{
    require(object.hasProperty(name), std::string("Journal property is missing: ") + name);
    return object.getProperty(name);
}

juce::String juceString(std::string_view value)
{
    if (value.empty()) return {};
    return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
}

std::string requiredString(juce::DynamicObject& object, const char* name)
{
    const auto value = requiredProperty(object, name);
    require(value.isString(), std::string("Journal property is not a string: ") + name);
    return value.toString().toStdString();
}

void writeJournal(const Path& operation, const Journal& journal)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("schemaVersion", 1);
    object->setProperty("product", juce::String::fromUTF8(kProduct.data(), static_cast<int>(kProduct.size())));
    object->setProperty("repository", juce::String::fromUTF8(kRepository.data(), static_cast<int>(kRepository.size())));
    object->setProperty("architecture", juceString(architectureAssetSuffix(kArchitecture)));
    object->setProperty("installedVersion", juce::String::fromUTF8(kInstalledVersion.data(),
                                                                    static_cast<int>(kInstalledVersion.size())));
    object->setProperty("operationId", juceString(journal.operationId));
    object->setProperty("operationMode",
                        journal.mode == OperationMode::releaseGate ? "release-gate" : "normal");
    if (journal.mode == OperationMode::releaseGate)
    {
        require(journal.releaseGate.has_value(), "Release-gate journal identity is missing");
        object->setProperty("releaseId", juceString(std::to_string(journal.releaseGate->releaseId)));
        object->setProperty("releaseTag", juceString(journal.releaseGate->tag));
        object->setProperty("sourceCommit", juceString(journal.releaseGate->sourceCommit));
    }
    object->setProperty("phase", phaseName(journal.phase));
    object->setProperty("targetVersion", juceString(journal.targetVersion));
    object->setProperty("assetUrl", juceString(journal.assetUrl));
    object->setProperty("digest", juceString(journal.digest));
    object->setProperty("size", static_cast<juce::int64>(journal.size));
    object->setProperty("lastError", juceString(journal.lastError.substr(0, 2048)));
    const auto json = juce::JSON::toString(juce::var(object.release()), true);
    atomicWrite(operation / L"journal.json", std::string(json.toRawUTF8(), json.getNumBytesAsUTF8()));
}

enum class JournalReadPurpose { resume, cleanup };

Journal readJournal(const Path& operation, std::string_view expectedId,
                    JournalReadPurpose purpose,
                    const std::optional<ReleaseGateIdentity>& expectedReleaseGate = std::nullopt)
{
    ensureNotReparsePoint(operation / L"journal.json", "Updater journal");
    const auto text = fileUtf8(operation / L"journal.json", 64u * 1024u);
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())));
    auto* object = parsed.getDynamicObject();
    require(object != nullptr, "Journal is not a JSON object");
    const auto schema = requiredProperty(*object, "schemaVersion");
    require(schema.isInt() && static_cast<int>(schema) == 1, "Unsupported journal schema");
    require(requiredString(*object, "product") == kProduct,
            "Journal product mismatch");
    require(requiredString(*object, "repository") == kRepository,
            "Journal repository mismatch");
    require(requiredString(*object, "architecture")
                == architectureAssetSuffix(kArchitecture), "Journal architecture mismatch");
    const auto writerVersion = requiredString(*object, "installedVersion");
    const auto parsedWriterVersion = parseVersion(writerVersion);
    const auto currentVersion = parseVersion(kInstalledVersion);
    require(parsedWriterVersion.has_value() && currentVersion.has_value(),
            "Journal installed-version is invalid");
    if (purpose == JournalReadPurpose::resume)
        require(writerVersion == kInstalledVersion, "Journal installed-version mismatch");
    else
        require(! isStrictlyNewer(*parsedWriterVersion, *currentVersion),
                "Journal belongs to a newer updater version");
    Journal result;
    result.operationId = requiredString(*object, "operationId");
    result.writerVersion = writerVersion;
    require(result.operationId == expectedId && isOperationId(result.operationId), "Journal operation mismatch");
    if (object->hasProperty("operationMode"))
    {
        const auto mode = requiredString(*object, "operationMode");
        require(mode == "normal" || mode == "release-gate", "Journal operation mode is invalid");
        result.mode = mode == "release-gate" ? OperationMode::releaseGate : OperationMode::normal;
    }
    if (result.mode == OperationMode::releaseGate)
    {
        const auto releaseIdText = requiredString(*object, "releaseId");
        const auto releaseId = parseCanonicalPositiveUint64(releaseIdText);
        require(releaseId.has_value(), "Journal release ID is invalid");
        ReleaseGateIdentity identity;
        identity.releaseId = *releaseId;
        identity.tag = requiredString(*object, "releaseTag");
        identity.sourceCommit = requiredString(*object, "sourceCommit");
        const auto version = parseStableTag(identity.tag);
        require(version.has_value() && identity.tag == "v" + toString(*version),
                "Journal release tag is invalid");
        require(isGitCommitHex(identity.sourceCommit)
                    && identity.sourceCommit == lowerAscii(identity.sourceCommit),
                "Journal source commit is invalid or non-canonical");
        result.releaseGate = std::move(identity);
    }
    else
    {
        require(! object->hasProperty("releaseId") && ! object->hasProperty("releaseTag")
                    && ! object->hasProperty("sourceCommit"),
                "Normal journal contains release-gate identity fields");
    }
    if (purpose == JournalReadPurpose::resume)
    {
        require(expectedReleaseGate.has_value() == result.releaseGate.has_value(),
                "Journal operation mode does not match the requested resume mode");
        if (expectedReleaseGate)
            require(*result.releaseGate == *expectedReleaseGate,
                    "Journal release-gate identity does not match the command line");
    }
    const auto phase = parsePhase(requiredString(*object, "phase"));
    require(phase.has_value(), "Journal phase is invalid");
    result.phase = *phase;
    if (purpose == JournalReadPurpose::resume && expectedReleaseGate)
        require(result.phase == Phase::created,
                "Release-gate operations are one-shot and cannot resume persisted progress");
    require(result.mode != OperationMode::releaseGate || result.phase != Phase::noUpdate,
            "Release-gate journal cannot enter no-update state");
    result.targetVersion = requiredString(*object, "targetVersion");
    result.assetUrl = requiredString(*object, "assetUrl");
    result.digest = requiredString(*object, "digest");
    const auto sizeValue = requiredProperty(*object, "size");
    require(sizeValue.isInt() || sizeValue.isInt64(), "Journal size is not an integer");
    const auto signedSize = static_cast<juce::int64>(sizeValue);
    require(signedSize >= 0 && static_cast<std::uint64_t>(signedSize) <= kMaximumMsiBytes,
            "Journal size exceeds policy");
    result.size = static_cast<std::uint64_t>(signedSize);
    result.lastError = requiredString(*object, "lastError");
    if (result.phase != Phase::created && result.phase != Phase::noUpdate)
    {
        const auto version = parseVersion(result.targetVersion);
        require(version.has_value(), "Journal target version is invalid");
        require(result.assetUrl == expectedAssetUrl(kOwner, kRepository, kProduct, *version, kArchitecture),
                "Journal asset URL is not canonical");
        require(isSha256Hex(result.digest) && result.size > 0, "Journal digest or size is invalid");
    }
    return result;
}

bool markForDeletion(HANDLE handle)
{
    FILE_DISPOSITION_INFO_EX extended{};
    extended.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (SetFileInformationByHandle(handle, FileDispositionInfoEx,
                                   &extended, sizeof(extended)) != FALSE)
        return true;
    const auto extendedError = GetLastError();
    if (extendedError != ERROR_INVALID_PARAMETER
        && extendedError != ERROR_INVALID_FUNCTION
        && extendedError != ERROR_NOT_SUPPORTED)
        return false;

    // Older Windows 10 builds may not implement FileDispositionInfoEx. Keep
    // the handle-based legacy path for those systems without weakening real
    // sharing/access failures into a pathname-based retry.
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo,
                                      &disposition, sizeof(disposition)) != FALSE;
}

bool safeChildName(std::wstring_view name)
{
    return ! name.empty() && name != L"." && name != L".."
        && name.find_first_of(L"\\/:") == std::wstring_view::npos
        && name.back() != L'.' && name.back() != L' ';
}

bool deleteDirectoryContents(const Path& directory);

// `directory` is pinned by the caller. Only a single enumerated leaf is ever
// appended, and OPEN_REPARSE_POINT makes links deletion leaves, never paths.
bool deleteChild(const Path& directory, std::wstring_view name)
{
    if (! safeChildName(name)) return false;
    const auto path = directory / std::wstring(name);
    Handle lease(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (! lease.valid())
    {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (! GetFileInformationByHandleEx(lease.get(), FileAttributeTagInfo,
                                       &attributes, sizeof(attributes)))
        return false;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
        && ! deleteDirectoryContents(path))
        return false;
    return markForDeletion(lease.get());
}

bool deleteDirectoryContents(const Path& directory)
{
    WIN32_FIND_DATAW data{};
    FindHandle search(FindFirstFileW((directory / L"*").c_str(), &data));
    if (search.get() == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do
    {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L".." && ! deleteChild(directory, name)) return false;
    }
    while (FindNextFileW(search.get(), &data));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

bool isTerminal(Phase phase)
{
    return phase == Phase::verified || phase == Phase::noUpdate;
}

bool cleanupPayloadFiles(const Path& operation, const Journal& journal)
{
    bool removed = true;
    if (const auto target = parseVersion(journal.targetVersion))
        removed = deleteChild(operation,
            widen(expectedAssetName(kProduct, *target, kArchitecture))) && removed;
    removed = deleteChild(operation, L"download.part") && removed;
    removed = deleteChild(operation, L"private-temp") && removed;
    return removed;
}

bool deleteOperationContents(const Path& operation, const Journal& journal)
{
    if (! cleanupPayloadFiles(operation, journal)) return false;
    const auto updaterName = widen(kProduct) + L"Updater.exe";
    std::vector<std::wstring> ordinaryChildren;
    std::optional<std::wstring> journalName;
    std::optional<std::wstring> lockName;
    std::optional<std::wstring> copiedUpdaterName;
    {
        WIN32_FIND_DATAW data{};
        FindHandle search(FindFirstFileW((operation / L"*").c_str(), &data));
        if (search.get() == INVALID_HANDLE_VALUE) return false;
        do
        {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            if (equalInsensitive(name, L"journal.json"))
            {
                if (journalName) return false;
                journalName = name;
            }
            else if (equalInsensitive(name, L"operation.lock"))
            {
                if (lockName) return false;
                lockName = name;
            }
            else if (equalInsensitive(name, updaterName))
            {
                if (copiedUpdaterName) return false;
                copiedUpdaterName = name;
            }
            else
                ordinaryChildren.emplace_back(name);
        }
        while (FindNextFileW(search.get(), &data));
        if (GetLastError() != ERROR_NO_MORE_FILES) return false;
    }
    if (! journalName) return false;
    for (const auto& name : ordinaryChildren)
        if (! deleteChild(operation, name)) return false;
    // Recovery state is removed strictly after every other observed child.
    return (! lockName || deleteChild(operation, *lockName))
        && (! copiedUpdaterName || deleteChild(operation, *copiedUpdaterName))
        && deleteChild(operation, *journalName);
}

std::optional<std::string> cleanupOldOperations(
    const Path& root, bool preserveNewestIncomplete) noexcept
{
    try
    {
        auto rootLease = lockDirectoryAgainstReplacement(root, "Updater operations root");
        struct Record
        {
            Path path;
            std::string id;
            std::string writerVersion;
            OperationMode mode;
            std::optional<ReleaseGateIdentity> releaseGate;
            Phase phase;
            std::filesystem::file_time_type time;
        };
        std::vector<Record> records;
        std::set<std::string> ids;
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(root,
                 std::filesystem::directory_options::skip_permission_denied, error), end;
             ! error && iterator != end; iterator.increment(error))
        {
            try
            {
                const auto path = iterator->path();
                const auto id = upperAscii(narrow(path.filename().wstring()));
                const auto attributes = GetFileAttributesW(path.c_str());
                if (! isOperationId(id) || attributes == INVALID_FILE_ATTRIBUTES
                    || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
                    || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    continue;
                const auto journal = readJournal(path, id, JournalReadPurpose::cleanup);
                const auto time = std::filesystem::last_write_time(path / L"journal.json", error);
                if (error || ! ids.insert(id).second) return std::nullopt;
                records.push_back({ path, id, journal.writerVersion, journal.mode,
                                    journal.releaseGate, journal.phase, time });
            }
            catch (...) {}
        }
        if (error) return std::nullopt;
        std::optional<std::size_t> newest;
        for (std::size_t index{}; index < records.size(); ++index)
            if (records[index].writerVersion == kInstalledVersion
                && records[index].mode == OperationMode::normal
                && ! isTerminal(records[index].phase)
                && (! newest || records[index].time > records[*newest].time
                    || (records[index].time == records[*newest].time
                        && records[index].id > records[*newest].id)))
                newest = index;

        for (std::size_t index{}; index < records.size(); ++index)
        {
            const auto& record = records[index];
            if (preserveNewestIncomplete && newest == index) continue;
            Handle lease(CreateFileW(record.path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (! lease.valid()
                || ! GetFileInformationByHandleEx(lease.get(), FileAttributeTagInfo,
                                                   &attributes, sizeof(attributes))
                || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
                || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                continue;
            try
            {
                // Revalidate while DELETE-without-delete-sharing proves that
                // no running updater holds this exact operation directory.
                const auto journal = readJournal(record.path, record.id, JournalReadPurpose::cleanup);
                if (journal.writerVersion != record.writerVersion || journal.mode != record.mode
                    || journal.releaseGate != record.releaseGate
                    || journal.phase != record.phase)
                    continue;
                if (deleteOperationContents(record.path, journal))
                    (void) markForDeletion(lease.get());
            }
            catch (...) {}
        }
        return newest ? std::optional<std::string>(records[*newest].id) : std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

void cleanupVerifiedPayload(const Path& operation, const Journal& journal) noexcept
{
    try
    {
        // Terminal journal state is committed first by the caller. These
        // potentially large artifacts are then best-effort only: failure must
        // neither turn a verified installation into an error nor remove the
        // copied updater, lock, or recovery journal.
        auto operationLease = lockDirectoryAgainstReplacement(operation, "Verified operation");
        (void) cleanupPayloadFiles(operation, journal);
    }
    catch (...) {}
}

int taskDialog(const std::wstring& title, const std::wstring& instruction,
               const std::wstring& text, TASKDIALOG_COMMON_BUTTON_FLAGS buttons,
               PCWSTR icon = TD_INFORMATION_ICON)
{
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = buttons;
    config.pszWindowTitle = title.c_str();
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = text.c_str();
    config.pszMainIcon = icon;
    int pressed{};
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)))
    {
        const auto style = (buttons & TDCBF_YES_BUTTON) != 0 ? MB_YESNO
                         : (buttons & TDCBF_CANCEL_BUTTON) != 0 ? MB_OKCANCEL : MB_OK;
        pressed = MessageBoxW(nullptr, (instruction + L"\n\n" + text).c_str(), title.c_str(),
                              style);
    }
    return pressed;
}

struct ParsedUrl
{
    std::wstring host;
    INTERNET_PORT port{};
    std::wstring path;
};

ParsedUrl parseHttpsUrl(const std::wstring& url)
{
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    require(WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts),
            winError("Invalid update URL"));
    require(parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == INTERNET_DEFAULT_HTTPS_PORT,
            "Only HTTPS on the default port is allowed");
    require(parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0, "URL credentials are forbidden");
    ParsedUrl result;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.host = lowerAscii(std::move(result.host));
    require(isAllowedHttpsHost(narrow(result.host)), "HTTPS host is not on the updater allowlist");
    result.port = parts.nPort;
    result.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0)
        result.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    require(! result.path.empty() && result.path.front() == L'/', "HTTPS URL has no absolute path");
    return result;
}

std::optional<std::wstring> queryHeader(HINTERNET request, DWORD query)
{
    DWORD bytes{};
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t))
        return std::nullopt;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    require(WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes,
                                WINHTTP_NO_HEADER_INDEX), winError("Cannot read HTTP header"));
    return std::wstring(buffer.data());
}

std::optional<std::uint64_t> parseUnsigned(std::wstring_view text)
{
    const auto ascii = narrow(text);
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    if (error != std::errc{} || end != ascii.data() + ascii.size()) return std::nullopt;
    return value;
}

struct HttpResult
{
    std::wstring finalUrl;
    std::uint64_t bytes{};
};

HttpResult httpGet(const std::wstring& firstUrl, std::uint64_t maximumBytes,
                   std::optional<std::uint64_t> exactBytes,
                   std::chrono::seconds totalTimeout,
                   const std::function<void(const unsigned char*, std::size_t)>& consume)
{
    InternetHandle session(WinHttpOpen(L"WhykikiAudio-WindowsUpdater/1",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(session.valid(), winError("WinHTTP initialization failed"));
    require(WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000),
            winError("Cannot set HTTPS timeouts"));

    auto url = firstUrl;
    const auto deadline = std::chrono::steady_clock::now() + totalTimeout;
    for (DWORD redirect = 0; redirect <= kMaximumRedirects; ++redirect)
    {
        require(std::chrono::steady_clock::now() < deadline, "HTTPS request exceeded its total timeout");
        const auto parsed = parseHttpsUrl(url);
        InternetHandle connection(WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0));
        require(connection.valid(), winError("HTTPS connection failed"));
        InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", parsed.path.c_str(), nullptr,
                                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE));
        require(request.valid(), winError("HTTPS request creation failed"));
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        require(WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                                 &disabled, sizeof(disabled)), winError("Cannot disable automatic redirects"));
        require(WinHttpSendRequest(request.get(), kGithubApiHeaders, static_cast<DWORD>(-1),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0),
                winError("HTTPS request failed"));
        require(WinHttpReceiveResponse(request.get(), nullptr), winError("HTTPS response failed"));
        DWORD status{};
        DWORD statusBytes = sizeof(status);
        require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                                    WINHTTP_NO_HEADER_INDEX), winError("Cannot read HTTP status"));
        if (status >= 300 && status <= 399)
        {
            require(redirect < kMaximumRedirects, "HTTPS redirect limit exceeded");
            const auto location = queryHeader(request.get(), WINHTTP_QUERY_LOCATION);
            require(location && location->starts_with(L"https://"),
                    "Redirect must be an absolute HTTPS URL");
            parseHttpsUrl(*location); // Fail before following an unapproved host.
            url = *location;
            continue;
        }
        require(status == 200, "GitHub returned HTTP status " + std::to_string(status));
        const auto contentLength = queryHeader(request.get(), WINHTTP_QUERY_CONTENT_LENGTH);
        if (contentLength)
        {
            const auto length = parseUnsigned(*contentLength);
            require(length && *length <= maximumBytes, "HTTP Content-Length exceeds policy");
            if (exactBytes) require(*length == *exactBytes, "HTTP Content-Length differs from GitHub metadata");
        }

        std::uint64_t total{};
        std::array<unsigned char, 64 * 1024> buffer{};
        for (;;)
        {
            require(std::chrono::steady_clock::now() < deadline, "HTTPS transfer exceeded its total timeout");
            DWORD read{};
            require(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read),
                    winError("HTTPS body read failed"));
            if (read == 0) break;
            require(total <= maximumBytes - read, "HTTPS body exceeds its policy limit");
            consume(buffer.data(), read);
            total += read;
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
                fail("Download canceled with Escape; the operation can be resumed later");
        }
        if (exactBytes) require(total == *exactBytes, "Downloaded size differs from GitHub metadata");
        return { url, total };
    }
    fail("Unreachable redirect state");
}

std::string httpGetText(const std::string& url)
{
    std::vector<unsigned char> bytes;
    bytes.reserve(64u * 1024u);
    httpGet(widen(url), kMaximumMetadataBytes, std::nullopt, std::chrono::seconds(120),
            [&] (const unsigned char* data, std::size_t size)
            {
                bytes.insert(bytes.end(), data, data + size);
            });
    require(! bytes.empty(), "GitHub metadata response is empty");
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct Release
{
    SemVersion version;
    std::string url;
    std::string digest;
    std::uint64_t size{};
};

Release parsePublishedRelease(juce::DynamicObject& object, const SemVersion& version,
                              std::string_view canonicalTag)
{
    const auto expectedHtml = "https://github.com/" + std::string(kOwner) + "/" + std::string(kRepository)
                            + "/releases/tag/" + std::string(canonicalTag);
    require(object.getProperty("html_url").toString().toStdString() == expectedHtml,
            "Release metadata does not belong to the configured repository/tag");
    auto* assets = object.getProperty("assets").getArray();
    require(assets != nullptr, "Release assets are missing");
    const auto expectedName = expectedAssetName(kProduct, version, kArchitecture);
    const auto expectedUrl = expectedAssetUrl(kOwner, kRepository, kProduct, version, kArchitecture);
    std::optional<Release> found;
    for (const auto& assetValue : *assets)
    {
        auto* asset = assetValue.getDynamicObject();
        if (asset == nullptr || asset->getProperty("name").toString().toStdString() != expectedName)
            continue;
        require(! found.has_value(), "Release contains duplicate architecture assets");
        require(asset->getProperty("state").toString() == "uploaded", "Release asset is not fully uploaded");
        const auto assetUrl = asset->getProperty("browser_download_url").toString().toStdString();
        require(assetUrl == expectedUrl, "Release asset URL is not the exact repository/tag/name URL");
        const auto digest = digestHex(asset->getProperty("digest").toString().toStdString());
        require(digest.has_value(), "GitHub release asset has no valid sha256: digest");
        const auto sizeValue = asset->getProperty("size");
        require(sizeValue.isInt() || sizeValue.isInt64(), "GitHub release asset size is not an integer");
        const auto signedSize = static_cast<juce::int64>(sizeValue);
        require(signedSize > 0 && static_cast<std::uint64_t>(signedSize) <= kMaximumMsiBytes,
                "GitHub release asset size is outside policy");
        found = Release { version, assetUrl, *digest, static_cast<std::uint64_t>(signedSize) };
    }
    require(found.has_value(), "Release does not contain exactly the expected MSI asset");
    return *found;
}

Release parseReleaseGateCandidate(const std::string& json,
                                  const SemVersion& baseline,
                                  const ReleaseGateIdentity& expected)
{
    const auto parsed = juce::JSON::parse(
        juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    auto* object = parsed.getDynamicObject();
    require(object != nullptr, "Release-gate metadata is not a JSON object");

    const auto idValue = object->getProperty("id");
    require(idValue.isInt() || idValue.isInt64(), "Release-gate release ID is not an integer");
    const auto signedId = static_cast<juce::int64>(idValue);
    require(signedId > 0 && static_cast<std::uint64_t>(signedId) == expected.releaseId,
            "Release-gate metadata belongs to a different release ID");

    const auto draft = object->getProperty("draft");
    const auto prerelease = object->getProperty("prerelease");
    const auto immutable = object->getProperty("immutable");
    require(draft.isBool() && prerelease.isBool() && immutable.isBool(),
            "Release-gate publication flags are not booleans");
    require(! static_cast<bool>(draft) && static_cast<bool>(prerelease)
                && static_cast<bool>(immutable),
            "Release-gate candidate must be a published immutable prerelease");

    const auto tag = object->getProperty("tag_name").toString().toStdString();
    const auto version = parseStableTag(tag);
    require(version.has_value() && tag == "v" + toString(*version) && tag == expected.tag,
            "Release-gate tag is not the exact canonical requested tag");
    require(isStrictlyNewer(*version, baseline),
            "Release-gate target is not newer than the installed helper version");
    require(object->getProperty("target_commitish").toString().toStdString()
                == expected.sourceCommit,
            "Release-gate target commit does not match the requested source commit");
    return parsePublishedRelease(*object, *version, tag);
}

std::optional<Release> parseNextRelease(const std::string& json, const SemVersion& baseline)
{
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    auto* releases = parsed.getArray();
    require(releases != nullptr, "GitHub releases metadata is not an array");
    require(releases->size() < 100,
            "GitHub releases page is full; refusing a potentially truncated rotation history");
    std::set<std::string> stableTags;
    std::optional<Release> selected;
    for (auto& releaseValue : *releases)
    {
        auto* object = releaseValue.getDynamicObject();
        require(object != nullptr, "GitHub releases metadata contains a non-object record");
        const auto draft = object->getProperty("draft");
        const auto prerelease = object->getProperty("prerelease");
        require(draft.isBool() && prerelease.isBool(),
                "GitHub release publication flags are not booleans");
        if (static_cast<bool>(draft) || static_cast<bool>(prerelease))
            continue;
        const auto tagText = object->getProperty("tag_name").toString().toStdString();
        const auto version = parseStableTag(tagText);
        require(version.has_value() && tagText == "v" + toString(*version),
                "Published release tag is not canonical");
        require(stableTags.insert(tagText).second, "GitHub releases metadata contains a duplicate stable tag");
        if (! isStrictlyNewer(*version, baseline))
            continue;
        // Immutable Releases was introduced after some legacy macOS-only
        // history entries. Only an actually selectable newer stable release
        // must carry the new field, but for that candidate neither absence,
        // truthy coercion nor false is acceptable.
        const auto immutable = object->getProperty("immutable");
        require(immutable.isBool() && static_cast<bool>(immutable),
                "Newer stable GitHub release is not explicitly immutable");
        const auto candidate = parsePublishedRelease(*object, *version, tagText);
        if (! selected || isStrictlyNewer(selected->version, candidate.version))
            selected = candidate;
    }
    return selected;
}

void downloadMsi(const Release& release, const Path& finalPath)
{
    const auto partial = finalPath.parent_path() / L"download.part";
    DeleteFileW(partial.c_str());
    Handle file(CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(file.valid(), winError("Cannot create MSI download"));
    Sha256 hash;
    try
    {
        httpGet(widen(release.url), kMaximumMsiBytes, release.size, std::chrono::minutes(10),
                [&] (const unsigned char* data, std::size_t size)
                {
                    writeAll(file.get(), data, size);
                    hash.add(data, size);
                });
        require(FlushFileBuffers(file.get()), winError("Cannot flush MSI download"));
        file.reset();
        require(constantTimeEqual(hash.finish(), release.digest), "Downloaded MSI SHA-256 mismatch");
        require(MoveFileExW(partial.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH),
                winError("Cannot commit verified MSI download"));
    }
    catch (...)
    {
        file.reset();
        DeleteFileW(partial.c_str());
        throw;
    }
}

std::string certificateThumbprint(const Path& path)
{
    HCERTSTORE store{};
    HCRYPTMSG message{};
    DWORD encoding{}, content{}, format{};
    const auto queried = CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                                          CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                                          &encoding, &content, &format, &store, &message, nullptr);
    CertificateStore storeOwner(store);
    CryptMessage messageOwner(message);
    require(queried, winError("Cannot read Authenticode signer"));
    DWORD signerBytes{};
    require(CryptMsgGetParam(messageOwner.get(), CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerBytes),
            winError("Cannot size Authenticode signer"));
    std::vector<unsigned char> signerStorage(signerBytes);
    require(CryptMsgGetParam(messageOwner.get(), CMSG_SIGNER_INFO_PARAM, 0,
                             signerStorage.data(), &signerBytes),
            winError("Cannot read Authenticode signer"));
    auto* signer = reinterpret_cast<CMSG_SIGNER_INFO*>(signerStorage.data());
    CERT_INFO certificateInfo{};
    certificateInfo.Issuer = signer->Issuer;
    certificateInfo.SerialNumber = signer->SerialNumber;
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        storeOwner.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
        &certificateInfo, nullptr);
    require(certificate != nullptr, winError("Cannot resolve Authenticode certificate"));
    CertificateContext certificateOwner(certificate);
    std::array<unsigned char, 32> digest{};
    DWORD digestBytes = static_cast<DWORD>(digest.size());
    const auto gotDigest = CertGetCertificateContextProperty(certificateOwner.get(), CERT_SHA256_HASH_PROP_ID,
                                                              digest.data(), &digestBytes);
    require(gotDigest && digestBytes == digest.size(), winError("Cannot hash Authenticode certificate"));
    constexpr char alphabet[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest)
    {
        result.push_back(alphabet[byte >> 4]);
        result.push_back(alphabet[byte & 0x0f]);
    }
    return result;
}

std::string trustedAuthenticodeSigner(const Path& path)
{
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &fileInfo;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_SAFER_FLAG;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status = WinVerifyTrust(nullptr, &action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    require(status == ERROR_SUCCESS, "WinVerifyTrust rejected " + narrow(path.wstring())
                                      + " with status " + std::to_string(status));
    require(! kTestMode, "Test-mode binaries are never trusted for installation");
    return upperAscii(certificateThumbprint(path));
}

bool isAllowedPayloadSigner(std::string_view signer)
{
    return constantTimeEqual(upperAscii(std::string(signer)),
                             upperAscii(std::string(kCurrentSignerSha256)))
        || (! kNextSignerSha256.empty()
            && constantTimeEqual(upperAscii(std::string(signer)),
                                 upperAscii(std::string(kNextSignerSha256))));
}

void verifyAuthenticodeCurrentSigner(const Path& path)
{
    require(constantTimeEqual(trustedAuthenticodeSigner(path),
                              upperAscii(std::string(kCurrentSignerSha256))),
            "Authenticode signer does not match the current updater certificate pin");
}

std::string verifyAuthenticodeAllowedPayload(const Path& path)
{
    const auto signer = trustedAuthenticodeSigner(path);
    require(isAllowedPayloadSigner(signer),
            "Authenticode signer is outside the updater payload certificate allowlist");
    return signer;
}

void verifyAuthenticodePackageSigner(const Path& path, std::string_view packageSigner)
{
    require(isAllowedPayloadSigner(packageSigner), "Package signer is outside the updater allowlist");
    require(constantTimeEqual(trustedAuthenticodeSigner(path), upperAscii(std::string(packageSigner))),
            "PE payload signer differs from the MSI package signer");
}

std::string msiString(MSIHANDLE record, UINT field)
{
    DWORD characters{};
    const auto first = MsiRecordGetStringW(record, field, nullptr, &characters);
    require(first == ERROR_MORE_DATA || (first == ERROR_SUCCESS && characters == 0),
            "Cannot size MSI string field");
    std::vector<wchar_t> buffer(static_cast<std::size_t>(characters) + 1u);
    DWORD capacity = characters + 1u;
    require(MsiRecordGetStringW(record, field, buffer.data(), &capacity) == ERROR_SUCCESS,
            "Cannot read MSI string field");
    return narrow(std::wstring_view(buffer.data(), capacity));
}

std::vector<std::vector<std::string>> msiRows(MSIHANDLE database, const wchar_t* query)
{
    MSIHANDLE rawView{};
    require(MsiDatabaseOpenViewW(database, query, &rawView) == ERROR_SUCCESS, "Cannot open MSI query");
    MsiHandle view(rawView);
    require(MsiViewExecute(view.get(), 0) == ERROR_SUCCESS, "Cannot execute MSI query");
    std::vector<std::vector<std::string>> rows;
    for (;;)
    {
        MSIHANDLE rawRecord{};
        const auto result = MsiViewFetch(view.get(), &rawRecord);
        if (result == ERROR_NO_MORE_ITEMS) break;
        require(result == ERROR_SUCCESS, "Cannot fetch MSI query row");
        MsiHandle record(rawRecord);
        const auto fields = MsiRecordGetFieldCount(record.get());
        std::vector<std::string> row;
        for (UINT field = 1; field <= fields; ++field) row.push_back(msiString(record.get(), field));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string normalizedGuid(std::string value)
{
    if (value.size() == 38 && value.front() == '{' && value.back() == '}')
        value = value.substr(1, 36);
    value = upperAscii(std::move(value));
    require(isCanonicalGuid(value), "MSI contains a malformed GUID");
    return value;
}

std::map<std::string, std::string> msiProperties(MSIHANDLE database)
{
    std::map<std::string, std::string> properties;
    for (const auto& row : msiRows(database, L"SELECT `Property`, `Value` FROM `Property`"))
    {
        require(row.size() == 2 && ! properties.contains(row[0]), "Duplicate MSI Property row");
        properties.emplace(row[0], row[1]);
    }
    return properties;
}

std::string summaryString(MSIHANDLE database, UINT property)
{
    MSIHANDLE rawSummary{};
    require(MsiGetSummaryInformationW(database, nullptr, 0, &rawSummary) == ERROR_SUCCESS,
            "Cannot open MSI summary information");
    MsiHandle summary(rawSummary);
    UINT type{};
    INT integer{};
    FILETIME time{};
    DWORD characters{};
    const auto first = MsiSummaryInfoGetPropertyW(summary.get(), property, &type, &integer, &time,
                                                   nullptr, &characters);
    require(first == ERROR_MORE_DATA || (first == ERROR_SUCCESS && characters == 0),
            "Cannot size MSI summary property");
    std::vector<wchar_t> buffer(static_cast<std::size_t>(characters) + 1u);
    DWORD capacity = characters + 1u;
    require(MsiSummaryInfoGetPropertyW(summary.get(), property, &type, &integer, &time,
                                       buffer.data(), &capacity) == ERROR_SUCCESS && type == VT_LPSTR,
            "Cannot read MSI summary property");
    return narrow(std::wstring_view(buffer.data(), capacity));
}

void verifyMsiDatabase(const Path& path, const SemVersion& version)
{
    require(version.major <= 255 && version.minor <= 255 && version.patch <= 65535,
            "Release version cannot be represented by Windows Installer");
    MSIHANDLE rawDatabase{};
    require(MsiOpenDatabaseW(path.c_str(), nullptr, &rawDatabase) == ERROR_SUCCESS,
            "Windows Installer cannot open the MSI read-only");
    MsiHandle database(rawDatabase);
    std::set<std::string> tables;
    for (const auto& row : msiRows(database.get(), L"SELECT `Name` FROM `_Tables`"))
        if (row.size() == 1) tables.insert(row[0]);
    for (const auto* requiredTable : { "Property", "Directory", "Component", "File", "Media",
                                      "Upgrade", "LaunchCondition", "InstallExecuteSequence" })
        require(tables.contains(requiredTable), std::string("MSI table is missing: ") + requiredTable);
    for (const auto& table : tables)
    {
        if (! isForbiddenMsiSideEffectTable(table)) continue;
        const auto query = widen("SELECT * FROM `" + table + "`");
        require(msiRows(database.get(), query.c_str()).empty(),
                "Dangerous MSI table is not empty: " + table);
    }

    const auto properties = msiProperties(database.get());
    const auto property = [&] (const char* name) -> const std::string&
    {
        const auto found = properties.find(name);
        require(found != properties.end(), std::string("MSI property is missing: ") + name);
        return found->second;
    };
    const auto displayName = std::string(kProduct) + " VST3 - "
        + (kArchitecture == Architecture::x64 ? "Windows x64" : "Windows on Arm (ARM64EC)");
    require(property("ProductName") == displayName, "MSI ProductName mismatch");
    require(property("Manufacturer") == kManufacturer, "MSI Manufacturer mismatch");
    require(property("ProductVersion") == toString(version), "MSI ProductVersion mismatch");
    require(normalizedGuid(property("UpgradeCode")) == kUpgradeCode, "MSI UpgradeCode mismatch");
    const auto productCode = normalizedGuid(property("ProductCode"));
    require(productCode != kUpgradeCode && productCode != kOtherUpgradeCode,
            "MSI ProductCode must be distinct from both fixed UpgradeCodes");
    require(property("ALLUSERS") == "1", "MSI is not a per-machine package");
    require(property("ProductLanguage") == "1033", "MSI ProductLanguage mismatch");
    require(property("MSIDEPLOYMENTCOMPLIANT") == "1",
            "MSI is not explicitly marked as UAC deployment compliant");
    std::set<std::string> secureCustomProperties;
    const auto& secureCustomPropertyText = property("SecureCustomProperties");
    std::size_t securePropertyStart{};
    for (;;)
    {
        const auto separator = secureCustomPropertyText.find(';', securePropertyStart);
        const auto secureProperty = secureCustomPropertyText.substr(
            securePropertyStart,
            separator == std::string::npos ? std::string::npos : separator - securePropertyStart);
        require(! secureProperty.empty() && secureCustomProperties.insert(secureProperty).second,
                "MSI SecureCustomProperties contains an empty or duplicate entry");
        if (separator == std::string::npos) break;
        securePropertyStart = separator + 1u;
    }
    require(secureCustomProperties == std::set<std::string> {
                "WIX_UPGRADE_DETECTED", "WIX_DOWNGRADE_DETECTED", "OTHERARCHITECTUREDETECTED" },
            "MSI SecureCustomProperties is not the exact upgrade-detection set");

    const auto summaryTemplate = summaryString(database.get(), kMsiSummaryTemplate);
    const auto separator = summaryTemplate.find(';');
    const auto platform = summaryTemplate.substr(0, separator);
    require(kArchitecture == Architecture::x64 ? (platform == "x64" || platform == "Intel64")
                                               : (platform == "Arm64" || platform == "ARM64"),
            "MSI summary architecture mismatch");
    const auto packageCode = normalizedGuid(summaryString(database.get(), kMsiSummaryRevisionNumber));
    require(packageCode != productCode && packageCode != kUpgradeCode && packageCode != kOtherUpgradeCode,
            "MSI PackageCode must be distinct from ProductCode and UpgradeCodes");

    std::map<std::string, std::pair<std::string, std::string>> directories;
    for (const auto& row : msiRows(database.get(),
                                   L"SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`"))
    {
        require(row.size() == 3 && ! directories.contains(row[0]), "Malformed MSI Directory table");
        require(row[2].find("..") == std::string::npos && row[2].find('/') == std::string::npos
                    && row[2].find('\\') == std::string::npos && row[2].find(':') == std::string::npos,
                "MSI Directory table contains an unsafe path component");
        directories.emplace(row[0], std::pair { row[1], row[2] });
    }
    require(directories.contains("VST3Folder") && directories.contains("INSTALLFOLDER"),
            "MSI VST3 directories are missing");
    const auto longName = [] (const std::string& value)
    {
        const auto pipe = value.rfind('|');
        return pipe == std::string::npos ? value : value.substr(pipe + 1);
    };
    require((directories.at("VST3Folder").first == "CommonFiles6432Folder"
                || directories.at("VST3Folder").first == "CommonFiles64Folder")
                && longName(directories.at("VST3Folder").second) == "VST3",
            "MSI VST3Folder is not the 64-bit Common Files VST3 directory");
    require(directories.at("INSTALLFOLDER").first == "VST3Folder"
                && longName(directories.at("INSTALLFOLDER").second) == std::string(kProduct) + ".vst3",
            "MSI install directory does not identify the exact product bundle");

    std::set<std::string> componentIds;
    std::vector<std::string> componentDirectories;
    for (const auto& row : msiRows(database.get(),
                                   L"SELECT `Component`, `Directory_`, `Attributes` FROM `Component`"))
    {
        require(row.size() == 3 && componentIds.insert(row[0]).second,
                "Malformed or duplicate MSI Component row");
        int attributes{};
        const auto [end, error] = std::from_chars(row[2].data(), row[2].data() + row[2].size(), attributes);
        require(error == std::errc{} && end == row[2].data() + row[2].size()
                    && (attributes & 256) != 0,
                "MSI contains a non-64-bit component");
        componentDirectories.push_back(row[1]);
    }
    require(! componentIds.empty(), "MSI has no payload components");
    std::map<std::string, std::string> directoryParents;
    for (const auto& [identifier, value] : directories) directoryParents.emplace(identifier, value.first);
    require(componentDirectoriesAreInsideInstallFolder(directoryParents, componentDirectories),
            "MSI component directory does not descend from INSTALLFOLDER");
    const auto files = msiRows(database.get(),
                               L"SELECT `File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence` FROM `File`");
    require(! files.empty() && files.size() <= 4096, "MSI File table size is invalid");
    std::uint64_t declaredPayloadBytes{};
    std::set<int> fileSequences;
    for (const auto& row : files)
    {
        require(row.size() == 6 && componentIds.contains(row[1]),
                "MSI File row refers to an unknown payload component");
        const auto fileName = longName(row[2]);
        require(! fileName.empty() && fileName != "." && fileName != ".."
                    && row[2].find("..") == std::string::npos
                    && row[2].find('/') == std::string::npos
                    && row[2].find('\\') == std::string::npos
                    && row[2].find(':') == std::string::npos,
                "MSI File table contains an unsafe payload filename");
        std::uint64_t size{};
        int attributes{};
        int sequenceNumber{};
        const auto sizeResult = std::from_chars(row[3].data(), row[3].data() + row[3].size(), size);
        const auto attributeResult = std::from_chars(row[4].data(), row[4].data() + row[4].size(), attributes);
        const auto sequenceResult = std::from_chars(row[5].data(), row[5].data() + row[5].size(), sequenceNumber);
        require(sizeResult.ec == std::errc{} && sizeResult.ptr == row[3].data() + row[3].size()
                    && attributeResult.ec == std::errc{}
                    && attributeResult.ptr == row[4].data() + row[4].size()
                    && sequenceResult.ec == std::errc{}
                    && sequenceResult.ptr == row[5].data() + row[5].size()
                    && sequenceNumber > 0 && fileSequences.insert(sequenceNumber).second,
                "MSI File row has invalid size, attributes or sequence");
        require((attributes & 0x2000) == 0, "MSI contains an external/uncompressed payload file");
        require(size <= kMaximumMsiBytes && declaredPayloadBytes <= kMaximumMsiBytes - size,
                "MSI declared payload exceeds its updater limit");
        declaredPayloadBytes += size;
        require(declaredPayloadBytes <= kMaximumMsiBytes, "MSI declared payload exceeds its updater limit");
    }
    for (const auto& row : msiRows(database.get(), L"SELECT `Cabinet` FROM `Media`"))
        require(row.size() == 1 && ! row[0].empty() && row[0].front() == '#',
                "MSI payload cabinet is not embedded");

    std::vector<MsiUpgradeRow> upgradeRows;
    for (const auto& row : msiRows(database.get(),
                                   L"SELECT `UpgradeCode`, `VersionMin`, `VersionMax`, `Language`, "
                                   L"`Attributes`, `Remove`, `ActionProperty` FROM `Upgrade`"))
    {
        require(row.size() == 7, "Malformed MSI Upgrade row");
        int attributes{};
        const auto [end, error] = std::from_chars(row[4].data(), row[4].data() + row[4].size(), attributes);
        require(error == std::errc{} && end == row[4].data() + row[4].size(),
                "MSI Upgrade attributes are invalid");
        upgradeRows.push_back({ normalizedGuid(row[0]), row[1], row[2], row[3], attributes,
                                row[5], row[6] });
    }
    require(hasExactUpgradeContract(upgradeRows, kUpgradeCode, kOtherUpgradeCode, version),
            "MSI Upgrade table is not the exact three-row architecture/downgrade contract");
    std::vector<std::string> conditions;
    for (const auto& row : msiRows(database.get(), L"SELECT `Condition` FROM `LaunchCondition`"))
    {
        require(row.size() == 1, "Malformed MSI LaunchCondition row");
        conditions.push_back(row[0]);
    }
    require(hasExactLaunchConditions(conditions),
            "MSI LaunchCondition table is not the exact downgrade/architecture contract");

    for (const auto* sequenceTable : { "InstallExecuteSequence", "InstallUISequence",
                                      "AdminExecuteSequence", "AdminUISequence", "AdvtExecuteSequence" })
    {
        if (! tables.contains(sequenceTable)) continue;
        const auto query = widen("SELECT `Action` FROM `" + std::string(sequenceTable) + "`");
        for (const auto& row : msiRows(database.get(), query.c_str()))
            require(row.size() == 1 && row[0] != "ForceReboot" && row[0] != "ScheduleReboot"
                        && row[0] != "DisableRollback",
                    std::string("MSI contains a forbidden reboot/rollback action in ") + sequenceTable);
    }

    std::map<std::string, int> sequence;
    for (const auto& row : msiRows(database.get(),
                                   L"SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`"))
    {
        if (row.size() != 2) continue;
        int value{};
        const auto [end, error] = std::from_chars(row[1].data(), row[1].data() + row[1].size(), value);
        if (error == std::errc{} && end == row[1].data() + row[1].size()) sequence[row[0]] = value;
    }
    require(sequence.contains("InstallInitialize") && sequence.contains("RemoveExistingProducts")
                && sequence.contains("InstallFiles")
                && sequence["RemoveExistingProducts"] > sequence["InstallInitialize"]
                && sequence["RemoveExistingProducts"] < sequence["InstallFiles"],
            "MSI major-upgrade sequence is not rollback-safe");
}

std::size_t msiPayloadFileCount(const Path& path)
{
    MSIHANDLE rawDatabase{};
    require(MsiOpenDatabaseW(path.c_str(), nullptr, &rawDatabase) == ERROR_SUCCESS,
            "Windows Installer cannot reopen the verified MSI");
    MsiHandle database(rawDatabase);
    const auto rows = msiRows(database.get(), L"SELECT `File` FROM `File`");
    require(! rows.empty() && rows.size() <= 4096, "MSI File table size is invalid");
    return rows.size();
}

std::string verifyDownloadedMsi(const Path& msi, const Journal& journal)
{
    ensureNotReparsePoint(msi, "Downloaded MSI");
    Handle locked(CreateFileW(msi.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(locked.valid(), winError("Cannot lock downloaded MSI against replacement"));
    const auto [size, digest] = hashFile(msi);
    require(size == journal.size && constantTimeEqual(digest, journal.digest),
            "Persisted MSI no longer matches GitHub size/digest");
    const auto packageSigner = verifyAuthenticodeAllowedPayload(msi);
    const auto version = parseVersion(journal.targetVersion);
    require(version.has_value(), "Journal version is invalid");
    verifyMsiDatabase(msi, *version);
    return packageSigner;
}

struct PeInfo { WORD machine{}; bool chpe{}; };

PeInfo readPeInfo(const Path& path)
{
    const auto bytes = readSmallFile(path, 64u * 1024u * 1024u);
    require(bytes.size() >= sizeof(IMAGE_DOS_HEADER), "PE file is truncated");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0,
            "Executable has no valid DOS header");
    const auto ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    require(ntOffset <= bytes.size() - sizeof(DWORD) - sizeof(IMAGE_FILE_HEADER), "PE header is truncated");
    DWORD signature{};
    std::memcpy(&signature, bytes.data() + ntOffset, sizeof(signature));
    require(signature == IMAGE_NT_SIGNATURE, "Executable has no valid PE header");
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, bytes.data() + ntOffset + sizeof(DWORD), sizeof(fileHeader));
    bool chpe{};
    const auto optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (fileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)
        && optionalOffset <= bytes.size() - sizeof(IMAGE_OPTIONAL_HEADER64))
    {
        IMAGE_OPTIONAL_HEADER64 optional{};
        std::memcpy(&optional, bytes.data() + optionalOffset, sizeof(optional));
        if (optional.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
            && optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
        {
            const auto directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
            const auto sectionOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
            require(sectionOffset <= bytes.size()
                        && fileHeader.NumberOfSections <= (bytes.size() - sectionOffset) / sizeof(IMAGE_SECTION_HEADER),
                    "PE section table is truncated");
            for (WORD i = 0; directory.VirtualAddress != 0 && i < fileHeader.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER section{};
                std::memcpy(&section, bytes.data() + sectionOffset + i * sizeof(section), sizeof(section));
                const auto span = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
                if (directory.VirtualAddress < section.VirtualAddress
                    || directory.VirtualAddress - section.VirtualAddress >= span)
                    continue;
                const auto fileOffset = static_cast<std::uint64_t>(section.PointerToRawData)
                                      + directory.VirtualAddress - section.VirtualAddress;
                constexpr auto fieldEnd = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer)
                                        + sizeof(ULONGLONG);
                if (directory.Size >= fieldEnd && fileOffset <= bytes.size() - fieldEnd)
                {
                    ULONGLONG pointer{};
                    std::memcpy(&pointer, bytes.data() + fileOffset
                                             + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer),
                                sizeof(pointer));
                    chpe = pointer != 0;
                }
                break;
            }
        }
    }
    return { fileHeader.Machine, chpe };
}

bool hasNamedDataStream(const Path& path)
{
    WIN32_FIND_STREAM_DATA stream{};
    HANDLE raw = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &stream, 0);
    if (raw == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        require(error == ERROR_HANDLE_EOF || error == ERROR_INVALID_FUNCTION,
                winError("Cannot enumerate file streams", error));
        return false;
    }
    FindHandle search(raw);
    do
    {
        if (std::wstring_view(stream.cStreamName) != L"::$DATA") return true;
    } while (FindNextStreamW(search.get(), &stream));
    require(GetLastError() == ERROR_HANDLE_EOF, winError("Cannot enumerate file streams"));
    return false;
}

struct FingerprintEntry
{
    std::string path;
    std::uint64_t size{};
    std::string digest;
    friend bool operator==(const FingerprintEntry&, const FingerprintEntry&) = default;
};

struct BundleFingerprint
{
    std::vector<std::string> directories;
    std::vector<FingerprintEntry> files;
    friend bool operator==(const BundleFingerprint&, const BundleFingerprint&) = default;
};

bool isExecutableScript(const Path& path)
{
    auto extension = lowerAscii(path.extension().wstring());
    return extension == L".ps1" || extension == L".bat" || extension == L".cmd"
        || extension == L".vbs" || extension == L".js" || extension == L".jse"
        || extension == L".wsf" || extension == L".hta" || extension == L".msi"
        || extension == L".py" || extension == L".pyw" || extension == L".pl"
        || extension == L".rb" || extension == L".sh" || extension == L".wsh"
        || extension == L".url" || extension == L".scf"
        || extension == L".com" || extension == L".scr" || extension == L".cpl"
        || extension == L".lnk";
}

bool requiresPeSignature(const Path& path)
{
    const auto extension = lowerAscii(path.extension().wstring());
    return extension == L".exe" || extension == L".dll" || extension == L".sys"
        || extension == L".ocx" || extension == L".vst3";
}

bool beginsWithMz(const Path& path)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.valid(), winError("Cannot open payload file"));
    WORD magic{};
    DWORD read{};
    return ReadFile(file.get(), &magic, sizeof(magic), &read, nullptr)
        && read == sizeof(magic) && magic == IMAGE_DOS_SIGNATURE;
}

void validateModuleInfo(const Path& bundle, const SemVersion& version)
{
    const auto manifest = bundle / L"Contents" / L"Resources" / L"moduleinfo.json";
    ensureNotReparsePoint(manifest, "moduleinfo.json");
    const auto json = fileUtf8(manifest, kMaximumManifestBytes);
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    auto* object = parsed.getDynamicObject();
    require(object != nullptr, "moduleinfo.json is not a JSON object");
    require(object->getProperty("Name").toString().toStdString() == kProduct,
            "moduleinfo.json product mismatch");
    require(object->getProperty("Version").toString().toStdString() == toString(version),
            "moduleinfo.json version mismatch");
    auto* factory = object->getProperty("Factory Info").getDynamicObject();
    require(factory != nullptr && factory->getProperty("Vendor").toString().toStdString() == kManufacturer,
            "moduleinfo.json vendor mismatch");
    auto* classes = object->getProperty("Classes").getArray();
    require(classes != nullptr && classes->size() == 2, "moduleinfo.json must contain exactly two plugin classes");
    std::set<std::string> classIds;
    std::set<std::string> classCategories;
    for (const auto& classValue : *classes)
    {
        auto* pluginClass = classValue.getDynamicObject();
        require(pluginClass != nullptr
                    && pluginClass->getProperty("Name").toString().toStdString() == kProduct
                    && pluginClass->getProperty("Vendor").toString().toStdString() == kManufacturer
                    && pluginClass->getProperty("Version").toString().toStdString() == toString(version),
                "moduleinfo.json contains a foreign class");
        const auto cid = pluginClass->getProperty("CID").toString().toStdString();
        const auto category = pluginClass->getProperty("Category").toString().toStdString();
        require(cid.size() == 32 && std::all_of(cid.begin(), cid.end(), isHex)
                    && classIds.insert(cid).second && classCategories.insert(category).second,
                "moduleinfo.json contains an invalid or duplicate class");
    }
    require(classCategories == std::set<std::string> {
                "Audio Module Class", "Component Controller Class" },
            "moduleinfo.json class categories are not the exact VST3 component/controller pair");
}

BundleFingerprint fingerprintBundle(const Path& bundle, const SemVersion& version,
                                    std::string_view packageSigner)
{
    require(std::filesystem::is_directory(bundle), "VST3 payload is not a directory");
    ensureNotReparsePoint(bundle, "VST3 payload root");
    require(! hasNamedDataStream(bundle), "VST3 payload root contains an alternate data stream");
    require(bundle.filename().wstring() == widen(kProduct) + L".vst3", "VST3 bundle name mismatch");
    validateModuleInfo(bundle, version);
    const auto expectedArchitectureDirectory = widen(architectureBundleDirectory(kArchitecture));
    const auto contents = bundle / L"Contents";
    std::vector<std::wstring> architectureDirectories;
    for (const auto& child : std::filesystem::directory_iterator(contents))
        if (child.is_directory() && lowerAscii(child.path().filename().wstring()).ends_with(L"-win"))
            architectureDirectories.push_back(child.path().filename().wstring());
    require(architectureDirectories.size() == 1
                && architectureDirectories.front() == expectedArchitectureDirectory,
            "VST3 contains the wrong or multiple architecture directories");
    const auto primary = contents / expectedArchitectureDirectory / (widen(kProduct) + L".vst3");
    require(std::filesystem::is_regular_file(primary), "Expected VST3 PE binary is missing");

    BundleFingerprint fingerprint;
    std::set<std::wstring, OrdinalInsensitiveLess> caseFolded;
    std::size_t peCount{};
    for (std::filesystem::recursive_directory_iterator iterator(bundle), end; iterator != end; ++iterator)
    {
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                "VST3 contains a reparse point");
        require(! hasNamedDataStream(iterator->path()), "VST3 contains an alternate data stream");
        const auto relative = std::filesystem::relative(iterator->path(), bundle).generic_u8string();
        std::string relativeUtf8(reinterpret_cast<const char*>(relative.data()), relative.size());
        require(caseFolded.insert(iterator->path().lexically_relative(bundle).wstring()).second,
                "VST3 contains case-colliding paths");
        if (iterator->is_directory())
        {
            fingerprint.directories.push_back(relativeUtf8);
            continue;
        }
        require(iterator->is_regular_file(), "VST3 contains a non-regular filesystem entry");
        Handle stableFile(CreateFileW(iterator->path().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(stableFile.valid(), winError("Cannot lock VST3 payload file"));
        require(! isExecutableScript(iterator->path()), "VST3 contains an executable script payload");
        const auto isPe = beginsWithMz(iterator->path());
        require(! requiresPeSignature(iterator->path()) || isPe,
                "Executable payload extension does not contain a PE image");
        if (isPe)
        {
            const auto pe = readPeInfo(iterator->path());
            const auto correct = kArchitecture == Architecture::x64
                ? pe.machine == IMAGE_FILE_MACHINE_AMD64 && ! pe.chpe
                : ((pe.machine == IMAGE_FILE_MACHINE_AMD64 && pe.chpe)
                    || pe.machine == 0xA641 || pe.machine == 0xA64E);
            require(correct, "VST3 contains a PE image for the wrong architecture");
            verifyAuthenticodePackageSigner(iterator->path(), packageSigner);
            ++peCount;
        }
        const auto [size, digest] = hashFile(iterator->path());
        fingerprint.files.push_back({ relativeUtf8, size, digest });
    }
    require(peCount > 0 && beginsWithMz(primary), "VST3 contains no signed PE payload");
    std::sort(fingerprint.directories.begin(), fingerprint.directories.end());
    std::sort(fingerprint.files.begin(), fingerprint.files.end(), [] (const auto& a, const auto& b)
    {
        return a.path < b.path;
    });
    return fingerprint;
}

Path findSingleVst3(const Path& extraction)
{
    ensureNotReparsePoint(extraction, "Administrative extraction root");
    std::vector<Path> found;
    for (std::filesystem::recursive_directory_iterator iterator(extraction), end; iterator != end; ++iterator)
    {
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                "Administrative extraction contains a reparse point");
        if (iterator->is_directory() && equalInsensitive(iterator->path().extension().wstring(), L".vst3"))
            found.push_back(iterator->path());
    }
    require(found.size() == 1, "Administrative extraction must contain exactly one VST3 bundle");
    return found.front();
}

bool pathInside(const Path& candidate, const Path& parent)
{
    const auto candidateText = std::filesystem::weakly_canonical(candidate).wstring();
    auto parentText = std::filesystem::weakly_canonical(parent).wstring();
    if (! parentText.ends_with(L"\\")) parentText.push_back(L'\\');
    return candidateText.size() > parentText.size()
        && CompareStringOrdinal(candidateText.data(), static_cast<int>(parentText.size()),
                                parentText.data(), static_cast<int>(parentText.size()), TRUE) == CSTR_EQUAL;
}

void validateAdministrativeImage(const Path& extraction, const Path& bundle,
                                 const Path& sourceMsi, const BundleFingerprint& fingerprint)
{
    require(fingerprint.files.size() == msiPayloadFileCount(sourceMsi),
            "Extracted VST3 file count differs from the verified MSI File table");
    std::size_t administrativeMsiFiles{};
    for (std::filesystem::recursive_directory_iterator iterator(extraction), end; iterator != end; ++iterator)
    {
        const auto path = iterator->path();
        ensureNotReparsePoint(path, "Administrative image entry");
        require(! hasNamedDataStream(path), "Administrative image contains an alternate data stream");
        if (iterator->is_directory()) continue;
        require(iterator->is_regular_file(), "Administrative image contains a special filesystem entry");
        if (pathInside(path, bundle)) continue;
        require(path.parent_path() == extraction && equalInsensitive(path.extension().wstring(), L".msi")
                    && ++administrativeMsiFiles == 1,
                "Administrative image contains a file outside the single VST3 payload");
    }
}

Path installedBundlePath()
{
    PWSTR raw{};
    require(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &raw) == S_OK,
            "Common Program Files is unavailable");
    Path result(raw);
    CoTaskMemFree(raw);
    ensureNotReparsePoint(result, "Common Program Files");
    const auto vst3 = result / L"VST3";
    if (std::filesystem::exists(vst3)) ensureNotReparsePoint(vst3, "System VST3 directory");
    const auto bundle = vst3 / (widen(kProduct) + L".vst3");
    if (std::filesystem::exists(bundle)) ensureNotReparsePoint(bundle, "Installed VST3");
    return bundle;
}

Path installedUpdaterPath()
{
    const auto bundle = installedBundlePath();
    require(std::filesystem::is_directory(bundle), "Installed VST3 bundle is missing");
    const auto contents = bundle / L"Contents";
    const auto helpers = contents / L"Helpers";
    const auto updater = helpers / (widen(kProduct) + L"Updater.exe");
    ensureNotReparsePoint(contents, "Installed VST3 Contents");
    ensureNotReparsePoint(helpers, "Installed VST3 Helpers");
    ensureNotReparsePoint(updater, "Installed updater executable");
    require(std::filesystem::is_regular_file(updater), "Installed updater executable is missing");
    return std::filesystem::weakly_canonical(updater);
}

std::optional<SemVersion> installedSystemVersion()
{
    const auto bundle = installedBundlePath();
    const auto contents = bundle / L"Contents";
    const auto resources = contents / L"Resources";
    const auto manifest = resources / L"moduleinfo.json";
    if (! std::filesystem::exists(manifest)) return std::nullopt;
    ensureNotReparsePoint(bundle, "Installed VST3");
    ensureNotReparsePoint(contents, "Installed VST3 Contents");
    ensureNotReparsePoint(resources, "Installed VST3 Resources");
    ensureNotReparsePoint(manifest, "Installed VST3 moduleinfo.json");
    const auto json = fileUtf8(manifest, kMaximumManifestBytes);
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())));
    auto* object = parsed.getDynamicObject();
    require(object != nullptr && object->getProperty("Name").toString().toStdString() == kProduct,
            "Existing system VST3 identity is invalid");
    const auto version = parseVersion(object->getProperty("Version").toString().toStdString());
    require(version.has_value(), "Existing system VST3 version is invalid");
    return version;
}

Path systemMsiExec()
{
    std::wstring directory(32768, L'\0');
    const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    require(length > 0 && length < directory.size(), winError("Cannot resolve System32"));
    directory.resize(length);
    const auto executable = Path(directory) / L"msiexec.exe";
    ensureNotReparsePoint(executable, "System Windows Installer");
    return executable;
}

DWORD launchMsi(const Path& msi, const Journal& journal, const std::wstring& arguments,
                bool elevate, std::chrono::minutes timeout)
{
    require(! kTestMode, "Test mode cannot launch Windows Installer or request elevation");
    Handle locked(CreateFileW(msi.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    require(locked.valid(), winError("Cannot lock MSI for Windows Installer"));
    verifyDownloadedMsi(msi, journal);
    const auto executable = systemMsiExec();
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = elevate ? L"runas" : nullptr;
    launch.lpFile = executable.c_str();
    launch.lpParameters = arguments.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (! ShellExecuteExW(&launch))
    {
        const auto error = GetLastError();
        if (elevate && error == ERROR_CANCELLED)
            fail("Administrator authorization was canceled; the operation can be resumed");
        fail(winError("Cannot start Windows Installer", error));
    }
    Handle process(launch.hProcess);
    const auto wait = WaitForSingleObject(process.get(), static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
    require(wait != WAIT_TIMEOUT, "Windows Installer is still running; leave hosts closed and resume verification later");
    require(wait == WAIT_OBJECT_0, winError("Cannot wait for Windows Installer"));
    DWORD exitCode{};
    require(GetExitCodeProcess(process.get(), &exitCode), winError("Cannot read Windows Installer result"));
    return exitCode;
}

std::wstring quote(const Path& path)
{
    auto value = path.wstring();
    require(value.find(L'"') == std::wstring::npos, "Unsafe quote in updater path");
    return L"\"" + value + L"\"";
}

void removePrivateTreeIfPresent(const Path& path)
{
    if (! std::filesystem::exists(path)) return;
    ensureNotReparsePoint(path, "Private extraction directory");
    for (std::filesystem::recursive_directory_iterator iterator(path), end; iterator != end; ++iterator)
        ensureNotReparsePoint(iterator->path(), "Private extraction entry");
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(! error, "Cannot clear the updater's private extraction directory");
}

void administrativeExtract(const Path& msi, const Journal& journal, const Path& extraction)
{
    auto privateTempLock = lockDirectoryAgainstReplacement(extraction.parent_path(),
                                                            "Private extraction parent");
    removePrivateTreeIfPresent(extraction);
    createPrivateDirectory(extraction, true);
    auto extractionLock = lockDirectoryAgainstReplacement(extraction, "Private extraction directory");
    const auto log = extraction.parent_path() / L"administrative-extract.log";
    const auto arguments = L"/a " + quote(msi) + L" /qn TARGETDIR=" + quote(extraction)
                         + L" /L*V " + quote(log);
    // An administrative image is a read/extract operation into our private
    // user directory. It deliberately runs unelevated; only `/i` may request UAC.
    const auto result = launchMsi(msi, journal, arguments, false, std::chrono::minutes(10));
    require(result == ERROR_SUCCESS || result == ERROR_SUCCESS_REBOOT_REQUIRED,
            "Administrative MSI extraction failed with code " + std::to_string(result));
}

DWORD installMsi(const Path& msi, const Journal& journal)
{
    // Do not give elevated msiexec a log path below this unelevated user's
    // operation directory. A same-user process could otherwise replace a
    // not-yet-created leaf with a reparse point while the UAC prompt is open
    // and make the elevated process truncate an unrelated privileged target.
    const auto arguments = L"/i " + quote(msi);
    const auto result = launchMsi(msi, journal, arguments, true, std::chrono::minutes(30));
    require(result == ERROR_SUCCESS || result == ERROR_SUCCESS_REBOOT_REQUIRED,
            "Windows Installer failed or was canceled with code " + std::to_string(result));
    return result;
}

Path currentExecutable()
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length > 0 && length < buffer.size(), winError("Cannot resolve updater executable"));
    buffer.resize(length);
    const auto executable = std::filesystem::weakly_canonical(Path(buffer));
    ensureNotReparsePoint(executable, "Updater executable");
    return executable;
}

std::optional<std::string> copiedUpdaterOperationId(const Path& root, const Path& executable)
{
    if (! equalInsensitive(executable.filename().wstring(),
                           widen(kProduct) + L"Updater.exe"))
        return std::nullopt;
    const auto operation = executable.parent_path();
    if (! equalInsensitive(operation.parent_path().lexically_normal().wstring(),
                           root.lexically_normal().wstring()))
        return std::nullopt;
    const auto id = upperAscii(narrow(operation.filename().wstring()));
    return isOperationId(id) ? std::optional<std::string>(id) : std::nullopt;
}

void launchCopiedUpdater(const Path& executable, const Path& source,
                         std::string_view operationId,
                         const std::optional<ReleaseGateInvocation>& releaseGate,
                         Handle& launchedProcess)
{
    ensureNotReparsePoint(executable, "Copied updater executable");
    Handle locked(CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(locked.valid(), winError("Cannot lock copied updater before launch"));
    BY_HANDLE_FILE_INFORMATION information{};
    require(GetFileInformationByHandle(locked.get(), &information),
            winError("Cannot inspect copied updater before launch"));
    require((information.dwFileAttributes
                & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0,
            "Copied updater is not a regular non-reparse file");
    verifyAuthenticodeCurrentSigner(executable);
    require(hashFile(source) == hashFile(executable),
            "Copied updater differs from its source executable");

    auto arguments = L"--resume " + widen(operationId);
    if (releaseGate)
    {
        arguments += L" --release-gate --challenge " + widen(releaseGate->challenge)
                   + L" --response-pipe " + widen(releaseGate->responsePipe)
                   + L" --parent-process-id " + std::to_wstring(releaseGate->parentProcessId)
                   + L" --release-id " + std::to_wstring(releaseGate->identity.releaseId)
                   + L" --tag " + widen(releaseGate->identity.tag)
                   + L" --source-commit " + widen(releaseGate->identity.sourceCommit)
                   + L" --parent-process-created-at-filetime "
                   + std::to_wstring(releaseGate->parentProcessCreatedAtFiletime)
                   + L" --expires-at-unix-seconds "
                   + std::to_wstring(releaseGate->expiresAtUnixSeconds)
                   + L" --authorization-signature-p1363 "
                   + widen(releaseGate->authorizationSignatureP1363);
    }
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpFile = executable.c_str();
    launch.lpParameters = arguments.c_str();
    launch.nShow = SW_SHOWNORMAL;
    require(ShellExecuteExW(&launch), winError("Cannot launch copied updater"));
    launchedProcess.reset(launch.hProcess);
    require(launchedProcess.valid(), "Copied updater launch returned no process handle");
}

class ProductMutex
{
public:
    ProductMutex()
    {
        const auto name = L"Local\\WhykikiAudio." + widen(kProduct) + L".WindowsUpdater";
        handle.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        require(handle.valid(), winError("Cannot create updater process lock"));
        const auto result = WaitForSingleObject(handle.get(), 10000);
        require(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED,
                "Another updater operation is active; try again after it finishes");
        acquired = true;
    }
    void release()
    {
        if (acquired)
        {
            require(ReleaseMutex(handle.get()), winError("Cannot release updater process lock"));
            acquired = false;
        }
    }
    ~ProductMutex() { if (acquired) ReleaseMutex(handle.get()); }
private:
    Handle handle;
    bool acquired{};
};

constexpr std::string_view kBuildContractSchema =
    "whykiki.windows-updater-build-contract";
constexpr std::string_view kBuildContractPipePrefix =
    "WhykikiAudio.UpdaterBuildContract.";
constexpr std::string_view kReleaseGateSchema =
    "whykiki.windows-updater-release-gate";
constexpr std::string_view kReleaseGatePipePrefix =
    "WhykikiAudio.UpdaterReleaseGate.";

bool isSafeBuildIdentityText(std::string_view value)
{
    if (value.empty() || value.size() > 100)
        return false;
    return std::all_of(value.begin(), value.end(), [] (unsigned char character)
    {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == ' ' || character == '-' || character == '_' || character == '.';
    });
}

bool isBuildContractChallenge(std::string_view value)
{
    return isSha256Hex(value);
}

bool isBuildContractPipeName(std::string_view value)
{
    if (! value.starts_with(kBuildContractPipePrefix)
        || value.size() != kBuildContractPipePrefix.size() + 32)
        return false;
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(kBuildContractPipePrefix.size()),
                       value.end(), isHex);
}

bool isReleaseGatePipeName(std::string_view value)
{
    return isCanonicalReleaseGatePipeName(value);
}

std::optional<DWORD> parseBuildContractProcessId(std::string_view value)
{
    if (value.empty() || (value.size() > 1 && value.front() == '0'))
        return std::nullopt;
    unsigned long parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0
        || parsed > std::numeric_limits<DWORD>::max())
        return std::nullopt;
    return static_cast<DWORD>(parsed);
}

bool compiledBuildIdentityMatches(std::string_view product,
                                  std::string_view version,
                                  std::string_view manufacturer,
                                  std::string_view githubOwner,
                                  std::string_view githubRepository,
                                  std::string_view architecture,
                                  std::string_view upgradeCode,
                                  std::string_view otherUpgradeCode,
                                  std::string_view currentSignerSha256,
                                  std::string_view nextSignerSha256,
                                  std::string_view releaseGatePublicKeyXY = kReleaseGatePublicKeyXY,
                                  std::string_view releaseGateNextPublicKeyXY = kReleaseGateNextPublicKeyXY)
{
    const auto parsedVersion = parseVersion(kInstalledVersion);
    const auto configured = isSafeRepositoryComponent(kProduct)
        && isSafeBuildIdentityText(kManufacturer)
        && isSafeRepositoryComponent(kOwner) && isSafeRepositoryComponent(kRepository)
        && parsedVersion.has_value()
        && toString(*parsedVersion) == kInstalledVersion
        && isCanonicalGuid(kUpgradeCode) && isCanonicalGuid(kOtherUpgradeCode)
        && kUpgradeCode != kOtherUpgradeCode
        && (kTestMode ? kCurrentSignerSha256.empty() || isSha256Hex(kCurrentSignerSha256)
                      : isSha256Hex(kCurrentSignerSha256))
        && (kNextSignerSha256.empty() || isSha256Hex(kNextSignerSha256))
        && (kNextSignerSha256.empty()
            || ! constantTimeEqual(upperAscii(std::string(kCurrentSignerSha256)),
                                   upperAscii(std::string(kNextSignerSha256))))
        && isP256PublicKeyXYHex(kReleaseGatePublicKeyXY)
        && p256PublicKeyIsOnCurve(kReleaseGatePublicKeyXY)
        && (kReleaseGateNextPublicKeyXY.empty()
            || (isP256PublicKeyXYHex(kReleaseGateNextPublicKeyXY)
                && kReleaseGateNextPublicKeyXY != kReleaseGatePublicKeyXY
                && p256PublicKeyIsOnCurve(kReleaseGateNextPublicKeyXY)));
    return configured
        && product == kProduct
        && version == kInstalledVersion
        && manufacturer == kManufacturer
        && githubOwner == kOwner
        && githubRepository == kRepository
        && architecture == architectureAssetSuffix(kArchitecture)
        && upgradeCode == kUpgradeCode
        && otherUpgradeCode == kOtherUpgradeCode
        && upperAscii(std::string(currentSignerSha256))
            == upperAscii(std::string(kCurrentSignerSha256))
        && upperAscii(std::string(nextSignerSha256))
            == upperAscii(std::string(kNextSignerSha256))
        && releaseGatePublicKeyXY == kReleaseGatePublicKeyXY
        && releaseGateNextPublicKeyXY == kReleaseGateNextPublicKeyXY;
}

bool buildContractMatches(std::string_view product,
                          std::string_view version,
                          std::string_view manufacturer,
                          std::string_view githubOwner,
                          std::string_view githubRepository,
                          std::string_view architecture,
                          std::string_view upgradeCode,
                          std::string_view otherUpgradeCode,
                          std::string_view currentSignerSha256,
                          std::string_view nextSignerSha256,
                          std::string_view releaseGatePublicKeyXY = kReleaseGatePublicKeyXY,
                          std::string_view releaseGateNextPublicKeyXY = kReleaseGateNextPublicKeyXY)
{
    // A test-mode executable can never certify a distribution build, even if a
    // caller supplies values matching all of its other compile definitions.
    return ! kTestMode && ! kCompileOnly
        && compiledBuildIdentityMatches(product, version, manufacturer, githubOwner,
                                        githubRepository, architecture, upgradeCode,
                                        otherUpgradeCode, currentSignerSha256,
                                        nextSignerSha256, releaseGatePublicKeyXY,
                                        releaseGateNextPublicKeyXY);
}

std::string canonicalBuildContractResponse(std::string_view challenge, DWORD serverProcessId)
{
    return "{\"schema\":\"" + std::string(kBuildContractSchema)
        + "\",\"schemaVersion\":3,\"challenge\":\"" + upperAscii(std::string(challenge))
        + "\",\"serverProcessId\":" + std::to_string(serverProcessId)
        + ",\"buildMode\":\"production\",\"compileOnly\":false,\"product\":\""
        + std::string(kProduct) + "\",\"version\":\"" + std::string(kInstalledVersion)
        + "\",\"manufacturer\":\"" + std::string(kManufacturer)
        + "\",\"githubOwner\":\"" + std::string(kOwner)
        + "\",\"githubRepository\":\"" + std::string(kRepository)
        + "\",\"architecture\":\"" + architectureAssetSuffix(kArchitecture)
        + "\",\"upgradeCode\":\"" + std::string(kUpgradeCode)
        + "\",\"otherUpgradeCode\":\"" + std::string(kOtherUpgradeCode)
        + "\",\"currentSignerSha256\":\"" + upperAscii(std::string(kCurrentSignerSha256))
        + "\",\"nextSignerSha256\":\"" + upperAscii(std::string(kNextSignerSha256))
        + "\",\"releaseGatePublicKeyXY\":\"" + std::string(kReleaseGatePublicKeyXY)
        + "\",\"releaseGateNextPublicKeyXY\":\"" + std::string(kReleaseGateNextPublicKeyXY)
        + "\"}\n";
}

void writeBuildContractResponse(std::string_view pipeName,
                                std::string_view challenge,
                                DWORD expectedServerProcessId)
{
    require(isBuildContractPipeName(pipeName), "Invalid build-contract response pipe name");
    const auto endpoint = L"\\\\.\\pipe\\" + widen(pipeName);
    Handle pipe(CreateFileW(endpoint.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                            nullptr));
    require(pipe.valid(), winError("Cannot connect to build-contract response pipe"));
    ULONG serverProcessId{};
    require(GetNamedPipeServerProcessId(pipe.get(), &serverProcessId),
            winError("Cannot identify build-contract response pipe server"));
    require(serverProcessId == expectedServerProcessId,
            "Build-contract response pipe belongs to a different process");
    const auto response = canonicalBuildContractResponse(challenge, expectedServerProcessId);
    require(response.size() <= 4096, "Build-contract response exceeds its fixed transport bound");
    writeAll(pipe.get(), response.data(), response.size());
}

// Evaluated before validation UI, mutexes, filesystem setup or networking. A
// production helper must answer a fresh challenge over the private named pipe;
// neither a zero exit code nor console output is treated as attestation.
std::optional<int> validateBuildContractCommandLine() noexcept
{
    try
    {
        int count{};
        auto** raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (raw == nullptr) return 3;
        struct Args { wchar_t** value; ~Args() { LocalFree(value); } } arguments { raw };
        if (count < 2 || std::wstring_view(raw[1]) != L"--validate-build-contract")
            return std::nullopt;
        constexpr std::array<std::wstring_view, 15> flags {
            L"--challenge", L"--response-pipe", L"--parent-process-id",
            L"--product", L"--version", L"--manufacturer", L"--github-owner",
            L"--github-repository", L"--architecture", L"--upgrade-code",
            L"--other-upgrade-code", L"--current-signer-sha256", L"--next-signer-sha256",
            L"--release-gate-public-key-xy", L"--release-gate-next-public-key-xy"
        };
        if (count != 2 + static_cast<int>(flags.size()) * 2) return 2;
        std::array<std::string, flags.size()> values;
        for (std::size_t index = 0; index < flags.size(); ++index)
        {
            if (std::wstring_view(raw[2 + index * 2]) != flags[index]) return 2;
            values[index] = narrow(raw[3 + index * 2]);
        }
        const auto parentProcessId = parseBuildContractProcessId(values[2]);
        if (! isBuildContractChallenge(values[0]) || ! isBuildContractPipeName(values[1])
            || ! parentProcessId
            || ! buildContractMatches(values[3], values[4], values[5], values[6], values[7],
                                       values[8], values[9], values[10], values[11], values[12],
                                       values[13], values[14]))
            return 3;
        writeBuildContractResponse(values[1], values[0], *parentProcessId);
        return 0;
    }
    catch (...)
    {
        return 3;
    }
}

ReleaseGateAuthorizationFields releaseGateAuthorizationFields(
    const ReleaseGateInvocation& gate)
{
    const auto installed = parseVersion(kInstalledVersion);
    require(installed.has_value(), "Installed updater version is not canonical");
    return {
        std::string(kOwner),
        std::string(kRepository),
        std::string(kProduct),
        kArchitecture,
        *installed,
        gate.identity.releaseId,
        gate.identity.tag,
        gate.identity.sourceCommit,
        gate.challenge,
        gate.responsePipe,
        static_cast<std::uint32_t>(gate.parentProcessId),
        gate.parentProcessCreatedAtFiletime,
        gate.expiresAtUnixSeconds
    };
}

void validateReleaseGateAuthorization(const ReleaseGateInvocation& gate,
                                      std::uint64_t nowUnixSeconds)
{
    require(isReleaseGateExpiryValid(gate.expiresAtUnixSeconds, nowUnixSeconds),
            "Release-gate authorization is expired or exceeds its five-minute lifetime");
    const auto canonical = canonicalReleaseGateAuthorization(
        releaseGateAuthorizationFields(gate));
    require(canonical.has_value(), "Release-gate authorization fields are not canonical");
    const auto signature = decodeP256P1363SignatureHex(
        gate.authorizationSignatureP1363);
    require(signature.has_value(),
            "Release-gate authorization signature is malformed or non-canonical");
    const auto digest = sha256Bytes(*canonical);
    const P256PublicKey currentKey(kReleaseGatePublicKeyXY);
    const auto currentAccepted = currentKey.verifies(digest, *signature);
    auto nextAccepted = false;
    if (! kReleaseGateNextPublicKeyXY.empty())
    {
        const P256PublicKey nextKey(kReleaseGateNextPublicKeyXY);
        nextAccepted = nextKey.verifies(digest, *signature);
    }
    require(currentAccepted || nextAccepted,
            "Release-gate invocation is not authorized by a pinned P-256 key");
}

#if defined(WK_WINDOWS_UPDATER_TEST_MODE) && WK_WINDOWS_UPDATER_TEST_MODE
std::array<std::uint8_t, 64> normalizeTestSignatureLowS(
    std::array<std::uint8_t, 64> signature)
{
    constexpr std::string_view orderHex =
        "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551";
    constexpr std::string_view halfOrderHex =
        "7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A8";
    const auto order = decodeUpperHex<32>(orderHex);
    const auto halfOrder = decodeUpperHex<32>(halfOrderHex);
    const auto sBegin = signature.begin() + 32;
    if (! std::lexicographical_compare(halfOrder.begin(), halfOrder.end(),
                                      sBegin, signature.end()))
        return signature;

    unsigned int borrow{};
    for (std::size_t offset{}; offset < 32; ++offset)
    {
        const auto index = 31 - offset;
        auto difference = static_cast<int>(order[index])
                        - static_cast<int>(signature[32 + index])
                        - static_cast<int>(borrow);
        if (difference < 0)
        {
            difference += 256;
            borrow = 1;
        }
        else
        {
            borrow = 0;
        }
        signature[32 + index] = static_cast<std::uint8_t>(difference);
    }
    require(borrow == 0, "Test ECDSA signature scalar exceeds the P-256 order");
    return signature;
}

std::string correspondingHighSTestSignature(std::string_view lowSignatureHex)
{
    const auto decoded = decodeP256P1363SignatureHex(lowSignatureHex);
    require(decoded.has_value(), "Test low-S signature is not canonical");
    auto result = *decoded;
    constexpr std::string_view orderHex =
        "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551";
    const auto order = decodeUpperHex<32>(orderHex);
    unsigned int borrow{};
    for (std::size_t offset{}; offset < 32; ++offset)
    {
        const auto index = 31 - offset;
        auto difference = static_cast<int>(order[index])
                        - static_cast<int>(result[32 + index])
                        - static_cast<int>(borrow);
        if (difference < 0)
        {
            difference += 256;
            borrow = 1;
        }
        else
        {
            borrow = 0;
        }
        result[32 + index] = static_cast<std::uint8_t>(difference);
    }
    require(borrow == 0, "Cannot construct corresponding high-S test signature");
    return encodeP256P1363SignatureHex(result);
}

std::string signReleaseGateAuthorizationForTest(
    const ReleaseGateAuthorizationFields& fields,
    std::string_view publicKeyXY,
    std::string_view privateScalarHex)
{
    const auto canonical = canonicalReleaseGateAuthorization(fields);
    require(canonical.has_value(), "Test release-gate authorization is not canonical");
    const auto coordinates = decodeUpperHex<64>(publicKeyXY);
    const auto privateScalar = decodeUpperHex<32>(privateScalarHex);
    struct PrivateBlob
    {
        BCRYPT_ECCKEY_BLOB header;
        std::array<unsigned char, 64> xy;
        std::array<unsigned char, 32> scalar;
    } blob { { BCRYPT_ECDSA_PRIVATE_P256_MAGIC, 32 }, coordinates, privateScalar };
    static_assert(sizeof(PrivateBlob) == sizeof(BCRYPT_ECCKEY_BLOB) + 96);
    BCryptAlgorithmHandle algorithm;
    BCryptKeyHandle key;
    require(BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                key.put(), reinterpret_cast<PUCHAR>(&blob),
                                static_cast<ULONG>(sizeof(blob)), 0) >= 0,
            "Cannot import test P-256 private key");
    const auto digest = sha256Bytes(*canonical);
    std::array<std::uint8_t, 64> signature{};
    ULONG signatureBytes{};
    require(BCryptSignHash(key.get(), nullptr,
                           const_cast<PUCHAR>(digest.data()),
                           static_cast<ULONG>(digest.size()), signature.data(),
                           static_cast<ULONG>(signature.size()), &signatureBytes, 0) >= 0
                && signatureBytes == static_cast<ULONG>(signature.size()),
            "Cannot create test P-256 P1363 signature");
    SecureZeroMemory(&blob, sizeof(blob));
    return encodeP256P1363SignatureHex(normalizeTestSignatureLowS(signature));
}
#endif

Invocation parseInvocationArguments(const std::vector<std::string>& arguments)
{
    Invocation result;
    if (arguments.empty()) return result;

    std::size_t index{};
    if (arguments[index] == "--resume")
    {
        require(arguments.size() >= 2, "Missing updater resume operation ID");
        const auto id = upperAscii(arguments[1]);
        require(isOperationId(id), "Invalid updater resume operation ID");
        result.resumeId = id;
        index = 2;
        if (index == arguments.size()) return result;
    }

    require(arguments[index] == "--release-gate", "Unsupported updater command line");
    ++index;
    constexpr std::array<std::string_view, 9> flags {
        "--challenge", "--response-pipe", "--parent-process-id",
        "--release-id", "--tag", "--source-commit",
        "--parent-process-created-at-filetime", "--expires-at-unix-seconds",
        "--authorization-signature-p1363"
    };
    require(arguments.size() - index == flags.size() * 2,
            "Release-gate command line has the wrong number of fields");
    std::array<std::string, flags.size()> values;
    for (std::size_t field{}; field < flags.size(); ++field)
    {
        require(arguments[index + field * 2] == flags[field],
                "Release-gate command-line fields are not in canonical order");
        values[field] = arguments[index + field * 2 + 1];
    }

    const auto parentProcessId = parseBuildContractProcessId(values[2]);
    const auto releaseId = parseCanonicalPositiveUint64(values[3]);
    const auto target = parseStableTag(values[4]);
    const auto installed = parseVersion(kInstalledVersion);
    const auto parentProcessCreatedAtFiletime = parseCanonicalPositiveUint64(values[6]);
    const auto expiresAtUnixSeconds = parseCanonicalPositiveUint64(values[7]);
    const auto signature = decodeP256P1363SignatureHex(values[8]);
    require(isSha256Hex(values[0]) && values[0] == upperAscii(values[0]),
            "Invalid or non-canonical release-gate challenge");
    require(isReleaseGatePipeName(values[1]), "Invalid release-gate response pipe name");
    require(parentProcessId.has_value(), "Invalid release-gate parent process ID");
    require(releaseId.has_value(), "Invalid release-gate release ID");
    require(target.has_value() && values[4] == "v" + toString(*target),
            "Invalid release-gate tag");
    require(installed.has_value() && isStrictlyNewer(*target, *installed),
            "Release-gate target must be newer than the installed helper version");
    require(isGitCommitHex(values[5]) && values[5] == lowerAscii(values[5]),
            "Invalid or non-canonical release-gate source commit");
    require(parentProcessCreatedAtFiletime.has_value(),
            "Invalid release-gate parent process creation FILETIME");
    require(expiresAtUnixSeconds.has_value(), "Invalid release-gate expiry");
    require(signature.has_value(),
            "Invalid, non-canonical or high-S release-gate P1363 signature");

    ReleaseGateInvocation gate;
    gate.challenge = values[0];
    gate.responsePipe = values[1];
    gate.parentProcessId = *parentProcessId;
    gate.identity.releaseId = *releaseId;
    gate.identity.tag = values[4];
    gate.identity.sourceCommit = values[5];
    gate.parentProcessCreatedAtFiletime = *parentProcessCreatedAtFiletime;
    gate.expiresAtUnixSeconds = *expiresAtUnixSeconds;
    gate.authorizationSignatureP1363 = values[8];
    result.releaseGate = std::move(gate);
    return result;
}

Invocation invocation()
{
    int count{};
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    require(arguments != nullptr, winError("Cannot parse command line"));
    struct Args { wchar_t** value; ~Args() { LocalFree(value); } } owner { arguments };
    std::vector<std::string> values;
    values.reserve(count > 1 ? static_cast<std::size_t>(count - 1) : 0u);
    for (int index = 1; index < count; ++index) values.push_back(narrow(arguments[index]));
    return parseInvocationArguments(values);
}

bool commandLineRequestsReleaseGate() noexcept
{
    int count{};
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return false;
    struct Args { wchar_t** value; ~Args() { LocalFree(value); } } owner { arguments };
    for (int index = 1; index < count; ++index)
        if (std::wstring_view(arguments[index]) == L"--release-gate") return true;
    return false;
}

std::string canonicalReleaseGateReceipt(const ReleaseGateInvocation& gate,
                                        const Journal& journal)
{
    require(journal.mode == OperationMode::releaseGate && journal.releaseGate
                && *journal.releaseGate == gate.identity,
            "Release-gate receipt identity does not match the verified journal");
    require(journal.phase == Phase::verified, "Release-gate receipt requires verified state");
    const auto target = parseStableTag(gate.identity.tag);
    require(target && journal.targetVersion == toString(*target),
            "Release-gate receipt target version does not match its release tag");
    require(isSha256Hex(journal.digest), "Release-gate receipt MSI digest is invalid");
    return "{\"schema\":\"" + std::string(kReleaseGateSchema)
        + "\",\"schemaVersion\":1,\"challenge\":\"" + gate.challenge
        + "\",\"serverProcessId\":" + std::to_string(gate.parentProcessId)
        + ",\"product\":\"" + std::string(kProduct)
        + "\",\"installedVersion\":\"" + std::string(kInstalledVersion)
        + "\",\"targetVersion\":\"" + journal.targetVersion
        + "\",\"architecture\":\"" + architectureAssetSuffix(kArchitecture)
        + "\",\"releaseId\":" + std::to_string(gate.identity.releaseId)
        + ",\"sourceCommit\":\"" + gate.identity.sourceCommit
        + "\",\"msiSha256\":\"" + upperAscii(journal.digest)
        + "\",\"phase\":\"verified\"}\n";
}

Handle connectReleaseGateResponsePipe(const ReleaseGateInvocation& gate)
{
    require(isReleaseGatePipeName(gate.responsePipe), "Invalid release-gate response pipe name");
    const auto endpoint = L"\\\\.\\pipe\\" + widen(gate.responsePipe);
    if (! WaitNamedPipeW(endpoint.c_str(), 30000))
        fail(winError("Release-gate response pipe is unavailable"));
    Handle pipe(CreateFileW(endpoint.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                            nullptr));
    require(pipe.valid(), winError("Cannot connect to release-gate response pipe"));
    ULONG serverProcessId{};
    require(GetNamedPipeServerProcessId(pipe.get(), &serverProcessId),
            winError("Cannot identify release-gate response pipe server"));
    require(serverProcessId == gate.parentProcessId,
            "Release-gate response pipe belongs to a different process");
    Handle serverProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     serverProcessId));
    require(serverProcess.valid(), winError("Cannot open release-gate pipe server process"));
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessTimes(serverProcess.get(), &creation, &exit, &kernel, &user),
            winError("Cannot read release-gate pipe server creation time"));
    require(fileTimeValue(creation) == gate.parentProcessCreatedAtFiletime,
            "Release-gate response pipe server is not the authorized process instance");
    return pipe;
}

void writeReleaseGateReceipt(HANDLE pipe,
                             const ReleaseGateInvocation& gate,
                             const Journal& journal)
{
    const auto receipt = canonicalReleaseGateReceipt(gate, journal);
    require(receipt.size() <= 4096, "Release-gate receipt exceeds its fixed transport bound");
    writeAll(pipe, receipt.data(), receipt.size());
    require(FlushFileBuffers(pipe), winError("Cannot flush release-gate receipt"));
}

std::string prepareOperation(const Path& root, const std::optional<std::string>& newest)
{
    if (newest)
    {
        const auto answer = taskDialog(widen(kProduct) + L" Update", L"Unvollständiges Update gefunden",
            L"Ja: sicher fortsetzen. Nein: einen neuen Vorgang beginnen. Es werden keine DAWs beendet.",
            TDCBF_YES_BUTTON | TDCBF_NO_BUTTON | TDCBF_CANCEL_BUTTON, TD_WARNING_ICON);
        if (answer == IDCANCEL) fail("Update canceled; the saved operation remains available");
        if (answer == IDYES) return *newest;
        // Once the user explicitly declines recovery, old valid incomplete
        // operations are no longer useful. Delete only those for which an
        // exclusive directory deletion lease can be acquired.
        (void) cleanupOldOperations(root, false);
    }
    const auto id = newOperationId();
    const auto operation = root / widen(id);
    createPrivateDirectory(operation, true);
    createPrivateDirectory(operation / L"private-temp", true);
    Journal journal;
    journal.operationId = id;
    writeJournal(operation, journal);
    return id;
}

std::string prepareReleaseGateOperation(const Path& root,
                                        const ReleaseGateIdentity& identity)
{
    const auto id = newOperationId();
    const auto operation = root / widen(id);
    createPrivateDirectory(operation, true);
    createPrivateDirectory(operation / L"private-temp", true);
    Journal journal;
    journal.operationId = id;
    journal.mode = OperationMode::releaseGate;
    journal.releaseGate = identity;
    writeJournal(operation, journal);
    return id;
}

void validateConfiguration()
{
    require(isSafeRepositoryComponent(kProduct) && isSafeRepositoryComponent(kOwner)
                && isSafeRepositoryComponent(kRepository), "Unsafe updater product/repository configuration");
    require(isSafeBuildIdentityText(kManufacturer), "Unsafe updater manufacturer configuration");
    require(parseVersion(kInstalledVersion).has_value(), "Installed updater version is not canonical");
    require(isCanonicalGuid(kUpgradeCode) && isCanonicalGuid(kOtherUpgradeCode)
                && kUpgradeCode != kOtherUpgradeCode,
            "Updater UpgradeCodes are missing, malformed or identical");
    if (! kTestMode)
        require(isSha256Hex(kCurrentSignerSha256), "Production current signer pin is invalid");
    require(kNextSignerSha256.empty() || isSha256Hex(kNextSignerSha256),
            "Optional next signer pin is invalid");
    require(kNextSignerSha256.empty()
                || ! constantTimeEqual(upperAscii(std::string(kCurrentSignerSha256)),
                                       upperAscii(std::string(kNextSignerSha256))),
            "Current and next signer pins must be distinct");
    require(isP256PublicKeyXYHex(kReleaseGatePublicKeyXY)
                && p256PublicKeyIsOnCurve(kReleaseGatePublicKeyXY),
            "Production release-gate current P-256 public key is invalid");
    require(kReleaseGateNextPublicKeyXY.empty()
                || (isP256PublicKeyXYHex(kReleaseGateNextPublicKeyXY)
                    && p256PublicKeyIsOnCurve(kReleaseGateNextPublicKeyXY)),
            "Optional release-gate next P-256 public key is invalid");
    require(kReleaseGateNextPublicKeyXY.empty()
                || kReleaseGateNextPublicKeyXY != kReleaseGatePublicKeyXY,
            "Current and next release-gate public keys must be distinct");
}

int worker(const Path& operation, Journal& journal,
           const std::optional<ReleaseGateInvocation>& releaseGate)
{
    const auto title = widen(kProduct) + L" Update";
    const auto headless = releaseGate.has_value();
    std::optional<Release> currentGateCandidate;
    if (releaseGate)
    {
        require(journal.releaseGate && *journal.releaseGate == releaseGate->identity,
                "Release-gate invocation does not match its journal");
        require(journal.phase == Phase::created,
                "Release-gate operations must start from a fresh one-shot journal");
        const auto installed = *parseVersion(kInstalledVersion);
        currentGateCandidate = parseReleaseGateCandidate(
            httpGetText(releaseByIdApiUrl(kOwner, kRepository,
                                          releaseGate->identity.releaseId)),
            installed, releaseGate->identity);
    }
    if (journal.phase == Phase::verified)
    {
        cleanupVerifiedPayload(operation, journal);
        if (! headless)
            taskDialog(title, L"Update bereits installiert und geprüft",
                       journal.lastError == "restart-required"
                           ? L"Die installierte VST3-Nutzlast wurde bereits vollständig geprüft. Windows verlangt einen Neustart."
                           : L"Die installierte VST3-Nutzlast wurde bereits vollständig geprüft.",
                       TDCBF_OK_BUTTON);
        return 0;
    }
    if (journal.phase == Phase::noUpdate)
    {
        if (! headless)
            taskDialog(title, L"Kein Update verfügbar",
                       L"Es ist keine neuere passende stabile Version verfügbar. Es wurde nichts installiert.",
                       TDCBF_OK_BUTTON);
        return 0;
    }
    if (journal.phase == Phase::created)
    {
        const auto installed = *parseVersion(kInstalledVersion);
        const auto system = installedSystemVersion();
        auto baseline = installed;
        std::optional<Release> release;
        if (releaseGate)
        {
            require(system.has_value() && *system == installed,
                    "Release gate requires the exact installed N system version");
            require(currentGateCandidate.has_value(),
                    "Release-gate candidate metadata is unavailable");
            release = *currentGateCandidate;
        }
        else
        {
            if (system && isStrictlyNewer(*system, baseline)) baseline = *system;
            release = parseNextRelease(
                httpGetText(releasesApiUrl(kOwner, kRepository)), baseline);
        }
        if (! release)
        {
            require(! releaseGate.has_value(), "Release-gate candidate metadata is missing");
            journal.phase = Phase::noUpdate;
            journal.lastError.clear();
            writeJournal(operation, journal);
            taskDialog(title, L"Kein Update verfügbar",
                       L"Es ist keine neuere passende stabile Version verfügbar. Ein Downgrade wird nicht angeboten.",
                       TDCBF_OK_BUTTON);
            return 0;
        }
        if (! headless)
        {
            const auto answer = taskDialog(title, L"Update verfügbar",
                widen("Installiert: " + toString(baseline) + "\nVerfügbar: " + toString(release->version)
                    + "\n\nDas MSI wird ausschließlich von der festgelegten GitHub-Release-Adresse geladen. "
                      "Escape bricht den Download ab; der Vorgang kann später fortgesetzt werden."),
                TDCBF_YES_BUTTON | TDCBF_NO_BUTTON);
            if (answer != IDYES) fail("Update canceled before download");
        }
        journal.targetVersion = toString(release->version);
        journal.assetUrl = release->url;
        journal.digest = release->digest;
        journal.size = release->size;
        journal.phase = Phase::metadata;
        journal.lastError.clear();
        writeJournal(operation, journal);
    }

    const auto target = *parseVersion(journal.targetVersion);
    const auto expectedMsi = operation / widen(expectedAssetName(kProduct, target, kArchitecture));
    if (journal.phase == Phase::metadata)
    {
        if (std::filesystem::exists(expectedMsi))
        {
            const auto [size, digest] = hashFile(expectedMsi);
            if (size != journal.size || ! constantTimeEqual(digest, journal.digest))
                require(DeleteFileW(expectedMsi.c_str()), winError("Cannot discard invalid prior download"));
        }
        if (! std::filesystem::exists(expectedMsi))
            downloadMsi({ target, journal.assetUrl, journal.digest, journal.size }, expectedMsi);
        journal.phase = Phase::downloaded;
        writeJournal(operation, journal);
    }

    const auto packageSigner = verifyDownloadedMsi(expectedMsi, journal);
    const auto extraction = operation / L"private-temp" / L"administrative-image";
    if (journal.phase == Phase::downloaded)
    {
        if (! headless)
            require(taskDialog(title, L"Updatepaket sicher prüfen",
                L"Das geprüfte MSI wird ohne Installation und ohne Administratorrechte in einen privaten "
                 L"Prüfordner extrahiert. Escape oder Abbrechen beendet diesen Schritt; der Vorgang bleibt fortsetzbar.",
                TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_WARNING_ICON) == IDOK,
                "Update canceled before administrative extraction");
        administrativeExtract(expectedMsi, journal, extraction);
        const auto extractedBundle = findSingleVst3(extraction);
        const auto fingerprint = fingerprintBundle(extractedBundle, target, packageSigner);
        validateAdministrativeImage(extraction, extractedBundle, expectedMsi, fingerprint);
        journal.phase = Phase::extracted;
        writeJournal(operation, journal);
    }

    const auto extractedBundle = findSingleVst3(extraction);
    const auto expectedFingerprint = fingerprintBundle(extractedBundle, target, packageSigner);
    validateAdministrativeImage(extraction, extractedBundle, expectedMsi, expectedFingerprint);
    if (journal.phase == Phase::extracted)
    {
        const auto systemVersion = installedSystemVersion();
        if (systemVersion && *systemVersion == target)
        {
            require(fingerprintBundle(installedBundlePath(), target, packageSigner) == expectedFingerprint,
                    "A same-version system VST3 differs from this verified update; overwrite refused");
            // The prior process may have ended after MSI success but before its
            // journal transition.  Exact payload equality makes this recovery
            // unambiguous and avoids launching the same installer twice.
            journal.phase = Phase::installed;
            journal.lastError.clear();
            writeJournal(operation, journal);
        }
        else
        {
            require(! systemVersion || isStrictlyNewer(target, *systemVersion),
                    "The system VST3 became newer while this operation was paused; downgrade refused");
            if (! headless)
                require(taskDialog(title, L"Geprüftes Update installieren",
                    L"MSI, Herausgeber, Architektur, Installer-Tabellen und die vollständige VST3-Nutzlast "
                     L"wurden geprüft. Lassen Sie alle DAWs geschlossen und starten Sie jetzt den sichtbaren Windows Installer. "
                     L"Der Updater beendet keine Programme und fragt niemals selbst nach einem Passwort; nur Windows kann eine UAC-Bestätigung anzeigen.",
                    TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_WARNING_ICON) == IDOK,
                    "Update canceled before installation");
            const auto result = installMsi(expectedMsi, journal);
            journal.phase = Phase::installed;
            journal.lastError = result == ERROR_SUCCESS_REBOOT_REQUIRED ? "restart-required" : "";
            writeJournal(operation, journal);
        }
    }

    require(constantTimeEqual(verifyDownloadedMsi(expectedMsi, journal), packageSigner),
            "MSI package signer changed during installation");
    require(fingerprintBundle(extractedBundle, target, packageSigner) == expectedFingerprint,
            "Administrative image changed during installation");
    const auto installedPath = installedBundlePath();
    require(fingerprintBundle(installedPath, target, packageSigner) == expectedFingerprint,
            "Installed system VST3 differs from the fully verified MSI payload");
    journal.phase = Phase::verified;
    writeJournal(operation, journal);
    cleanupVerifiedPayload(operation, journal);
    if (! headless)
        taskDialog(title, L"Update installiert und geprüft",
            journal.lastError == "restart-required"
                ? L"Die installierte VST3-Nutzlast stimmt vollständig überein. Windows verlangt einen Neustart."
                : L"Die installierte VST3-Nutzlast stimmt vollständig überein. Starten Sie die DAW neu und führen Sie einen Plugin-Rescan aus.",
            TDCBF_OK_BUTTON);
    return 0;
}

int worker(const Path& operation, Journal& journal)
{
    return worker(operation, journal, std::nullopt);
}
}

int runWindowsUpdater()
{
    if (const auto contractStatus = validateBuildContractCommandLine()) return *contractStatus;
    if (kCompileOnly) return 4;
    const auto releaseGateCommandLine = commandLineRequestsReleaseGate();
    try
    {
        validateConfiguration();
        require(! kTestMode, "This development/test updater cannot download, elevate or install");
        const auto arguments = invocation();
        const auto self = currentExecutable();
        verifyAuthenticodeCurrentSigner(self);
        if (arguments.releaseGate)
            validateReleaseGateAuthorization(*arguments.releaseGate, currentUnixSeconds());
        const auto root = localOperationsRoot();
        const auto selfOperation = copiedUpdaterOperationId(root, self);
        require(! selfOperation || ! arguments.resumeId || *selfOperation == *arguments.resumeId,
                "Copied updater operation does not match --resume");
        if (arguments.releaseGate)
        {
            if (selfOperation)
                require(arguments.resumeId.has_value(),
                        "A copied release-gate updater requires its explicit --resume operation ID");
            else
            {
                require(! arguments.resumeId.has_value(),
                        "The installed release-gate helper cannot resume an existing operation");
                require(equalInsensitive(self.wstring(), installedUpdaterPath().wstring()),
                        "Release gate must start from the exact installed N updater helper");
                const auto compiledVersion = parseVersion(kInstalledVersion);
                const auto systemVersion = installedSystemVersion();
                require(compiledVersion && systemVersion && *systemVersion == *compiledVersion,
                        "Release gate requires the exact installed N system version");
            }
        }
        const auto protectedOperation = selfOperation ? selfOperation : arguments.resumeId;
        std::optional<Handle> protectedOperationLock;
        if (protectedOperation)
            protectedOperationLock.emplace(lockDirectoryAgainstReplacement(
                root / widen(*protectedOperation), "Current updater operation"));
        ProductMutex mutex;
        const auto newest = cleanupOldOperations(
            root, arguments.releaseGate.has_value() || ! arguments.resumeId.has_value());
        const auto id = arguments.resumeId ? *arguments.resumeId
                      : arguments.releaseGate
                          ? prepareReleaseGateOperation(root, arguments.releaseGate->identity)
                          : prepareOperation(root, newest);
        const auto operation = root / widen(id);
        ensureNotReparsePoint(operation, "Updater operation");
        auto operationDirectoryLock = lockDirectoryAgainstReplacement(operation, "Updater operation");
        const auto copied = operation / (widen(kProduct) + L"Updater.exe");
        if (std::filesystem::exists(copied))
            ensureNotReparsePoint(copied, "Copied updater executable");
        if (! equalInsensitive(self.wstring(), std::filesystem::weakly_canonical(copied).wstring()))
        {
            if (! std::filesystem::exists(copied))
            {
                require(CopyFileW(self.c_str(), copied.c_str(), TRUE), winError("Cannot copy updater outside VST3"));
            }
            Handle child;
            launchCopiedUpdater(copied, self, id, arguments.releaseGate, child);
            if (arguments.releaseGate)
            {
                // The copied child owns the operation from here. Release the
                // product mutex that the child must acquire, but retain the
                // share-compatible directory leases so no competing process
                // can delete this exact operation before the child locks it.
                mutex.release();
                const auto wait = WaitForSingleObject(child.get(), INFINITE);
                require(wait == WAIT_OBJECT_0, winError("Cannot wait for copied release-gate updater"));
                DWORD exitCode{};
                require(GetExitCodeProcess(child.get(), &exitCode),
                        winError("Cannot read copied release-gate updater result"));
                require(exitCode == 0,
                        "Copied release-gate updater failed with code " + std::to_string(exitCode));
            }
            return 0;
        }

        Handle operationLock(CreateFileW((operation / L"operation.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                         0, nullptr, OPEN_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        require(operationLock.valid(), "This saved updater operation is already active");
        ensureNotReparsePoint(operation / L"operation.lock", "Updater operation lock");
        const auto expectedReleaseGate = arguments.releaseGate
            ? std::optional<ReleaseGateIdentity>(arguments.releaseGate->identity)
            : std::nullopt;
        auto journal = readJournal(operation, id, JournalReadPurpose::resume,
                                   expectedReleaseGate);
        std::optional<Handle> releaseGatePipe;
        if (arguments.releaseGate)
            releaseGatePipe.emplace(connectReleaseGateResponsePipe(*arguments.releaseGate));
        try
        {
            const auto result = arguments.releaseGate
                ? worker(operation, journal, arguments.releaseGate)
                : worker(operation, journal);
            if (arguments.releaseGate)
            {
                require(releaseGatePipe && releaseGatePipe->valid(),
                        "Release-gate response pipe is unavailable");
                writeReleaseGateReceipt(releaseGatePipe->get(), *arguments.releaseGate, journal);
            }
            return result;
        }
        catch (const std::exception& error)
        {
            journal.lastError = error.what();
            writeJournal(operation, journal);
            throw;
        }
    }
    catch (const std::exception& error)
    {
        if (! releaseGateCommandLine)
            taskDialog(widen(kProduct) + L" Update", L"Update nicht abgeschlossen",
                       widen(std::string(error.what())
                           + "\n\nEs wurde kein Erfolg gemeldet. Offline-, Abbruch- und Neustartfälle können "
                             "über den gespeicherten Vorgang erneut versucht werden."),
                       TDCBF_OK_BUTTON, TD_ERROR_ICON);
        return 1;
    }
}

#if defined(WK_WINDOWS_UPDATER_TEST_MODE) && WK_WINDOWS_UPDATER_TEST_MODE
int runWindowsUpdaterSelfTests()
{
    try
    {
        validateConfiguration();
        require(kTestMode, "Self-tests require test mode");
        require(compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                             kManufacturer, kOwner, kRepository,
                                             architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                             kOtherUpgradeCode, kCurrentSignerSha256,
                                             kNextSignerSha256),
                "Compiled build-contract identity does not match itself");
        require(! buildContractMatches(kProduct, kInstalledVersion,
                                       kManufacturer, kOwner, kRepository,
                                       architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                       kOtherUpgradeCode, kCurrentSignerSha256,
                                       kNextSignerSha256),
                "Test-mode executable was allowed to certify a distribution build");
        require(! compiledBuildIdentityMatches("WrongProduct", kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256),
                "Build-contract product mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               "Wrong Manufacturer", kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256),
                "Build-contract manufacturer mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, "WrongOwner", kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256),
                "Build-contract owner mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, "WrongRepository",
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256),
                "Build-contract repository mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kOtherUpgradeCode,
                                               kUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256),
                "Build-contract UpgradeCode mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, "255.255.65535",
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256),
                "Build-contract version mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               kArchitecture == Architecture::x64 ? "arm64ec" : "x64",
                                               kUpgradeCode, kOtherUpgradeCode,
                                               kCurrentSignerSha256, kNextSignerSha256),
                "Build-contract architecture mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, "00", kNextSignerSha256),
                "Build-contract current-signer mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256, "00"),
                "Build-contract next-signer mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256, "00",
                                               kReleaseGateNextPublicKeyXY),
                "Build-contract release-gate current-key mutation was accepted");
        require(! compiledBuildIdentityMatches(kProduct, kInstalledVersion,
                                               kManufacturer, kOwner, kRepository,
                                               architectureAssetSuffix(kArchitecture), kUpgradeCode,
                                               kOtherUpgradeCode, kCurrentSignerSha256,
                                               kNextSignerSha256, kReleaseGatePublicKeyXY, "00"),
                "Build-contract release-gate next-key mutation was accepted");
        require(p256PublicKeyIsOnCurve(kReleaseGatePublicKeyXY)
                    && (kReleaseGateNextPublicKeyXY.empty()
                        || p256PublicKeyIsOnCurve(kReleaseGateNextPublicKeyXY))
                    && ! p256PublicKeyIsOnCurve(std::string(128, '0')),
                "Build-contract release-gate keys did not enforce real P-256 curve points");
        const auto buildContractResponse = canonicalBuildContractResponse(
            std::string(64, 'A'), 42);
        require(buildContractResponse.find("\"schemaVersion\":3") != std::string::npos
                    && buildContractResponse.find("\"releaseGatePublicKeyXY\":\""
                        + std::string(kReleaseGatePublicKeyXY) + "\"") != std::string::npos
                    && buildContractResponse.find("\"releaseGateNextPublicKeyXY\":\""
                        + std::string(kReleaseGateNextPublicKeyXY) + "\"") != std::string::npos,
                "Build-contract response does not attest the exact release-gate key pair");
        require(isBuildContractChallenge(
                    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF")
                    && ! isBuildContractChallenge("predictable"),
                "Build-contract challenge validation failed");
        require(isBuildContractPipeName(
                    "WhykikiAudio.UpdaterBuildContract.0123456789ABCDEF0123456789ABCDEF")
                    && ! isBuildContractPipeName("\\\\.\\pipe\\attacker"),
                "Build-contract pipe-name validation failed");
        require(parseBuildContractProcessId("1") == DWORD { 1 }
                    && ! parseBuildContractProcessId("0")
                    && ! parseBuildContractProcessId("01"),
                "Build-contract process-ID validation failed");
        const auto expectFailure = [] (const std::function<void()>& action)
        {
            try { action(); }
            catch (const Failure&) { return true; }
            return false;
        };
        auto gateTarget = *parseVersion(kInstalledVersion);
        if (gateTarget.patch < 65535)
            ++gateTarget.patch;
        else if (gateTarget.minor < 255)
        {
            ++gateTarget.minor;
            gateTarget.patch = 0;
        }
        else
        {
            require(gateTarget.major < 255, "Test helper version leaves no valid newer MSI version");
            ++gateTarget.major;
            gateTarget.minor = 0;
            gateTarget.patch = 0;
        }
        const auto gateTag = "v" + toString(gateTarget);
        const std::string gateChallenge(64, 'A');
        const std::string gatePipe = std::string(kReleaseGatePipePrefix)
            + "0123456789ABCDEF0123456789ABCDEF";
        const std::string gateCommit = "0123456789abcdef0123456789abcdef01234567";
        constexpr std::uint64_t gateParentCreatedAt = 133444736000000000;
        constexpr std::uint64_t gateNow = 1700000000;
        constexpr std::uint64_t gateExpiresAt = gateNow + 300;
        const ReleaseGateAuthorizationFields gateAuthorization {
            std::string(kOwner), std::string(kRepository), std::string(kProduct),
            kArchitecture, *parseVersion(kInstalledVersion), 123456789, gateTag,
            gateCommit, gateChallenge, gatePipe, 42, gateParentCreatedAt,
            gateExpiresAt
        };
        const auto gateSignature = signReleaseGateAuthorizationForTest(
            gateAuthorization, kReleaseGatePublicKeyXY,
            "0000000000000000000000000000000000000000000000000000000000000001");
        const std::vector<std::string> gateArguments {
            "--release-gate",
            "--challenge", gateChallenge,
            "--response-pipe", gatePipe,
            "--parent-process-id", "42",
            "--release-id", "123456789",
            "--tag", gateTag,
            "--source-commit", gateCommit,
            "--parent-process-created-at-filetime", std::to_string(gateParentCreatedAt),
            "--expires-at-unix-seconds", std::to_string(gateExpiresAt),
            "--authorization-signature-p1363", gateSignature
        };
        const auto parsedGateInvocation = parseInvocationArguments(gateArguments);
        require(! parsedGateInvocation.resumeId && parsedGateInvocation.releaseGate
                    && parsedGateInvocation.releaseGate->challenge == gateChallenge
                    && parsedGateInvocation.releaseGate->responsePipe == gatePipe
                    && parsedGateInvocation.releaseGate->parentProcessId == 42
                    && parsedGateInvocation.releaseGate->parentProcessCreatedAtFiletime
                        == gateParentCreatedAt
                    && parsedGateInvocation.releaseGate->expiresAtUnixSeconds == gateExpiresAt
                    && parsedGateInvocation.releaseGate->authorizationSignatureP1363
                        == gateSignature
                    && parsedGateInvocation.releaseGate->identity.releaseId == 123456789
                    && parsedGateInvocation.releaseGate->identity.tag == gateTag
                    && parsedGateInvocation.releaseGate->identity.sourceCommit == gateCommit,
                "Canonical initial release-gate command line was not parsed exactly");
        validateReleaseGateAuthorization(*parsedGateInvocation.releaseGate, gateNow);
        auto gateResumeArguments = std::vector<std::string> {
            "--resume", "01234567-89AB-CDEF-0123-456789ABCDEF"
        };
        gateResumeArguments.insert(gateResumeArguments.end(), gateArguments.begin(), gateArguments.end());
        const auto parsedGateResume = parseInvocationArguments(gateResumeArguments);
        require(parsedGateResume.resumeId == "01234567-89AB-CDEF-0123-456789ABCDEF"
                    && parsedGateResume.releaseGate
                    && parsedGateResume.releaseGate->identity
                        == parsedGateInvocation.releaseGate->identity
                    && parsedGateResume.releaseGate->challenge
                        == parsedGateInvocation.releaseGate->challenge
                    && parsedGateResume.releaseGate->responsePipe
                        == parsedGateInvocation.releaseGate->responsePipe
                    && parsedGateResume.releaseGate->parentProcessId
                        == parsedGateInvocation.releaseGate->parentProcessId
                    && parsedGateResume.releaseGate->parentProcessCreatedAtFiletime
                        == parsedGateInvocation.releaseGate->parentProcessCreatedAtFiletime
                    && parsedGateResume.releaseGate->expiresAtUnixSeconds
                        == parsedGateInvocation.releaseGate->expiresAtUnixSeconds
                    && parsedGateResume.releaseGate->authorizationSignatureP1363
                        == parsedGateInvocation.releaseGate->authorizationSignatureP1363,
                "Canonical release-gate resume command line did not preserve every authorization field");
        require(! parseInvocationArguments({}).resumeId
                    && ! parseInvocationArguments({}).releaseGate
                    && parseInvocationArguments({ "--resume",
                         "01234567-89AB-CDEF-0123-456789ABCDEF" }).resumeId.has_value(),
                "Normal stable invocation forms changed");
        auto mutatedGateArguments = gateArguments;
        mutatedGateArguments[3] = "--parent-process-id";
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Reordered release-gate fields were accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[2] = "short";
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Malformed release-gate challenge was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[2] = std::string(64, 'a');
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical lowercase release-gate challenge was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[4].back() = 'a';
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical lowercase release-gate pipe token was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[6] = "01";
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical release-gate parent process ID was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[8] = "01";
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical release-gate release ID was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[10] = std::string(kInstalledVersion).insert(0, "v");
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-newer release-gate tag was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[12] = std::string(40, 'g');
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Malformed release-gate source commit was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[12][0] = 'A';
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical uppercase release-gate source commit was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[14] = "01";
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical parent creation FILETIME was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[16] = "01";
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Non-canonical release-gate expiry was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[18] = gateSignature.substr(2);
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Truncated release-gate P1363 signature was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[18] = lowerAscii(gateSignature);
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Lowercase release-gate P1363 signature was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments[18] = correspondingHighSTestSignature(gateSignature);
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Malleated high-S release-gate P1363 signature was accepted");
        mutatedGateArguments = gateArguments;
        mutatedGateArguments.insert(mutatedGateArguments.end(), { "--github-owner", "attacker" });
        require(expectFailure([&] { (void) parseInvocationArguments(mutatedGateArguments); }),
                "Release-gate repository override was accepted");
        require(isReleaseGatePipeName(gatePipe)
                    && ! isReleaseGatePipeName("WhykikiAudio.UpdaterReleaseGate.short")
                    && ! isReleaseGatePipeName("\\\\.\\pipe\\WhykikiAudio.UpdaterReleaseGate.0123456789ABCDEF0123456789ABCDEF"),
                "Release-gate pipe-name validation failed");
        require(expectFailure([&]
                {
                    validateReleaseGateAuthorization(*parsedGateInvocation.releaseGate,
                                                     gateNow - 1);
                })
                    && expectFailure([&]
                {
                    validateReleaseGateAuthorization(*parsedGateInvocation.releaseGate,
                                                     gateExpiresAt);
                }),
                "Release-gate authorization accepted a lifetime beyond five minutes or an expired token");
        auto nextKeyGate = *parsedGateInvocation.releaseGate;
        nextKeyGate.authorizationSignatureP1363 = signReleaseGateAuthorizationForTest(
            releaseGateAuthorizationFields(nextKeyGate), kReleaseGateNextPublicKeyXY,
            "0000000000000000000000000000000000000000000000000000000000000002");
        validateReleaseGateAuthorization(nextKeyGate, gateNow);

        const auto requireSignedFieldMutationRejected = [&] (ReleaseGateInvocation mutation)
        {
            require(expectFailure([&]
                    {
                        validateReleaseGateAuthorization(mutation, gateNow);
                    }),
                    "Mutation of a signed release-gate field retained authorization");
        };
        auto mutatedGate = *parsedGateInvocation.releaseGate;
        ++mutatedGate.identity.releaseId;
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        mutatedGate.identity.tag = "v1.4.2";
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        mutatedGate.identity.sourceCommit = std::string(40, 'f');
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        mutatedGate.challenge = std::string(64, 'B');
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        mutatedGate.responsePipe = std::string(kReleaseGatePipePrefix)
            + "FEDCBA9876543210FEDCBA9876543210";
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        ++mutatedGate.parentProcessId;
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        ++mutatedGate.parentProcessCreatedAtFiletime;
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        --mutatedGate.expiresAtUnixSeconds;
        requireSignedFieldMutationRejected(mutatedGate);
        mutatedGate = *parsedGateInvocation.releaseGate;
        mutatedGate.authorizationSignatureP1363[0] =
            mutatedGate.authorizationSignatureP1363[0] == '0' ? '1' : '0';
        requireSignedFieldMutationRejected(mutatedGate);
        const auto releaseJson = [] (const SemVersion& version, bool draft, bool prerelease,
                                     std::string_view immutableJson = "true")
        {
            const auto tag = "v" + toString(version);
            const auto name = expectedAssetName(kProduct, version, kArchitecture);
            const auto url = expectedAssetUrl(kOwner, kRepository, kProduct, version, kArchitecture);
            return std::string("{\"draft\":") + (draft ? "true" : "false")
                + ",\"prerelease\":" + (prerelease ? "true" : "false")
                + (immutableJson.empty()
                    ? std::string()
                    : ",\"immutable\":" + std::string(immutableJson))
                + ",\"tag_name\":\"" + tag + "\",\"html_url\":\"https://github.com/"
                + std::string(kOwner) + "/" + std::string(kRepository) + "/releases/tag/" + tag
                + "\",\"assets\":[{\"name\":\"" + name
                + "\",\"state\":\"uploaded\",\"browser_download_url\":\"" + url
                + "\",\"digest\":\"sha256:" + std::string(64, 'A') + "\",\"size\":42}]}";
        };
        const auto gateReleaseJson = [&] (std::uint64_t releaseId,
                                          bool draft,
                                          bool prerelease,
                                          bool immutable,
                                          std::string_view tag,
                                          std::string_view sourceCommit,
                                          std::string_view assetUrl)
        {
            const auto version = parseStableTag(tag);
            require(version.has_value(), "Release-gate JSON test tag is invalid");
            const auto name = expectedAssetName(kProduct, *version, kArchitecture);
            return std::string("{\"id\":") + std::to_string(releaseId)
                + ",\"draft\":" + (draft ? "true" : "false")
                + ",\"prerelease\":" + (prerelease ? "true" : "false")
                + ",\"immutable\":" + (immutable ? "true" : "false")
                + ",\"tag_name\":\"" + std::string(tag)
                + "\",\"target_commitish\":\"" + std::string(sourceCommit)
                + "\",\"html_url\":\"https://github.com/" + std::string(kOwner)
                + "/" + std::string(kRepository) + "/releases/tag/" + std::string(tag)
                + "\",\"assets\":[{\"name\":\"" + name
                + "\",\"state\":\"uploaded\",\"browser_download_url\":\""
                + std::string(assetUrl)
                + "\",\"digest\":\"sha256:" + std::string(64, 'A') + "\",\"size\":42}]}";
        };
        const auto baseline = *parseVersion("1.0.0");
        const auto nextRelease = parseNextRelease(
            "[" + releaseJson(*parseVersion("3.0.0"), false, false) + ","
                + releaseJson(*parseVersion("2.0.0"), false, true) + ","
                + releaseJson(*parseVersion("1.1.0"), false, false) + ","
                + releaseJson(*parseVersion("1.2.0"), false, false) + "]",
            baseline);
        require(nextRelease && nextRelease->version == *parseVersion("1.1.0"),
                "Bounded release history did not select the smallest stable newer version");
        const auto nextStableVersion = *parseVersion("1.1.0");
        for (const auto malformedImmutable : {
                 std::string_view {}, std::string_view { "false" },
                 std::string_view { "\"true\"" } })
            require(expectFailure([&]
                    {
                        (void) parseNextRelease(
                            "[" + releaseJson(nextStableVersion, false, false,
                                               malformedImmutable) + "]",
                            baseline);
                    }),
                    "Newer stable release with missing, false, or non-boolean immutable was accepted");
        const auto stableAfterLegacyMutable = parseNextRelease(
            "[" + releaseJson(baseline, false, false, "false") + ","
                + releaseJson(nextStableVersion, false, false) + "]",
            baseline);
        require(stableAfterLegacyMutable
                    && stableAfterLegacyMutable->version == nextStableVersion,
                "Legacy non-newer mutable history blocked a newer immutable stable release");
        const ReleaseGateIdentity gateIdentity { 123456789, gateTag, gateCommit };
        const auto canonicalGateAssetUrl = expectedAssetUrl(
            kOwner, kRepository, kProduct, gateTarget, kArchitecture);
        const auto canonicalGateJson = gateReleaseJson(
            gateIdentity.releaseId, false, true, true, gateIdentity.tag,
            gateIdentity.sourceCommit, canonicalGateAssetUrl);
        const auto parsedGateRelease = parseReleaseGateCandidate(
            canonicalGateJson, *parseVersion(kInstalledVersion), gateIdentity);
        require(parsedGateRelease.version == gateTarget
                    && parsedGateRelease.url == canonicalGateAssetUrl
                    && parsedGateRelease.digest == std::string(64, 'A')
                    && parsedGateRelease.size == 42,
                "Canonical immutable prerelease was not accepted by the exact-ID release gate");
        require(expectFailure([&]
                {
                    (void) parseReleaseGateCandidate(
                        gateReleaseJson(gateIdentity.releaseId + 1, false, true, true,
                                        gateIdentity.tag, gateIdentity.sourceCommit,
                                        canonicalGateAssetUrl),
                        *parseVersion(kInstalledVersion), gateIdentity);
                }),
                "Different release ID was accepted by the release gate");
        for (const auto flags : { std::array { true, true, true },
                                  std::array { false, false, true },
                                  std::array { false, true, false } })
            require(expectFailure([&]
                    {
                        (void) parseReleaseGateCandidate(
                            gateReleaseJson(gateIdentity.releaseId, flags[0], flags[1], flags[2],
                                            gateIdentity.tag, gateIdentity.sourceCommit,
                                            canonicalGateAssetUrl),
                            *parseVersion(kInstalledVersion), gateIdentity);
                    }),
                    "Wrong draft/prerelease/immutable state was accepted by the release gate");
        require(expectFailure([&]
                {
                    (void) parseReleaseGateCandidate(
                        gateReleaseJson(gateIdentity.releaseId, false, true, true,
                                        gateIdentity.tag, std::string(40, 'f'),
                                        canonicalGateAssetUrl),
                        *parseVersion(kInstalledVersion), gateIdentity);
                }),
                "Different source commit was accepted by the release gate");
        require(expectFailure([&]
                {
                    (void) parseReleaseGateCandidate(
                        gateReleaseJson(gateIdentity.releaseId, false, true, true,
                                        gateIdentity.tag, gateIdentity.sourceCommit,
                                        "https://github.com/TheWhykiki/Other/releases/download/v1.0.0/a.msi"),
                        *parseVersion(kInstalledVersion), gateIdentity);
                }),
                "Non-canonical release-gate asset URL was accepted");
        const auto stableBesideGate = parseNextRelease(
            "[" + canonicalGateJson + ","
                + releaseJson(gateTarget, false, false) + "]",
            *parseVersion(kInstalledVersion));
        require(stableBesideGate && stableBesideGate->version == gateTarget,
                "Normal update selection consumed or was confused by the release-gate prerelease");
        require(! parseNextRelease(
                    "[" + releaseJson(baseline, false, false) + ","
                        + releaseJson(*parseVersion("9.0.0"), true, false) + "]",
                    baseline),
                "Old, draft, or prerelease records produced an update candidate");
        require(std::wstring_view(kGithubApiHeaders).find(L"X-GitHub-Api-Version: 2026-03-10\r\n")
                    != std::wstring_view::npos
                    && std::wstring_view(kGithubApiHeaders).find(L"2022-11-28")
                        == std::wstring_view::npos,
                "GitHub REST API header does not pin the immutable-release schema version");
        bool duplicateTagDenied{};
        try
        {
            (void) parseNextRelease(
                "[" + releaseJson(*parseVersion("1.1.0"), false, false) + ","
                    + releaseJson(*parseVersion("1.1.0"), false, false) + "]",
                baseline);
        }
        catch (const Failure&) { duplicateTagDenied = true; }
        require(duplicateTagDenied, "Duplicate stable release tags were accepted");
        std::string fullPage = "[";
        for (int index = 0; index < 100; ++index)
        {
            if (index != 0) fullPage += ',';
            fullPage += releaseJson(*parseVersion("9.0.0"), true, false);
        }
        fullPage += ']';
        bool truncatedHistoryDenied{};
        try { (void) parseNextRelease(fullPage, baseline); }
        catch (const Failure&) { truncatedHistoryDenied = true; }
        require(truncatedHistoryDenied, "A full, potentially truncated release-history page was accepted");
        bool installationDenied{};
        try
        {
            Journal journal;
            launchMsi(L"this-file-must-never-be-opened.msi", journal, L"/i", true,
                      std::chrono::minutes(1));
        }
        catch (const Failure& error)
        {
            installationDenied = std::string_view(error.what()).find("Test mode") != std::string_view::npos;
        }
        require(installationDenied, "Test mode did not deny elevation before touching an MSI");
        const auto approvedUrl = parseHttpsUrl(
            L"https://api.github.com/repos/TheWhykiki/Test/releases?per_page=100");
        require(approvedUrl.host == L"api.github.com" && approvedUrl.port == INTERNET_DEFAULT_HTTPS_PORT,
                "Approved HTTPS URL parsing failed");
        bool maliciousHostDenied{};
        try
        {
            (void) parseHttpsUrl(L"https://api.github.com.attacker.invalid/payload.msi");
        }
        catch (const Failure&)
        {
            maliciousHostDenied = true;
        }
        require(maliciousHostDenied, "An unapproved redirect host was accepted");
        const auto executable = readPeInfo(currentExecutable());
        const auto expectedArchitecture = kArchitecture == Architecture::x64
            ? executable.machine == IMAGE_FILE_MACHINE_AMD64 && ! executable.chpe
            : ((executable.machine == IMAGE_FILE_MACHINE_AMD64 && executable.chpe)
                || executable.machine == 0xA641 || executable.machine == 0xA64E);
        require(expectedArchitecture, "Updater self-test executable architecture mismatch");
        require(expectedAssetName(kProduct, *parseVersion("2.3.4"), kArchitecture)
                    == std::string(kProduct) + "-2.3.4-Windows-"
                       + architectureAssetSuffix(kArchitecture) + ".msi",
                "Architecture asset contract failed");
        Journal receiptJournal;
        receiptJournal.mode = OperationMode::releaseGate;
        receiptJournal.releaseGate = gateIdentity;
        receiptJournal.phase = Phase::verified;
        receiptJournal.targetVersion = toString(gateTarget);
        receiptJournal.digest = std::string(64, 'B');
        const auto receipt = canonicalReleaseGateReceipt(
            *parsedGateInvocation.releaseGate, receiptJournal);
        const auto expectedReceipt = std::string("{\"schema\":\"")
            + std::string(kReleaseGateSchema)
            + "\",\"schemaVersion\":1,\"challenge\":\"" + std::string(64, 'A')
            + "\",\"serverProcessId\":42,\"product\":\"" + std::string(kProduct)
            + "\",\"installedVersion\":\"" + std::string(kInstalledVersion)
            + "\",\"targetVersion\":\"" + toString(gateTarget)
            + "\",\"architecture\":\"" + architectureAssetSuffix(kArchitecture)
            + "\",\"releaseId\":123456789,\"sourceCommit\":\"" + gateCommit
            + "\",\"msiSha256\":\"" + std::string(64, 'B')
            + "\",\"phase\":\"verified\"}\n";
        require(receipt == expectedReceipt,
                "Release-gate receipt is not the canonical challenge/version/architecture/release/commit/digest attestation");

        wchar_t temporaryBuffer[MAX_PATH + 1]{};
        const auto length = GetTempPathW(static_cast<DWORD>(std::size(temporaryBuffer)), temporaryBuffer);
        require(length > 0 && length < std::size(temporaryBuffer), "Cannot resolve cleanup test root");
        const Path temporaryRoot(temporaryBuffer);
        auto temporaryRootLock = lockDirectoryAgainstReplacement(temporaryRoot, "Cleanup test root");
        const auto cleanupRoot = temporaryRoot / (L"WhykikiUpdaterCleanupTest-" + widen(newOperationId()));
        createPrivateDirectory(cleanupRoot, true);
        const juce::ScopeGuard removeFixture { [&]
        {
            (void) deleteChild(temporaryRoot, cleanupRoot.filename().wstring());
        } };

        const auto gateJournalId = newOperationId();
        const auto gateJournalOperation = cleanupRoot / widen(gateJournalId);
        createPrivateDirectory(gateJournalOperation, true);
        Journal gateJournal;
        gateJournal.operationId = gateJournalId;
        gateJournal.mode = OperationMode::releaseGate;
        gateJournal.releaseGate = gateIdentity;
        writeJournal(gateJournalOperation, gateJournal);
        const auto gateJournalText = fileUtf8(gateJournalOperation / L"journal.json", 64u * 1024u);
        require(gateJournalText.find("\"operationMode\": \"release-gate\"") != std::string::npos
                    && gateJournalText.find("\"releaseId\": \"123456789\"") != std::string::npos
                    && gateJournalText.find("\"challenge\"") == std::string::npos
                    && gateJournalText.find(gateChallenge) == std::string::npos
                    && gateJournalText.find(gatePipe) == std::string::npos
                    && gateJournalText.find(gateSignature) == std::string::npos
                    && gateJournalText.find("\"parentProcessId\"") == std::string::npos
                    && gateJournalText.find("\"parentProcessCreatedAtFiletime\"") == std::string::npos
                    && gateJournalText.find("\"expiresAtUnixSeconds\"") == std::string::npos
                    && gateJournalText.find("\"authorizationSignatureP1363\"") == std::string::npos,
                "Release-gate journal persisted transport secrets or omitted its non-secret identity");
        require(readJournal(gateJournalOperation, gateJournalId,
                            JournalReadPurpose::resume, gateIdentity).releaseGate == gateIdentity,
                "Fresh matching release-gate journal could not be consumed by its copied child");
        gateJournal.phase = Phase::verified;
        gateJournal.targetVersion = toString(gateTarget);
        gateJournal.assetUrl = canonicalGateAssetUrl;
        gateJournal.digest = std::string(64, 'A');
        gateJournal.size = 42;
        writeJournal(gateJournalOperation, gateJournal);
        require(expectFailure([&]
                {
                    (void) readJournal(gateJournalOperation, gateJournalId,
                                       JournalReadPurpose::resume, gateIdentity);
                }),
                "Persisted verified release-gate journal was replayable as a fresh acceptance");
        gateJournal.phase = Phase::created;
        gateJournal.targetVersion.clear();
        gateJournal.assetUrl.clear();
        gateJournal.digest.clear();
        gateJournal.size = 0;
        writeJournal(gateJournalOperation, gateJournal);
        require(expectFailure([&]
                {
                    (void) readJournal(gateJournalOperation, gateJournalId,
                                       JournalReadPurpose::resume);
                }),
                "Normal resume accepted a release-gate journal");
        auto mutatedGateIdentity = gateIdentity;
        ++mutatedGateIdentity.releaseId;
        require(expectFailure([&]
                {
                    (void) readJournal(gateJournalOperation, gateJournalId,
                                       JournalReadPurpose::resume, mutatedGateIdentity);
                }),
                "Release-gate resume accepted a mutated release identity");
        const auto normalJournalId = newOperationId();
        const auto normalJournalOperation = cleanupRoot / widen(normalJournalId);
        createPrivateDirectory(normalJournalOperation, true);
        Journal normalJournal;
        normalJournal.operationId = normalJournalId;
        writeJournal(normalJournalOperation, normalJournal);
        require(expectFailure([&]
                {
                    (void) readJournal(normalJournalOperation, normalJournalId,
                                       JournalReadPurpose::resume, gateIdentity);
                }),
                "Release-gate resume accepted a normal stable journal");
        require(deleteChild(cleanupRoot, gateJournalOperation.filename().wstring())
                    && deleteChild(cleanupRoot, normalJournalOperation.filename().wstring()),
                "Cannot remove release-gate journal-isolation fixtures");

        const auto makeOperation = [&] (Phase phase, int ageMinutes)
        {
            const auto id = newOperationId();
            const auto operation = cleanupRoot / widen(id);
            createPrivateDirectory(operation, true);
            Journal journal;
            journal.operationId = id;
            journal.phase = phase;
            if (phase != Phase::created && phase != Phase::noUpdate)
            {
                const auto target = *parseVersion("2.3.4");
                journal.targetVersion = toString(target);
                journal.assetUrl = expectedAssetUrl(kOwner, kRepository, kProduct,
                                                    target, kArchitecture);
                journal.digest = std::string(64, 'A');
                journal.size = 1;
            }
            writeJournal(operation, journal);
            std::error_code timeError;
            std::filesystem::last_write_time(operation / L"journal.json",
                std::filesystem::file_time_type::clock::now()
                    - std::chrono::minutes(ageMinutes), timeError);
            require(! timeError, "Cannot timestamp updater-cleanup fixture");
            return std::pair<std::string, Path>{ id, operation };
        };
        const auto rewriteWriterVersion = [&] (const Path& operation,
                                               std::string_view writerVersion,
                                               int ageMinutes)
        {
            require(parseVersion(writerVersion).has_value(),
                    "Historical cleanup fixture version is invalid");
            const auto text = fileUtf8(operation / L"journal.json", 64u * 1024u);
            auto parsed = juce::JSON::parse(
                juce::String::fromUTF8(text.data(), static_cast<int>(text.size())));
            auto* object = parsed.getDynamicObject();
            require(object != nullptr, "Cannot parse historical cleanup fixture");
            object->setProperty("installedVersion", juceString(writerVersion));
            const auto json = juce::JSON::toString(parsed, true);
            atomicWrite(operation / L"journal.json",
                        std::string(json.toRawUTF8(), json.getNumBytesAsUTF8()));
            std::error_code timeError;
            std::filesystem::last_write_time(operation / L"journal.json",
                std::filesystem::file_time_type::clock::now()
                    - std::chrono::minutes(ageMinutes), timeError);
            require(! timeError, "Cannot timestamp historical cleanup fixture");
        };

        const auto terminal = makeOperation(Phase::noUpdate, 40).second;
        const auto old = makeOperation(Phase::metadata, 30).second;
        const auto [newestId, newest] = makeOperation(Phase::downloaded, 10);
        const auto active = makeOperation(Phase::metadata, 20).second;
        const auto invalidOperation = cleanupRoot / widen(newOperationId());
        createPrivateDirectory(invalidOperation, true);
        atomicWrite(invalidOperation / L"journal.json", "not-json");
        auto activeLease = lockDirectoryAgainstReplacement(active, "Active cleanup fixture");
        Handle blockedDeletion(CreateFileW(active.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS
                                                | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const auto blockedDeletionError = GetLastError();
        require(! blockedDeletion.valid() && blockedDeletionError == ERROR_SHARING_VIOLATION,
                "Active updater directory lease did not block deletion");
        const auto retained = cleanupOldOperations(cleanupRoot, true);
        require(retained == newestId, "Cleanup did not select the newest resumable operation");
        require(! std::filesystem::exists(terminal), "Terminal operation was not removed");
        require(! std::filesystem::exists(old), "Old incomplete operation was not removed");
        require(std::filesystem::exists(newest), "Newest incomplete operation was not retained");
        require(std::filesystem::exists(active), "Active operation was not retained");
        require(std::filesystem::exists(invalidOperation),
                "Invalid operation was not retained fail-closed");
        activeLease.reset();
        Handle allowedDeletion(CreateFileW(active.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS
                                                | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        require(allowedDeletion.valid(), "Released updater directory lease still blocked deletion");
        allowedDeletion.reset();
        (void) cleanupOldOperations(cleanupRoot, true);
        require(! std::filesystem::exists(active), "Inactive old operation was retained");

        const auto historicalVersion = kInstalledVersion == "0.0.0" ? "0.0.1" : "0.0.0";
        const auto [historicalIncompleteId, historicalIncomplete] =
            makeOperation(Phase::metadata, 1);
        const auto historicalTerminal = makeOperation(Phase::verified, 2).second;
        const auto futureOperation = makeOperation(Phase::verified, 3).second;
        rewriteWriterVersion(historicalIncomplete, historicalVersion, 1);
        rewriteWriterVersion(historicalTerminal, historicalVersion, 2);
        rewriteWriterVersion(futureOperation, "255.255.65535", 3);
        bool historicalResumeDenied{};
        try
        {
            (void) readJournal(historicalIncomplete, historicalIncompleteId,
                               JournalReadPurpose::resume);
        }
        catch (const Failure&) { historicalResumeDenied = true; }
        require(historicalResumeDenied,
                "A journal from another helper version was accepted for recovery");
        const auto retainedAfterHistoricalCleanup = cleanupOldOperations(cleanupRoot, true);
        require(retainedAfterHistoricalCleanup == newestId
                    && ! std::filesystem::exists(historicalIncomplete)
                    && ! std::filesystem::exists(historicalTerminal)
                    && std::filesystem::exists(futureOperation),
                "Historical cleanup or future-version fail-closed retention failed");

        const auto [payloadId, payloadOperation] = makeOperation(Phase::verified, 5);
        const auto target = *parseVersion("2.3.4");
        const auto msi = payloadOperation
            / widen(expectedAssetName(kProduct, target, kArchitecture));
        atomicWrite(msi, "m");
        atomicWrite(payloadOperation / L"download.part", "p");
        createPrivateDirectory(payloadOperation / L"private-temp", true);
        createPrivateDirectory(payloadOperation / L"private-temp" / L"administrative-image", true);
        atomicWrite(payloadOperation / L"private-temp" / L"administrative-image" / L"payload.bin", "x");
        const auto copiedUpdater = payloadOperation / (widen(kProduct) + L"Updater.exe");
        atomicWrite(copiedUpdater, "u");
        const auto outside = cleanupRoot / L"outside";
        createPrivateDirectory(outside, true);
        atomicWrite(outside / L"sentinel.bin", "safe");
        require(CreateSymbolicLinkW((payloadOperation / L"private-temp" / L"escape").c_str(),
                                    outside.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY
                                        | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE),
                winError("Cannot create cleanup reparse fixture"));
        const auto unexpected = payloadOperation / L"unexpected.bin";
        atomicWrite(unexpected, "hold");
        const auto journalCaseTemporary = payloadOperation / L"journal-case.tmp";
        require(MoveFileExW((payloadOperation / L"journal.json").c_str(),
                            journalCaseTemporary.c_str(), MOVEFILE_WRITE_THROUGH)
                    && MoveFileExW(journalCaseTemporary.c_str(),
                                   (payloadOperation / L"JOURNAL.JSON").c_str(),
                                   MOVEFILE_WRITE_THROUGH),
                winError("Cannot create case-variant cleanup journal fixture"));
        Handle heldMsi(CreateFileW(msi.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Handle heldUnexpected(CreateFileW(unexpected.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(heldMsi.valid(), "Cannot lock cleanup MSI fixture");
        require(heldUnexpected.valid(), "Cannot lock unexpected cleanup fixture");
        const auto payloadJournal = readJournal(payloadOperation, payloadId, JournalReadPurpose::resume);
        cleanupVerifiedPayload(payloadOperation, payloadJournal);
        require(std::filesystem::exists(msi)
                    && ! std::filesystem::exists(payloadOperation / L"download.part")
                    && ! std::filesystem::exists(payloadOperation / L"private-temp")
                    && std::filesystem::exists(payloadOperation / L"journal.json")
                    && std::filesystem::exists(copiedUpdater)
                    && std::filesystem::exists(outside / L"sentinel.bin"),
                "Best-effort payload cleanup escaped containment or removed recovery state");
        (void) cleanupOldOperations(cleanupRoot, true);
        require(readJournal(payloadOperation, payloadId, JournalReadPurpose::resume).phase == Phase::verified
                    && std::filesystem::exists(copiedUpdater),
                "Locked MSI cleanup destroyed its valid recovery state");
        heldMsi.reset();
        (void) cleanupOldOperations(cleanupRoot, true);
        require(readJournal(payloadOperation, payloadId, JournalReadPurpose::resume).phase == Phase::verified
                    && std::filesystem::exists(copiedUpdater)
                    && std::filesystem::exists(unexpected),
                "Locked unexpected child destroyed its valid recovery state");
        heldUnexpected.reset();
        (void) cleanupOldOperations(cleanupRoot, true);
        require(! std::filesystem::exists(payloadOperation)
                    && std::filesystem::exists(outside / L"sentinel.bin"),
                "Unlocked operation was not fully removed or cleanup escaped containment");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Windows updater self-test failed: %s\n", error.what());
        return 1;
    }
    catch (...)
    {
        std::fputs("Windows updater self-test failed with an unknown exception\n", stderr);
        return 1;
    }
}
#endif
}
