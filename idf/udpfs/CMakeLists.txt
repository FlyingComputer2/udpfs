# ESP-IDF component: the udpfs server as a service on its own reactor and
# thread (src/udpfs_idf/service.hpp). Add <udpfs>/idf/udpfs to
# EXTRA_COMPONENT_DIRS.
#
# From the parent project:
# - UDPFS_CONFIG_DIR: the directory of its udpfs_config.hpp (required, see
#   src/udpfs/config.hpp).
# - bfc, declared with FetchContent (name `bfc`); this repo pins none, so a
#   firmware keeps one bfc for all its libraries. BFC_CONFIG_DIR (its
#   bfc_config.hpp) is added when set.
set(UDPFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT UDPFS_CONFIG_DIR)
        message(FATAL_ERROR "udpfs: set UDPFS_CONFIG_DIR to the directory of the project's udpfs_config.hpp")
    endif()
    include(FetchContent)
    FetchContent_GetProperties(bfc)
    if(NOT bfc_POPULATED)
        FetchContent_Populate(bfc)
    endif()
endif()

idf_component_register(
    SRCS ${UDPFS_ROOT}/src/udpfs/wire.cpp
         ${UDPFS_ROOT}/src/udpfs/server.cpp
         ${UDPFS_ROOT}/src/udpfs_idf/service.cpp
    INCLUDE_DIRS ${UDPFS_ROOT}/src
    PRIV_INCLUDE_DIRS compat
    REQUIRES freertos
    PRIV_REQUIRES lwip log esp_timer esp_hw_support pthread heap vfs
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_include_directories(${COMPONENT_LIB} PRIVATE ${bfc_SOURCE_DIR}/src ${UDPFS_CONFIG_DIR})
    if(BFC_CONFIG_DIR)
        target_include_directories(${COMPONENT_LIB} PRIVATE ${BFC_CONFIG_DIR})
    endif()
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fexceptions>)
