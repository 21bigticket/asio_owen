#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cctype>
#include <asio.hpp>

#include "../common/logger.hpp"

// Normalized IP: both string form and parsed asio::ip::address.
// Avoids double-parsing when callers need both forms (e.g. match_cidr).
struct NormalizedIp {
    std::string str;
    asio::ip::address addr;
    bool parse_ok = false;
};

inline NormalizedIp normalize_ip(const std::string& ip_str) {
    NormalizedIp out;
    out.str = ip_str;

    asio::error_code ec;
    auto addr = asio::ip::make_address(ip_str, ec);
    if (ec) return out;

    if (addr.is_v6()) {
        auto v6 = addr.to_v6();
        if (v6.is_v4_mapped()) {
            auto bytes = v6.to_bytes();
            asio::ip::address_v4::bytes_type v4{
                bytes[12], bytes[13], bytes[14], bytes[15]
            };
            auto v4_addr = asio::ip::make_address_v4(v4);
            out.str = v4_addr.to_string();
            out.addr = v4_addr;
            out.parse_ok = true;
            return out;
        }
    }

    out.str = ip_str;
    out.addr = addr;
    out.parse_ok = true;
    return out;
}

// Backward-compatible thin wrapper.
inline std::string normalize_ip_str(const std::string& ip_str) {
    return normalize_ip(ip_str).str;
}

// Strip port/bracket from IP string for address comparison.
// Handles: [::1]:8080, 1.2.3.4:8080, ::1, 1.2.3.4
inline std::string strip_ip_port(const std::string& s) {
    if (s.size() > 2 && s.front() == '[') {
        auto end = s.find(']');
        if (end != std::string::npos) return s.substr(1, end - 1);
    }
    auto colon = s.rfind(':');
    if (colon != std::string::npos && s.find(':') != colon) return s;  // IPv6 with multiple colons, keep as-is
    if (colon != std::string::npos) return s.substr(0, colon);
    return s;
}

// Extract real client IP from X-Forwarded-For
// Algorithm: Nginx realip module standard algorithm
//   1. Only parse XFF if the direct IP is in trusted_proxies (otherwise XFF is forgeable)
//   2. Scan XFF from right to left
//   3. Skip all IPs in the trusted_proxies list
//   4. The first untrusted IP is the real client IP
//   5. If all IPs are trusted or empty, fall back to direct IP
inline std::string get_client_ip(
    asio::ip::tcp::socket& socket,
    const std::string& xff_header,
    const std::vector<std::string>& trusted_proxies)
{
    // direct IP (fallback)
    std::string direct_ip;
    try {
        direct_ip = socket.remote_endpoint().address().to_string();
    } catch (...) {
        direct_ip = "unknown";
    }

    // Normalize direct IP for comparison (handles IPv6-mapped IPv4 like ::ffff:127.0.0.1)
    // trusted_proxies is pre-normalized at load_from_config time, so only direct_ip needs normalization here.
    auto direct_norm = normalize_ip_str(direct_ip);

    // key check: only parse XFF if direct IP is in trusted_proxies
    // otherwise any client can forge XFF to spoof their IP
    bool direct_is_trusted = false;
    for (auto& tp : trusted_proxies) {
        if (direct_norm == tp) {
            direct_is_trusted = true;
            break;
        }
    }
    if (!direct_is_trusted || xff_header.empty()) {
        return direct_ip;
    }

    // parse XFF: split by comma
    std::vector<std::string> ips;
    size_t pos = 0;
    while (pos < xff_header.size()) {
        auto comma = xff_header.find(',', pos);
        auto part = (comma == std::string::npos)
            ? xff_header.substr(pos)
            : xff_header.substr(pos, comma - pos);
        // trim
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
            part.erase(part.begin());
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
            part.pop_back();
        if (!part.empty()) {
            ips.push_back(strip_ip_port(part));
        }
        pos = (comma == std::string::npos) ? xff_header.size() : comma + 1;
    }

    if (ips.empty()) {
        return direct_ip;
    }

    // scan right to left: skip trusted proxies
    // trusted_proxies is pre-normalized, so only the XFF IP needs normalization.
    for (auto it = ips.rbegin(); it != ips.rend(); ++it) {
        auto& ip = *it;
        auto ip_norm = normalize_ip_str(ip);
        // check if in trusted list
        bool trusted = false;
        for (auto& tp : trusted_proxies) {
            if (ip_norm == tp) {
                trusted = true;
                break;
            }
        }
        if (!trusted) {
            return ip;
        }
    }

    // all IPs are trusted or empty, fall back to direct IP
    return direct_ip;
}
