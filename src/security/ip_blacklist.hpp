#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <cstdint>

#include <asio.hpp>
#include "real_ip.hpp"

// Pre-parsed CIDR rule (avoids re-parsing the string on every is_blocked call)
struct ParsedCidr {
    asio::ip::address_v6 network;
    int prefix_len;
};

// IP blacklist: supports exact IP + CIDR ranges
class IpBlacklist {
public:
    // Build from a list of config items
    // Each item can be an exact IP ("10.0.0.1") or CIDR range ("10.0.0.0/8")
    void reload(const std::vector<std::string>& items) {
        auto new_exact = std::make_shared<std::unordered_set<std::string>>();
        auto new_cidrs = std::make_shared<std::vector<ParsedCidr>>();

        for (auto& item : items) {
            if (item.find('/') != std::string::npos) {
                // CIDR range: pre-parse into (v6, prefix_len)
                auto parsed = parse_cidr(item);
                if (parsed) {
                    new_cidrs->push_back(*parsed);
                }
            } else {
                auto normalized = normalize_ip_str(item);
                new_exact->insert(normalized);
            }
        }

        std::lock_guard<std::mutex> lock(mu_);
        exact_ = std::move(new_exact);
        cidrs_ = std::move(new_cidrs);
    }

    bool is_blocked(const std::string& ip) const {
        auto normalized = normalize_ip_str(ip);

        std::lock_guard<std::mutex> lock(mu_);

        // 1. exact match
        if (exact_ && exact_->count(normalized)) {
            return true;
        }

        // 2. CIDR match (using pre-parsed v6 byte comparison, no string re-parsing)
        if (cidrs_) {
            asio::error_code ec;
            auto addr = asio::ip::make_address(normalized, ec);
            if (ec) return false;
            asio::ip::address_v6 addr_v6 = addr.is_v6() ? addr.to_v6()
                : asio::ip::make_address_v6(asio::ip::v4_mapped, addr.to_v4());
            auto addr_bytes = addr_v6.to_bytes();

            for (auto& cidr : *cidrs_) {
                auto cidr_bytes = cidr.network.to_bytes();
                bool match = true;
                for (int i = 0; i < cidr.prefix_len / 8; ++i) {
                    if (addr_bytes[i] != cidr_bytes[i]) { match = false; break; }
                }
                if (match && cidr.prefix_len % 8 != 0) {
                    int r = cidr.prefix_len % 8;
                    uint8_t mask = static_cast<uint8_t>(0xFF << (8 - r));
                    if ((addr_bytes[cidr.prefix_len / 8] & mask) != (cidr_bytes[cidr.prefix_len / 8] & mask)) {
                        match = false;
                    }
                }
                if (match) return true;
            }
        }

        return false;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<std::unordered_set<std::string>> exact_;
    std::shared_ptr<std::vector<ParsedCidr>> cidrs_;

    // Pre-parse a CIDR string into (v6, prefix_len)
    static std::optional<ParsedCidr> parse_cidr(const std::string& cidr_str) {
        auto slash = cidr_str.find('/');
        if (slash == std::string::npos) return std::nullopt;

        auto ip_part = cidr_str.substr(0, slash);
        int prefix_len = 0;
        try {
            prefix_len = std::stoi(cidr_str.substr(slash + 1));
        } catch (...) {
            return std::nullopt;
        }
        if (prefix_len < 0 || prefix_len > 128) return std::nullopt;

        asio::error_code ec;
        auto addr = asio::ip::make_address(ip_part, ec);
        if (ec) return std::nullopt;

        asio::ip::address_v6 v6 = addr.is_v6() ? addr.to_v6()
            : asio::ip::make_address_v6(asio::ip::v4_mapped, addr.to_v4());

        return ParsedCidr{v6, prefix_len};
    }
};
