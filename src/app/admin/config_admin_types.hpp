#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace config_admin {

struct ManagedFile {
    std::string name;
    std::string content;
};

struct SaveRequest {
    int64_t base_version = 0;
    std::string reason;
    std::vector<ManagedFile> files;
};

struct ParseResult {
    bool ok = false;
    SaveRequest request;
    std::string error;
};

struct LoginRequest {
    std::string username;
    std::string password;
};

struct LoginParseResult {
    bool ok = false;
    LoginRequest request;
    std::string error;
};

struct RollbackRequest {
    int64_t base_version = 0;
    int64_t target_version = 0;
    std::string reason;
};

struct RollbackParseResult {
    bool ok = false;
    RollbackRequest request;
    std::string error;
};

struct RepairRequest {
    int64_t version = 0;
    std::string reason;
};

struct RepairParseResult {
    bool ok = false;
    RepairRequest request;
    std::string error;
};

struct OrphanResolutionRequest {
    int64_t current_version = 0;
    int64_t target_version = 0;
    std::string action;
    std::string reason;
    std::string confirmation;
};

struct OrphanResolutionParseResult {
    bool ok = false;
    OrphanResolutionRequest request;
    std::string error;
};

struct IssuedToken {
    std::string token;
    int expires_in = 0;
};

}  // namespace config_admin
