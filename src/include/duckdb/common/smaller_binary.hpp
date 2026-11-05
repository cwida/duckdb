//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/smaller_binary.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

// DUCKDB_SMALLER_BINARY(feature) is 1 when `feature` is trimmed out of this build, 0 otherwise.
// The feature name is a bare identifier, not a string:
//
//   #if !DUCKDB_SMALLER_BINARY(between_select)
//       ... specialized code ...
//   #endif
//
// Every feature must be registered below. An unregistered name is a compile error, not a silent 0.
//
// Build system interface:
//   -DDUCKDB_SMALLER_BINARY_ALL              trim everything
//   -DDUCKDB_SB_FEATURE_<name>=DUCKDB_SB_ON  trim one feature
//   -DDUCKDB_SB_FEATURE_<name>=DUCKDB_SB_OFF keep one feature
// See the SMALLER_BINARY / SMALLER_BINARY_EXCEPT options in CMakeLists.txt.

#define DUCKDB_SB_CAT_INDIRECT(a, b) a##b
#define DUCKDB_SB_CAT(a, b)          DUCKDB_SB_CAT_INDIRECT(a, b)

#define DUCKDB_SMALLER_BINARY(feature) (DUCKDB_SB_CAT(DUCKDB_SB_FEATURE_, feature)())

#define DUCKDB_SB_ON()  (1)
#define DUCKDB_SB_OFF() (0)

#ifdef DUCKDB_SMALLER_BINARY_ALL
#define DUCKDB_SB_DEFAULT() (1)
#else
#define DUCKDB_SB_DEFAULT() (0)
#endif

//===----------------------------------------------------------------------===//
// Feature registry
//===----------------------------------------------------------------------===//
// Each entry must be #ifndef-guarded so a -D on the command line wins over the default.
// The trailing `group:` tag is parsed by CMakeLists.txt to build the group aliases.
//
// Sites belonging to one feature must be trimmed together: a definition and every call site that
// references it share a feature name, otherwise trimming produces an unused or undefined symbol.

// --- vector_specialization: bool-template specializations in the vectorized executors ----------

#ifndef DUCKDB_SB_FEATURE_unary_executor_flat
#define DUCKDB_SB_FEATURE_unary_executor_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_unary_executor_select_flat
#define DUCKDB_SB_FEATURE_unary_executor_select_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_unary_executor_select_flags
#define DUCKDB_SB_FEATURE_unary_executor_select_flags DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_binary_executor_flat
#define DUCKDB_SB_FEATURE_binary_executor_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_binary_executor_select_flat
#define DUCKDB_SB_FEATURE_binary_executor_select_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_binary_executor_select_flags
#define DUCKDB_SB_FEATURE_binary_executor_select_flags DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_binary_executor_generic_nullable
#define DUCKDB_SB_FEATURE_binary_executor_generic_nullable DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_variadic_executor_select_flat
#define DUCKDB_SB_FEATURE_variadic_executor_select_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_variadic_executor_flat
#define DUCKDB_SB_FEATURE_variadic_executor_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_variadic_executor_select_flags
#define DUCKDB_SB_FEATURE_variadic_executor_select_flags DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_aggregate_executor_flat
#define DUCKDB_SB_FEATURE_aggregate_executor_flat DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_aggregate_executor_sel_flags
#define DUCKDB_SB_FEATURE_aggregate_executor_sel_flags DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_scalar_executor_constant
#define DUCKDB_SB_FEATURE_scalar_executor_constant DUCKDB_SB_DEFAULT // group: vector_specialization
#endif

// --- row_layout_specialization: bool-template specializations in the row-layout operations -----

#ifndef DUCKDB_SB_FEATURE_tuple_data_heap_sizes
#define DUCKDB_SB_FEATURE_tuple_data_heap_sizes DUCKDB_SB_DEFAULT // group: row_layout_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_tuple_data_scatter
#define DUCKDB_SB_FEATURE_tuple_data_scatter DUCKDB_SB_DEFAULT // group: row_layout_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_tuple_data_gather
#define DUCKDB_SB_FEATURE_tuple_data_gather DUCKDB_SB_DEFAULT // group: row_layout_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_row_matcher_validity
#define DUCKDB_SB_FEATURE_row_matcher_validity DUCKDB_SB_DEFAULT // group: row_layout_specialization
#endif

