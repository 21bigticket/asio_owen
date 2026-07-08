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
