#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "real_ip.hpp"

struct ParsedCidr {
    asio::ip::address_v6 network;
    int prefix_len = 0;
};

inline std::optional<ParsedCidr> parse_cidr_rule(std::string_view cidr) {
    auto slash = cidr.find('/');
    if (slash == std::string_view::npos) return std::nullopt;

    std::string ip_part(cidr.substr(0, slash));
    std::string prefix_part(cidr.substr(slash + 1));

    int prefix_len = 0;
    try {
        prefix_len = std::stoi(prefix_part);
    } catch (...) {
        return std::nullopt;
    }

    asio::error_code ec;
    auto addr = asio::ip::make_address(ip_part, ec);
    if (ec) return std::nullopt;

    if (addr.is_v4()) {
        if (prefix_len < 0 || prefix_len > 32) return std::nullopt;
        return ParsedCidr{
            asio::ip::make_address_v6(asio::ip::v4_mapped, addr.to_v4()),
            prefix_len + 96
        };
    }

    if (prefix_len < 0 || prefix_len > 128) return std::nullopt;
    return ParsedCidr{addr.to_v6(), prefix_len};
}

inline bool match_cidr(const std::string& ip, const ParsedCidr& rule) {
    auto normalized = normalize_ip_str(ip);

    asio::error_code ec;
    auto addr = asio::ip::make_address(normalized, ec);
    if (ec) return false;

    auto addr_v6 = addr.is_v6()
        ? addr.to_v6()
        : asio::ip::make_address_v6(asio::ip::v4_mapped, addr.to_v4());

    auto addr_bytes = addr_v6.to_bytes();
    auto cidr_bytes = rule.network.to_bytes();

    for (int i = 0; i < rule.prefix_len / 8; ++i) {
        if (addr_bytes[i] != cidr_bytes[i]) return false;
    }

    if (rule.prefix_len % 8 != 0) {
        int remaining = rule.prefix_len % 8;
        uint8_t mask = static_cast<uint8_t>(0xFF << (8 - remaining));
        if ((addr_bytes[rule.prefix_len / 8] & mask) !=
            (cidr_bytes[rule.prefix_len / 8] & mask)) {
            return false;
        }
    }

    return true;
}
