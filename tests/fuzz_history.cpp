#include "../src/app/admin/config_history.hpp"
#include <cstddef>
#include <map>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);
    (void)config_history::content_sha256({{"fuzz.ini", input}});
    (void)config_history::parse_int64(input);
    (void)config_history::parse_detail_elements({input, input, input});
    (void)config_history::json_string_field(input, input);
    (void)config_history::looks_sensitive(input);
    return 0;
}
