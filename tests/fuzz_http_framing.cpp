#include "../src/http/http_protocol.hpp"
#include "../src/http/http_body_reader.hpp"
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);
    (void)parse_decimal_size(input);
    (void)parse_hex_size_line(input);
    (void)parse_http_status_line(input);
    HeaderParseState state;
    update_header_state(input, input, state);
    std::vector<std::pair<std::string, std::string>> headers;
    parse_header_fields(input, headers, state);
    return 0;
}
