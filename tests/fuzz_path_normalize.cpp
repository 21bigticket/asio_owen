#include "../src/security/path_normalize.hpp"

#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    (void)normalize_path(input, false);
    (void)normalize_path(input, true);
    return 0;
}
