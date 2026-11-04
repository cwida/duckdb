//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/vector_operations/aggregate_executor.hpp"
#include "duckdb/function/aggregate_state.hpp"
#include "duckdb/function/aggregate_state_layout.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BufferManager;
class FunctionBinder;
class InterruptState;
class BoundAggregateFunction;
struct AggregateRewriteInput;
struct AggregateRewritePlan;
struct AggregateRewriteCostInput;

//! A half-open range of frame boundary values _relative to the current row_
//! This is why they are signed values.
struct FrameDelta {
	FrameDelta() : begin(0), end(0) {};
	FrameDelta(int64_t begin, int64_t end) : begin(begin), end(end) {};
	int64_t begin = 0;
	int64_t end = 0;
};

//! The half-open ranges of frame boundary values relative to the current row
using FrameStats = array<FrameDelta, 2>;

//! The partition data for custom window functions
//! Note that if the inputs is nullptr then the column count is 0,
//! but the row count will still be valid
class ColumnDataCollection;
struct WindowPartitionInput {
	WindowPartitionInput(ExecutionContext &context, const ColumnDataCollection *inputs, const idx_t count,
	                     const vector<column_t> &column_ids, const vector<bool> &all_valid,
	                     const ValidityMask &filter_mask, const FrameStats &stats, InterruptState &interrupt_state)
	    : context(context), inputs(inputs), count(count), column_ids(column_ids), all_valid(all_valid),
	      filter_mask(filter_mask), stats(stats), interrupt_state(interrupt_state) {
	}
	ExecutionContext &context;
	const ColumnDataCollection *inputs;
	const idx_t count;
	const vector<column_t> column_ids;
	const vector<bool> &all_valid;
	const ValidityMask &filter_mask;
	const FrameStats stats;
	InterruptState &interrupt_state;
};

class BindAggregateFunctionInput : public BindFunctionInput {
public:
	// Defined out-of-line: converting to the BindFunctionInput base requires the complete BoundAggregateFunction.
	BindAggregateFunctionInput(ClientContext &context_p, BoundAggregateFunction &bound_function_p,
	                           vector<unique_ptr<Expression>> &arguments_p);

	BoundAggregateFunction &GetBoundFunction() const {
		return bound_function;
	}

private:
	BoundAggregateFunction &bound_function;
};

class FunctionExpression;

struct AggregateFunctionUnbindInput {
	AggregateFunctionUnbindInput(const BoundAggregateExpression &expression_p,
	                             vector<unique_ptr<ParsedExpression>> children_p);
	~AggregateFunctionUnbindInput();

	const BoundAggregateExpression &expression;
	vector<unique_ptr<ParsedExpression>> children;
};

typedef unique_ptr<FunctionExpression> (*aggregate_function_unbind_t)(AggregateFunctionUnbindInput &input);

//! The type used for sizing hashed aggregate function states
typedef idx_t (*aggregate_size_t)(AggregateStateInput &input);
//! The type used for initializing hashed aggregate function states (batched: initializes `count` states)
typedef void (*aggregate_initialize_t)(AggregateStateInput &input, data_ptr_t *states, idx_t count);
//! The type used for updating hashed aggregate functions
typedef void (*aggregate_update_t)(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                                   Vector &state, idx_t count);
//! The type used for combining hashed aggregate states
typedef void (*aggregate_combine_t)(Vector &state, Vector &combined, AggregateInputData &aggr_input_data, idx_t count);
//! The type used for finalizing hashed aggregate function payloads
typedef void (*aggregate_finalize_t)(Vector &state, AggregateFinalizeInputData &finalize_input_data, Vector &result,
                                     idx_t count, idx_t offset);
//! Initializes the local state used by the finalize of the aggregate (optional).
//! The local state can be used to cache expensive intermediates between finalized groups - callers can keep the
//! local state alive to re-use it across multiple finalize calls (see AggregateFinalizeInputData).
typedef unique_ptr<FunctionLocalState> (*aggregate_init_local_state_finalize_t)(const BoundAggregateFunction &function,
                                                                                optional_ptr<FunctionData> bind_data);
//! The type used for propagating statistics in aggregate functions (optional)
typedef unique_ptr<BaseStatistics> (*aggregate_statistics_t)(ClientContext &context, BoundAggregateExpression &expr,
                                                             AggregateStatisticsInput &input);
//! Binds the scalar function and creates the function data
typedef unique_ptr<FunctionData> (*bind_aggregate_function_t)(BindAggregateFunctionInput &input);
//! The type used for the aggregate destructor method. NOTE: this method is used in destructors and MAY NOT throw.
typedef void (*aggregate_destructor_t)(Vector &state, AggregateInputData &aggr_input_data, idx_t count);

//! The type used for updating a clustered set of aggregate states.
typedef void (*aggregate_cluster_update_t)(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                                           const ClusteredAggr &clustered, idx_t count);

//! The type used for computing complex/custom windowed aggregate functions (optional)
typedef void (*aggregate_window_t)(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
                                   const_data_ptr_t g_state, data_ptr_t l_state, const SubFrames &subframes,
                                   Vector &result, idx_t rid);

//! Batched variant of aggregate_window_t — called once per Evaluate() with frame
//! bounds for all `count` output rows pre-computed. `subframes_per_row` points
//! to `count` SubFrames entries (each 1-3 FrameBounds depending on EXCLUDE
//! clause). When set, the window executor prefers this over the per-row
//! callback, letting implementations issue a single batched call (e.g., one
//! RPC for the whole Evaluate chunk instead of count separate calls).
typedef void (*aggregate_window_batch_t)(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
                                         const_data_ptr_t g_state, data_ptr_t l_state,
                                         const SubFrames *subframes_per_row, idx_t count, Vector &result,
                                         idx_t row_idx);

//! The type used for initializing shared complex/custom windowed aggregate state (optional)
typedef void (*aggregate_wininit_t)(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
                                    data_ptr_t g_state);

typedef void (*aggregate_serialize_t)(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                      const BoundAggregateFunction &function);
typedef unique_ptr<FunctionData> (*aggregate_deserialize_t)(Deserializer &deserializer,
                                                            BoundAggregateFunction &function);

typedef AggregateStateLayout (*aggregate_get_state_type_t)(AggregateLayoutInput &input);

