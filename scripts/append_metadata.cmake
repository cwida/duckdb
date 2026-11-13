cmake_minimum_required(VERSION 3.15...3.29)

# Usage: cmake -DEXTENSION=path/to/extension.duckdb_extension -DPLATFORM_FILE=README.md -DDUCKDB_VERSION=tag1 -DEXTENSION_VERSION=tag2 -P scripts/append_metadata.cmake
# Currently hardcoded to host up to 8 fields
# Example: ./scripts/append_metadata.sh file.duckdb_extension git_hash_duckdb_file git_hash_extension_file platform_file

set(EXTENSION "" CACHE PATH "Path to the extension where to add metadata")
set(NULL_FILE "" CACHE PATH "Path to file containing a single 0 byte")
set(META1 "4" CACHE STRING "Metadata field" FORCE)
set(PLATFORM_FILE "" CACHE PATH "Metadata field: path of file containing duckdb_platform")
set(VERSION_FIELD "" CACHE STRING "Metadata field: path of file containing duckdb_version")
set(EXTENSION_VERSION "" CACHE STRING "Metadata field: path of file containing extension_version")
set(ABI_TYPE "" CACHE STRING "Metadata field: the ABI type of the extension")
set(META6 "" CACHE STRING "Metadata field")
set(META7 "" CACHE STRING "Metadata field")
set(META8 "" CACHE STRING "Metadata field")
set(SIGNATURE_SIZE "256" CACHE STRING "Size in bytes of the trailing signature slot: 256/384/512 for RSA-2048/3072/4096")
set(SUPPORTED_SIGNATURE_SIZES 256 384 512)

if(NOT SIGNATURE_SIZE IN_LIST SUPPORTED_SIGNATURE_SIZES)
  message(FATAL_ERROR "SIGNATURE_SIZE must be one of ${SUPPORTED_SIGNATURE_SIZES}, got '${SIGNATURE_SIZE}'")
endif()

# META7 (reader field 6) holds the signature-size marker: empty for legacy 256, else the size in decimal.
if(SIGNATURE_SIZE STREQUAL "256")
  set(SIGNATURE_MARKER "")
else()
  set(SIGNATURE_MARKER "${SIGNATURE_SIZE}")
endif()
if(NOT META7 STREQUAL "" AND NOT META7 STREQUAL "${SIGNATURE_MARKER}")
  if(SIGNATURE_MARKER STREQUAL "")
    message(FATAL_ERROR "META7 is reserved for the signature-size marker; it must be empty, got '${META7}'")
  else()
    message(FATAL_ERROR "META7 is reserved for the signature-size marker; it must be empty or '${SIGNATURE_MARKER}', got '${META7}'")
  endif()
endif()
set(META7 "${SIGNATURE_MARKER}")

# null.txt should contain exactly 1 byte of value \x00
file(READ "${NULL_FILE}" EMPTY_BYTE)

string(REPEAT "${EMPTY_BYTE}" 32 EMPTY_32)
string(REPEAT "${EMPTY_BYTE}" 256 EMPTY_256)
string(REPEAT "${EMPTY_BYTE}" ${SIGNATURE_SIZE} EMPTY_SIGNATURE)

# Appended after the WASM header: [meta][sig 256] for legacy, else [M1][sig][M2 = M1][256 zeros].
if(SIGNATURE_SIZE STREQUAL "256")
  set(FOOTER_SIZE 512)
else()
  math(EXPR FOOTER_SIZE "768 + ${SIGNATURE_SIZE}")
endif()

# Appends the unsigned LEB128 encoding of VALUE to the variable OUT, which must not hold NUL bytes.
function(append_uleb128 OUT VALUE)
  set(encoded "${${OUT}}")
  while(TRUE)
    math(EXPR byte "${VALUE} & 127")
    math(EXPR VALUE "${VALUE} >> 7")
    if(VALUE GREATER 0)
      math(EXPR byte "${byte} | 128")
    endif()
    string(ASCII ${byte} char)
    string(APPEND encoded "${char}")
    if(VALUE EQUAL 0)
      break()
    endif()
  endwhile()
  set(${OUT} "${encoded}" PARENT_SCOPE)
endfunction()

set(FOOTER_SIZE_LEB "")
append_uleb128(FOOTER_SIZE_LEB ${FOOTER_SIZE})
string(LENGTH "${FOOTER_SIZE_LEB}" FOOTER_SIZE_LEB_LENGTH)
# Outer section length: 1 (name length) + 16 (name) + inner size field + footer
math(EXPR SECTION_SIZE "1 + 16 + ${FOOTER_SIZE_LEB_LENGTH} + ${FOOTER_SIZE}")
set(SECTION_SIZE_LEB "")
append_uleb128(SECTION_SIZE_LEB ${SECTION_SIZE})

# 0 for custom section
string(APPEND CUSTOM_SECTION "${EMPTY_BYTE}")
string(APPEND CUSTOM_SECTION "${SECTION_SIZE_LEB}")
# 10 in hex = 16 in decimal, length of name, 1 byte
string(ASCII 16 CUSTOM_SECTION_2)
string(APPEND CUSTOM_SECTION "${CUSTOM_SECTION_2}")

# the name of the WebAssembly custom section, 16 bytes
string(APPEND CUSTOM_SECTION "duckdb_signature")

# Inner footer size
string(APPEND CUSTOM_SECTION "${FOOTER_SIZE_LEB}")

# Second metadata-field is special, since content comes from a file
file(READ "${PLATFORM_FILE}" META2)

# Build METADATA variables by appending and then truncating empty strings
string(SUBSTRING "${META1}${EMPTY_32}" 0 32 METADATA1)
string(SUBSTRING "${META2}${EMPTY_32}" 0 32 METADATA2)
string(SUBSTRING "${VERSION_FIELD}${EMPTY_32}" 0 32 METADATA3)
string(SUBSTRING "${EXTENSION_VERSION}${EMPTY_32}" 0 32 METADATA4)
string(SUBSTRING "${ABI_TYPE}${EMPTY_32}" 0 32 METADATA5)
string(SUBSTRING "${META6}${EMPTY_32}" 0 32 METADATA6)
string(SUBSTRING "${META7}${EMPTY_32}" 0 32 METADATA7)
string(SUBSTRING "${META8}${EMPTY_32}" 0 32 METADATA8)

# Append metadata fields, backwards
string(APPEND METADATA_BLOCK "${METADATA8}")
string(APPEND METADATA_BLOCK "${METADATA7}")
string(APPEND METADATA_BLOCK "${METADATA6}")
string(APPEND METADATA_BLOCK "${METADATA5}")
string(APPEND METADATA_BLOCK "${METADATA4}")
string(APPEND METADATA_BLOCK "${METADATA3}")
string(APPEND METADATA_BLOCK "${METADATA2}")
string(APPEND METADATA_BLOCK "${METADATA1}")
string(APPEND CUSTOM_SECTION "${METADATA_BLOCK}")

# Append signature (yet to be computed)
string(APPEND CUSTOM_SECTION "${EMPTY_SIGNATURE}")

# Larger signatures repeat the metadata at the fixed tail position, followed by a zeroed legacy slot
if(NOT SIGNATURE_SIZE STREQUAL "256")
  string(APPEND CUSTOM_SECTION "${METADATA_BLOCK}${EMPTY_256}")
endif()

# Append generated custom section to the extension
file(APPEND "${EXTENSION}" "${CUSTOM_SECTION}")
