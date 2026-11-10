//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/common/clustered_aggregate.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

namespace duckdb {
class BoundAggregateFunction;
struct AggregateObject;

enum class AggregateType : uint8_t { NON_DISTINCT = 1, DISTINCT = 2 };
//! Whether or not the input order influences the result of the aggregate
enum class AggregateOrderDependent : uint8_t { ORDER_DEPENDENT = 1, NOT_ORDER_DEPENDENT = 2 };
//! Whether or not the input distinctness influences the result of the aggregate
enum class AggregateDistinctDependent : uint8_t { DISTINCT_DEPENDENT = 1, NOT_DISTINCT_DEPENDENT = 2 };
//! Whether or not the combiner needs to preserve the source
enum class AggregateCombineType : uint8_t { PRESERVE_INPUT = 1, ALLOW_DESTRUCTIVE = 2 };
//! Whether or not we are exporting the state of the aggregate
enum class AggregateStateExportMode : uint8_t { NONE = 1, STATE_EXPORT = 2 };

class BoundAggregateExpression;

struct AggregateInputData {
	AggregateInputData(const BoundAggregateFunction &function_p, optional_ptr<FunctionData> bind_data_p,
	                   ArenaAllocator &allocator_p,
	                   AggregateCombineType combine_type_p = AggregateCombineType::PRESERVE_INPUT)
	    : function(function_p), bind_data(bind_data_p), allocator(allocator_p), combine_type(combine_type_p) {
	}
	AggregateInputData(const BoundAggregateExpression &expr, ArenaAllocator &allocator_p,
	                   AggregateCombineType combine_type_p = AggregateCombineType::PRESERVE_INPUT);
	AggregateInputData(const AggregateObject &aggr, ArenaAllocator &allocator_p,
	                   AggregateCombineType combine_type_p = AggregateCombineType::PRESERVE_INPUT);

	const BoundAggregateFunction &function;
	optional_ptr<FunctionData> bind_data;
	ArenaAllocator &allocator;
	AggregateCombineType combine_type;
	optional_ptr<const ClusteredAggr> clustered;
	optional_ptr<const Vector> combine_multiplicities;
};

//! Input to the get_state_type callback - bundles the bound aggregate function with its bind data so that the
//! callback can resolve the exported state layout (including any constant parameters stored in the bind data).
struct AggregateLayoutInput {
	AggregateLayoutInput(const BoundAggregateFunction &function_p, optional_ptr<FunctionData> bind_data_p)
	    : function(function_p), bind_data(bind_data_p) {
	}

	const BoundAggregateFunction &function;
	optional_ptr<FunctionData> bind_data;
};

//! Input to the state size/init callbacks - bundles the bound aggregate function with its bind data so that the
//! callbacks can size and initialize states using any constant parameters stored in the bind data.
struct AggregateStateInput {
	AggregateStateInput(const BoundAggregateFunction &function_p, optional_ptr<const FunctionData> bind_data_p)
	    : function(function_p), bind_data(bind_data_p) {
	}

	const BoundAggregateFunction &function;
	optional_ptr<const FunctionData> bind_data;
};

//! The input data provided to the finalize callback of an aggregate function.
//! If the function defines an "init_local_state_finalize" callback, the local state is initialized on construction.
//! Callers can instead pass in an externally-owned local state - this way the local state can be kept alive and
//! re-used across multiple finalize calls (e.g. for the duration of a hash table scan).
struct AggregateFinalizeInputData : public AggregateInputData {
	DUCKDB_API AggregateFinalizeInputData(const BoundAggregateFunction &function_p,
	                                      optional_ptr<FunctionData> bind_data_p, ArenaAllocator &allocator_p,
	                                      optional_ptr<FunctionLocalState> local_state_p = nullptr);
	DUCKDB_API AggregateFinalizeInputData(const BoundAggregateExpression &expr, ArenaAllocator &allocator_p,
	                                      optional_ptr<FunctionLocalState> local_state_p = nullptr);
	DUCKDB_API AggregateFinalizeInputData(const AggregateObject &aggr, ArenaAllocator &allocator_p,
	                                      optional_ptr<FunctionLocalState> local_state_p = nullptr);

	//! The local state of the finalize (set if the function defines an "init_local_state_finalize" callback)
	optional_ptr<FunctionLocalState> local_state;

private:
	//! Initializes the local state when the caller did not pass in an external one
	void InitializeLocalState();

private:
	//! The local state owned by this input data - used when the caller does not pass in an external local state
	unique_ptr<FunctionLocalState> owned_state;
};

struct AggregateUnaryInput {
	AggregateUnaryInput(AggregateInputData &input_p, const ValidityMask &input_mask_p)
	    : input(input_p), input_mask(input_mask_p), input_idx(0) {
	}

	AggregateInputData &input;
	const ValidityMask &input_mask;
	idx_t input_idx;

	inline bool RowIsValid() {
		return input_mask.RowIsValid(input_idx);
	}
};

struct AggregateBinaryInput {
	AggregateBinaryInput(AggregateInputData &input_p, const ValidityMask &left_mask_p, const ValidityMask &right_mask_p)
	    : input(input_p), left_mask(left_mask_p), right_mask(right_mask_p) {
	}

	AggregateInputData &input;
	const ValidityMask &left_mask;
	const ValidityMask &right_mask;
	idx_t lidx;
	idx_t ridx;
};

struct AggregateFinalizeData {
	AggregateFinalizeData(Vector &result_p, AggregateFinalizeInputData &input_p, idx_t result_count_p = 1)
	    : result(result_p), input(input_p), result_idx(0), result_count(result_count_p) {
	}

	Vector &result;
	AggregateFinalizeInputData &input;
	idx_t result_idx;
	idx_t result_count;

	inline void ReturnNull() {
		switch (result.GetVectorType()) {
		case VectorType::FLAT_VECTOR:
			FlatVector::SetNull(result, result_idx, true);
			break;
		case VectorType::CONSTANT_VECTOR:
			ConstantVector::SetNull(result, count_t(result_count));
			break;
		default:
			throw InternalException("Invalid result vector type for aggregate");
		}
	}

	inline string_t ReturnString(string_t value) {
		return StringVector::AddStringOrBlob(result, value);
	}
};

struct AggregateStatisticsInput {
	AggregateStatisticsInput(optional_ptr<FunctionData> bind_data_p, vector<BaseStatistics> &child_stats_p,
	                         optional_ptr<NodeStatistics> node_stats_p)
	    : bind_data(bind_data_p), child_stats(child_stats_p), node_stats(node_stats_p) {
	}

	optional_ptr<FunctionData> bind_data;
	vector<BaseStatistics> &child_stats;
	optional_ptr<NodeStatistics> node_stats;
	//! When unset, the callback must not change the SQL shape of the aggregate - implementation swaps that
	//! preserve the bound definition remain allowed
	bool allow_plan_rewrite = true;
};

} // namespace duckdb