//! Replaces an aggregate with another expression in the same aggregate operator.
typedef unique_ptr<Expression> (*aggregate_direct_rewrite_t)(AggregateRewriteInput &input);
//! Creates an owned logical plan rewrite for a bound aggregate.
typedef unique_ptr<AggregateRewritePlan> (*aggregate_rewrite_t)(AggregateRewriteInput &input);
//! Decides whether a cost-based logical rewrite should be applied.
typedef bool (*aggregate_rewrite_cost_t)(AggregateRewriteCostInput &input);

enum class AggregateRewritePolicy : uint8_t {
	//! Required lowering for aggregates whose native state is not suitable for ordinary grouped execution.
	MANDATORY,
	//! An optional rewrite that is always applied when its optimizer strategy is enabled.
	UNCONDITIONAL,
	//! An optional rewrite selected using propagated input statistics.
	COST_BASED
};

//! Input to the import_aggregate_state callback: deserializes the input_vec.size() exported states from input_vec into
//! dest_buffer (state i at offset i * layout.total_state_size).
struct AggregateImportInputData {
	AggregateImportInputData(const AggregateStateLayout &layout, const Vector &input_vec, data_ptr_t dest_buffer,
	                         ArenaAllocator &allocator)
	    : layout(layout), input_vec(input_vec), dest_buffer(dest_buffer), allocator(allocator) {
	}

	const AggregateStateLayout &layout;
	const Vector &input_vec;
	data_ptr_t dest_buffer;
	ArenaAllocator &allocator;
};

//! The counterpart of export_aggregate_state, for states a static field layout cannot describe (e.g. a hash map).
typedef void (*aggregate_import_state_t)(AggregateImportInputData &input);

struct AggregateFunctionInfo {
	DUCKDB_API virtual ~AggregateFunctionInfo();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

enum class AggregateDestructorType {
	STANDARD,
	// legacy destructors allow non-trivial destructors in aggregate states
	// these might not be trivial to off-load to disk
	LEGACY
};

class AggregateFunctionCallbacks {
public:
	// clang-format off
	bool HasBindCallback() const { return bind != nullptr; }
	bind_aggregate_function_t GetBindCallback() const { return bind; }
	void SetBindCallback(bind_aggregate_function_t callback) { bind = callback; }
	bool HasUnbindCallback() const { return unbind != nullptr; }
	aggregate_function_unbind_t GetUnbindCallback() const { return unbind; }
	void SetUnbindCallback(aggregate_function_unbind_t callback) { unbind = callback; }

	bool HasStateInitCallback() const { return initialize != nullptr; }
	aggregate_initialize_t GetStateInitCallback() const { return initialize; }
	void SetStateInitCallback(aggregate_initialize_t callback) { initialize = callback; }

	bool HasStateSizeCallback() const { return state_size != nullptr; }
	aggregate_size_t GetStateSizeCallback() const { return state_size; }
	void SetStateSizeCallback(aggregate_size_t callback) { state_size = callback; }

	bool HasStateDestructorCallback() const { return destructor != nullptr; }
	aggregate_destructor_t GetStateDestructorCallback() const { return destructor; }
	void SetStateDestructorCallback(aggregate_destructor_t callback) { destructor = callback; }

	bool HasStateUpdateCallback() const { return update != nullptr; }
	aggregate_update_t GetStateUpdateCallback() const { return update; }
	void SetStateUpdateCallback(aggregate_update_t callback) { update = callback; }

	bool HasStateClusterUpdateCallback() const { return cluster_update != nullptr; }
	aggregate_cluster_update_t GetStateClusterUpdateCallback() const { return cluster_update; }
	void SetStateClusterUpdateCallback(aggregate_cluster_update_t callback) { cluster_update = callback; }

	void SetStateCombineCallback(aggregate_combine_t callback) { combine = callback; }
	aggregate_combine_t GetStateCombineCallback() const { return combine; }
	bool HasStateCombineCallback() const { return combine != nullptr; }

	void SetStateFinalizeCallback(aggregate_finalize_t callback) { finalize = callback; }
	aggregate_finalize_t GetStateFinalizeCallback() const { return finalize; }
	bool HasStateFinalizeCallback() const { return finalize != nullptr; }

	void SetInitLocalStateFinalizeCallback(aggregate_init_local_state_finalize_t callback) { init_local_state_finalize = callback; }
	aggregate_init_local_state_finalize_t GetInitLocalStateFinalizeCallback() const { return init_local_state_finalize; }
	bool HasInitLocalStateFinalizeCallback() const { return init_local_state_finalize != nullptr; }

	bool HasWindowCallback() const { return window != nullptr; }
	aggregate_window_t GetWindowCallback() const { return window; }
	void SetWindowCallback(aggregate_window_t callback) { window = callback; }

	void SetWindowInitCallback(aggregate_wininit_t callback) { window_init = callback; }
	aggregate_wininit_t GetWindowInitCallback() const { return window_init; }
	bool HasWindowInitCallback() const { return window_init != nullptr; }

	//! Batched window callback — takes precedence over the per-row window
	//! callback when set. See aggregate_window_batch_t for semantics.
	bool HasWindowBatchCallback() const { return window_batch != nullptr; }
	aggregate_window_batch_t GetWindowBatchCallback() const { return window_batch; }
	void SetWindowBatchCallback(aggregate_window_batch_t callback) { window_batch = callback; }

	bool HasStatisticsCallback() const { return statistics != nullptr; }
	aggregate_statistics_t GetStatisticsCallback() const { return statistics; }
	void SetStatisticsCallback(aggregate_statistics_t callback) { statistics = callback; }

	bool HasSerializationCallbacks() const { return serialize != nullptr && deserialize != nullptr; }
	void SetSerializeCallback(aggregate_serialize_t callback) { serialize = callback; }
	void SetDeserializeCallback(aggregate_deserialize_t callback) { deserialize = callback; }
	aggregate_serialize_t GetSerializeCallback() const { return serialize; }
	aggregate_deserialize_t GetDeserializeCallback() const { return deserialize; }

	bool HasDirectRewriteCallback() const { return direct_rewrite != nullptr; }
	aggregate_direct_rewrite_t GetDirectRewriteCallback() const { return direct_rewrite; }
	void SetDirectRewriteCallback(aggregate_direct_rewrite_t callback) { direct_rewrite = callback; }

