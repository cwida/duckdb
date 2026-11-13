include_directories(include)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_library(duckdb_mbedtls STATIC
        mbedtls_wrapper.cpp
        library/aes.cpp
        library/asn1parse.cpp
        library/asn1write.cpp
        library/base64.cpp
        library/bignum.cpp
        library/bignum_core.cpp
        library/cipher.cpp
        library/cipher_wrap.cpp
        library/constant_time.cpp
        library/gcm.cpp
        library/md.cpp
        library/oid.cpp
        library/pem.cpp
        library/pk.cpp
        library/pk_wrap.cpp
        library/pkparse.cpp
        library/platform.cpp
        library/platform_util.cpp
        library/rsa.cpp
        library/rsa_alt_helpers.cpp
        library/sha1.cpp
        library/sha256.cpp)

install(TARGETS duckdb_mbedtls
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
