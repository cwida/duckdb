set(ZSTD_FILES
	compress/zstd_compress_superblock.cpp
	compress/zstdmt_compress.cpp
	compress/zstd_double_fast.cpp
	compress/zstd_fast.cpp
	compress/zstd_compress_sequences.cpp
	compress/zstd_ldm.cpp
	compress/hist.cpp
	compress/zstd_compress.cpp
	compress/zstd_lazy.cpp
	compress/zstd_compress_literals.cpp
	compress/huf_compress.cpp
	compress/zstd_opt.cpp
	compress/fse_compress.cpp
	decompress/zstd_ddict.cpp
	decompress/huf_decompress.cpp
	decompress/zstd_decompress.cpp
	decompress/zstd_decompress_block.cpp
	common/entropy_common.cpp
	common/fse_decompress.cpp
	common/debug.cpp
	common/xxhash.cpp
	common/pool.cpp
	common/threading.cpp
	common/zstd_common.cpp
	common/error_private.cpp
	dict/cover.cpp
	dict/divsufsort.cpp
	dict/fastcover.cpp
	dict/zdict.cpp
	deprecated/zbuff_common.cpp
	deprecated/zbuff_decompress.cpp
	deprecated/zbuff_compress.cpp
)

add_library(duckdb_zstd STATIC ${ZSTD_FILES})

target_include_directories(
		duckdb_zstd
		PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(duckdb_zstd PROPERTIES EXPORT_NAME duckdb_duckdb_zstd)

install(TARGETS duckdb_zstd
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_zstd)