	bool HasRewriteCallback() const { return rewrite != nullptr; }
	aggregate_rewrite_t GetRewriteCallback() const { return rewrite; }
	AggregateRewritePolicy GetRewritePolicy() const { return rewrite_policy; }
	OptimizerType GetRewriteOptimizerType() const { return rewrite_optimizer_type; }
	aggregate_rewrite_cost_t GetRewriteCostCallback() const { return rewrite_cost; }
	void SetRewriteCallback(aggregate_rewrite_t callback, AggregateRewritePolicy policy,
	                        OptimizerType optimizer_type = OptimizerType::INVALID,
	                        aggregate_rewrite_cost_t cost = nullptr) {
		D_ASSERT(callback);
		D_ASSERT((policy == AggregateRewritePolicy::MANDATORY) == (optimizer_type == OptimizerType::INVALID));
		D_ASSERT((policy == AggregateRewritePolicy::COST_BASED) == (cost != nullptr));
		rewrite = callback;
		rewrite_policy = policy;
		rewrite_optimizer_type = optimizer_type;
		rewrite_cost = cost;
	}

public:
	//! The hashed aggregate state sizing function
	aggregate_size_t state_size = nullptr;
	//! The hashed aggregate state initialization function
	aggregate_initialize_t initialize = nullptr;
	//! The hashed aggregate update state function (may be null, if window is set)
	aggregate_update_t update = nullptr;
	//! The hashed aggregate combine states function (may be null, if window is set)
	aggregate_combine_t combine = nullptr;
	//! The hashed aggregate finalization function (may be null, if window is set)
	aggregate_finalize_t finalize = nullptr;
	//! Initializes the local state used by the finalize (may be null)
	aggregate_init_local_state_finalize_t init_local_state_finalize = nullptr;
	//! The clustered aggregate update function (may be null)
	aggregate_cluster_update_t cluster_update = nullptr;
	//! The windowed aggregate custom function (may be null)
	aggregate_window_t window = nullptr;
	//! The windowed aggregate custom initialization function (may be null)
	aggregate_wininit_t window_init = nullptr;
	//! Batched windowed aggregate function (may be null; preferred when set)
	aggregate_window_batch_t window_batch = nullptr;

	//! The bind function (may be null)
	bind_aggregate_function_t bind = nullptr;
	aggregate_function_unbind_t unbind = nullptr;

	//! The destructor method (may be null)
	aggregate_destructor_t destructor = nullptr;

	//! The statistics propagation function (may be null)
	aggregate_statistics_t statistics = nullptr;

	aggregate_serialize_t serialize = nullptr;

	aggregate_deserialize_t deserialize = nullptr;

	//! Optional expression rewrite that remains inside the original aggregate operator.
	aggregate_direct_rewrite_t direct_rewrite = nullptr;

	//! Optional logical-plan rewrite for aggregates represented by multiple aggregate stages.
	aggregate_rewrite_t rewrite = nullptr;
	AggregateRewritePolicy rewrite_policy = AggregateRewritePolicy::UNCONDITIONAL;
	OptimizerType rewrite_optimizer_type = OptimizerType::INVALID;
	aggregate_rewrite_cost_t rewrite_cost = nullptr;

	aggregate_get_state_type_t get_state_type = nullptr;

	//! Explicit state export/import callbacks for states a static field layout cannot describe (e.g. a hash map).
	//! Must be set together; export acts as the finalize in state-export mode.
	aggregate_finalize_t export_aggregate_state = nullptr;
	aggregate_import_state_t import_aggregate_state = nullptr;

	bool operator==(const AggregateFunctionCallbacks &rhs) const;
	bool operator!=(const AggregateFunctionCallbacks &rhs) const;
};

class AggregateFunctionProperties : public FunctionProperties {
public:
	//! Whether the aggregate is order dependent
	AggregateOrderDependent order_dependent = AggregateOrderDependent::ORDER_DEPENDENT;

	//! Whether the aggregate is affect by distinct modifiers
	AggregateDistinctDependent distinct_dependent = AggregateDistinctDependent::DISTINCT_DEPENDENT;

	//! Whether a single input row finalizes to that input's first argument unchanged
	bool single_value_identity = false;

	//! Whether an aggregate is holistic (e.g., don't use it for window segment trees...)
	bool is_holistic = false;

	bool operator==(const AggregateFunctionProperties &rhs) const;
	bool operator!=(const AggregateFunctionProperties &rhs) const;
};

class BaseAggregateFunction {
public:
	// clang-format off
	auto GetProperties() const -> const AggregateFunctionProperties & { return properties; }
	auto GetProperties() -> AggregateFunctionProperties & { return properties; }
	auto SetProperties(const AggregateFunctionProperties &value) -> void { properties = value; }

	auto GetCallbacks() const -> const AggregateFunctionCallbacks & { return callbacks; }
	auto GetCallbacks() -> AggregateFunctionCallbacks & { return callbacks; }
	auto SetCallbacks(const AggregateFunctionCallbacks &value) -> void { callbacks = value; }

public: // Properties

	auto GetStability() const -> FunctionStability { return properties.stability; }
	auto SetStability(FunctionStability value) -> void { properties.stability = value; }

	auto GetNullHandling() const -> FunctionNullHandling { return properties.null_handling; }
	auto SetNullHandling(FunctionNullHandling value) -> void { properties.null_handling = value; }

	auto GetErrorMode() const -> FunctionErrors { return properties.errors; }
	auto SetErrorMode(FunctionErrors value) -> void { properties.errors = value; }

	auto GetCollationHandling() const -> FunctionCollationHandling { return properties.collation_handling; }
	auto SetCollationHandling(FunctionCollationHandling value) -> void { properties.collation_handling = value; }

	//! Set this functions error-mode as fallible (can throw runtime errors)
	void SetFallible() { properties.errors = FunctionErrors::CAN_THROW_RUNTIME_ERROR; }
	//! Set this functions stability as volatile (can not be cached per row)
	void SetVolatile() { properties.stability = FunctionStability::VOLATILE; }

	//! Whether the aggregate is order dependent
	auto GetOrderDependent() const -> AggregateOrderDependent { return properties.order_dependent; }
	auto SetOrderDependent(AggregateOrderDependent value) -> void { properties.order_dependent = value; }

	//! Whether the aggregate is affect by distinct modifiers
	auto GetDistinctDependent() const -> AggregateDistinctDependent { return properties.distinct_dependent; }
	auto SetDistinctDependent(AggregateDistinctDependent value) -> void { properties.distinct_dependent = value; }

	//! Whether a single input row finalizes to that input's first argument unchanged
	auto HasSingleValueIdentity() const -> bool { return properties.single_value_identity; }
	auto SetSingleValueIdentity(bool value) -> void { properties.single_value_identity = value; }

	//! Whether the aggregate is holistic.
	auto IsHolistic() const -> bool { return properties.is_holistic; }
	auto SetIsHolistic(bool value) -> void { properties.is_holistic = value; }

