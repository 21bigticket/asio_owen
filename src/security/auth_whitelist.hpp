#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <mutex>

// Auth whitelist: specific paths or services can skip JWT verification
class AuthWhitelist {
public:
    void reload(const std::vector<std::string>& items) {
        auto new_exact = std::make_shared<std::unordered_set<std::string>>();
        auto new_prefixes = std::make_shared<std::vector<std::string>>();
        auto new_services = std::make_shared<std::unordered_set<std::string>>();

        for (auto& item : items) {
            if (item.empty()) continue;
            if (item[0] == '/') {
                if (item.back() == '/') {
                    new_prefixes->push_back(item);
                } else {
                    new_exact->insert(item);
                }
            } else {
                new_services->insert(item);
            }
        }

        std::lock_guard<std::mutex> lock(mu_);
        exact_paths_ = std::move(new_exact);
        prefix_paths_ = std::move(new_prefixes);
        services_ = std::move(new_services);
    }

    bool is_path_whitelisted(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (exact_paths_ && exact_paths_->count(path)) {
            return true;
        }
        if (prefix_paths_) {
            for (auto& p : *prefix_paths_) {
                if (path.find(p) == 0) return true;
            }
        }
        return false;
    }

    bool is_service_whitelisted(const std::string& service) const {
        std::lock_guard<std::mutex> lock(mu_);
        return services_ && services_->count(service);
    }

    bool is_whitelisted(const std::string& path, const std::string& service) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (exact_paths_ && exact_paths_->count(path)) {
            return true;
        }
        if (prefix_paths_) {
            for (auto& p : *prefix_paths_) {
                if (path.find(p) == 0) return true;
            }
        }
        if (!service.empty() && services_ && services_->count(service)) {
            return true;
        }
        return false;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<std::unordered_set<std::string>> exact_paths_;
    std::shared_ptr<std::vector<std::string>> prefix_paths_;
    std::shared_ptr<std::unordered_set<std::string>> services_;
};
