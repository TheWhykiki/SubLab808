// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string_view>

namespace wk::authenticode
{
inline constexpr std::string_view publicTrustEku = "1.3.6.1.4.1.311.97.1.0";
constexpr bool validProfileEku(std::string_view value)
{
    constexpr std::string_view prefix = "1.3.6.1.4.1.311.97.";
    if (! value.starts_with(prefix) || value.size() > 200
        || value == publicTrustEku || value.starts_with("1.3.6.1.4.1.311.97.1."))
        return false;
    auto suffix = value.substr(prefix.size());
    unsigned parts{};
    while (! suffix.empty())
    {
        const auto dot = suffix.find('.');
        const auto part = suffix.substr(0, dot);
        if (part.empty() || part.size() > 10 || (part.size() > 1 && part.front() == '0')) return false;
        unsigned long long number{};
        for (const auto c : part)
        {
            if (c < '0' || c > '9') return false;
            number = number * 10 + static_cast<unsigned>(c - '0');
        }
        if (number > 0xffffffffULL) return false;
        ++parts;
        if (dot == std::string_view::npos) break;
        suffix.remove_prefix(dot + 1);
        if (suffix.empty()) return false;
    }
    return parts == 4;
}
}
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace wk::authenticode
{
struct Identity { std::string profileEku; std::string leafSha256; };
inline Identity verify(const std::filesystem::path& path, std::string_view current,
                       std::string_view next = {}, bool online = true)
{
    if (! validProfileEku(current) || (! next.empty() && (! validProfileEku(next) || next == current)))
        throw std::runtime_error("Artifact Signing profile pins are missing or malformed");
    WINTRUST_FILE_INFO file{ sizeof(WINTRUST_FILE_INFO), path.c_str(), nullptr, nullptr };
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = online ? WTD_REVOKE_WHOLECHAIN : WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_DISABLE_MD2_MD4 | (online ? WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT
                                                   : WTD_CACHE_ONLY_URL_RETRIEVAL);
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status = WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
    struct Cleanup
    {
        WINTRUST_DATA& data; GUID& action;
        ~Cleanup() { data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &data); }
    } cleanup{ trust, action };
    if (status != ERROR_SUCCESS) throw std::runtime_error("WinVerifyTrust rejected the signed file");
    // Inspect the exact signer and counter-signer authenticated by WinVerifyTrust,
    // not an independently selected PKCS#7 signature.
    auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
    auto* signer = provider == nullptr ? nullptr : WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    if (signer == nullptr || signer->dwError != ERROR_SUCCESS || signer->csCertChain == 0
        || signer->pasCertChain == nullptr || signer->psSigner == nullptr)
        throw std::runtime_error("Authenticated signer chain is missing");
    bool rfc3161{};
    for (DWORD i = 0; i < signer->psSigner->UnauthAttrs.cAttr; ++i)
    {
        const auto& attribute = signer->psSigner->UnauthAttrs.rgAttr[i];
        if (attribute.pszObjId != nullptr && std::string_view(attribute.pszObjId) == "1.3.6.1.4.1.311.3.3.1")
        {
            if (rfc3161 || attribute.cValue != 1) throw std::runtime_error("Ambiguous RFC3161 timestamp");
            rfc3161 = true;
        }
    }
    auto* counter = WTHelperGetProvSignerFromChain(provider, 0, TRUE, 0);
    if (! rfc3161 || signer->csCounterSigners != 1 || counter == nullptr
        || counter->dwError != ERROR_SUCCESS || counter->csCertChain == 0)
        throw std::runtime_error("A verified RFC3161 timestamp is required");
    const auto* certificate = signer->pasCertChain[0].pCert;
    if (certificate == nullptr
        || CertVerifyTimeValidity(&signer->sftVerifyAsOf, certificate->pCertInfo) != 0)
        throw std::runtime_error("Timestamp is outside the signer certificate validity");
    DWORD usageBytes{};
    if (! CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &usageBytes)
        || usageBytes < sizeof(CERT_ENHKEY_USAGE) || usageBytes > 65536)
        throw std::runtime_error("Signer EKU extension is missing or oversized");
    std::vector<unsigned char> storage(usageBytes);
    auto* usages = reinterpret_cast<PCERT_ENHKEY_USAGE>(storage.data());
    if (! CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usages, &usageBytes))
        throw std::runtime_error("Signer EKUs could not be decoded");
    bool codeSigning{}, publicTrust{};
    std::string profile;
    for (DWORD i = 0; i < usages->cUsageIdentifier; ++i)
    {
        const std::string_view oid(usages->rgpszUsageIdentifier[i]);
        if (oid == "1.3.6.1.5.5.7.3.3") codeSigning = true;
        if (oid == publicTrustEku) publicTrust = true;
        if (oid == current || (! next.empty() && oid == next))
        {
            if (! profile.empty()) throw std::runtime_error("Ambiguous profile identity");
            profile = oid;
        }
    }
    if (! codeSigning || ! publicTrust || profile.empty())
        throw std::runtime_error("Signer is outside the pinned Artifact Signing Public Trust identity");
    std::array<unsigned char, 32> digest{};
    DWORD size = static_cast<DWORD>(digest.size());
    if (! CertGetCertificateContextProperty(certificate, CERT_SHA256_HASH_PROP_ID, digest.data(), &size)
        || size != digest.size()) throw std::runtime_error("Cannot hash signer certificate");
    std::string leaf;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto byte : digest) { leaf += hex[byte >> 4]; leaf += hex[byte & 15]; }
    return { profile, leaf };
}
}
#endif
