#pragma once

#include <string>
#include <vector>

struct Principal {
    std::string subject;
    std::string username;
    std::vector<std::string> roles;
};

inline bool has_role(const Principal& principal, const std::string& role) {
    for (const auto& item : principal.roles) {
        if (item == role) return true;
    }
    return false;
}
