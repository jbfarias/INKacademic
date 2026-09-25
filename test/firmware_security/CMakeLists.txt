cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(firmware_security_tests C CXX)
  enable_testing()
endif()
get_filename_component(REPO_ROOT "../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(WOLFSSL_ROOT "${REPO_ROOT}/.pio/libdeps/x4-pro/Arduino-wolfSSL/src" CACHE PATH "Arduino-wolfSSL source directory (5.7.2)")
if(NOT EXISTS "${WOLFSSL_ROOT}/wolfcrypt/src/ed25519.c")
  message(FATAL_ERROR "Install PlatformIO x4-pro dependencies or set WOLFSSL_ROOT to Arduino-wolfSSL 5.7.2/src")
endif()
find_package(OpenSSL REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_library(ota_test_crypto STATIC
  ${WOLFSSL_ROOT}/wolfcrypt/src/ed25519.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/fe_operations.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/ge_operations.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/sha512.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/hash.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/memory.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/wc_port.c
  ${WOLFSSL_ROOT}/wolfcrypt/src/logging.c)
target_include_directories(ota_test_crypto PUBLIC crypto ${WOLFSSL_ROOT})
target_compile_definitions(ota_test_crypto PUBLIC WOLFSSL_USER_SETTINGS WOLFSSL_OPTIONS_H
  INKADEMIC_REQUIRE_SIGNED_OTA=1 HAVE_ED25519 HAVE_ED25519_VERIFY HAVE_ED25519_KEY_IMPORT WOLFSSL_SHA512)
add_executable(firmware_security_test FirmwareSecurityTest.cpp ${REPO_ROOT}/src/network/FirmwareFlasher.cpp ${REPO_ROOT}/src/network/OtaManualUpdater.cpp)
target_include_directories(firmware_security_test PRIVATE stubs ${REPO_ROOT}/include ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/Memory)
target_link_libraries(firmware_security_test PRIVATE ota_test_crypto OpenSSL::Crypto)
target_compile_options(firmware_security_test PRIVATE -UNDEBUG -Wall -Wextra -Wno-deprecated-declarations)
option(OTA_SANITIZERS "Enable host memory and undefined-behavior checks" ON)
if(OTA_SANITIZERS)
  foreach(target ota_test_crypto firmware_security_test)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endforeach()
  # wolfSSL 5.7.2 ref10 field arithmetic deliberately uses signed shifts.
  # Keep ASan and other UBSan checks on that dependency, suppress only shift-base
  # there; all sanitizer checks remain enabled on our production updater code.
  target_compile_options(ota_test_crypto PRIVATE -fno-sanitize=shift-base)
  target_link_options(firmware_security_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME firmware_security COMMAND firmware_security_test)
add_test(NAME firmware_release_metadata COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ReleaseMetadataTest.py)

add_test(NAME firmware_bitmap_orientation COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/RendererImageTest.py)

set_tests_properties(firmware_security PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
add_subdirectory(${REPO_ROOT}/test/ota_sector_writer ${CMAKE_CURRENT_BINARY_DIR}/ota_sector_writer)

find_program(NODE_EXECUTABLE node REQUIRED)
add_test(NAME firmware_browser_controls COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/FirmwarePageTest.js)
