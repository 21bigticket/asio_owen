#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config_admin.hpp"

// A process-local immutable view of the never-sync Admin credentials. The
// request path checks cheap file metadata and only reparses INI/PEM material
// when a deployment atomically replaces one of the source files.
class AdminCredentialStore {
public:
    struct Snapshot {
        AdminConfig config;
        std::string private_key_pem;
        std::string public_key_pem;
        std::shared_ptr<JWTAuth> verifier;
        std::string error;
        bool loaded = false;
        bool configured = false;
    };

    explicit AdminCredentialStore(std::filesystem::path config_base)
        : config_base_(std::move(config_base)) {}

    std::shared_ptr<const Snapshot> snapshot(
        const AdminConfig* fallback = nullptr,
        std::string* error = nullptr) const {
        std::lock_guard lock(mu_);
        const auto stamp = source_stamp_locked();
        if (!snapshot_ || stamp != stamp_) {
            snapshot_ = load_locked(fallback, stamp);
            stamp_ = stamp;
        }
        if (error) *error = snapshot_->error;
        return snapshot_;
    }

    bool configured(const AdminConfig* fallback = nullptr,
                    std::string* error = nullptr) const {
        return snapshot(fallback, error)->configured;
    }

    std::optional<config_admin::IssuedToken> issue(
        const std::string& username,
        const AdminConfig* fallback = nullptr,
        std::string* error = nullptr) const {
        auto current = snapshot(fallback, error);
        if (!current->configured) return std::nullopt;
        return config_admin::issue_admin_token_with_pem(
            current->config, username, current->private_key_pem);
    }

    std::optional<Principal> verify(
        const std::string& auth_header,
        const AdminConfig* fallback = nullptr,
        std::string* error = nullptr) const {
        auto current = snapshot(fallback, error);
        if (!current->configured || !current->verifier) return std::nullopt;
        return config_admin::verify_admin_token_with_auth(
            current->config, auth_header, *current->verifier);
    }

private:
    struct FileStamp {
        std::filesystem::path path;
        bool exists = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type modified{};

        friend bool operator==(const FileStamp&, const FileStamp&) = default;
    };

    struct Stamp {
        std::vector<FileStamp> files;

        friend bool operator==(const Stamp&, const Stamp&) = default;
    };

    static FileStamp stamp_file(const std::filesystem::path& path) {
        FileStamp result;
        result.path = path;
        std::error_code ec;
        result.exists = std::filesystem::is_regular_file(path, ec);
        if (!result.exists || ec) return result;
        result.size = std::filesystem::file_size(path, ec);
        if (ec) {
            result.exists = false;
            return result;
        }
        result.modified = std::filesystem::last_write_time(path, ec);
        if (ec) result.exists = false;
        return result;
    }

    Stamp source_stamp_locked() const {
        if (config_base_.empty()) return {};
        Stamp stamp;
        stamp.files.push_back(stamp_file(
            config_base_ / "config.d" / "12-config-sync.ini"));
        stamp.files.push_back(stamp_file(
            config_base_ / "config.d" / "99-local.ini"));
        if (snapshot_ && snapshot_->loaded) {
            for (const auto& value : {
                     snapshot_->config.jwt_private_key,
                     snapshot_->config.jwt_public_key}) {
                if (value.empty() || value.find("-----BEGIN") != std::string::npos) {
                    continue;
                }
                auto path = std::filesystem::path(value);
                if (path.is_relative()) path = config_base_ / path;
                stamp.files.push_back(stamp_file(path));
            }
        }
        return stamp;
    }

    std::shared_ptr<const Snapshot> load_locked(
        const AdminConfig* fallback, const Stamp& stamp) const {
        auto result = std::make_shared<Snapshot>();
        std::string error;
        if (config_base_.empty()) {
            if (!fallback) {
                result->error = "config base is unavailable";
                return result;
            }
            result->config = *fallback;
            result->loaded = true;
        } else {
            auto loaded = config_admin::load_local_admin_config(
                config_base_, &error);
            if (!loaded) {
                result->error = error.empty() ? "admin config unavailable" : error;
                return result;
            }
            result->config = std::move(*loaded);
            result->loaded = true;
        }

        auto private_key = config_admin::load_pem_or_file(
            result->config.jwt_private_key, config_base_);
        auto public_key = config_admin::load_pem_or_file(
            result->config.jwt_public_key, config_base_);
        if (private_key) result->private_key_pem = std::move(*private_key);
        if (public_key) result->public_key_pem = std::move(*public_key);
        if (!result->public_key_pem.empty()) {
            try {
                result->verifier = std::make_shared<JWTAuth>(
                    "admin-rs256-unused-secret", std::string(config_admin::kAdminIssuer),
                    "RS256", result->public_key_pem);
            } catch (const std::exception& e) {
                result->error = e.what();
            }
        }
        result->configured = result->loaded && !result->config.accounts.empty() &&
            !result->private_key_pem.empty() && result->verifier != nullptr;
        if (!result->configured) {
            if (result->error.empty()) result->error = "admin API is not configured";
        }
        (void)stamp;
        return result;
    }

    std::filesystem::path config_base_;
    mutable std::mutex mu_;
    mutable Stamp stamp_;
    mutable std::shared_ptr<const Snapshot> snapshot_;
};
