#include "mysql_result_json.hpp"

void append_json_string(std::string& out, const char* data, unsigned long len) {
    out += '"';
    for (unsigned long i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0x0f];
                    out += kHex[c & 0x0f];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

std::string mysql_result_to_json(MYSQL_RES* mr) {
    unsigned int nf = mysql_num_fields(mr);
    MYSQL_FIELD* fields = mysql_fetch_fields(mr);

    std::string json;
    json.reserve(4096);
    json += '[';

    MYSQL_ROW row;
    bool first_row = true;
    while ((row = mysql_fetch_row(mr))) {
        unsigned long* lengths = mysql_fetch_lengths(mr);
        if (!first_row) json += ',';
        first_row = false;
        json += '{';
        for (unsigned int i = 0; i < nf; ++i) {
            if (i > 0) json += ',';
            append_json_string(json, fields[i].name, fields[i].name_length);
            json += ':';
            if (!row[i]) {
                json += "null";
            } else {
                append_json_string(json, row[i], lengths[i]);
            }
        }
        json += '}';
    }

    json += ']';
    return json;
}

std::optional<std::string> extract_first_string_value(std::string_view json) {
    size_t search_from = 0;
    while (search_from < json.size()) {
        auto colon = json.find(':', search_from);
        if (colon == std::string_view::npos) return std::nullopt;

        size_t i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
        if (i >= json.size()) return std::nullopt;

        if (json[i] != '"') {
            // Value is not a string — skip past it (comma separator) and try next field.
            // Simple skip: advance to next ',' so the next loop iteration finds the next colon.
            auto comma = json.find(',', i);
            if (comma == std::string_view::npos) return std::nullopt;
            search_from = comma + 1;
            continue;
        }
        ++i;

        std::string out;
        out.reserve(32);
        while (i < json.size()) {
            char c = json[i];
            if (c == '"') return out;
            if (c == '\\') {
                if (++i >= json.size()) return std::nullopt;
                char esc = json[i];
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 >= json.size()) return std::nullopt;
                        auto hex = [&](size_t k) -> int {
                            char h = json[i + k];
                            if (h >= '0' && h <= '9') return h - '0';
                            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                            return -1;
                        };
                        int h1 = hex(1), h2 = hex(2), h3 = hex(3), h4 = hex(4);
                        if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return std::nullopt;
                        unsigned int cp = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i += 4;
                        break;
                    }
                    default: return std::nullopt;
                }
                ++i;
                continue;
            }
            out += c;
            ++i;
        }
        return std::nullopt;
    }
    return std::nullopt;
}
