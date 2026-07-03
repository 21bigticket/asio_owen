cmake_minimum_required(VERSION 3.20)
project(asio_owen LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ==================== 通用编译选项 ====================
add_library(asio_owen_options INTERFACE)
target_compile_features(asio_owen_options INTERFACE cxx_std_20)
target_compile_definitions(asio_owen_options INTERFACE ASIO_STANDALONE ASIO_HAS_CO_AWAIT)
target_include_directories(asio_owen_options INTERFACE ${PROJECT_SOURCE_DIR}/src)

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
    MySQL::Client
    Hiredis::Hiredis
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# 可执行文件
add_executable(server
        src/main.cpp
        picohttpparser.c
)
target_link_libraries(server PRIVATE asio_owen_deps)

# 构建后自动复制 config.ini 到输出目录
configure_file(${PROJECT_SOURCE_DIR}/config.ini ${CMAKE_CURRENT_BINARY_DIR}/config.ini COPYONLY)

# ==================== GoogleTest（本地已有才启用，否则跳过）====================
if(EXISTS ${PROJECT_SOURCE_DIR}/googletest/CMakeLists.txt)
    enable_testing()
    add_subdirectory(googletest)
    add_subdirectory(tests)
endif()
