#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/function/aggregate_function.hpp"

#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

AggregateFunctionUnbindInput::AggregateFunctionUnbindInput(const BoundAggregateExpression &expression_p,
                                                           vector<unique_ptr<ParsedExpression>> children_p)
    : expression(expression_p), children(std::move(children_p)) {
}

AggregateFunctionUnbindInput::~AggregateFunctionUnbindInput() {
}

unique_ptr<BaseStatistics> AggregateFunction::PropagateInputValueStats(ClientContext &context,
                                                                       BoundAggregateExpression &expr,
                                                                       AggregateStatisticsInput &input) {
	if (input.child_stats.empty() || expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
		return nullptr;
	}
	auto &child_stats = input.child_stats[0];
	auto &return_type = expr.GetReturnType();
	auto result = child_stats.GetType() == return_type
	                  ? child_stats.ToUnique()
	                  : CastStatistics::TryPropagate(child_stats, child_stats.GetType(), return_type);
	if (!result) {
		return nullptr;
	}
	result->ResetAdditiveStatistics();
	// the result is NULL when the aggregate sees no valid rows
	result->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	return result;
}

AggregateInputData::AggregateInputData(const BoundAggregateExpression &expr, ArenaAllocator &allocator_p,
                                       AggregateCombineType combine_type_p)
    : AggregateInputData(expr.Function(), expr.BindInfo().get(), allocator_p, combine_type_p) {
}

AggregateInputData::AggregateInputData(const AggregateObject &aggr, ArenaAllocator &allocator_p,
                                       AggregateCombineType combine_type_p)
    : AggregateInputData(aggr.function, aggr.bind_data_wrapper ? aggr.bind_data_wrapper->function_data.get() : nullptr,
                         allocator_p, combine_type_p) {
}

AggregateFinalizeInputData::AggregateFinalizeInputData(const BoundAggregateFunction &function_p,
                                                       optional_ptr<FunctionData> bind_data_p,
                                                       ArenaAllocator &allocator_p,
                                                       optional_ptr<FunctionLocalState> local_state_p)
    : AggregateInputData(function_p, bind_data_p, allocator_p), local_state(local_state_p) {
	InitializeLocalState();
}

AggregateFinalizeInputData::AggregateFinalizeInputData(const BoundAggregateExpression &expr,
                                                       ArenaAllocator &allocator_p,
                                                       optional_ptr<FunctionLocalState> local_state_p)
    : AggregateInputData(expr, allocator_p), local_state(local_state_p) {
	InitializeLocalState();
}

AggregateFinalizeInputData::AggregateFinalizeInputData(const AggregateObject &aggr, ArenaAllocator &allocator_p,
                                                       optional_ptr<FunctionLocalState> local_state_p)
    : AggregateInputData(aggr, allocator_p), local_state(local_state_p) {
	InitializeLocalState();
}

void AggregateFinalizeInputData::InitializeLocalState() {
	if (local_state) {
		// the caller passed in an externally-owned local state
		return;
	}
	auto &callbacks = function.GetCallbacks();
	if (callbacks.HasInitLocalStateFinalizeCallback()) {
		owned_state = callbacks.GetInitLocalStateFinalizeCallback()(function, bind_data);
		local_state = owned_state.get();
	}
}

bool AggregateFunctionProperties::operator==(const AggregateFunctionProperties &rhs) const {
	return FunctionProperties::operator==(rhs) && order_dependent == rhs.order_dependent &&
	       distinct_dependent == rhs.distinct_dependent && single_value_identity == rhs.single_value_identity &&
	       is_holistic == rhs.is_holistic;
}
bool AggregateFunctionProperties::operator!=(const AggregateFunctionProperties &rhs) const {
	return !(*this == rhs);
}

