cmake_minimum_required(VERSION 3.14...3.29)

include("${CMAKE_CURRENT_LIST_DIR}/../architecture_flags.cmake")

function(assert_architecture_flags NAME SYSTEM_NAME PROCESSOR COMPILER PROFILE EXPLICIT_PLATFORM OSX_ARCHITECTURES
         USE_NATIVE IS_ANDROID EXPECTED_FLAGS)
  set(CMAKE_SYSTEM_NAME "${SYSTEM_NAME}")
  set(CMAKE_SYSTEM_PROCESSOR "${PROCESSOR}")
  set(CMAKE_CXX_COMPILER_ID "${COMPILER}")
  set(DUCKDB_OPTIMIZATION_PROFILE "${PROFILE}")
  set(DUCKDB_EXPLICIT_PLATFORM "${EXPLICIT_PLATFORM}")
  set(CMAKE_OSX_ARCHITECTURES "${OSX_ARCHITECTURES}")
  set(NATIVE_ARCH "${USE_NATIVE}")
  set(ANDROID "${IS_ANDROID}")

  duckdb_resolve_architecture_flags(ACTUAL_FLAGS)
  if(NOT "${ACTUAL_FLAGS}" STREQUAL "${EXPECTED_FLAGS}")
    message(FATAL_ERROR
      "${NAME}: expected architecture flags '${EXPECTED_FLAGS}', got '${ACTUAL_FLAGS}'")
  endif()
endfunction()

assert_architecture_flags(linux_amd64_cli Linux x86_64 GNU CLI "" "" FALSE FALSE
  "-march=x86-64-v3;-mtune=generic")
assert_architecture_flags(linux_amd64_extension Linux x86_64 Clang EXTENSION "" "" FALSE FALSE
  "-march=x86-64-v2;-mtune=generic")
assert_architecture_flags(linux_arm64 Linux aarch64 GNU CLI "" "" FALSE FALSE
  "-march=armv8-a;-mtune=generic")
assert_architecture_flags(windows_amd64_clang_cl Windows AMD64 Clang CLI "" "" FALSE FALSE
  "-march=haswell;-mtune=generic")
assert_architecture_flags(windows_amd64_mingw Windows unknown GNU EXTENSION windows_amd64_mingw "" FALSE FALSE
  "-march=haswell;-mtune=generic")
assert_architecture_flags(windows_arm64_clang_cl Windows ARM64 Clang EXTENSION "" "" FALSE FALSE
  "-march=armv8-a;-mtune=generic")
assert_architecture_flags(windows_msvc_baseline Windows AMD64 MSVC CLI "" "" FALSE FALSE "")
assert_architecture_flags(explicit_platform Linux unknown GNU EXTENSION linux_amd64_musl "" FALSE FALSE
  "-march=x86-64-v2;-mtune=generic")
assert_architecture_flags(macos_arm64 Darwin arm64 AppleClang CLI "" arm64 FALSE FALSE
  "-mcpu=apple-m1")
assert_architecture_flags(macos_universal Darwin arm64 AppleClang CLI "" "x86_64;arm64" FALSE FALSE
  "-Xarch_arm64;-mcpu=apple-m1")
assert_architecture_flags(macos_amd64 Darwin x86_64 AppleClang CLI "" x86_64 FALSE FALSE "")
assert_architecture_flags(disabled Linux x86_64 GNU NONE "" "" FALSE FALSE "")
assert_architecture_flags(native_override Linux x86_64 GNU CLI "" "" TRUE FALSE "")
assert_architecture_flags(android_unchanged Linux aarch64 Clang CLI android_arm64 "" FALSE TRUE "")

function(assert_x86_64_v3_check NAME FLAGS EXPECTED)
  duckdb_requires_x86_64_v3_check("${FLAGS}" ACTUAL)
  if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
      "${NAME}: expected x86-64-v3 check '${EXPECTED}', got '${ACTUAL}'")
  endif()
endfunction()

assert_x86_64_v3_check(x86_64_v3 "-march=x86-64-v3;-mtune=generic" TRUE)
assert_x86_64_v3_check(haswell "-march=haswell;-mtune=generic" TRUE)
assert_x86_64_v3_check(x86_64_v2 "-march=x86-64-v2;-mtune=generic" FALSE)
assert_x86_64_v3_check(arm64 "-march=armv8-a;-mtune=generic" FALSE)

function(assert_target_architecture_flags NAME TARGET FLAGS EXPECTED_FLAGS EXPECTED_CHECK)
  duckdb_resolve_target_architecture_flags("${TARGET}" "${FLAGS}" ACTUAL_FLAGS ACTUAL_CHECK)
  if(NOT "${ACTUAL_FLAGS}" STREQUAL "${EXPECTED_FLAGS}")
    message(FATAL_ERROR
      "${NAME}: expected target flags '${EXPECTED_FLAGS}', got '${ACTUAL_FLAGS}'")
  endif()
  if(NOT "${ACTUAL_CHECK}" STREQUAL "${EXPECTED_CHECK}")
    message(FATAL_ERROR
      "${NAME}: expected target check '${EXPECTED_CHECK}', got '${ACTUAL_CHECK}'")
  endif()
endfunction()

assert_target_architecture_flags(shell_v3 shell "-march=x86-64-v3;-mtune=generic" "" TRUE)
assert_target_architecture_flags(core_v3 duckdb_static "-march=x86-64-v3;-mtune=generic"
  "-march=x86-64-v3;-mtune=generic" FALSE)
assert_target_architecture_flags(shell_v2 shell "-march=x86-64-v2;-mtune=generic"
  "-march=x86-64-v2;-mtune=generic" FALSE)

message(STATUS "Architecture flag tests passed")
