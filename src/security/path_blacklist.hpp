#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// Path blacklist: block paths by prefix, optional role-based access control
class PathBlacklist {
public:
    void reload(const std::vector<std::pair<std::string, std::string>>& items) {
        std::vector<std::string> new_paths;
        std::vector<std::pair<std::string, std::string>> new_role_paths;

        for (auto& [key, val] : items) {
            if (key.empty()) continue;
            if (val.empty()) {
                new_paths.push_back(key);
            } else {
                new_role_paths.emplace_back(key, val);
            }
        }

        // sort by path length descending (longest prefix matches first)
        std::sort(new_paths.begin(), new_paths.end(),
            [](const std::string& a, const std::string& b) {
                return a.size() > b.size();
            });
        std::sort(new_role_paths.begin(), new_role_paths.end(),
            [](const auto& a, const auto& b) {
                return a.first.size() > b.first.size();
            });

        paths_ = std::move(new_paths);
        role_paths_ = std::move(new_role_paths);
    }

    // Combined check returns blocked and required-role state in one pass.
    // This is the only entry point used by SecurityRules; is_blocked() and
    // required_role() were removed because they each took the same lock separately
    // and required two calls to get both pieces of state.
    struct BlockResult {
        bool blocked = false;
        std::string required_role;
    };

    BlockResult check(const std::string& path) const {
        BlockResult result;
        for (auto& p : paths_) {
            // exact match or path-segment boundary match:
            // - /api/internal matches /api/internal and /api/internal/
            // - /api/internal does NOT match /api/internalxxx
            if (path == p) return {true, {}};
            if (path.find(p) == 0 &&
                (p.back() == '/' || path.size() == p.size() || path[p.size()] == '/')) {
                return {true, {}};
            }
        }
        for (auto& [p, role] : role_paths_) {
            bool match = (path == p) ||
                (path.find(p) == 0 &&
                 (p.back() == '/' || path.size() == p.size() || path[p.size()] == '/'));
            if (match) {
                result.required_role = extract_role_from_val(role);
                break;
            }
        }
        return result;
    }

    // Extract "admin" from "role:admin"
    static std::string extract_role_from_val(const std::string& role) {
        auto colon = role.find(':');
        if (colon != std::string::npos) {
            return role.substr(colon + 1);
        }
        return role;
    }

private:
    std::vector<std::string> paths_;
    std::vector<std::pair<std::string, std::string>> role_paths_;
};
