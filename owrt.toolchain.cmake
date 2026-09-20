#
# CMake toolchain file for building against an OpenWrt SDK by hand.
#
# Only needed for out-of-package builds, e.g. iterating on a single .so. The
# OpenWrt package build does not use this - it gets its toolchain from the SDK.
#
#   export STAGING_DIR=<sdk>/staging_dir
#   cmake -S . -B build-owrt \
#         -DCMAKE_TOOLCHAIN_FILE=owrt.toolchain.cmake \
#         -DAST_HEADER_DIR=$STAGING_DIR/target-<arch>_musl/usr/include
#
# Everything is derived from STAGING_DIR, so nothing here is tied to one machine
# or one architecture.
#

IF(NOT DEFINED ENV{STAGING_DIR})
    MESSAGE(FATAL_ERROR "Set STAGING_DIR to <openwrt-sdk>/staging_dir before configuring")
ENDIF()

SET(_staging $ENV{STAGING_DIR})

FILE(GLOB _toolchains "${_staging}/toolchain-*")
LIST(LENGTH _toolchains _toolchain_count)
IF(NOT _toolchain_count EQUAL 1)
    MESSAGE(FATAL_ERROR "Expected exactly one toolchain-* under ${_staging}, found ${_toolchain_count}")
ENDIF()
LIST(GET _toolchains 0 _toolchain)

FILE(GLOB _targets "${_staging}/target-*")
LIST(LENGTH _targets _target_count)
IF(NOT _target_count EQUAL 1)
    MESSAGE(FATAL_ERROR "Expected exactly one target-* under ${_staging}, found ${_target_count}")
ENDIF()
LIST(GET _targets 0 _target)

FILE(GLOB _gcc "${_toolchain}/bin/*-openwrt-linux-musl-gcc")
LIST(LENGTH _gcc _gcc_count)
IF(_gcc_count LESS 1)
    MESSAGE(FATAL_ERROR "No *-openwrt-linux-musl-gcc found in ${_toolchain}/bin")
ENDIF()
LIST(GET _gcc 0 _cc)

SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_C_COMPILER "${_cc}")
SET(CMAKE_SYSROOT "${_target}")
SET(CMAKE_FIND_ROOT_PATH "${_target};${_toolchain}")
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
