# Toolchain for cross-compiling DuckDB for OpenHarmony (arm64).
#
# OHOS_SDK must point at the "native" directory of the OpenHarmony SDK, i.e.
# the directory that contains llvm/ and sysroot/.
#
# Two SDK layouts are supported:
#   * device SDKs ship aarch64-unknown-linux-ohos-clang(++) wrappers that add
#     the target triple and sysroot themselves
#   * the Linux x64 SDK ships plain clang(++), so the flags are added here
#
# Usage:
#   OHOS_SDK=/path/to/ohos-sdk/linux/native cmake -G Ninja -B build/ohos \
#     -DCMAKE_TOOLCHAIN_FILE=$PWD/scripts/ohos-toolchain.cmake \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DDUCKDB_EXPLICIT_PLATFORM=openharmony_arm64
#
# DUCKDB_EXPLICIT_PLATFORM is required because the platform probe binary cannot
# be executed on the build host.

if(NOT OHOS_SDK)
  if(DEFINED ENV{OHOS_SDK})
    set(OHOS_SDK "$ENV{OHOS_SDK}")
  else()
    message(
      FATAL_ERROR
        "OHOS_SDK must point to the OpenHarmony SDK 'native' directory")
  endif()
endif()

if(EXISTS "${OHOS_SDK}/llvm/bin/aarch64-unknown-linux-ohos-clang++")
  set(CMAKE_C_COMPILER
      "${OHOS_SDK}/llvm/bin/aarch64-unknown-linux-ohos-clang")
  set(CMAKE_CXX_COMPILER
      "${OHOS_SDK}/llvm/bin/aarch64-unknown-linux-ohos-clang++")
elseif(EXISTS "${OHOS_SDK}/llvm/bin/clang++")
  if(NOT EXISTS "${OHOS_SDK}/sysroot")
    message(FATAL_ERROR "No OpenHarmony sysroot found in ${OHOS_SDK}")
  endif()
  set(CMAKE_C_COMPILER "${OHOS_SDK}/llvm/bin/clang")
  set(CMAKE_CXX_COMPILER "${OHOS_SDK}/llvm/bin/clang++")
  set(OHOS_TARGET_FLAGS
      "-target aarch64-linux-ohos --sysroot=${OHOS_SDK}/sysroot -D__MUSL__")
  set(CMAKE_C_FLAGS_INIT "${OHOS_TARGET_FLAGS}")
  set(CMAKE_CXX_FLAGS_INIT "${OHOS_TARGET_FLAGS}")
else()
  message(FATAL_ERROR "No OpenHarmony toolchain found in ${OHOS_SDK}")
endif()

# OpenHarmony is a Linux-like musl target, so reuse the Linux platform module
# and rely on the __OHOS__/__MUSL__ compile definitions for platform detection.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Test binaries cannot run on the build host.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
