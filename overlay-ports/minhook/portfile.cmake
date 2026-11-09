# Upstream: microsoft/vcpkg ports/minhook. Deltas:
# - Pinned to 8af6b4ac (2026-03), patches dropped, cmake config path changed
# - Static linkage
if (VCPKG_TARGET_ARCHITECTURE MATCHES "x86")
    set(BUILD_ARCH "Win32")
    set(OUTPUT_DIR "Win32")
elseif (VCPKG_TARGET_ARCHITECTURE MATCHES "x64")
    set(BUILD_ARCH "x64")
    set(OUTPUT_DIR "Win64")
else()
    message(FATAL_ERROR "Unsupported architecture: ${VCPKG_TARGET_ARCHITECTURE}")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO TsudaKageyu/minhook
    REF "8af6b4acae5a9388fd742b56fa79ece89d96f823"
    SHA512 ef3098e9d019aa7e01d82413ba5457f3aed35420cab9eacd33600e0350ee79272b85574a2a44c902eb2c13549b67aef985c91bc2d858502c3cf64b7d475e102a
    HEAD_REF master
)

set(VCPKG_LIBRARY_LINKAGE static) # static lib

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH share/minhook)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")