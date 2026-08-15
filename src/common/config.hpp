#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <tuple>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include "logger.hpp"

class Config {
public:
    // Load a single ini file. Returns false (and stops parsing) on a
    // malformed line or section header, so a typo cannot silently fall back to
    // defaults while the caller believes the config was applied.
    bool load_file(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::string line, section;
        while (std::getline(file, line)) {
            trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line.front() == '[') {
                if (line.back() != ']') return false;
                section = line.substr(1, line.size() - 2);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) return false;

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
                LOG_ERROR("Failed to load config: ", f.string());
                return false;
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
        auto it = data_.find(section + "." + key);
        if (it == data_.end()) return def;

        const auto& value = it->second;
        int parsed = 0;
        auto [end, ec] = std::from_chars(
            value.data(), value.data() + value.size(), parsed);
        if (value.empty() || ec != std::errc{} || end != value.data() + value.size()) {
            throw std::invalid_argument(
                "invalid int config " + section + "." + key + "=" + value);
        }
        return parsed;
    }

    double get_double(const std::string& section, const std::string& key, double def = 0.0) const {
        auto it = data_.find(section + "." + key);
        if (it == data_.end()) return def;

        const auto& value = it->second;
        errno = 0;
        char* end = nullptr;
        const char* begin = value.c_str();
        double parsed = std::strtod(begin, &end);
        if (value.empty() || errno == ERANGE ||
            end != begin + value.size() || !std::isfinite(parsed)) {
            throw std::invalid_argument(
                "invalid double config " + section + "." + key + "=" + value);
        }
        return parsed;
    }

    bool get_bool(const std::string& section, const std::string& key, bool def = false) const {
        auto it = data_.find(section + "." + key);
        if (it == data_.end()) return def;

        auto value = it->second;
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
        if (value == "false" || value == "0" || value == "no" || value == "off") return false;
        throw std::invalid_argument(
            "invalid bool config " + section + "." + key + "=" + it->second);
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
