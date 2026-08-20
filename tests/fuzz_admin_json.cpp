#include "../src/app/admin/config_admin.hpp"

#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    (void)config_admin::parse_login_request(input);
    (void)config_admin::parse_save_request(input);
    (void)config_admin::parse_rollback_request(input);
    (void)config_admin::parse_repair_request(input);
    (void)config_admin::parse_orphan_resolution_request(input);
    return 0;
}
