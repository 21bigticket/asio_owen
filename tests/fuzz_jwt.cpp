#include "../src/security/jwt_auth.hpp"
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);
    try {
        JWTAuth auth("01234567890123456789012345678901", "", "HS256");
        (void)auth.verify(input);
    } catch (...) {
    }
    try {
        JWTAuth auth("", "", "RS256", input);
        (void)auth.verify(input);
    } catch (...) {
    }
    return 0;
}
