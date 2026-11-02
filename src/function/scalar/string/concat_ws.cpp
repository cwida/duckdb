#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

#include <string.h>

namespace duckdb {

struct ConcatWSBindData : public FunctionData {
	explicit ConcatWSBindData(vector<bool> is_list_p) : is_list(std::move(is_list_p)) {
	}
	vector<bool> is_list; // one entry per non-separator argument, in call order

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ConcatWSBindData>(is_list);
	}
	bool Equals(const FunctionData &other_p) const override {
		return is_list == other_p.Cast<ConcatWSBindData>().is_list;
	}
};

static void ConcatWSFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ConcatWSBindData>();

	auto count = args.size();
	auto sep_data = args.data[0].Values<string_t>();

	// build one iterator per vararg, keyed by column; lists get two (entries + child elements)
	vector<VectorIterator<string_t>> scalar_iterators;
	vector<VectorIterator<VectorListType<string_t>>> list_iterators;

	for (idx_t col_idx = 1; col_idx < args.ColumnCount(); col_idx++) {
		if (info.is_list[col_idx - 1]) {
			list_iterators.emplace_back(args.data[col_idx]);
		} else {
			scalar_iterators.emplace_back(args.data[col_idx]);
		}
	}

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t r = 0; r < count; r++) {
		auto sep_entry = sep_data[r];
		if (!sep_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		auto sep = sep_entry.GetValue();
		auto sep_ptr = sep.GetData();
		auto sep_size = sep.GetSize();
		// first figure out the length of the result string
		idx_t result_length = 0;

		// track separate counters into scalar_iterators/list_iterators
		idx_t scalar_i = 0, list_i = 0;

		bool has_result = false;
		for (idx_t col_idx = 1; col_idx < args.ColumnCount(); col_idx++) {
			if (!info.is_list[col_idx - 1]) {
				auto input = scalar_iterators[scalar_i++][r];
				if (!input.IsValid()) {
					continue;
				}
				if (has_result) {
					result_length += sep.GetSize();
				}
				result_length += input.GetValue().GetSize();
				has_result = true;
			} else {
				auto list_entry = list_iterators[list_i++][r];
				if (!list_entry.IsValid()) {
					continue;
				}
				for (idx_t e = 0; e < list_entry.GetListLength(); e++) {
					auto elem = list_entry.GetChildValue(e);
					if (!elem.IsValid()) {
						continue;
					}
					auto elem_str = elem.GetValue();
					if (has_result) {
						result_length += sep.GetSize();
					}
					result_length += elem_str.GetSize();
					has_result = true;
				}
			}
		}

		auto &result_str = result_data.WriteEmptyString(result_length);
		auto result_ptr = result_str.GetDataWriteable();
		// now write the result string
		result_length = 0;
		has_result = false;
		scalar_i = 0;
		list_i = 0;

		for (idx_t col_idx = 1; col_idx < args.ColumnCount(); col_idx++) {
			if (!info.is_list[col_idx - 1]) {
				auto input = scalar_iterators[scalar_i++][r];
				if (!input.IsValid()) {
					continue;
				}
				if (has_result) {
					memcpy(result_ptr + result_length, sep_ptr, sep_size);
					result_length += sep.GetSize();
				}
				auto input_str = input.GetValue();
				memcpy(result_ptr + result_length, input_str.GetData(), input_str.GetSize());
				result_length += input_str.GetSize();
				has_result = true;
			} else {
				auto list_entry = list_iterators[list_i++][r];
				if (!list_entry.IsValid()) {
					continue;
				}
				auto entry = list_entry.GetValue();
				for (idx_t e = 0; e < entry.length; e++) {
					auto elem = list_entry.GetChildValue(e);
					if (!elem.IsValid()) {
						continue;
					}
					if (has_result) {
						memcpy(result_ptr + result_length, sep_ptr, sep_size);
						result_length += sep.GetSize();
					}
					auto elem_str = elem.GetValue();
					memcpy(result_ptr + result_length, elem_str.GetData(), elem_str.GetSize());
					result_length += elem_str.GetSize();
					has_result = true;
				}
			}
		}
		result_str.Finalize();
	}
}

static unique_ptr<FunctionData> BindConcatWSFunction(BindScalarFunctionInput &input) {
	auto &args = input.GetArguments();
	vector<bool> is_list(args.size() - 1, false);

	for (idx_t i = 1; i < args.size(); i++) {
		auto &arg_type = args[i]->GetReturnType();
		if (arg_type.id() == LogicalTypeId::LIST) {
			is_list[i - 1] = true;
			auto child_type = ListType::GetChildType(arg_type);
			if (child_type.id() == LogicalTypeId::LIST) {
				throw BinderException("concat_ws() does not support nested lists");
			}
			if (child_type.id() != LogicalTypeId::VARCHAR) {
				args[i] = BoundCastExpression::AddCastToType(input.GetClientContext(), std::move(args[i]),
				                                             LogicalType::LIST(LogicalType::VARCHAR));
			}
		} else if (arg_type.id() != LogicalTypeId::VARCHAR) {
			args[i] =
			    BoundCastExpression::AddCastToType(input.GetClientContext(), std::move(args[i]), LogicalType::VARCHAR);
		}
	}

	return make_uniq<ConcatWSBindData>(std::move(is_list));
}

ScalarFunction ConcatWsFun::GetFunction() {
	// concat_ws functions similarly to the concat function, except the result is NULL if the separator is NULL
	// if the separator is not NULL, however, NULL values are counted as empty string
	// there is one separate rule: there are no separators added between NULL values,
	// so the NULL value and empty string are different!
	// e.g.:
	// concat_ws(',', NULL, NULL) = ""
	// concat_ws(',', '', '') = ","

	ScalarFunction concat_ws =
	    ScalarFunction("concat_ws", {}, LogicalType::VARCHAR, ConcatWSFunction, BindConcatWSFunction);
	concat_ws.GetSignature().AddParameter("separator", LogicalType::VARCHAR).AddParameter("string", LogicalType::ANY);
	concat_ws.GetSignature().AddArgs("args", LogicalType::ANY);
	concat_ws.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return ScalarFunction(concat_ws);
}

} // namespace duckdb
