#include "../src/app/config_sync_service.hpp"
#include "../src/common/config.hpp"
#include <cstddef>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
std::atomic<unsigned long long> g_fuzz_file_id{0};
}

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);
    (void)ConfigSyncService::is_valid_managed_filename(input);
    (void)ConfigSyncService::validate_managed_file(input, input);
    (void)ConfigSyncService::has_reserved_admin_rule(input);
    (void)ConfigSyncService::contains_section(input, input);
    const auto id = g_fuzz_file_id.fetch_add(1, std::memory_order_relaxed);
    auto path = std::filesystem::temp_directory_path() /
        ("asio_owen_fuzz_" + std::to_string(static_cast<unsigned long long>(getpid())) +
         "_" + std::to_string(id) + ".ini");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << input;
    }
    try {
        Config cfg;
        if (cfg.load_file(path)) {
            (void)cfg.get("fuzz", "key");
            try { (void)cfg.get_int("fuzz", "key"); } catch (...) {}
            try { (void)cfg.get_bool("fuzz", "key"); } catch (...) {}
        }
    } catch (...) {
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}
