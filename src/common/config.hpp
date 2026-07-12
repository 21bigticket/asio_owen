#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <tuple>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "logger.hpp"

class Config {
public:
    // Load a single ini file
    bool load_file(const std::filesystem::path& path) {
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

    // Load all config files from config.d/ under the given base directory.
    // Files are loaded in sorted order by name (00-*.ini loaded first, 99-*.ini last).
    bool load(const std::filesystem::path& base_dir) {
        auto dir_path = base_dir / "config.d";

        std::error_code ec;
        if (!std::filesystem::is_directory(dir_path, ec)) {
            LOG_ERROR("Config directory not found: ", dir_path.string());
            return false;
        }

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
            if (ec) {
                LOG_ERROR("Failed to scan config directory: ", dir_path.string());
                return false;
            }
            if (entry.is_regular_file(ec) && entry.path().extension() == ".ini") {
                files.push_back(entry.path());
            }
        }

        std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) {
                return a.filename().string() < b.filename().string();
            });
        for (auto& f : files) {
            if (!load_file(f)) {
                LOG_WARN("Failed to load config: ", f.string());
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

    bool get_bool(const std::string& section, const std::string& key, bool def = false) const {
        auto s = get(section, key);
        if (s.empty()) return def;
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
        LOG_WARN("Invalid bool config ", section, ".", key, "=", s, ", using default=", def);
        return def;
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

private:
    std::unordered_map<std::string, std::string> data_;
    // Ordered entries preserving duplicates and insertion order
    std::vector<std::tuple<std::string, std::string, std::string>> raw_entries_;

    static void trim(std::string& s) {
        while (!s.empty() && std::isspace(s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace(s.back())) s.pop_back();
    }
};
