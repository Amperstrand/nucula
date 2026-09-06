# Host unit-test build for nucula's platform-neutral core.
#
#   cmake -B build && cmake --build build && ./build/nucula_host_tests
#
# Compiles the wallet/crypto/codec sources from ../../main against
# system mbedtls/cJSON/libsecp256k1, the tinycbor sources from the
# espressif component, and thin shims for the ESP-IDF surface
# (logging, RNG, NVS). No hardware, no network.

cmake_minimum_required(VERSION 3.16)
project(nucula_host_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)

# Build against the same vendored mbedtls the firmware targets
# (ESP-IDF 5.4 bundles 3.x; system mbedtls on this host is 2.28 with an
# incompatible sha256 API). Override with -DMBEDTLS_DIR=... if needed.
set(MBEDTLS_DIR "$ENV{HOME}/esp/esp-idf/components/mbedtls/mbedtls"
    CACHE PATH "mbedtls source (ESP-IDF vendored 3.x)")
if(NOT EXISTS ${MBEDTLS_DIR}/include/mbedtls/sha256.h)
    message(FATAL_ERROR "vendored mbedtls not found at ${MBEDTLS_DIR}; pass -DMBEDTLS_DIR=")
endif()

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../main)
set(TINYCBOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__cbor/tinycbor/src)

file(GLOB TINYCBOR_SRC ${TINYCBOR_DIR}/*.c)

set(MBEDTLS_SRC
    ${MBEDTLS_DIR}/library/platform_util.c
    ${MBEDTLS_DIR}/library/constant_time.c
    ${MBEDTLS_DIR}/library/sha256.c
    ${MBEDTLS_DIR}/library/md.c
    ${MBEDTLS_DIR}/library/md5.c
    ${MBEDTLS_DIR}/library/sha1.c
    ${MBEDTLS_DIR}/library/sha512.c
    ${MBEDTLS_DIR}/library/ripemd160.c
    ${MBEDTLS_DIR}/library/pkcs5.c
    ${MBEDTLS_DIR}/library/base64.c
)

add_executable(nucula_host_tests
    test_main.cpp
    nvs_shim.cpp
    ${TINYCBOR_SRC}
    ${MBEDTLS_SRC}
    ${MAIN_DIR}/hex.c
    ${MAIN_DIR}/base64url.cpp
    ${MAIN_DIR}/crypto.c
    ${MAIN_DIR}/crypto_bls.c
    ${MAIN_DIR}/crypto_test.c
    ${MAIN_DIR}/cashu_json.cpp
    ${MAIN_DIR}/cashu_cbor.cpp
    ${MAIN_DIR}/keyset.cpp
    ${MAIN_DIR}/nut10.cpp
    ${MAIN_DIR}/bip39.c
    ${MAIN_DIR}/unit.cpp
    ${MAIN_DIR}/selftest.cpp
    ${MAIN_DIR}/wallet.cpp
    ${MAIN_DIR}/wallet_nvs.cpp
    ${MAIN_DIR}/wallet_blind.cpp
    ${MAIN_DIR}/wallet_identity.cpp
    ${MAIN_DIR}/wallet_selftest.cpp
)

target_include_directories(nucula_host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
    ${MAIN_DIR}
    ${TINYCBOR_DIR}
    ${MBEDTLS_DIR}/include
)

target_link_libraries(nucula_host_tests PRIVATE
    PkgConfig::CJSON
    PkgConfig::SECP256K1
)

# The wallet sources call the http-backed flow methods (defined in
# wallet_flows.cpp against esp_http_client); the test build provides
# minimal fakes so the pure logic links.
target_compile_definitions(nucula_host_tests PRIVATE
    NUCULA_HOST_TEST
    MBEDTLS_CONFIG_FILE="mbedtls_config_host.h"
)
