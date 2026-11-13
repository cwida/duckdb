#include "duckdb/function/scalar/variant_path_function.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/vector/list_vector.hpp"

namespace duckdb {

namespace {

//! Emits the value selected for each row, or a NULL if it is missing or not kept
template <class SELECT_FUNCTION>
struct SelectedVariantSource {
	bool Emit(idx_t row, VariantBuilder &builder) const {
		const auto node = select_fn(row);
		if (!node || !keep(*node)) {
			return true;
		}
		EmitIterator(*node, builder);
		return false;
	}

	const SELECT_FUNCTION &select_fn;
	variant_node_filter_t keep;
};

template <class SELECT_FUNCTION>
void BuildSelectedVariants(idx_t count, Vector &result, variant_node_filter_t keep, const SELECT_FUNCTION &select_fn) {
	SelectedVariantSource<SELECT_FUNCTION> source {select_fn, keep};
	result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	BuildVariant(source, count, result);
}

} // namespace

void VariantPathFunction::ExecuteVariant(DataChunk &input, const ExpressionState &state, Vector &result,
                                         variant_node_filter_t keep) {
	D_ASSERT(input.ColumnCount() == 1 || input.ColumnCount() == 2);
	const auto count = input.size();
	const auto &variant_vec = input.data[0];

	if (input.ColumnCount() == 1) {
		ExecuteVariantUnary(variant_vec, {}, result, count, keep);
		return;
	}
	D_ASSERT(input.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR);

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<VariantPathBindData>();
	const auto path_type_id = input.data[1].GetType().id();
	if (path_type_id == LogicalTypeId::VARCHAR) {
		ExecuteVariantUnary(variant_vec, info.paths[0], result, count, keep);
		return;
	}
	D_ASSERT(path_type_id == LogicalTypeId::LIST);
	ExecuteVariantMany(variant_vec, info.paths, result, count, keep);
}

void VariantPathFunction::ExecuteVariantUnary(const Vector &input, const vector<VariantPathComponent> &path,
                                              Vector &result, idx_t count, variant_node_filter_t keep) {
	const VectorIterator<VectorVariantType> variants(input);
	BuildSelectedVariants(count, result, keep, [&](idx_t row) -> optional<VariantNode> {
		if (!variants.RowIsValid(row)) {
			return {};
		}
		return ResolvePath(variants[row], path);
	});
}

void VariantPathFunction::ExecuteVariantMany(const Vector &input, const vector<vector<VariantPathComponent>> &paths,
                                             Vector &result, idx_t count, variant_node_filter_t keep) {
	const VectorIterator<VectorVariantType> variants(input);

	result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	//! The row of each list element, the elements of a row hold its value at each of the paths in order
	vector<idx_t> element_rows;
	{
		auto list_writer = FlatVector::Writer<list_entry_t>(result, count);
		for (idx_t row = 0; row < count; row++) {
			if (!variants.RowIsValid(row)) {
				list_writer.WriteNull();
				continue;
			}
			list_writer.WriteValue(list_entry_t(element_rows.size(), paths.size()));
			element_rows.insert(element_rows.end(), paths.size(), row);
		}
	}
	const auto element_count = element_rows.size();
	if (element_count == 0) {
		return;
	}
	Vector elements(LogicalType::VARIANT(), element_count);
	BuildSelectedVariants(element_count, elements, keep, [&](idx_t element_idx) {
		return ResolvePath(variants[element_rows[element_idx]], paths[element_idx % paths.size()]);
	});
	ListVector::Append(result, elements, element_count);
}

ScalarFunctionSet VariantPathFunction::CreateFunctionSet(const Identifier &name, const scalar_function_t &function,
                                                         const LogicalType &return_type, const bool path_optional,
                                                         const init_local_state_t init_state) {
	ScalarFunctionSet fun_set(name);

	if (path_optional) {
		ScalarFunction no_path_fun({}, return_type, function, VariantBindUtils::VariantPathBind, nullptr);
		no_path_fun.GetSignature().AddParameter("input_variant", LogicalType::VARIANT());
		fun_set.AddFunction(std::move(no_path_fun));
	}

	ScalarFunction path_fun({}, return_type, function, VariantBindUtils::VariantPathBind, nullptr, init_state);
	path_fun.GetSignature()
	    .AddParameter("input_variant", LogicalType::VARIANT())
	    .AddParameter("path", LogicalType::VARCHAR);
	fun_set.AddFunction(std::move(path_fun));

	ScalarFunction path_list_fun({}, LogicalType::LIST(return_type), function, VariantBindUtils::VariantPathBind,
	                             nullptr, init_state);
	path_list_fun.GetSignature()
	    .AddParameter("input_variant", LogicalType::VARIANT())
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	fun_set.AddFunction(std::move(path_list_fun));

	return fun_set;
}

} // namespace duckdb
