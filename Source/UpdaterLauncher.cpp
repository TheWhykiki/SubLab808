#include "UpdaterLauncher.h"

#if JUCE_WINDOWS && WK_UPDATER_ENABLED

#define NOMINMAX 1
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WK_WINDOWS_UPDATER_SIGNER_SHA256
#error "Enabled Windows updater launcher requires its exact distribution signer SHA-256"
#endif

namespace wk
{
namespace
{
constexpr std::string_view expectedSigner = WK_WINDOWS_UPDATER_SIGNER_SHA256;

constexpr bool validSignerPin()
{
    if (expectedSigner.size() != 64) return false;
    for (const auto c : expectedSigner)
        if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}
static_assert(validSignerPin(), "Windows updater launcher signer pin must be 64 hexadecimal characters");

class Handle
{
public:
    explicit Handle(HANDLE valueToUse = INVALID_HANDLE_VALUE) : value(valueToUse) {}
    ~Handle() { if (valid()) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value; }
private:
    HANDLE value;
};

struct CertificateStore
{
    HCERTSTORE value{};
    ~CertificateStore() { if (value != nullptr) CertCloseStore(value, 0); }
};
struct CryptMessage
{
    HCRYPTMSG value{};
    ~CryptMessage() { if (value != nullptr) CryptMsgClose(value); }
};
struct Certificate
{
    PCCERT_CONTEXT value{};
    ~Certificate() { if (value != nullptr) CertFreeCertificateContext(value); }
};

juce::String windowsError(const char* context, DWORD code = GetLastError())
{
    wchar_t* buffer = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                           | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const juce::String detail(buffer != nullptr && length != 0 ? buffer : L"unknown error");
    if (buffer != nullptr) LocalFree(buffer);
    return juce::String(context) + " (" + juce::String(static_cast<juce::int64>(code)) + "): " + detail.trimEnd();
}

juce::Result loadedBundle(std::filesystem::path& bundle)
{
    static const int moduleAnchor = 0;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&moduleAnchor), &module) == FALSE)
        return juce::Result::fail(windowsError("The loaded plugin module could not be identified"));

    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return juce::Result::fail(windowsError("The installed plugin location could not be determined"));

    const auto binary = std::filesystem::path(std::wstring(path.data(), length));
    bundle = binary.parent_path().parent_path().parent_path();
    if (bundle.extension() != L".vst3")
        return juce::Result::fail("The loaded module is not inside a complete VST3 bundle.");
    return juce::Result::ok();
}

juce::Result finalPathFromHandle(HANDLE file, std::filesystem::path& finalPath)
{
    const auto needed = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) return juce::Result::fail(windowsError("The updater path could not be resolved"));
    std::vector<wchar_t> path(static_cast<std::size_t>(needed) + 1u);
    const auto written = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= path.size())
        return juce::Result::fail(windowsError("The updater path could not be resolved"));
    finalPath = std::filesystem::path(std::wstring(path.data(), written));
    return juce::Result::ok();
}

