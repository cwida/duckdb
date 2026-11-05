if(NOT CLANG_TIDY)
  add_subdirectory(adbc)
endif()

add_subdirectory(capi)
add_subdirectory(cpp)

set(TEST_API_OBJECTS
    test_api.cpp
    test_multi_file_column_mapper.cpp
    test_process_util.cpp
    test_remove_directory_extended.cpp
    test_temporary_file_owner.cpp
    test_task_error_out_of_memory.cpp
    test_bind_statement.cpp
    test_function_binding.cpp
    test_function_signature_variadic.cpp
    test_scalar_function_decimal_return.cpp
    test_aggregate_state_bind_data.cpp
    test_config.cpp
    test_connect_prepare.cpp
    test_remote_pushdown_macro_arguments.cpp
    test_os_util.cpp
    test_compression_select.cpp
    test_connection_secrets.cpp
    test_copy_output_lifecycle.cpp
    test_custom_allocator.cpp
    test_extension_install_in_progress.cpp
    test_extension_repositories.cpp
    test_external_extension_provider.cpp
    test_extension_setting_autoload.cpp
    test_instance_cache.cpp
    test_metadata_pin_race.cpp
    test_metadata_chain_old_readers.cpp
    test_storage_extension_alias.cpp
    test_storage_extension_remote_path.cpp
    test_static_database_callback.cpp
    test_result_unit.cpp
    test_results.cpp
    test_reset.cpp
    test_get_table_names.cpp
    test_prepared_api.cpp
    test_table_info.cpp
    test_appender_api.cpp
    test_lifecycle_hooks.cpp
    test_extension_schema_api.cpp
    test_pending_query.cpp
    test_query_result.cpp
    test_query_result_arrow.cpp
    test_query_result_format.cpp
    test_query_result_format_buffer.cpp
    test_query_result_stream.cpp
    test_expanding_statements.cpp
    test_retained_result_collection.cpp
    test_plan_serialization.cpp
    test_relation_api.cpp
    test_stream_result_buffer.cpp
    test_operator_memory_limit.cpp
    test_query_profiler.cpp
    test_extension_metrics.cpp
    test_read_ahead_abandon.cpp
    test_read_ahead_progress.cpp
    test_read_ahead_failure.cpp
    test_row_id_deduplicator.cpp
    test_dbdir.cpp
    test_pending_with_parameters.cpp
    test_parse_statement_iterator.cpp
    test_dialect_extension.cpp
    test_query_node_copy.cpp
    test_grammar_extension.cpp
    test_passthrough_dialect.cpp
    test_passthrough_token_matcher.cpp
    test_progress_bar.cpp
    test_live_query_metrics.cpp
    test_uuid.cpp
    test_insertion_order_preserving_map.cpp
    test_bignum.cpp
    test_threads.cpp
    test_windows_header_compatibility.cpp
    test_windows_unicode_path.cpp
    test_object_cache.cpp
    test_partial_executor.cpp
    test_scheduler_fairness.cpp
    test_worker_exception.cpp
    test_buffer_pool_eviction.cpp
    test_block_allocator_tls.cpp
    test_rollback_delete_checkpoint_flag.cpp
    test_group_commit_bound.cpp
    test_data_table_scan_without_row_groups.cpp)

if(NOT WIN32)
  set(TEST_API_OBJECTS ${TEST_API_OBJECTS} test_read_only.cpp)
endif()

if(DUCKDB_EXTENSION_TPCH_SHOULD_LINK)
  include_directories(../../extension/tpch/include)
  set(TEST_API_OBJECTS
      ${TEST_API_OBJECTS} test_tpch_with_relations.cpp
      test_tpch_with_streaming.cpp
      serialized_plans/test_plan_serialization_bwc.cpp)
endif()

if(DUCKDB_EXTENSION_TPCDS_SHOULD_LINK)
  include_directories(../../extension/tpcds/include)
  set(TEST_API_OBJECTS ${TEST_API_OBJECTS} test_tpcds_with_streaming.cpp)
endif()

add_library(test_api OBJECT ${TEST_API_OBJECTS})
set(UNITTEST_OBJECT_FILES
    ${UNITTEST_OBJECT_FILES} $<TARGET_OBJECTS:test_api>
    PARENT_SCOPE)
