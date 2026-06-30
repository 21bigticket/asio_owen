cmake_minimum_required(VERSION 3.20)
project(asio_full_stack)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

# 零 Boost
add_definitions(-DASIO_STANDALONE -DASIO_HAS_CO_AWAIT)

include_directories(
        ${PROJECT_SOURCE_DIR}/asio/include
        ${PROJECT_SOURCE_DIR}/aedis/include
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/spdlog/include
)

# ==================== macOS Homebrew 依赖路径 ====================
if(APPLE)
    # ---------- MySQL Client ----------
    if(EXISTS /usr/local/opt/mysql-client)
        # 注意：只到 include 这一层，不要加 /mysql
        include_directories(/usr/local/opt/mysql-client/include)
        link_directories(/usr/local/opt/mysql-client/lib)
    endif()

    # Apple Silicon 兼容
    if(EXISTS /opt/homebrew/opt/mysql-client)
        include_directories(/opt/homebrew/opt/mysql-client/include)
        link_directories(/opt/homebrew/opt/mysql-client/lib)
    endif()

    # ---------- Boost (from Homebrew) ----------
    if(EXISTS /opt/homebrew/include/boost)
        include_directories(/opt/homebrew/include)
    endif()
    if(EXISTS /usr/local/include/boost)
        include_directories(/usr/local/include)
    endif()

    # ---------- hiredis ----------
    if(EXISTS /opt/homebrew/opt/hiredis)
        include_directories(/opt/homebrew/opt/hiredis/include)
        link_directories(/opt/homebrew/opt/hiredis/lib)
    endif()
    if(EXISTS /usr/local/opt/hiredis)
        include_directories(/usr/local/opt/hiredis/include)
        link_directories(/usr/local/opt/hiredis/lib)
    endif()

    # ---------- OpenSSL 3 ----------
    if(EXISTS /usr/local/opt/openssl@3)
        include_directories(/usr/local/opt/openssl@3/include)
        link_directories(/usr/local/opt/openssl@3/lib)
    endif()
    if(EXISTS /opt/homebrew/opt/openssl@3)
        include_directories(/opt/homebrew/opt/openssl@3/include)
        link_directories(/opt/homebrew/opt/openssl@3/lib)
    endif()
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