	// Derived properties
	bool CanAggregate() const { return callbacks.update || callbacks.combine || callbacks.finalize; }
	bool CanWindow() const { return callbacks.window  || callbacks.window_batch; }

public: // Callbacks

	auto HasBindCallback() const -> bool { return callbacks.bind != nullptr; }
	auto GetBindCallback() const -> bind_aggregate_function_t { return callbacks.bind; }
	auto SetBindCallback(bind_aggregate_function_t callback) -> void { callbacks.bind = callback; }
	auto HasUnbindCallback() const -> bool { return callbacks.unbind != nullptr; }
	auto GetUnbindCallback() const -> aggregate_function_unbind_t { return callbacks.unbind; }
	auto SetUnbindCallback(aggregate_function_unbind_t callback) -> void { callbacks.unbind = callback; }

	auto HasStateInitCallback() const -> bool { return callbacks.initialize != nullptr; }
	auto GetStateInitCallback() const -> aggregate_initialize_t { return callbacks.initialize; }
	auto SetStateInitCallback(aggregate_initialize_t callback) -> void { callbacks.initialize = callback; }

	auto HasStateSizeCallback() const -> bool { return callbacks.state_size != nullptr; }
	auto GetStateSizeCallback() const -> aggregate_size_t { return callbacks.state_size; }
	auto SetStateSizeCallback(aggregate_size_t callback) -> void { callbacks.state_size = callback; }

	auto HasStateDestructorCallback() const -> bool { return callbacks.destructor != nullptr; }
	auto GetStateDestructorCallback() const -> aggregate_destructor_t { return callbacks.destructor; }
	auto SetStateDestructorCallback(aggregate_destructor_t callback) -> void { callbacks.destructor = callback; }

	auto HasStateUpdateCallback() const -> bool { return callbacks.update != nullptr; }
	auto GetStateUpdateCallback() const -> aggregate_update_t { return callbacks.update; }
	auto SetStateUpdateCallback(aggregate_update_t callback) -> void { callbacks.update = callback; }

	auto HasStateClusterUpdateCallback() const -> bool { return callbacks.cluster_update != nullptr; }
	auto GetStateClusterUpdateCallback() const -> aggregate_cluster_update_t { return callbacks.cluster_update; }
	auto SetStateClusterUpdateCallback(aggregate_cluster_update_t callback) -> void { callbacks.cluster_update = callback; }

	auto HasStateCombineCallback() const -> bool { return callbacks.combine != nullptr; }
	auto GetStateCombineCallback() const -> aggregate_combine_t { return callbacks.combine; }
	auto SetStateCombineCallback(aggregate_combine_t callback) -> void { callbacks.combine = callback; }

	auto HasStateFinalizeCallback() const -> bool { return callbacks.finalize != nullptr; }
	auto GetStateFinalizeCallback() const -> aggregate_finalize_t { return callbacks.finalize; }
	auto SetStateFinalizeCallback(aggregate_finalize_t callback) -> void { callbacks.finalize = callback; }

	auto HasInitLocalStateFinalizeCallback() const -> bool { return callbacks.init_local_state_finalize != nullptr; }
	auto GetInitLocalStateFinalizeCallback() const -> aggregate_init_local_state_finalize_t { return callbacks.init_local_state_finalize; }
	auto SetInitLocalStateFinalizeCallback(aggregate_init_local_state_finalize_t callback) -> void { callbacks.init_local_state_finalize = callback; }

	auto HasWindowCallback() const -> bool { return callbacks.window != nullptr; }
	auto GetWindowCallback() const -> aggregate_window_t { return callbacks.window; }
	auto SetWindowCallback(aggregate_window_t callback) -> void { callbacks.window = callback; }

	auto SetWindowInitCallback(aggregate_wininit_t callback) -> void { callbacks.window_init = callback; }
	auto GetWindowInitCallback() const -> aggregate_wininit_t { return callbacks.window_init; }
	auto HasWindowInitCallback() const -> bool { return callbacks.window_init != nullptr; }

	auto HasWindowBatchCallback() const -> bool { return callbacks.window_batch != nullptr; }
	auto GetWindowBatchCallback() const -> aggregate_window_batch_t { return callbacks.window_batch; }
	auto SetWindowBatchCallback(aggregate_window_batch_t callback) -> void { callbacks.window_batch = callback; }

	auto HasStatisticsCallback() const -> bool { return callbacks.statistics != nullptr; }
	auto GetStatisticsCallback() const -> aggregate_statistics_t { return callbacks.statistics; }
	auto SetStatisticsCallback(aggregate_statistics_t callback) -> void { callbacks.statistics = callback; }

	auto HasSerializationCallbacks() const -> bool { return callbacks.serialize != nullptr && callbacks.deserialize != nullptr; }
	auto SetSerializeCallback(aggregate_serialize_t callback) -> void { callbacks.serialize = callback; }
	auto SetDeserializeCallback(aggregate_deserialize_t callback) -> void { callbacks.deserialize = callback; }
	auto GetSerializeCallback() const -> aggregate_serialize_t { return callbacks.serialize; }
	auto GetDeserializeCallback() const -> aggregate_deserialize_t { return callbacks.deserialize; }

	auto HasDirectRewriteCallback() const -> bool { return callbacks.direct_rewrite != nullptr; }
	auto GetDirectRewriteCallback() const -> aggregate_direct_rewrite_t { return callbacks.direct_rewrite; }
	auto SetDirectRewriteCallback(aggregate_direct_rewrite_t callback) -> void { callbacks.direct_rewrite = callback; }

	auto HasRewriteCallback() const -> bool { return callbacks.rewrite != nullptr; }
	auto GetRewriteCallback() const -> aggregate_rewrite_t { return callbacks.rewrite; }
	auto GetRewritePolicy() const -> AggregateRewritePolicy { return callbacks.rewrite_policy; }
	auto GetRewriteOptimizerType() const -> OptimizerType { return callbacks.rewrite_optimizer_type; }
	auto GetRewriteCostCallback() const -> aggregate_rewrite_cost_t { return callbacks.rewrite_cost; }
	auto SetRewriteCallback(aggregate_rewrite_t callback, AggregateRewritePolicy policy,
	                        OptimizerType optimizer_type = OptimizerType::INVALID,
	                        aggregate_rewrite_cost_t cost = nullptr) -> void {
		callbacks.SetRewriteCallback(callback, policy, optimizer_type, cost);
	}

	bool HasGetStateTypeCallback() const { return callbacks.get_state_type != nullptr; }
	aggregate_get_state_type_t GetStateTypeCallback() const { return callbacks.get_state_type; }

