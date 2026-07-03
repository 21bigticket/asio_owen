cmake_minimum_required(VERSION 3.20)
project(asio_owen)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 零 Boost
add_definitions(-DASIO_STANDALONE -DASIO_HAS_CO_AWAIT)

# ==================== ASIO + spdlog（优先本地，否则 FetchContent 自动拉取）====================
include(FetchContent)

if(EXISTS ${PROJECT_SOURCE_DIR}/asio/include/asio.hpp)
    include_directories(${PROJECT_SOURCE_DIR}/asio/include)
    message(STATUS "ASIO: using local ${PROJECT_SOURCE_DIR}/asio")
else()
    FetchContent_Declare(
        asio
        URL      https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-0.tar.gz
        URL_HASH SHA256=c24922183dd91632140e65e7ef4c48c4f6f219ec4d9e16c9ec4536ee442669f4
    )
    FetchContent_MakeAvailable(asio)
    include_directories(${asio_SOURCE_DIR}/include)
    message(STATUS "ASIO: fetched from remote")
endif()

if(EXISTS ${PROJECT_SOURCE_DIR}/spdlog/include/spdlog/spdlog.h)
    include_directories(${PROJECT_SOURCE_DIR}/spdlog/include)
    message(STATUS "spdlog: using local ${PROJECT_SOURCE_DIR}/spdlog")
else()
    FetchContent_Declare(
        spdlog
        URL      https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=68fea002cd716e62e9e6ee8d5066c9a0eee13f15c8026bd4e4b2d2a87e1d6473
    )
    FetchContent_MakeAvailable(spdlog)
    include_directories(${spdlog_SOURCE_DIR}/include)
    message(STATUS "spdlog: fetched from remote")
endif()

include_directories(
        ${PROJECT_SOURCE_DIR}/src
)

# ==================== 系统依赖路径 ====================
if(APPLE)
    # macOS Homebrew 路径
    foreach(prefix /usr/local /opt/homebrew)
        if(EXISTS ${prefix}/opt/mysql-client)
            include_directories(${prefix}/opt/mysql-client/include)
            link_directories(${prefix}/opt/mysql-client/lib)
        endif()
        if(EXISTS ${prefix}/opt/hiredis)
            include_directories(${prefix}/opt/hiredis/include)
            link_directories(${prefix}/opt/hiredis/lib)
        endif()
        if(EXISTS ${prefix}/opt/openssl@3)
            include_directories(${prefix}/opt/openssl@3/include)
            link_directories(${prefix}/opt/openssl@3/lib)
        endif()
        if(EXISTS ${prefix}/include/boost)
            include_directories(${prefix}/include)
        endif()
    endforeach()
elseif(UNIX)
    # Linux: 使用 pkg-config 查找系统安装的库
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSQL mysqlclient IMPORTED_TARGET)
        if(MYSQL_FOUND)
            include_directories(${MYSQL_INCLUDE_DIRS})
            link_directories(${MYSQL_LIBRARY_DIRS})
        endif()
        pkg_check_modules(HIREDIS hiredis IMPORTED_TARGET)
        if(HIREDIS_FOUND)
            include_directories(${HIREDIS_INCLUDE_DIRS})
            link_directories(${HIREDIS_LIBRARY_DIRS})
        endif()
        pkg_check_modules(OPENSSL openssl IMPORTED_TARGET)
        if(OPENSSL_FOUND)
            include_directories(${OPENSSL_INCLUDE_DIRS})
            link_directories(${OPENSSL_LIBRARY_DIRS})
        endif()
    endif()
    # fallback: 标准系统路径
    include_directories(/usr/include /usr/local/include)
endif()

# 链接库
link_libraries(pthread mysqlclient hiredis ssl crypto)

# 可执行文件
add_executable(server
        src/main.cpp
        picohttpparser.c
)

# 构建后自动复制 config.ini 到输出目录
configure_file(${PROJECT_SOURCE_DIR}/config.ini ${CMAKE_CURRENT_BINARY_DIR}/config.ini COPYONLY)

# ==================== GoogleTest（本地已有才启用，否则跳过）====================
if(EXISTS ${PROJECT_SOURCE_DIR}/googletest/CMakeLists.txt)
    enable_testing()
    add_subdirectory(googletest)
    include_directories(${PROJECT_SOURCE_DIR}/googletest/googletest/include)
    add_subdirectory(tests)
endif()
