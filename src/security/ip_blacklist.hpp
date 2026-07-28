#pragma once
#include <string>
#include <vector>
#include <unordered_set>

#include "cidr.hpp"

// IP blacklist: supports exact IP + CIDR ranges
class IpBlacklist {
public:
    // Build from a list of config items
    // Each item can be an exact IP ("10.0.0.1") or CIDR range ("10.0.0.0/8")
    void reload(const std::vector<std::string>& items) {
        std::unordered_set<std::string> new_exact;
        std::vector<ParsedCidr> new_cidrs;

        for (auto& item : items) {
            if (item.find('/') != std::string::npos) {
                // CIDR range: pre-parse into (v6, prefix_len)
                auto parsed = parse_cidr_rule(item);
                if (parsed) {
                    new_cidrs.push_back(*parsed);
                }
            } else {
                auto normalized = normalize_ip_str(item);
                new_exact.insert(normalized);
            }
        }

        exact_ = std::move(new_exact);
        cidrs_ = std::move(new_cidrs);
    }

    bool is_blocked(const std::string& ip) const {
        auto normalized = normalize_ip_str(ip);

        // 1. exact match
        if (exact_.count(normalized)) {
            return true;
        }

        // 2. CIDR match (using pre-parsed v6 byte comparison, no string re-parsing)
        for (auto& cidr : cidrs_) {
            if (match_cidr(normalized, cidr)) return true;
        }

        return false;
    }

private:
    std::unordered_set<std::string> exact_;
    std::vector<ParsedCidr> cidrs_;
};