	bool HasExportAggregateStateCallback() const { return callbacks.export_aggregate_state != nullptr; }
	aggregate_finalize_t GetExportAggregateStateCallback() const { return callbacks.export_aggregate_state; }
	void SetExportAggregateStateCallback(aggregate_finalize_t callback) { callbacks.export_aggregate_state = callback; }

	bool HasImportAggregateStateCallback() const { return callbacks.import_aggregate_state != nullptr; }
	aggregate_import_state_t GetImportAggregateStateCallback() const { return callbacks.import_aggregate_state; }
	void SetImportAggregateStateCallback(aggregate_import_state_t callback) { callbacks.import_aggregate_state = callback; }
	// clang-format on

public: // Extra function info
	auto HasExtraFunctionInfo() const -> bool {
		return function_info != nullptr;
	}
	auto GetExtraFunctionInfo() const -> AggregateFunctionInfo & {
		D_ASSERT(function_info.get());
		return *function_info;
	}
	auto SetExtraFunctionInfo(shared_ptr<AggregateFunctionInfo> info) -> void {
		function_info = std::move(info);
	}
	template <class T, class... ARGS>
	auto SetExtraFunctionInfo(ARGS &&... args) -> void {
		function_info = make_shared_ptr<T>(std::forward<ARGS>(args)...);
	}
	auto GetFunctionInfo() const -> shared_ptr<AggregateFunctionInfo> {
		return function_info;
	}

protected:
	AggregateFunctionProperties properties;
	AggregateFunctionCallbacks callbacks;
	shared_ptr<AggregateFunctionInfo> function_info;
};

class AggregateFunction : public BaseAggregateFunction, public SimpleFunction { // NOLINT: work-around bug in clang-tidy
public:
	static constexpr const FunctionKind KIND = FunctionKind::AGGREGATE;
	FunctionKind GetKind() const override {
		return KIND;
	}
	//! The properties of the function, for code that only knows it as a SimpleFunction
	auto GetFunctionProperties() const -> optional_ptr<const FunctionProperties> override {
		return GetProperties();
	}
	static constexpr aggregate_cluster_update_t NoClusterUpdate() {
		return nullptr;
	}
	static constexpr bind_aggregate_function_t NoBind() {
		return nullptr;
	}

	AggregateFunction(const Identifier &name, const vector<LogicalType> &arguments, const LogicalType &return_type,
	                  aggregate_size_t state_size, aggregate_initialize_t initialize, aggregate_update_t update,
	                  aggregate_combine_t combine, aggregate_finalize_t finalize,
	                  FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                  aggregate_cluster_update_t cluster_update_p = nullptr, bind_aggregate_function_t bind = nullptr,
	                  aggregate_destructor_t destructor = nullptr, aggregate_statistics_t statistics = nullptr,
	                  aggregate_window_batch_t window_batch = nullptr, aggregate_serialize_t serialize = nullptr,
	                  aggregate_deserialize_t deserialize = nullptr)
	    : SimpleFunction(name, arguments, return_type) {
		properties.null_handling = null_handling;

		callbacks.state_size = state_size;
		callbacks.initialize = initialize;
		callbacks.update = update;
		callbacks.combine = combine;
		callbacks.finalize = finalize;
		callbacks.cluster_update = cluster_update_p;
		callbacks.window_batch = window_batch;
		callbacks.bind = bind;
		callbacks.destructor = destructor;
		callbacks.statistics = statistics;
		callbacks.serialize = serialize;
		callbacks.deserialize = deserialize;
	}

	AggregateFunction(const Identifier &name, const vector<LogicalType> &arguments, const LogicalType &return_type,
	                  aggregate_size_t state_size, aggregate_initialize_t initialize, aggregate_update_t update,
	                  aggregate_combine_t combine, aggregate_finalize_t finalize,
	                  aggregate_cluster_update_t cluster_update_p = nullptr, bind_aggregate_function_t bind = nullptr,
	                  aggregate_destructor_t destructor = nullptr, aggregate_statistics_t statistics = nullptr,
	                  aggregate_window_batch_t window_batch = nullptr, aggregate_serialize_t serialize = nullptr,
	                  aggregate_deserialize_t deserialize = nullptr)
	    : SimpleFunction(name, arguments, return_type) {
		callbacks.state_size = state_size;
		callbacks.initialize = initialize;
		callbacks.update = update;
		callbacks.combine = combine;
		callbacks.finalize = finalize;
		callbacks.cluster_update = cluster_update_p;
		callbacks.bind = bind;
		callbacks.destructor = destructor;
		callbacks.statistics = statistics;
		callbacks.window_batch = window_batch;
		callbacks.serialize = serialize;
		callbacks.deserialize = deserialize;
	}

	AggregateFunction(const vector<LogicalType> &arguments, const LogicalType &return_type, aggregate_size_t state_size,
	                  aggregate_initialize_t initialize, aggregate_update_t update, aggregate_combine_t combine,
	                  aggregate_finalize_t finalize,
	                  FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                  aggregate_cluster_update_t cluster_update_p = nullptr, bind_aggregate_function_t bind = nullptr,
	                  aggregate_destructor_t destructor = nullptr, aggregate_statistics_t statistics = nullptr,
	                  aggregate_window_batch_t window = nullptr, aggregate_serialize_t serialize = nullptr,
	                  aggregate_deserialize_t deserialize = nullptr)
	    : AggregateFunction(Identifier(), arguments, return_type, state_size, initialize, update, combine, finalize,
	                        null_handling, cluster_update_p, bind, destructor, statistics, window, serialize,
	                        deserialize) {
	}

	AggregateFunction(const vector<LogicalType> &arguments, const LogicalType &return_type, aggregate_size_t state_size,
	                  aggregate_initialize_t initialize, aggregate_update_t update, aggregate_combine_t combine,
	                  aggregate_finalize_t finalize, aggregate_cluster_update_t cluster_update_p = nullptr,
	                  bind_aggregate_function_t bind = nullptr, aggregate_destructor_t destructor = nullptr,
	                  aggregate_statistics_t statistics = nullptr, aggregate_window_batch_t window = nullptr,
	                  aggregate_serialize_t serialize = nullptr, aggregate_deserialize_t deserialize = nullptr)
	    : AggregateFunction(Identifier(), arguments, return_type, state_size, initialize, update, combine, finalize,
	                        FunctionNullHandling::DEFAULT_NULL_HANDLING, cluster_update_p, bind, destructor, statistics,
	                        window, serialize, deserialize) {
	}

