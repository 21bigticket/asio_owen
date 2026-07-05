#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <tuple>
#include <fstream>
#include <dirent.h>
#include <algorithm>
#include <cstring>
#include "logger.hpp"

class Config {
public:
    // Load a single ini file
    bool load_file(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

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
            raw_entries_.emplace_back(section, key, val);
        }
        return true;
    }

    // Load all config files from config.d/ directory next to the given path.
    // Files are loaded in sorted order by name (00-*.ini loaded first, 99-*.ini last).
    bool load(const std::string& path) {
        auto base_dir = path.substr(0, path.find_last_of("/\\") + 1);
        auto dir_path = base_dir + "config.d";

        DIR* dir = opendir(dir_path.c_str());
        if (!dir) {
            LOG_ERROR("Config directory not found: ", dir_path);
            return false;
        }

        // Collect .ini files, sort by name, load in order
        std::vector<std::string> files;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(name.size() - 4) == ".ini") {
                files.push_back(name);
            }
        }
        closedir(dir);

        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            if (!load_file(dir_path + "/" + f)) {
                LOG_WARN("Failed to load config: ", dir_path, "/", f);
            }
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

    // Get all key-value pairs in a section (preserves insertion order, allows duplicate keys)
    std::vector<std::pair<std::string, std::string>> get_section(const std::string& section) const {
        std::vector<std::pair<std::string, std::string>> result;
        for (auto& [sec, key, val] : raw_entries_) {
            if (sec == section) {
                result.emplace_back(key, val);
            }
        }
        return result;
    }

    // Get all values in a section (ignore keys, return values only)
    // Preserves insertion order and allows duplicate keys.
    std::vector<std::string> get_list(const std::string& section) const {
        std::vector<std::string> result;
        for (auto& [sec, key, val] : raw_entries_) {
            if (sec == section && !val.empty()) {
                result.push_back(val);
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
    // Ordered entries preserving duplicates and insertion order
    std::vector<std::tuple<std::string, std::string, std::string>> raw_entries_;

    static void trim(std::string& s) {
        while (!s.empty() && std::isspace(s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace(s.back())) s.pop_back();
    }
};
