cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(asio_owen LANGUAGES C CXX)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(ASIO_OWEN_SANITIZER "" CACHE STRING "Optional sanitizer: address or thread")
if(ASIO_OWEN_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
elseif(ASIO_OWEN_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
elseif(NOT ASIO_OWEN_SANITIZER STREQUAL "")
    message(FATAL_ERROR "ASIO_OWEN_SANITIZER must be address or thread")
endif()

option(ASIO_OWEN_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
option(ASIO_OWEN_ENABLE_CLANG_TIDY "Run clang-tidy during C++ compilation" OFF)
option(ASIO_OWEN_BUILD_INTEGRATION_TESTS "Register Docker/service integration tests" OFF)
if(ASIO_OWEN_ENABLE_CLANG_TIDY)
    find_program(ASIO_OWEN_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY
        "${ASIO_OWEN_CLANG_TIDY_EXECUTABLE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
        CACHE STRING "clang-tidy command" FORCE)
endif()
if(ASIO_OWEN_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ASIO_OWEN_BUILD_FUZZERS requires Clang/libFuzzer")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(ASIO_OWEN_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(ASIO_OWEN_ADMIN_LOGIN_HTML ${PROJECT_SOURCE_DIR}/src/app/admin/pages/login.html)
set(ASIO_OWEN_ADMIN_LOGIN_HEADER ${ASIO_OWEN_GENERATED_DIR}/admin_login_html.hpp)
set(ASIO_OWEN_ADMIN_SETTINGS_HTML ${PROJECT_SOURCE_DIR}/src/app/admin/pages/settings.html)
set(ASIO_OWEN_ADMIN_SETTINGS_HEADER ${ASIO_OWEN_GENERATED_DIR}/admin_settings_html.hpp)
add_custom_command(
    OUTPUT ${ASIO_OWEN_ADMIN_LOGIN_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASIO_OWEN_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${ASIO_OWEN_ADMIN_LOGIN_HTML}
        -DOUTPUT_FILE=${ASIO_OWEN_ADMIN_LOGIN_HEADER}
        -DVARIABLE_NAME=admin_login_html
        -P ${PROJECT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS ${ASIO_OWEN_ADMIN_LOGIN_HTML} ${PROJECT_SOURCE_DIR}/cmake/embed_text.cmake
    VERBATIM
)
add_custom_command(
    OUTPUT ${ASIO_OWEN_ADMIN_SETTINGS_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASIO_OWEN_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${ASIO_OWEN_ADMIN_SETTINGS_HTML}
        -DOUTPUT_FILE=${ASIO_OWEN_ADMIN_SETTINGS_HEADER}
        -DVARIABLE_NAME=admin_settings_html
        -P ${PROJECT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS ${ASIO_OWEN_ADMIN_SETTINGS_HTML} ${PROJECT_SOURCE_DIR}/cmake/embed_text.cmake
    VERBATIM
)
add_custom_target(asio_owen_generated_assets
    DEPENDS ${ASIO_OWEN_ADMIN_LOGIN_HEADER} ${ASIO_OWEN_ADMIN_SETTINGS_HEADER})

# The business JWT verification key is public and remains part of the local
# runtime bundle. Admin signing/verification keys are deployment-owned and are
# deliberately never copied from the source tree.
if(EXISTS ${PROJECT_SOURCE_DIR}/jwt_keys/public-key.pem)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/jwt_keys)
    configure_file(
        ${PROJECT_SOURCE_DIR}/jwt_keys/public-key.pem
        ${CMAKE_CURRENT_BINARY_DIR}/jwt_keys/public-key.pem
        COPYONLY)
endif()

# ==================== 通用编译选项 ====================
add_library(asio_owen_options INTERFACE)
target_compile_features(asio_owen_options INTERFACE cxx_std_20)
target_compile_definitions(asio_owen_options INTERFACE ASIO_STANDALONE ASIO_HAS_CO_AWAIT)
target_include_directories(asio_owen_options INTERFACE
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

# ==================== ASIO + spdlog（优先本地，否则 FetchContent 自动拉取）====================
include(FetchContent)

add_library(asio_owen_asio INTERFACE)
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/asio/include/asio.hpp)
    target_include_directories(asio_owen_asio INTERFACE ${PROJECT_SOURCE_DIR}/third_party/asio/include)
    message(STATUS "ASIO: using vendored ${PROJECT_SOURCE_DIR}/third_party/asio")
elseif(EXISTS ${PROJECT_SOURCE_DIR}/asio/include/asio.hpp)
    target_include_directories(asio_owen_asio INTERFACE ${PROJECT_SOURCE_DIR}/asio/include)
    message(STATUS "ASIO: using local ${PROJECT_SOURCE_DIR}/asio")
else()
    FetchContent_Declare(
        asio
        URL      https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-0.tar.gz
        URL_HASH SHA256=c24922183dd91632140e65e7ef4c48c4f6f219ec4d9e16c9ec4536ee442669f4
    )
    FetchContent_GetProperties(asio)
    if(NOT asio_POPULATED)
        FetchContent_Populate(asio)
    endif()
    if(EXISTS ${asio_SOURCE_DIR}/asio/include/asio.hpp)
        target_include_directories(asio_owen_asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
    else()
        target_include_directories(asio_owen_asio INTERFACE ${asio_SOURCE_DIR}/include)
    endif()
    message(STATUS "ASIO: fetched from remote")
endif()

if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/spdlog/include/spdlog/spdlog.h)
    add_library(asio_owen_spdlog INTERFACE)
    target_include_directories(asio_owen_spdlog INTERFACE ${PROJECT_SOURCE_DIR}/third_party/spdlog/include)
    message(STATUS "spdlog: using vendored ${PROJECT_SOURCE_DIR}/third_party/spdlog")
elseif(EXISTS ${PROJECT_SOURCE_DIR}/spdlog/include/spdlog/spdlog.h)
    add_library(asio_owen_spdlog INTERFACE)
    target_include_directories(asio_owen_spdlog INTERFACE ${PROJECT_SOURCE_DIR}/spdlog/include)
    message(STATUS "spdlog: using local ${PROJECT_SOURCE_DIR}/spdlog")
else()
    FetchContent_Declare(
        spdlog
        URL      https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=68fea002cd716e62e9e6ee8d5066c9a0eee13f15c8026bd4e4b2d2a87e1d6473
    )
    FetchContent_MakeAvailable(spdlog)
    add_library(asio_owen_spdlog INTERFACE)
    target_link_libraries(asio_owen_spdlog INTERFACE spdlog::spdlog_header_only)
    message(STATUS "spdlog: fetched from remote")
endif()

# ==================== jwt-cpp（header-only，优先本地，否则 FetchContent）====================
add_library(asio_owen_jwtcpp INTERFACE)
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include/jwt-cpp/jwt.h)
    target_include_directories(asio_owen_jwtcpp INTERFACE ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include)
    message(STATUS "jwt-cpp: using vendored ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp")
else()
    FetchContent_Declare(
        jwt-cpp
        URL      https://github.com/Thalhammer/jwt-cpp/archive/refs/tags/v0.7.0.tar.gz
    )
    FetchContent_GetProperties(jwt-cpp)
    if(NOT jwt-cpp_POPULATED)
        FetchContent_Populate(jwt-cpp)
    endif()
    if(EXISTS ${jwt-cpp_SOURCE_DIR}/include/jwt-cpp/jwt.h)
        target_include_directories(asio_owen_jwtcpp INTERFACE ${jwt-cpp_SOURCE_DIR}/include)
    else()
        target_include_directories(asio_owen_jwtcpp INTERFACE ${jwt-cpp_SOURCE_DIR})
    endif()
    message(STATUS "jwt-cpp: fetched from remote")
endif()

# ==================== 系统依赖：pkg-config 可选，macOS/Linux 通用 fallback ====================
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
find_package(OpenSSL REQUIRED)

function(find_optional_pkg_or_library target optional_pkg_name header library_name)
    string(REPLACE "::" "_" target_var "${target}")
    if(PkgConfig_FOUND)
        pkg_check_modules(${target_var}_PKG QUIET ${optional_pkg_name} IMPORTED_TARGET)
    endif()

    if(${target_var}_PKG_FOUND)
        add_library(${target} INTERFACE IMPORTED)
        target_link_libraries(${target} INTERFACE PkgConfig::${target_var}_PKG)
        message(STATUS "${target}: using optional pkg-config package ${optional_pkg_name}")
        return()
    endif()

    set(search_prefixes)
    if(APPLE)
        list(APPEND search_prefixes
            /opt/homebrew/opt/${optional_pkg_name}
            /usr/local/opt/${optional_pkg_name}
            /opt/homebrew
            /usr/local
        )
        if(optional_pkg_name STREQUAL "mysqlclient")
            list(APPEND search_prefixes
                /opt/homebrew/opt/mysql-client
                /usr/local/opt/mysql-client
            )
        endif()
    endif()

    find_path(${target_var}_INCLUDE_DIR
        NAMES ${header}
        HINTS ${search_prefixes}
        PATH_SUFFIXES include
    )
    find_library(${target_var}_LIBRARY
        NAMES ${library_name}
        HINTS ${search_prefixes}
        PATH_SUFFIXES lib
    )

    if(NOT ${target_var}_INCLUDE_DIR OR NOT ${target_var}_LIBRARY)
        message(FATAL_ERROR
            "Could not find ${target}. Install ${optional_pkg_name} development files "
            "or set ${target_var}_INCLUDE_DIR and ${target_var}_LIBRARY.")
    endif()

    add_library(${target} UNKNOWN IMPORTED)
    set_target_properties(${target} PROPERTIES
        IMPORTED_LOCATION "${${target_var}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${${target_var}_INCLUDE_DIR}"
    )
    message(STATUS "${target}: using ${${target_var}_LIBRARY}")
endfunction()

function(find_mysql_client)
    if(PkgConfig_FOUND)
        pkg_check_modules(MySQL_Client_PKG QUIET mysqlclient IMPORTED_TARGET)
        if(MySQL_Client_PKG_FOUND)
            add_library(MySQL::Client INTERFACE IMPORTED)
            target_link_libraries(MySQL::Client INTERFACE PkgConfig::MySQL_Client_PKG)
            message(STATUS "MySQL::Client: using pkg-config package mysqlclient")
            return()
        endif()

        pkg_check_modules(MySQL_MariaDB_PKG QUIET mariadb IMPORTED_TARGET)
        if(MySQL_MariaDB_PKG_FOUND)
            add_library(MySQL::Client INTERFACE IMPORTED)
            target_link_libraries(MySQL::Client INTERFACE PkgConfig::MySQL_MariaDB_PKG)
            message(STATUS "MySQL::Client: using pkg-config package mariadb")
            return()
        endif()
    endif()

    find_optional_pkg_or_library(MySQL::Client mysqlclient mysql/mysql.h mysqlclient)
endfunction()

find_mysql_client()
find_optional_pkg_or_library(Hiredis::Hiredis hiredis hiredis/hiredis.h hiredis)

add_library(asio_owen_deps INTERFACE)
target_link_libraries(asio_owen_deps INTERFACE
    asio_owen_options
    asio_owen_asio
    asio_owen_spdlog
    asio_owen_jwtcpp
    MySQL::Client
    Hiredis::Hiredis
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

add_library(asio_owen_core STATIC
        src/db/mysql_connection.cpp
        src/db/mysql_pool_stats.cpp
        src/db/mysql_result_json.cpp
        src/db/redis_command.cpp
        src/db/redis_connection.cpp
        src/db/redis_pool_stats.cpp
        src/db/redis_reply.cpp
)
target_link_libraries(asio_owen_core PUBLIC asio_owen_deps)

add_library(asio_owen_app STATIC
    src/app/routes.cpp
    src/app/public_routes.cpp
    src/app/admin/config_admin.cpp
    src/app/admin/config_history.cpp
    src/app/config_sync_service.cpp
    src/app/admin/admin_routes.cpp
    src/app/admin/admin_config_routes.cpp
    src/app/admin/admin_history_routes.cpp
    src/app/admin/admin_history_operation.cpp
)
target_link_libraries(asio_owen_app PUBLIC asio_owen_core)
add_dependencies(asio_owen_app asio_owen_generated_assets)

# 可执行文件
add_executable(server
        src/main.cpp
        src/app/application.cpp
        picohttpparser.c
)
target_link_libraries(server PRIVATE asio_owen_app)
target_compile_options(server PRIVATE $<$<CONFIG:Release>:-g>)
add_dependencies(server asio_owen_generated_assets)

# 复制 config.d/ 目录（如果存在），排除本地覆盖文件
if(EXISTS ${PROJECT_SOURCE_DIR}/config.d)
    file(COPY ${PROJECT_SOURCE_DIR}/config.d/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/config.d/
        PATTERN "99-local.ini" EXCLUDE)
    message(STATUS "config.d: copied to build directory (excluded 99-local.ini)")
endif()

# ==================== GoogleTest（优先本地，否则 FetchContent 自动拉取）====================
include(CTest)
if(BUILD_TESTING)
    if(EXISTS ${PROJECT_SOURCE_DIR}/googletest/CMakeLists.txt)
        message(STATUS "googletest: using local ${PROJECT_SOURCE_DIR}/googletest")
        add_subdirectory(googletest)
    else()
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
        message(STATUS "googletest: fetched from remote")
    endif()
    add_subdirectory(tests)
    if(ASIO_OWEN_BUILD_INTEGRATION_TESTS)
        add_test(NAME redis_fault_matrix
            COMMAND bash ${PROJECT_SOURCE_DIR}/tests/redis_fault_matrix.sh)
        set_tests_properties(redis_fault_matrix PROPERTIES LABELS integration)
    endif()
endif()