	// Window constructor
	AggregateFunction(const vector<LogicalType> &arguments, const LogicalType &return_type, aggregate_size_t state_size,
	                  aggregate_initialize_t initialize, aggregate_wininit_t window_init,
	                  aggregate_window_batch_t window_batch, bind_aggregate_function_t bind = nullptr,
	                  aggregate_destructor_t destructor = nullptr, aggregate_statistics_t statistics = nullptr,
	                  aggregate_serialize_t serialize = nullptr, aggregate_deserialize_t deserialize = nullptr)
	    : SimpleFunction(Identifier(), arguments, return_type) {
		callbacks.state_size = state_size;
		callbacks.initialize = initialize;
		callbacks.window_batch = window_batch;
		callbacks.window_init = window_init;
		callbacks.bind = bind;
		callbacks.destructor = destructor;
		callbacks.statistics = statistics;
		callbacks.serialize = serialize;
		callbacks.deserialize = deserialize;
	}

public:
	bool operator==(const AggregateFunction &rhs) const {
		return callbacks == rhs.callbacks;
	}
	bool operator!=(const AggregateFunction &rhs) const {
		return !(*this == rhs);
	}

	unique_ptr<BoundAggregateExpression> Bind(ClientContext &context, vector<unique_ptr<Expression>> arguments) const;

	//! Statistics callback for aggregates whose result always lies within the range of their first
	//! argument (e.g. min, max, first, median): the output inherits the input column statistics
	static unique_ptr<BaseStatistics> PropagateInputValueStats(ClientContext &context, BoundAggregateExpression &expr,
	                                                           AggregateStatisticsInput &input);

	AggregateFunction &SetStructStateExport(aggregate_get_state_type_t get_state_type_callback) {
		callbacks.get_state_type = get_state_type_callback;
		return *this;
	}

	//! Registers explicit export/import callbacks for states a static field layout cannot describe (e.g. a hash map);
	//! get_state_type still supplies the exported logical type.
	AggregateFunction &SetStateExportCallbacks(aggregate_get_state_type_t get_state_type_callback,
	                                           aggregate_finalize_t export_aggregate_state_callback,
	                                           aggregate_import_state_t import_aggregate_state_callback) {
		callbacks.get_state_type = get_state_type_callback;
		callbacks.export_aggregate_state = export_aggregate_state_callback;
		callbacks.import_aggregate_state = import_aggregate_state_callback;
		return *this;
	}

	AggregateFunction &SetClusterCallback(aggregate_cluster_update_t cluster_update) {
		callbacks.cluster_update = cluster_update;
		return *this;
	}

public:
	template <class STATE, class INPUT_TYPE, class OP>
	static aggregate_cluster_update_t UnaryClusterUpdateCallback() {
		if constexpr (AggregateExecutor::HasClusteredLocalState<OP, STATE>::value ||
		              AggregateExecutor::HasClusteredOperation<OP, INPUT_TYPE, STATE>::value) {
			return AggregateFunction::UnaryClusterUpdate<STATE, INPUT_TYPE, OP>;
		} else {
			return nullptr;
		}
	}

	template <class STATE, class RESULT_TYPE, class OP>
	static AggregateFunction NullaryAggregate(const LogicalType &return_type) {
		AggregateFunction result(
		    Identifier(), {}, return_type, AggregateFunction::StateSize<STATE>,
		    AggregateFunction::StateInitialize<STATE, OP>, AggregateFunction::NullaryScatterUpdate<STATE, OP>,
		    AggregateFunction::StateCombine<STATE, OP>, AggregateFunction::StateFinalize<STATE, RESULT_TYPE, OP>,
		    FunctionNullHandling::DEFAULT_NULL_HANDLING, AggregateFunction::NullaryClusterUpdate<STATE, OP>);
		WireStructStateType<STATE>(result);
		return result;
	}

	template <class STATE, class INPUT_TYPE, class RESULT_TYPE, class OP,
	          AggregateDestructorType destructor_type = AggregateDestructorType::STANDARD>
	static AggregateFunction
	UnaryAggregate(const LogicalType &input_type, const LogicalType &return_type,
	               FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING) {
		AggregateFunction result(Identifier(), {input_type}, return_type, AggregateFunction::StateSize<STATE>,
		                         AggregateFunction::StateInitialize<STATE, OP, destructor_type>,
		                         AggregateFunction::UnaryScatterUpdate<STATE, INPUT_TYPE, OP>,
		                         AggregateFunction::StateCombine<STATE, OP>,
		                         AggregateFunction::StateFinalize<STATE, RESULT_TYPE, OP>, null_handling,
		                         UnaryClusterUpdateCallback<STATE, INPUT_TYPE, OP>());
		// automatically wire up the destructor if the operation defines a Destroy method
		if constexpr (OperationHasDestroy<STATE, OP>::value) {
			result.callbacks.destructor = AggregateFunction::StateDestroy<STATE, OP>;
		}
		WireStructStateType<STATE>(result);
		return result;
	}

	//! Deprecated: use UnaryAggregate instead - the destructor is now automatically wired up when the operation
	//! defines a Destroy method
	template <class STATE, class INPUT_TYPE, class RESULT_TYPE, class OP,
	          AggregateDestructorType destructor_type = AggregateDestructorType::STANDARD>
	[[deprecated("Use UnaryAggregate instead - the destructor is now wired up automatically")]] static AggregateFunction
	UnaryAggregateDestructor(LogicalType input_type, LogicalType return_type) {
		auto aggregate = UnaryAggregate<STATE, INPUT_TYPE, RESULT_TYPE, OP, destructor_type>(input_type, return_type);
		aggregate.callbacks.destructor = AggregateFunction::StateDestroy<STATE, OP>;
		return aggregate;
	}

	template <class STATE, class A_TYPE, class B_TYPE, class RESULT_TYPE, class OP,
	          AggregateDestructorType destructor_type = AggregateDestructorType::STANDARD>
	static AggregateFunction BinaryAggregate(const LogicalType &a_type, const LogicalType &b_type,
	                                         const LogicalType &return_type) {
		AggregateFunction result({a_type, b_type}, return_type, AggregateFunction::StateSize<STATE>,
		                         AggregateFunction::StateInitialize<STATE, OP, destructor_type>,
		                         AggregateFunction::BinaryScatterUpdate<STATE, A_TYPE, B_TYPE, OP>,
		                         AggregateFunction::StateCombine<STATE, OP>,
		                         AggregateFunction::StateFinalize<STATE, RESULT_TYPE, OP>, nullptr);
		WireStructStateType<STATE>(result);
		return result;
	}

public:
	//! Returns the logical type for state type ST, resolving types that are only known after binding
	//! (sort keys, linked lists and StateString fields) from the bound aggregate function.
	//! Defined out-of-line (after BoundAggregateFunction is complete).
	template <class ST, class STATE>
	static LogicalType BuildStateLogical(const BoundAggregateFunction &bound_function);

