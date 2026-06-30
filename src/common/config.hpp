#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
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

private:
    std::unordered_map<std::string, std::string> data_;

    static void trim(std::string& s) {
        while (!s.empty() && std::isspace(s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace(s.back())) s.pop_back();
    }
};