// --- select_paths: comparison paths that filter directly into a selection vector ---------------
// Trimming these does not change results, but comparisons fall back to materializing a boolean
// vector and filtering afterwards.

#ifndef DUCKDB_SB_FEATURE_comparison_select
#define DUCKDB_SB_FEATURE_comparison_select DUCKDB_SB_DEFAULT // group: select_paths
#endif

#ifndef DUCKDB_SB_FEATURE_between_select
#define DUCKDB_SB_FEATURE_between_select DUCKDB_SB_DEFAULT // group: select_paths
#endif

#ifndef DUCKDB_SB_FEATURE_primitive_comparison_execute
#define DUCKDB_SB_FEATURE_primitive_comparison_execute DUCKDB_SB_DEFAULT // group: select_paths
#endif

#ifndef DUCKDB_SB_FEATURE_primitive_comparator_execute
#define DUCKDB_SB_FEATURE_primitive_comparator_execute DUCKDB_SB_DEFAULT // group: select_paths
#endif

#ifndef DUCKDB_SB_FEATURE_primitive_distinct_execute
#define DUCKDB_SB_FEATURE_primitive_distinct_execute DUCKDB_SB_DEFAULT // group: select_paths
#endif

#ifndef DUCKDB_SB_FEATURE_primitive_select_execute
#define DUCKDB_SB_FEATURE_primitive_select_execute DUCKDB_SB_DEFAULT // group: select_paths
#endif

// --- function_type_specialization: per-physical-type function instances -------------------------
// Trimming these binds a generic (wider or fallback) implementation instead.

#ifndef DUCKDB_SB_FEATURE_arg_min_max_types
#define DUCKDB_SB_FEATURE_arg_min_max_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_arg_min_max_n_types
#define DUCKDB_SB_FEATURE_arg_min_max_n_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_quantile_types
#define DUCKDB_SB_FEATURE_quantile_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_mode_types
#define DUCKDB_SB_FEATURE_mode_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_entropy_types
#define DUCKDB_SB_FEATURE_entropy_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_histogram_types
#define DUCKDB_SB_FEATURE_histogram_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_binned_histogram_types
#define DUCKDB_SB_FEATURE_binned_histogram_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_least_greatest_types
#define DUCKDB_SB_FEATURE_least_greatest_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_list_aggregate_types
#define DUCKDB_SB_FEATURE_list_aggregate_types DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_unchecked_numeric_casts
#define DUCKDB_SB_FEATURE_unchecked_numeric_casts DUCKDB_SB_DEFAULT // group: function_type_specialization
#endif

// --- window_specialization: custom windowed implementations of holistic aggregates --------------
// Trimming these falls back to re-aggregating each window frame.

#ifndef DUCKDB_SB_FEATURE_quantile_window
#define DUCKDB_SB_FEATURE_quantile_window DUCKDB_SB_DEFAULT // group: window_specialization
#endif

#ifndef DUCKDB_SB_FEATURE_mad_window
#define DUCKDB_SB_FEATURE_mad_window DUCKDB_SB_DEFAULT // group: window_specialization
#endif

// --- sort_specialization: per-layout instances of the sorting stack -----------------------------
// The whole sorting stack (run sort, merge, sort key scatter/gather, range joins) is templated on
// SortKeyType, so each of the nine layouts is a full instance of it, ~70 KB. Trimming binds a
// wider layout instead, which costs memory and time on every sort, not just code size.

#ifndef DUCKDB_SB_FEATURE_sort_key_layouts
#define DUCKDB_SB_FEATURE_sort_key_layouts DUCKDB_SB_DEFAULT // group: sort_specialization
#endif