	//! Defined out-of-line (after BoundAggregateFunction is complete) so the lambda body can call GetReturnType().
	template <class STATE>
	static void WireStructStateType(AggregateFunction &result);

	//! The state layout of a STATE with a STATE_TYPE - defined out-of-line (after BoundAggregateFunction is complete)
	template <class STATE>
	static AggregateStateLayout StructStateLayout(AggregateLayoutInput &input);

	template <class STATE>
	static idx_t StateSize(AggregateStateInput &) {
		return sizeof(STATE);
	}

	template <class...>
	using initialize_void_t = void;

	//! Detects whether "OP" provides an "Initialize(STATE &)" method
	template <class STATE, class OP, class = void>
	struct OperationHasInitialize : std::false_type {};
	template <class STATE, class OP>
	struct OperationHasInitialize<STATE, OP, initialize_void_t<decltype(OP::Initialize(std::declval<STATE &>()))>>
	    : std::true_type {};

	//! Detects whether "OP" provides a "Destroy(STATE &, AggregateInputData &)" method
	template <class STATE, class OP, class = void>
	struct OperationHasDestroy : std::false_type {};
	template <class STATE, class OP>
	struct OperationHasDestroy<STATE, OP,
	                           initialize_void_t<decltype(OP::template Destroy<STATE>(
	                               std::declval<STATE &>(), std::declval<AggregateInputData &>()))>> : std::true_type {
	};

	template <class STATE, class OP, AggregateDestructorType destructor_type = AggregateDestructorType::STANDARD>
	static void StateInitialize(AggregateStateInput &, data_ptr_t *states, idx_t count) {
		// FIXME: we should remove the "destructor_type" option in the future
#if !defined(__GNUC__) || (__GNUC__ >= 5)
		static_assert(std::is_trivially_move_constructible<STATE>::value ||
		                  destructor_type == AggregateDestructorType::LEGACY,
		              "Aggregate state must be trivially move constructible");
#endif
		for (idx_t i = 0; i < count; i++) {
			if constexpr (OperationHasInitialize<STATE, OP>::value) {
				OP::Initialize(*reinterpret_cast<STATE *>(states[i]));
			} else {
				// if the operation does not define an Initialize method - initialize the state by zero-initializing it
				memset(states[i], 0, sizeof(STATE));
			}
		}
	}

	template <class STATE, class OP>
	static void NullaryScatterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
	                                 Vector &states, idx_t count) {
		D_ASSERT(input_count == 0);
		AggregateExecutor::NullaryScatter<STATE, OP>(states, aggr_input_data, count);
	}

	template <class STATE, class OP>
	static void NullaryUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, data_ptr_t state,
	                          idx_t count) {
		D_ASSERT(input_count == 0);
		AggregateExecutor::NullaryUpdate<STATE, OP>(state, aggr_input_data, count);
	}

	template <class STATE, class OP>
	static void NullaryClusterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
	                                 const ClusteredAggr &clustered, idx_t count) {
		D_ASSERT(input_count == 0);
		AggregateExecutor::NullaryClustUpdate<STATE, OP>(aggr_input_data, clustered, count);
	}

	//! Update callbacks consume their leading arguments. They can be handed more: a bind may fold trailing
	//! arguments into its bind data (e.g. the separator of string_agg), and those stay part of the argument list and
	//! are still evaluated into the payload - they are simply not read here.
	template <class STATE, class T, class OP>
	static void UnaryScatterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
	                               Vector &states, idx_t count) {
		D_ASSERT(input_count >= 1);
		AggregateExecutor::UnaryScatter<STATE, T, OP>(inputs[0], states, aggr_input_data, count);
	}

	template <class STATE, class INPUT_TYPE, class OP>
	static void UnaryUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, data_ptr_t state,
	                        idx_t count) {
		D_ASSERT(input_count >= 1);
		AggregateExecutor::UnaryUpdate<STATE, INPUT_TYPE, OP>(inputs[0], aggr_input_data, state, count);
	}

	template <class STATE, class INPUT_TYPE, class OP>
	static void UnaryClusterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
	                               const ClusteredAggr &clustered, idx_t count) {
		D_ASSERT(input_count >= 1);
		AggregateExecutor::ExecuteUnaryClustered<STATE, INPUT_TYPE, OP>(inputs[0], aggr_input_data, clustered, count);
	}

	template <class STATE, class A_TYPE, class B_TYPE, class OP>
	static void BinaryScatterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
	                                Vector &states, idx_t count) {
		D_ASSERT(input_count >= 2);
		AggregateExecutor::BinaryScatter<STATE, A_TYPE, B_TYPE, OP>(aggr_input_data, inputs[0], inputs[1], states,
		                                                            count);
	}

	template <class STATE, class A_TYPE, class B_TYPE, class OP>
	static void BinaryUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, data_ptr_t state,
	                         idx_t count) {
		D_ASSERT(input_count >= 2);
		AggregateExecutor::BinaryUpdate<STATE, A_TYPE, B_TYPE, OP>(aggr_input_data, inputs[0], inputs[1], state, count);
	}

	template <class STATE, class OP>
	static void StateCombine(Vector &source, Vector &target, AggregateInputData &aggr_input_data, idx_t count) {
		AggregateExecutor::Combine<STATE, OP>(source, target, aggr_input_data, count);
	}

	template <class STATE, class RESULT_TYPE, class OP>
	static void StateFinalize(Vector &states, AggregateFinalizeInputData &finalize_input_data, Vector &result,
	                          idx_t count, idx_t offset) {
		AggregateExecutor::Finalize<STATE, RESULT_TYPE, OP>(states, finalize_input_data, result, count, offset);
	}

	template <class STATE, class OP>
	static void StateVoidFinalize(Vector &states, AggregateFinalizeInputData &finalize_input_data, Vector &result,
	                              idx_t count, idx_t offset) {
		AggregateExecutor::VoidFinalize<STATE, OP>(states, finalize_input_data, result, count, offset);
	}

	template <class STATE, class OP>
	static void StateDestroy(Vector &states, AggregateInputData &aggr_input_data, idx_t count) {
		AggregateExecutor::Destroy<STATE, OP>(states, aggr_input_data, count);
	}
};

