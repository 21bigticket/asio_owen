#pragma once
#include <string>
#include <vector>
#include <unordered_set>

// Auth whitelist: specific paths or services can skip JWT verification
class AuthWhitelist {
public:
    void reload(const std::vector<std::string>& items) {
        std::unordered_set<std::string> new_exact;
        std::vector<std::string> new_prefixes;
        std::unordered_set<std::string> new_services;

        for (auto& item : items) {
            if (item.empty()) continue;
            if (item[0] == '/') {
                // A bare "/" must NOT be treated as a prefix: is_whitelisted
                // uses path.find(prefix) == 0, so "/" would match every path and
                // turn the whitelist into a catch-all that bypasses JWT for the
                // whole site. Classify it as exact instead (the root path is
                // 404'd earlier anyway); only real prefixes like "/foo/" prefix.
                if (item.size() > 1 && item.back() == '/') {
                    new_prefixes.push_back(item);
                } else {
                    new_exact.insert(item);
                }
            } else {
                new_services.insert(item);
            }
        }

        exact_paths_ = std::move(new_exact);
        prefix_paths_ = std::move(new_prefixes);
        services_ = std::move(new_services);
    }

    bool is_path_whitelisted(const std::string& path) const {
        if (exact_paths_.count(path)) {
            return true;
        }
        for (auto& p : prefix_paths_) {
            if (path.find(p) == 0) return true;
        }
        return false;
    }

    bool is_service_whitelisted(const std::string& service) const {
        return services_.count(service);
    }

    bool is_whitelisted(const std::string& path, const std::string& service) const {
        if (exact_paths_.count(path)) {
            return true;
        }
        for (auto& p : prefix_paths_) {
            if (path.find(p) == 0) return true;
        }
        if (!service.empty() && services_.count(service)) {
            return true;
        }
        return false;
    }

private:
    std::unordered_set<std::string> exact_paths_;
    std::vector<std::string> prefix_paths_;
    std::unordered_set<std::string> services_;
};
