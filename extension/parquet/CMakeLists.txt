cmake_minimum_required(VERSION 3.5...3.29)

project(ParquetExtension)

# Before any subdirectory or target, which would otherwise inherit the
# directory-wide definition
set_extension_version_sources(parquet parquet_extension.cpp)

include_directories(
  include ../../third_party/lz4 ../../third_party/parquet
  ../../third_party/thrift ../../third_party/snappy
  ../../third_party/brotli/include)

add_subdirectory(decoder)
add_subdirectory(reader)
add_subdirectory(writer)
set(PARQUET_EXTENSION_FILES
    ${PARQUET_EXTENSION_FILES}
    column_reader.cpp
    column_writer.cpp
    parquet_crypto.cpp
    parquet_extension.cpp
    parquet_file_metadata_cache.cpp
    parquet_float16.cpp
    parquet_multi_file_info.cpp
    read_single_parquet_file.cpp
    parquet_metadata.cpp
    parquet_prefetch_cost_model.cpp
    parquet_reader.cpp
    parquet_field_id.cpp
    parquet_statistics.cpp
    parquet_timestamp.cpp
    parquet_writer.cpp
    parquet_shredding.cpp
    parquet_column_schema.cpp
    parquet_geometry.cpp
    parquet_int96.cpp
    serialize_parquet.cpp
    variant_arrow_capi.cpp
    zstd_file_system.cpp)

if(NOT CLANG_TIDY)
  # parquet/thrift/snappy
  set(PARQUET_EXTENSION_FILES
      ${PARQUET_EXTENSION_FILES}
      ../../third_party/parquet/parquet_types.cpp
      ../../third_party/thrift/thrift/protocol/TProtocol.cpp
      ../../third_party/thrift/thrift/transport/TTransportException.cpp
      ../../third_party/thrift/thrift/transport/TBufferTransports.cpp
      ../../third_party/snappy/snappy.cc
      ../../third_party/snappy/snappy-sinksource.cc)
  # lz4
  set(PARQUET_EXTENSION_FILES ${PARQUET_EXTENSION_FILES}
                              ../../third_party/lz4/lz4.cpp)
  # brotli
  set(PARQUET_EXTENSION_FILES
      ${PARQUET_EXTENSION_FILES}
      ../../third_party/brotli/enc/dictionary_hash.cpp
      ../../third_party/brotli/enc/backward_references_hq.cpp
      ../../third_party/brotli/enc/histogram.cpp
      ../../third_party/brotli/enc/memory.cpp
      ../../third_party/brotli/enc/entropy_encode.cpp
      ../../third_party/brotli/enc/compound_dictionary.cpp
      ../../third_party/brotli/enc/compress_fragment_two_pass.cpp
      ../../third_party/brotli/enc/block_splitter.cpp
      ../../third_party/brotli/enc/command.cpp
      ../../third_party/brotli/enc/encode.cpp
      ../../third_party/brotli/enc/encoder_dict.cpp
      ../../third_party/brotli/enc/cluster.cpp
      ../../third_party/brotli/enc/backward_references.cpp
      ../../third_party/brotli/enc/utf8_util.cpp
      ../../third_party/brotli/enc/compress_fragment.cpp
      ../../third_party/brotli/enc/fast_log.cpp
      ../../third_party/brotli/enc/brotli_bit_stream.cpp
      ../../third_party/brotli/enc/bit_cost.cpp
      ../../third_party/brotli/enc/static_dict.cpp
      ../../third_party/brotli/enc/literal_cost.cpp
      ../../third_party/brotli/enc/metablock.cpp
      ../../third_party/brotli/common/dictionary.cpp
      ../../third_party/brotli/common/constants.cpp
      ../../third_party/brotli/common/transform.cpp
      ../../third_party/brotli/common/platform.cpp
      ../../third_party/brotli/common/shared_dictionary.cpp
      ../../third_party/brotli/common/context.cpp
      ../../third_party/brotli/dec/state.cpp
      ../../third_party/brotli/dec/decode.cpp
      ../../third_party/brotli/dec/huffman.cpp
      ../../third_party/brotli/dec/bit_reader.cpp)
endif()

build_extension_library(parquet ${PARQUET_EXTENSION_FILES})
target_link_libraries(parquet_loadable_extension duckdb_mbedtls duckdb_zstd)

install(
  TARGETS parquet_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
