# udpfs: host build of the core (wire + POSIX server), the client library, the
# udpfs command line tool and the tests. ESP-IDF: idf/udpfs.
#
# A parent project sets UDPFS_CONFIG_DIR (the directory of its
# udpfs_config.hpp, see src/udpfs/config.hpp); built on its own, udpfs uses
# tools/config (POSIX map by name).
cmake_minimum_required(VERSION 3.16)
project(udpfs CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(UDPFS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(UDPFS_TOP_LEVEL ON)
endif()
option(UDPFS_BUILD_TOOLS "Build the udpfs command line tool" ${UDPFS_TOP_LEVEL})
option(UDPFS_BUILD_TESTS "Build the udpfs tests" ${UDPFS_TOP_LEVEL})

if(NOT UDPFS_CONFIG_DIR)
    if(UDPFS_TOP_LEVEL)
        set(UDPFS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/config)
    else()
        message(FATAL_ERROR "udpfs: set UDPFS_CONFIG_DIR to the directory of your udpfs_config.hpp")
    endif()
endif()

add_library(udpfs_core src/udpfs/wire.cpp src/udpfs/server.cpp)
target_include_directories(udpfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${UDPFS_CONFIG_DIR})
target_compile_options(udpfs_core PRIVATE -Wall -Wextra)

add_library(udpfs_client src/udpfs/client.cpp src/udpfs/udp_transport.cpp)
target_link_libraries(udpfs_client PUBLIC udpfs_core)
target_compile_options(udpfs_client PRIVATE -Wall -Wextra)

if(UDPFS_BUILD_TOOLS)
    add_executable(udpfs tools/udpfs.cpp)
    target_link_libraries(udpfs PRIVATE udpfs_client)
    target_compile_options(udpfs PRIVATE -Wall -Wextra)
    install(TARGETS udpfs RUNTIME DESTINATION bin)
    # A local test server: serves this machine's filesystem on a UDP port.
    add_executable(udpfs_serve tools/udpfs_serve.cpp)
    target_link_libraries(udpfs_serve PRIVATE udpfs_core)
    target_compile_options(udpfs_serve PRIVATE -Wall -Wextra)
endif()

if(UDPFS_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(udpfs_tests tests/wire_test.cpp tests/server_test.cpp tests/client_test.cpp)
    target_link_libraries(udpfs_tests PRIVATE udpfs_client GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(udpfs_tests)
endif()
