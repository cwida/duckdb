//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/histogram_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/function/create_sort_key.hpp"

namespace duckdb {

struct HistogramFunctor {
	template <class T>
	static void HistogramFinalize(T value, Vector &result, idx_t offset) {
		FlatVector::GetDataMutable<T>(result)[offset] = value;
	}

	static bool CreateExtraState() {
		return false;
	}

	static void PrepareData(const Vector &input, bool &, UnifiedVectorFormat &result) {
		input.ToUnifiedFormat(result);
	}

	template <class T>
	static T ExtractValue(UnifiedVectorFormat &bin_data, idx_t offset, AggregateInputData &) {
		return UnifiedVectorFormat::GetData<T>(bin_data)[bin_data.sel->get_index(offset)];
	}
};

struct HistogramStringFunctorBase {
	template <class T>
	static T ExtractValue(UnifiedVectorFormat &bin_data, idx_t offset, AggregateInputData &aggr_input) {
		auto &input_str = UnifiedVectorFormat::GetData<T>(bin_data)[bin_data.sel->get_index(offset)];
		if (input_str.IsInlined()) {
			// inlined strings can be inserted directly
			return input_str;
		}
		// if the string is not inlined we need to allocate space for it
		auto input_str_size = UnsafeNumericCast<uint32_t>(input_str.GetSize());
		auto string_memory = aggr_input.allocator.Allocate(input_str_size);
		// copy over the string
		memcpy(string_memory, input_str.GetData(), input_str_size);
		// now insert it into the histogram
		string_t histogram_str(char_ptr_cast(string_memory), input_str_size);
		return histogram_str;
	}
};

struct HistogramStringFunctor : HistogramStringFunctorBase {
	template <class T>
	static void HistogramFinalize(T value, Vector &result, idx_t offset) {
		FlatVector::GetDataMutable<string_t>(result)[offset] = StringVector::AddStringOrBlob(result, value);
	}

	static bool CreateExtraState() {
		return false;
	}

	static void PrepareData(const Vector &input, bool &, UnifiedVectorFormat &result) {
		input.ToUnifiedFormat(result);
	}
};

struct HistogramGenericFunctor : HistogramStringFunctorBase {
	template <class T>
	static void HistogramFinalize(T value, Vector &result, idx_t offset) {
		CreateSortKeyHelpers::DecodeSortKey(value, result, offset,
		                                    OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
	}

	static Vector CreateExtraState() {
		return Vector(LogicalType::BLOB);
	}

	static void PrepareData(const Vector &input, Vector &extra_state, UnifiedVectorFormat &result) {
		OrderModifiers modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
		extra_state.Reserve(input.size());
		CreateSortKeyHelpers::CreateSortKey(input, modifiers, extra_state);
		input.Flatten();
		extra_state.Flatten();
		FlatVector::ValidityMutable(extra_state).Initialize(FlatVector::Validity(input));
		extra_state.ToUnifiedFormat(result);
	}
};

} // namespace duckdb
