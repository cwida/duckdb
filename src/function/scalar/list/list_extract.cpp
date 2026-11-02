#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

namespace duckdb {

static optional_idx TryGetChildOffset(const list_entry_t &list_entry, const int64_t offset) {
	// 1-based indexing
	if (offset == 0) {
		return optional_idx::Invalid();
	}

	const auto index_offset = (offset > 0) ? offset - 1 : offset;
	if (index_offset < 0) {
		const auto signed_list_length = UnsafeNumericCast<int64_t>(list_entry.length);
		if (signed_list_length + index_offset < 0) {
			return optional_idx::Invalid();
		}
		return optional_idx(list_entry.offset + UnsafeNumericCast<idx_t>(signed_list_length + index_offset));
	}

	const auto unsigned_offset = UnsafeNumericCast<idx_t>(index_offset);

	// Check that the offset is within the list
	if (unsigned_offset >= list_entry.length) {
		return optional_idx::Invalid();
	}

	return optional_idx(list_entry.offset + unsigned_offset);
}

static void ExecuteListExtract(Vector &result, const Vector &list, const Vector &offsets) {
	D_ASSERT(list.GetType().id() == LogicalTypeId::LIST);
	const auto count = list.size();

	auto list_entries = list.Values<list_entry_t>();
	auto offsets_entries = offsets.Values<int64_t>();
	auto &child_vector = ListVector::GetChild(list);

	SelectionVector sel(count);
	vector<idx_t> invalid_offsets;

	optional_idx first_valid_child_idx;
	for (idx_t i = 0; i < count; i++) {
		auto list_entry = list_entries[i];
		auto offsets_entry = offsets_entries[i];

		if (!list_entry.IsValid() || !offsets_entry.IsValid()) {
			invalid_offsets.push_back(i);
			continue;
		}

		const auto child_offset = TryGetChildOffset(list_entry.GetValue(), offsets_entry.GetValue());

		if (!child_offset.IsValid()) {
			invalid_offsets.push_back(i);
			continue;
		}

		const auto child_idx = child_offset.GetIndex();
		sel.set_index(i, child_idx);

		if (!first_valid_child_idx.IsValid()) {
			// Save the first valid child as a dummy index to copy in VectorOperations::Copy later
			first_valid_child_idx = child_idx;
		}
	}
	if (invalid_offsets.empty()) {
		// all entries found a match - we can just slice the child vector
		result.Slice(child_vector, sel, count);
		return;
	}

	if (first_valid_child_idx.IsValid()) {
		// Only copy if we found at least one valid child
		for (const auto &invalid_offset : invalid_offsets) {
			sel.set_index(invalid_offset, first_valid_child_idx.GetIndex());
		}
		VectorOperations::Copy(child_vector, result, sel, count, 0, 0);
	}

	// Copy:ing the vectors also copies the validity mask, so we set the rows with invalid offsets (0) to false here.
	for (const auto &invalid_idx : invalid_offsets) {
		FlatVector::SetNull(result, invalid_idx, true);
	}
}

static void ExecuteStringExtract(Vector &result, const Vector &input_vector, const Vector &subscript_vector) {
	BinaryExecutor::Execute<string_t, int64_t, string_t>(
	    input_vector, subscript_vector, result,
	    [&](string_t input_string, int64_t subscript) { return SubstringUnicode(result, input_string, subscript, 1); });
}

static void ListExtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);
	const auto count = args.size();

	const Vector &base = args.data[0];
	const Vector &subscript = args.data[1];

	switch (base.GetType().id()) {
	case LogicalTypeId::LIST:
		ExecuteListExtract(result, base, subscript);
		break;
	case LogicalTypeId::VARCHAR:
		ExecuteStringExtract(result, base, subscript);
		break;
	case LogicalTypeId::SQLNULL:
		ConstantVector::SetNull(result, count_t(count));
		break;
	default:
		throw NotImplementedException("Specifier type not implemented");
	}
}

static unique_ptr<FunctionData> ListExtractBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	D_ASSERT(input.GetBoundFunction().GetArguments().size() == 2);
	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	return nullptr;
}

//! Extracting from a string throws if the index is outside of the range supported by substring - if the index is a
//! constant within that range the extract cannot throw
//! Extracting from a list returns NULL for indexes that are out of range instead
static unique_ptr<FunctionData> StringExtractBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto index = input.TryGetConstant(1);
	if (!index) {
		bound_function.SetFallible();
		return nullptr;
	}
	string error;
	auto index_value = index->DefaultTryCastAs(LogicalType::BIGINT, &error);
	if (!index_value) {
		// the index is a constant that cannot be converted to an integer at all - report that directly
		throw ConversionException(error);
	}
	if (index_value->IsNull() || !SubstringInSupportedRange(BigIntValue::Get(*index_value), 1)) {
		bound_function.SetFallible();
	}
	return nullptr;
}

static unique_ptr<BaseStatistics> ListExtractStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &list_child_stats = ListStats::GetChildStats(child_stats[0]);
	auto child_copy = list_child_stats.Copy();
	// list_extract always pushes a NULL, since if the offset is out of range for a list it inserts a null
	child_copy.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	if (!child_stats[0].CanHaveNoNull() || !child_stats[1].CanHaveNoNull()) {
		child_copy.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	}
	return child_copy.ToUnique();
}

ScalarFunctionSet ListExtractFun::GetFunctions() {
	ScalarFunctionSet list_extract_set("list_extract");

	// the arguments and return types are actually set in the binder function
	ScalarFunction lfun({LogicalType::LIST(LogicalType::TEMPLATE("T")), LogicalType::BIGINT},
	                    LogicalType::TEMPLATE("T"), ListExtractFunction, ListExtractBind, ListExtractStats);

	lfun.GetSignature().GetParameter(0).SetName("list");
	lfun.GetSignature().GetParameter(1).SetName("index");

	ScalarFunction sfun({LogicalType::VARCHAR, LogicalType::BIGINT}, LogicalType::VARCHAR, ListExtractFunction,
	                    StringExtractBind);
	sfun.GetSignature().GetParameter(0).SetName("string");
	sfun.GetSignature().GetParameter(1).SetName("index");

	lfun.SetFallible();
	list_extract_set.AddFunction(lfun);
	list_extract_set.AddFunction(sfun);
	return list_extract_set;
}

ScalarFunctionSet ArrayExtractFun::GetFunctions() {
	ScalarFunctionSet array_extract_set("array_extract");

	// the arguments and return types are actually set in the binder function
	ScalarFunction lfun({LogicalType::LIST(LogicalType::TEMPLATE("T")), LogicalType::BIGINT},
	                    LogicalType::TEMPLATE("T"), ListExtractFunction, ListExtractBind, ListExtractStats);

	lfun.GetSignature().GetParameter(0).SetName("list");
	lfun.GetSignature().GetParameter(1).SetName("index");

	ScalarFunction sfun({}, LogicalType::VARCHAR, ListExtractFunction, StringExtractBind);
	sfun.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("index", LogicalType::BIGINT);

	array_extract_set.AddFunction(lfun);
	array_extract_set.AddFunction(sfun);
	array_extract_set.AddFunction(GetKeyExtractFunction());
	array_extract_set.AddFunction(GetIndexExtractFunction());
	return array_extract_set;
}

} // namespace duckdb
