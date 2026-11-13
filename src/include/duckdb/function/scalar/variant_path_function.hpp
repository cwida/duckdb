//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/variant_path_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//! Whether a VARIANT value selected by a path is returned
typedef bool (*variant_node_filter_t)(const VariantNode &node);

struct VariantPathFunction {
	//! Provides unified construction of path based processing functions.
	static ScalarFunctionSet CreateFunctionSet(const Identifier &name, const scalar_function_t &function,
	                                           const LogicalType &return_type, bool path_optional = true,
	                                           init_local_state_t init_state = nullptr);

	//! Executes scalar VARIANT functions that accept an optional constant path argument.
	//! The function may be called as f(VARIANT), f(VARIANT, VARCHAR) or f(VARIANT, VARCHAR[]).
	//! The callback `write_fn` writes the result for one row.
	template <class RESULT_TYPE, class WRITE_FUNCTION>
	static void Execute(DataChunk &input, const ExpressionState &state, Vector &result,
	                    const WRITE_FUNCTION &write_fn) {
		D_ASSERT(input.ColumnCount() == 1 || input.ColumnCount() == 2);
		const auto count = input.size();
		const auto &variant_vec = input.data[0];

		if (input.ColumnCount() == 2) {
			const auto &path = input.data[1];
			D_ASSERT(path.GetVectorType() == VectorType::CONSTANT_VECTOR);
			(void)path;
		}

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<VariantPathBindData>();
		auto n_columns = input.ColumnCount();

		if (n_columns == 1) {
			ExecuteUnary<RESULT_TYPE>(variant_vec, {}, result, count, write_fn);
			return;
		}

		D_ASSERT(n_columns == 2);
		const auto &path_type_id = input.data[1].GetType().id();

		if (path_type_id == LogicalTypeId::VARCHAR) {
			ExecuteUnary<RESULT_TYPE>(variant_vec, info.paths[0], result, count, write_fn);
			return;
		}
		if (path_type_id == LogicalTypeId::LIST) {
			ExecuteMany<RESULT_TYPE>(variant_vec, info.paths, result, count, write_fn);
			return;
		}
	}

	//! Executes scalar VARIANT functions that return the VARIANT value selected by an optional constant path argument.
	//! The function may be called as f(VARIANT), f(VARIANT, VARCHAR) or f(VARIANT, VARCHAR[]), and returns VARIANT, or
	//! VARIANT[] for a list of paths. Missing values, and values for which `keep` returns false, are written as NULL.
	static void ExecuteVariant(DataChunk &input, const ExpressionState &state, Vector &result,
	                           variant_node_filter_t keep);

private:
	//! Executes a single path and writes RESULT_TYPE per input row.
	template <class RESULT_TYPE, class WRITE_FUNCTION>
	static void ExecuteUnary(const Vector &input, const vector<VariantPathComponent> &path, Vector &result,
	                         const idx_t count, const WRITE_FUNCTION &write_fn) {
		const VectorIterator<VectorVariantType> variants(input);

		result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
		auto row_writer = FlatVector::Writer<RESULT_TYPE>(result, count);

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			if (!variants.RowIsValid(row_idx)) {
				row_writer.WriteNull();
				continue;
			}

			write_fn(ResolvePath(variants[row_idx], path), row_writer);
		}
	}

	//! Executes a list of paths and writes LIST<RESULT_TYPE> per input row.
	template <class RESULT_TYPE, class WRITE_FUNCTION>
	static void ExecuteMany(const Vector &input, const vector<vector<VariantPathComponent>> &paths, Vector &result,
	                        const idx_t count, const WRITE_FUNCTION &write_fn) {
		const VectorIterator<VectorVariantType> variants(input);

		result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
		auto result_writer = FlatVector::Writer<VectorListType<RESULT_TYPE>>(result, count);

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			if (!variants.RowIsValid(row_idx)) {
				result_writer.WriteNull();
				continue;
			}

			auto row_writer = result_writer.WriteList(paths.size());
			const auto variant = variants[row_idx];

			idx_t path_idx = 0;
			for (auto &path_writer : row_writer) {
				write_fn(ResolvePath(variant, paths[path_idx]), path_writer);
				path_idx++;
			}
		}
	}

	//! Executes a single path and writes the selected VARIANT value per input row.
	static void ExecuteVariantUnary(const Vector &input, const vector<VariantPathComponent> &path, Vector &result,
	                                idx_t count, variant_node_filter_t keep);
	//! Executes a list of paths and writes LIST<VARIANT> of the selected values per input row.
	static void ExecuteVariantMany(const Vector &input, const vector<vector<VariantPathComponent>> &paths,
	                               Vector &result, idx_t count, variant_node_filter_t keep);

	//! Walks the VARIANT using a path, which may be either (partially) shredded or unshredded. Returns nullopt if the
	//! path is missing in the VARIANT, and the present value with VariantNode otherwise.
	static optional<VariantNode> ResolvePath(VariantNode node, const vector<VariantPathComponent> &path) {
		for (const auto &component : path) {
			if (component.lookup_mode == VariantChildLookupMode::BY_INDEX) {
				throw InternalException("Path indexes are not supported for this function");
			}
			if (node.IsNull() || node.IsMissing()) {
				return {};
			}

			if (node.GetTypeId() != VariantLogicalType::OBJECT) {
				return {};
			}

			bool found = false;
			for (const auto &[key, value] : node.GetObjectChildren()) {
				if (key == component.key) {
					node = value;
					found = true;
					break;
				}
			}

			if (!found) {
				return {};
			}
		}

		return node;
	}
};
} // namespace duckdb