bool AggregateFunctionCallbacks::operator==(const AggregateFunctionCallbacks &rhs) const {
	return state_size == rhs.state_size && initialize == rhs.initialize && update == rhs.update &&
	       combine == rhs.combine && finalize == rhs.finalize &&
	       init_local_state_finalize == rhs.init_local_state_finalize && cluster_update == rhs.cluster_update &&
	       window == rhs.window && window_init == rhs.window_init && window_batch == rhs.window_batch &&
	       bind == rhs.bind && unbind == rhs.unbind && destructor == rhs.destructor && statistics == rhs.statistics &&
	       serialize == rhs.serialize && deserialize == rhs.deserialize && direct_rewrite == rhs.direct_rewrite &&
	       rewrite == rhs.rewrite && rewrite_policy == rhs.rewrite_policy &&
	       rewrite_optimizer_type == rhs.rewrite_optimizer_type && rewrite_cost == rhs.rewrite_cost &&
	       get_state_type == rhs.get_state_type && export_aggregate_state == rhs.export_aggregate_state &&
	       import_aggregate_state == rhs.import_aggregate_state;
}

bool AggregateFunctionCallbacks::operator!=(const AggregateFunctionCallbacks &rhs) const {
	return !(*this == rhs);
}

AggregateFunctionInfo::~AggregateFunctionInfo() {
}

unique_ptr<BoundAggregateExpression> AggregateFunction::Bind(ClientContext &context,
                                                             vector<unique_ptr<Expression>> arguments) const {
	FunctionBinder func_binder(context);
	return func_binder.BindAggregateFunction(*this, std::move(arguments));
}

BoundAggregateFunction::BoundAggregateFunction(const AggregateFunction &function)
    // the function does not come from a function set - copy it into a definition of its own
    : BoundAggregateFunction(make_shared_ptr<AggregateFunction>(function)) {
}

BoundAggregateFunction::BoundAggregateFunction(shared_ptr<const AggregateFunction> function_p)
    : definition(std::move(function_p)) {
	auto &function = *definition;
	qualified_name = function.GetQualifiedName();
	extra_info = function.extra_info;
	return_type = function.GetReturnType();
	properties = function.GetProperties();
	callbacks = function.GetCallbacks();
	function_info = function.GetFunctionInfo();

	// Try to default bind the function, to fill in any missing information in the BoundScalarFunction (e.g. from the
	// "bind" callback)
	for (auto &param : function.GetSignature().GetParameters()) {
		if (!param.IsVariadic()) {
			arguments.push_back(param.GetType());
		}
	}
	positional_arguments = arguments.size();
	logical_arguments = arguments;
	logical_return_type = return_type;
}

bool BoundAggregateFunction::operator==(const BoundAggregateFunction &rhs) const {
	return callbacks == rhs.callbacks && properties == rhs.properties && arguments == rhs.arguments &&
	       positional_arguments == rhs.positional_arguments && named_arguments == rhs.named_arguments &&
	       return_type == rhs.return_type;
}
bool BoundAggregateFunction::operator!=(const BoundAggregateFunction &rhs) const {
	return !(*this == rhs);
}

void BoundAggregateFunction::ReplaceImplementation(const BoundAggregateFunction &function) {
	BaseAggregateFunction::operator=(function);
	BoundSimpleFunction::operator=(function);
	SetDefinition(definition);
}

void BoundAggregateFunction::ReplaceImplementation(const AggregateFunction &function) {
	SetName(function.GetName());
	// The replacement is a specialized implementation of the function we were bound from, and is usually built by
	// a factory rather than handed out by a catalog entry. Only take its qualification when it has one, so that
	// specializing an implementation does not drop the catalog and schema name of the definition.
	if (!function.GetCatalogName().empty() || !function.GetSchemaName().empty()) {
		qualified_name = function.GetQualifiedName();
	}
	this->return_type = function.GetReturnType();
	this->properties = function.GetProperties();
	this->callbacks = function.GetCallbacks();
	this->function_info = function.GetFunctionInfo();

	// Try to default bind the function, to fill in any missing information in the BoundScalarFunction (e.g. from the
	// "bind" callback)
	arguments.clear();
	for (auto &param : function.GetSignature().GetParameters()) {
		if (!param.IsVariadic()) {
			arguments.push_back(param.GetType());
		}
	}
}

BindAggregateFunctionInput::BindAggregateFunctionInput(ClientContext &context_p,
                                                       BoundAggregateFunction &bound_function_p,
                                                       vector<unique_ptr<Expression>> &arguments_p)
    : BindFunctionInput(context_p, bound_function_p, bound_function_p.GetDefinition()->GetSignature(), arguments_p),
      bound_function(bound_function_p) {
}

} // namespace duckdb
