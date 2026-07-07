#pragma once

#include <cctype>
#include <string>
#include <vector>

// Convert JSON object's snake_case keys to camelCase.
// String values, including strings inside arrays, are preserved verbatim.
inline void json_keys_snake_to_camel(std::string& json) {
    if (json.empty()) return;

    enum class ScopeKind { Object, Array };
    struct Scope {
        ScopeKind kind;
        bool expect_key;
    };

    std::vector<Scope> scopes;
    std::string out;
    out.reserve(json.size());

    bool in_string = false;
    bool in_key = false;
    bool next_upper = false;

    auto current_object_expects_key = [&]() {
        return !scopes.empty() &&
            scopes.back().kind == ScopeKind::Object &&
            scopes.back().expect_key;
    };

    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];

        if (in_string) {
            if (c == '\\' && i + 1 < json.size()) {
                out += c;
                out += json[++i];
                continue;
            }
            if (c == '"') {
                out += c;
                in_string = false;
                if (in_key && !scopes.empty() && scopes.back().kind == ScopeKind::Object) {
                    scopes.back().expect_key = false;
                }
                in_key = false;
                next_upper = false;
                continue;
            }
            if (in_key) {
                if (c == '_') {
                    next_upper = true;
                } else if (next_upper) {
                    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    next_upper = false;
                } else {
                    out += c;
                }
            } else {
                out += c;
            }
            continue;
        }

        if (c == '"') {
            out += c;
            in_string = true;
            in_key = current_object_expects_key();
            next_upper = false;
            continue;
        }

        out += c;

        if (c == '{') {
            scopes.push_back({ScopeKind::Object, true});
        } else if (c == '[') {
            scopes.push_back({ScopeKind::Array, false});
        } else if (c == '}' || c == ']') {
            if (!scopes.empty()) {
                scopes.pop_back();
            }
        } else if (c == ',' && !scopes.empty() && scopes.back().kind == ScopeKind::Object) {
            scopes.back().expect_key = true;
        }
    }

    json = std::move(out);
}