class BoundAggregateFunction : public BaseAggregateFunction, public BoundSimpleFunction {
public:
	explicit BoundAggregateFunction(const AggregateFunction &function);
	explicit BoundAggregateFunction(shared_ptr<const AggregateFunction> function);

	//! Swap in a different implementation, keeping the definition this was bound from intact
	void ReplaceImplementation(const AggregateFunction &function);
	void ReplaceImplementation(const BoundAggregateFunction &function);

	DUCKDB_API bool operator==(const BoundAggregateFunction &rhs) const;
	DUCKDB_API bool operator!=(const BoundAggregateFunction &rhs) const;

public:
	//! The function this was bound from. Unaffected by ReplaceImplementation and by later mutation of the bound
	//! function, e.g. statistics propagation swapping in a specialized implementation. For a function bound from an
	//! AggregateFunctionSet this is the set's own overload, so it compares equal by pointer across binds. Functions
	//! bound outside of a set are copied into a definition of their own.
	//! Only null in a moved-from bound function.
	const shared_ptr<const AggregateFunction> &GetDefinition() const {
		return definition;
	}
	//! The number of arguments that were received by the standard and positional-only parameters, they come first
	idx_t GetStandardArgumentCount() const {
		return BoundSimpleFunction::GetStandardArgumentCount(definition->GetSignature());
	}
	//! The number of arguments that were received by "*args", they directly follow the standard parameters
	idx_t GetVarArgsCount() const {
		return BoundSimpleFunction::GetVarArgsCount(definition->GetSignature());
	}
	//! The number of arguments that were received by the keyword-only parameters, they follow "*args"
	idx_t GetKeywordOnlyArgumentCount() const {
		return BoundSimpleFunction::GetKeywordOnlyArgumentCount(definition->GetSignature());
	}
	//! The number of arguments that were received by "**kwargs", they are the last arguments
	idx_t GetKwargsCount() const {
		return BoundSimpleFunction::GetKwargsCount(definition->GetSignature());
	}
	//! The kind of the parameter that received the argument at the given index
	FunctionParameterKind GetArgumentParameterKind(idx_t argument_index) const {
		return BoundSimpleFunction::GetArgumentParameterKind(definition->GetSignature(), argument_index);
	}
	//! Restore the definition after the bound function has been replaced wholesale, together with the
	//! qualification it carries - the replacement is a specialized implementation, not a different function
	void SetDefinition(shared_ptr<const AggregateFunction> definition_p) {
		definition = std::move(definition_p);
		if (definition) {
			qualified_name = definition->GetQualifiedName().WithName(GetName());
		}
	}
	const vector<LogicalType> &GetLogicalArguments() const {
		return logical_arguments;
	}
	const LogicalType &GetLogicalReturnType() const {
		return logical_return_type;
	}
	AggregateStateLayout GetStateType(optional_ptr<FunctionData> bind_data) const {
		D_ASSERT(callbacks.get_state_type);
		AggregateLayoutInput input(*this, bind_data);
		return callbacks.get_state_type(input);
	}

	//! Sizes an aggregate state by constructing an AggregateStateInput and invoking the state-size callback
	idx_t GetStateSize(optional_ptr<const FunctionData> bind_data) const {
		D_ASSERT(callbacks.state_size);
		AggregateStateInput input(*this, bind_data);
		return callbacks.state_size(input);
	}

private:
	void SetLogicalArguments(vector<LogicalType> arguments_p) {
		logical_arguments = std::move(arguments_p);
	}
	void SetLogicalReturnType(LogicalType return_type_p) {
		logical_return_type = std::move(return_type_p);
	}
	shared_ptr<const AggregateFunction> definition;
	vector<LogicalType> logical_arguments;
	LogicalType logical_return_type;

	friend class FunctionSerializer;
	friend class FunctionBinder;
};

// Defined here (after BoundAggregateFunction is complete) so the lambda body can call GetReturnType().
template <class STATE>
inline void AggregateFunction::WireStructStateType(AggregateFunction &result) {
	if constexpr (HasStructStateType<STATE>::value) {
		result.SetStructStateExport(StructStateLayout<STATE>);
	} else if constexpr (HasPrimitiveLogicalType<STATE>::value) {
		result.SetStructStateExport([](AggregateLayoutInput &) {
			return AggregateStateLayout(PrimitiveToLogicalType<STATE>(), AlignValue<idx_t>(sizeof(STATE)));
		});
	}
}

template <class STATE>
inline AggregateStateLayout AggregateFunction::StructStateLayout(AggregateLayoutInput &input) {
	using ST = typename STATE::STATE_TYPE;
	auto &bound = input.function;
	AggregateStateLayout layout;
	if (bound.GetReturnType().IsAggregateState()) {
		// the function has been modified for state export (see ExportAggregateFunction::SetStateExport) -
		// its return type IS the state type already
		layout.type = bound.GetReturnType();
	} else {
		layout.type = AggregateFunction::BuildStateLogical<ST, STATE>(bound);
	}
	layout.total_state_size = AlignValue<idx_t>(sizeof(STATE));
	layout.field = BuildStateField<ST>();
	AggregateStateField::PopulateListFunctions(layout.type, layout.field);
	return layout;
}

// Defined here (after BoundAggregateFunction is complete) so the body can access the bound function's types.
template <class ST, class STATE>
inline LogicalType AggregateFunction::BuildStateLogical(const BoundAggregateFunction &bound_function) {
	// the runtime types of the bound function - used to resolve StateReturnType/StateInputType sources
	StateLayoutTypeInfo info {bound_function.GetReturnType(), bound_function.GetArguments()};
	if constexpr (IsStateListType<ST>::value) {
		using SRC = typename ST::SOURCE_TYPE;
		if constexpr (IsStateTypeSource<SRC>::value) {
			return ResolveStateSourceType<SRC>(info);
		} else {
			// the element is described by a nested field descriptor (e.g. a sort key)
			return LogicalType::LIST(FieldToLogicalType<SRC>(info));
		}
	} else if constexpr (IsOptionalStateType<ST>::value) {
		using V = typename ST::value_type;
		if constexpr (IsStructStateType<V>::value) {
			return V::GetLogicalType(STATE::STATE_NAMES, info);
		} else {
			return FieldToLogicalType<V>(info);
		}
	} else {
		return ST::GetLogicalType(STATE::STATE_NAMES, info);
	}
}

} // namespace duckdb