juce::Result verifySigner(const std::filesystem::path& helper)
{
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = helper.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    // Keep the DAW UI path strictly local: validate the signed bytes, timestamp
    // and locally available certificate chain, then bind the exact leaf below.
    // Requiring revocation data from a cache would reject valid first-run/offline
    // systems when no CRL/OCSP response has been cached yet. The standalone
    // updater repeats WinVerifyTrust with online whole-chain revocation before it
    // copies itself, downloads anything or starts Windows Installer.
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &fileInfo;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL
                      | WTD_DISABLE_MD2_MD4;
    trust.dwUIContext = WTD_UICONTEXT_EXECUTE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto nonInteractive = reinterpret_cast<HWND>(INVALID_HANDLE_VALUE);
    const auto status = WinVerifyTrust(nonInteractive, &action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nonInteractive, &action, &trust);
    if (status != ERROR_SUCCESS)
        return juce::Result::fail(windowsError(
            "Windows rejected the updater's local Authenticode signature or certificate chain",
            static_cast<DWORD>(status)));

    DWORD encoding{}, content{}, format{};
    CertificateStore store;
    CryptMessage message;
    if (! CryptQueryObject(CERT_QUERY_OBJECT_FILE, helper.c_str(),
                           CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                           CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format,
                           &store.value, &message.value, nullptr))
        return juce::Result::fail(windowsError("The updater signer could not be read"));

    DWORD signerBytes{};
    if (! CryptMsgGetParam(message.value, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerBytes))
        return juce::Result::fail(windowsError("The updater signer could not be sized"));
    std::vector<unsigned char> signerStorage(signerBytes);
    if (! CryptMsgGetParam(message.value, CMSG_SIGNER_INFO_PARAM, 0, signerStorage.data(), &signerBytes))
        return juce::Result::fail(windowsError("The updater signer could not be read"));
    const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(signerStorage.data());
    CERT_INFO certificateInfo{};
    certificateInfo.Issuer = signer->Issuer;
    certificateInfo.SerialNumber = signer->SerialNumber;
    Certificate certificate;
    certificate.value = CertFindCertificateInStore(store.value,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr);
    if (certificate.value == nullptr)
        return juce::Result::fail(windowsError("The updater certificate could not be resolved"));

    std::array<unsigned char, 32> digest{};
    DWORD digestBytes = static_cast<DWORD>(digest.size());
    if (! CertGetCertificateContextProperty(certificate.value, CERT_SHA256_HASH_PROP_ID,
                                             digest.data(), &digestBytes)
        || digestBytes != digest.size())
        return juce::Result::fail(windowsError("The updater certificate could not be hashed"));

    constexpr char alphabet[] = "0123456789ABCDEF";
    std::array<char, 64> actual{};
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        actual[index * 2] = alphabet[digest[index] >> 4];
        actual[index * 2 + 1] = alphabet[digest[index] & 0x0f];
    }
    unsigned int mismatch{};
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        auto expected = expectedSigner[index];
        if (expected >= 'a' && expected <= 'f') expected = static_cast<char>(expected - ('a' - 'A'));
        mismatch |= static_cast<unsigned char>(actual[index] ^ expected);
    }
    return mismatch == 0 ? juce::Result::ok()
                         : juce::Result::fail("The updater signer does not match this plugin's pinned certificate.");
}
}

juce::Result launchNativeUpdater(const juce::String& product, const juce::String& version)
{
    juce::ignoreUnused(version);
    if (product.isEmpty() || product.containsAnyOf("\\/:*?\"<>|"))
        return juce::Result::fail("The plugin update configuration is invalid.");

    std::filesystem::path bundle;
    if (const auto result = loadedBundle(bundle); result.failed()) return result;
    const auto helper = bundle / L"Contents" / L"Helpers"
                      / (product.toStdWString() + std::wstring(L"Updater.exe"));

    // Deny writes/deletion while trust is checked and CreateProcess opens the
    // exact final path. The helper independently repeats its own signature pin.
    Handle held(CreateFileW(helper.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (! held.valid())
        return juce::Result::fail(windowsError("The signed updater is missing from this VST3 build"));
    BY_HANDLE_FILE_INFORMATION information{};
    if (! GetFileInformationByHandle(held.get(), &information))
        return juce::Result::fail(windowsError("The updater file could not be inspected"));
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return juce::Result::fail("The updater path is not a regular non-reparse file.");

    std::filesystem::path finalHelper;
    if (const auto result = finalPathFromHandle(held.get(), finalHelper); result.failed()) return result;
    if (const auto result = verifySigner(finalHelper); result.failed()) return result;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(finalHelper.c_str(), nullptr, nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process) == FALSE)
        return juce::Result::fail(windowsError("The updater could not be started"));

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return juce::Result::ok();
}
}

#endif
