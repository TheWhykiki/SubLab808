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
