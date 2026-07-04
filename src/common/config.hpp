#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include "logger.hpp"

class Config {
public:
    bool load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_ERROR("Config file not found: ", path);
            return false;
        }

        std::string line, section;
        while (std::getline(file, line)) {
            trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key); trim(val);
            data_[section + "." + key] = val;
        }
        return true;
    }

    std::string get(const std::string& section, const std::string& key, 
                    const std::string& def = "") const {
        auto it = data_.find(section + "." + key);
        return it != data_.end() ? it->second : def;
    }

    int get_int(const std::string& section, const std::string& key, int def = 0) const {
        auto s = get(section, key);
        if (s.empty()) return def;
        try {
            return std::stoi(s);
        } catch (...) {
            LOG_WARN("Invalid int config ", section, ".", key, "=", s, ", using default=", def);
            return def;
        }
    }

    double get_double(const std::string& section, const std::string& key, double def = 0.0) const {
        auto s = get(section, key);
        if (s.empty()) return def;
        try {
            return std::stod(s);
        } catch (...) {
            LOG_WARN("Invalid double config ", section, ".", key, "=", s, ", using default=", def);
            return def;
        }
    }

    // Get all key-value pairs in a section
    std::vector<std::pair<std::string, std::string>> get_section(const std::string& section) const {
        std::vector<std::pair<std::string, std::string>> result;
        auto prefix = section + ".";
        for (auto& [k, v] : data_) {
            if (k.find(prefix) == 0) {
                result.emplace_back(k.substr(prefix.size()), v);
            }
        }
        return result;
    }

    // Get all values in a section (ignore keys, return values only)
    // Used for sections like [ip_blacklist] ip = 1.2.3.4
    std::vector<std::string> get_list(const std::string& section) const {
        std::vector<std::string> result;
        auto prefix = section + ".";
        for (auto& [k, v] : data_) {
            if (k.find(prefix) == 0 && !v.empty()) {
                result.push_back(v);
            }
        }
        return result;
    }

    // Get all values preserving occurrence order (allows duplicate keys)
    // Used for sections like [auth_whitelist] path = /api/health
    std::vector<std::string> get_all_values(const std::string& section) const {
        return get_list(section);
    }

    // Get all key=value pairs in a section (for [path_blacklist])
    // /api/internal =       (empty value means no role restriction)
    // /admin/ = role:admin  (with role restriction)
    std::vector<std::pair<std::string, std::string>> get_section_raw(const std::string& section) const {
        return get_section(section);
    }

private:
    std::unordered_map<std::string, std::string> data_;

    static void trim(std::string& s) {
        while (!s.empty() && std::isspace(s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace(s.back())) s.pop_back();
    }
};
