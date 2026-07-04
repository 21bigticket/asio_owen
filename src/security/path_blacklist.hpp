#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <algorithm>

// Path blacklist: block paths by prefix, optional role-based access control
class PathBlacklist {
public:
    void reload(const std::vector<std::pair<std::string, std::string>>& items) {
        auto new_paths = std::make_shared<std::vector<std::string>>();
        auto new_role_paths = std::make_shared<std::vector<std::pair<std::string, std::string>>>();

        for (auto& [key, val] : items) {
            if (key.empty()) continue;
            if (val.empty()) {
                new_paths->push_back(key);
            } else {
                new_role_paths->emplace_back(key, val);
            }
        }

        // sort by path length descending (longest prefix matches first)
        std::sort(new_paths->begin(), new_paths->end(),
            [](const std::string& a, const std::string& b) {
                return a.size() > b.size();
            });
        std::sort(new_role_paths->begin(), new_role_paths->end(),
            [](const auto& a, const auto& b) {
                return a.first.size() > b.first.size();
            });

        std::lock_guard<std::mutex> lock(mu_);
        paths_ = std::move(new_paths);
        role_paths_ = std::move(new_role_paths);
    }

    bool is_blocked(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (paths_) {
            for (auto& p : *paths_) {
                // exact match or path-segment boundary match:
                // - /api/internal matches /api/internal and /api/internal/
                // - /api/internal does NOT match /api/internalxxx
                if (path == p) return true;
                if (path.find(p) == 0 &&
                    (p.back() == '/' || path.size() == p.size() || path[p.size()] == '/')) {
                    return true;
                }
            }
        }
        return false;
    }

    // Combined check: returns (blocked, required_role) in one lock instead of two
    struct BlockResult {
        bool blocked = false;
        std::string required_role;
    };

    BlockResult check(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mu_);
        BlockResult result;
        if (paths_) {
            for (auto& p : *paths_) {
                if (path == p) return {true, {}};
                if (path.find(p) == 0 &&
                    (p.back() == '/' || path.size() == p.size() || path[p.size()] == '/')) {
                    return {true, {}};
                }
            }
        }
        if (role_paths_) {
            for (auto& [p, role] : *role_paths_) {
                bool match = (path == p) ||
                    (path.find(p) == 0 &&
                     (p.back() == '/' || path.size() == p.size() || path[p.size()] == '/'));
                if (match) {
                    result.required_role = extract_role_from_val(role);
                    break;
                }
            }
        }
        return result;
    }

    // Return the required role, empty means unrestricted
    std::string required_role(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (!role_paths_) return {};
        for (auto& [p, role] : *role_paths_) {
            // same path-segment boundary logic as is_blocked
            if (path == p) {
                return extract_role_from_val(role);
            }
            if (path.find(p) == 0 &&
                (p.back() == '/' || path.size() == p.size() || path[p.size()] == '/')) {
                return extract_role_from_val(role);
            }
        }
        return {};
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
    mutable std::mutex mu_;
    std::shared_ptr<std::vector<std::string>> paths_;
    std::shared_ptr<std::vector<std::pair<std::string, std::string>>> role_paths_;
};
