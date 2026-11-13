#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/capi/capi_function_signature.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct CAggregateFunctionInfo : public AggregateFunctionInfo {
	~CAggregateFunctionInfo() override {
		if (extra_info && delete_callback) {
			delete_callback(extra_info);
		}
		extra_info = nullptr;
		delete_callback = nullptr;
	}

	duckdb_aggregate_state_size state_size = nullptr;
	duckdb_aggregate_init_t state_init = nullptr;
	duckdb_aggregate_update_t update = nullptr;
	duckdb_aggregate_combine_t combine = nullptr;
	duckdb_aggregate_finalize_t finalize = nullptr;
	duckdb_aggregate_destroy_t destroy = nullptr;
	duckdb_function_info extra_info = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

struct CAggregateExecuteInfo {
	explicit CAggregateExecuteInfo(CAggregateFunctionInfo &info) : info(info) {
	}

	CAggregateFunctionInfo &info;
	bool success = true;
	string error;
};

struct CAggregateFunctionBindData : public FunctionData {
	explicit CAggregateFunctionBindData(CAggregateFunctionInfo &info) : info(info) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CAggregateFunctionBindData>(info);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CAggregateFunctionBindData>();
		return info.extra_info == other.info.extra_info && info.update == other.info.update &&
		       info.combine == other.info.combine && info.finalize == other.info.finalize;
	}

	CAggregateFunctionInfo &info;
};

duckdb::AggregateFunction &GetCAggregateFunction(duckdb_aggregate_function function) {
	return *reinterpret_cast<duckdb::AggregateFunction *>(function);
}

duckdb::AggregateFunctionSet &GetCAggregateFunctionSet(duckdb_aggregate_function_set function_set) {
	return *reinterpret_cast<duckdb::AggregateFunctionSet *>(function_set);
}

unique_ptr<FunctionData> CAPIAggregateBind(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &info = function.GetExtraFunctionInfo().Cast<CAggregateFunctionInfo>();
	return make_uniq<CAggregateFunctionBindData>(info);
}

idx_t CAPIAggregateStateSize(AggregateStateInput &input) {
	auto &function_info = input.function.GetExtraFunctionInfo().Cast<duckdb::CAggregateFunctionInfo>();
	CAggregateExecuteInfo exec_info(function_info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	auto result = function_info.state_size(c_function_info);
	if (!exec_info.success) {
		throw InvalidInputException(exec_info.error);
	}
	return result;
}

void CAPIAggregateStateInit(AggregateStateInput &input, data_ptr_t *states, idx_t count) {
	auto &function_info = input.function.GetExtraFunctionInfo().Cast<duckdb::CAggregateFunctionInfo>();
	CAggregateExecuteInfo exec_info(function_info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	// the V1 C init callback is single-state; drive it once per requested state
	for (idx_t i = 0; i < count; i++) {
		function_info.state_init(c_function_info, reinterpret_cast<duckdb_aggregate_state>(states[i]));
		if (!exec_info.success) {
			throw InvalidInputException(exec_info.error);
		}
	}
}

void CAPIAggregateUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state,
                         idx_t count) {
	DataChunk chunk;
	for (idx_t c = 0; c < input_count; c++) {
		inputs[c].Flatten();
		PrepareCAPIVector(inputs[c]);
		chunk.data.emplace_back(Vector::Ref(inputs[c]));
	}

	// the callback expects one state per row - flatten a reference, as callers may hold on to the state buffer
	auto states = Vector::Ref(state);
	if (states.GetVectorType() != VectorType::FLAT_VECTOR) {
		states.Flatten(*FlatVector::IncrementalSelectionVector(), count);
	}

	auto &bind_data = aggr_input_data.bind_data->Cast<CAggregateFunctionBindData>();
	auto state_data = FlatVector::GetDataMutableUnsafe<duckdb_aggregate_state>(states);
	auto c_input_chunk = reinterpret_cast<duckdb_data_chunk>(&chunk);

	CAggregateExecuteInfo exec_info(bind_data.info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	bind_data.info.update(c_function_info, c_input_chunk, state_data);
	if (!exec_info.success) {
		throw InvalidInputException(exec_info.error);
	}
}

void CAPIAggregateCombine(Vector &state, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {
	state.Flatten();
	auto &bind_data = aggr_input_data.bind_data->Cast<CAggregateFunctionBindData>();
	auto input_state_data = FlatVector::GetDataMutableUnsafe<duckdb_aggregate_state>(state);
	auto result_state_data = FlatVector::GetDataMutableUnsafe<duckdb_aggregate_state>(combined);
	CAggregateExecuteInfo exec_info(bind_data.info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	bind_data.info.combine(c_function_info, input_state_data, result_state_data, count);
	if (!exec_info.success) {
		throw InvalidInputException(exec_info.error);
	}
}

void CAPIAggregateFinalize(Vector &state, AggregateFinalizeInputData &aggr_input_data, Vector &result, idx_t count,
                           idx_t offset) {
	PrepareCAPIVector(result);
	state.Flatten();
	auto &bind_data = aggr_input_data.bind_data->Cast<CAggregateFunctionBindData>();
	auto input_state_data = FlatVector::GetDataMutableUnsafe<duckdb_aggregate_state>(state);
	auto result_vector = reinterpret_cast<duckdb_vector>(&result);

	CAggregateExecuteInfo exec_info(bind_data.info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	bind_data.info.finalize(c_function_info, input_state_data, result_vector, count, offset);
	if (!exec_info.success) {
		throw InvalidInputException(exec_info.error);
	}
}

void CAPIAggregateDestructor(Vector &state, AggregateInputData &aggr_input_data, idx_t count) {
	auto &bind_data = aggr_input_data.bind_data->Cast<CAggregateFunctionBindData>();
	auto input_state_data = FlatVector::GetDataMutableUnsafe<duckdb_aggregate_state>(state);
	bind_data.info.destroy(input_state_data, count);
}

} // namespace duckdb

using duckdb::GetCAggregateFunction;

duckdb_aggregate_function duckdb_create_aggregate_function() {
	auto function = new duckdb::AggregateFunction(
	    "", {}, duckdb::LogicalType::INVALID, duckdb::CAPIAggregateStateSize, duckdb::CAPIAggregateStateInit,
	    duckdb::CAPIAggregateUpdate, duckdb::CAPIAggregateCombine, duckdb::CAPIAggregateFinalize,
	    duckdb::FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, duckdb::CAPIAggregateBind);
	try {
		function->SetExtraFunctionInfo<duckdb::CAggregateFunctionInfo>();
		return reinterpret_cast<duckdb_aggregate_function>(function);
	} catch (...) {
		delete function;
		return nullptr;
	}
}

void duckdb_destroy_aggregate_function(duckdb_aggregate_function *function) {
	if (function && *function) {
		auto aggregate_function = reinterpret_cast<duckdb::AggregateFunction *>(*function);
		delete aggregate_function;
		*function = nullptr;
	}
}

void duckdb_aggregate_function_set_name(duckdb_aggregate_function function, const char *name) {
	if (!function || !name) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	aggregate_function.name = duckdb::Identifier(name);
}

void duckdb_aggregate_function_add_parameter(duckdb_aggregate_function function, duckdb_logical_type type) {
	if (!function || !type) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	auto logical_type = reinterpret_cast<duckdb::LogicalType *>(type);
	// v1 declares no parameter name, so the parameter is positional-only - see duckdb_scalar_function_add_parameter
	duckdb::CAPIFunctionSignature::AddPositionalOnly(aggregate_function.GetSignature(), *logical_type);
}

void duckdb_aggregate_function_set_return_type(duckdb_aggregate_function function, duckdb_logical_type type) {
	if (!function || !type) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	auto logical_type = reinterpret_cast<duckdb::LogicalType *>(type);
	aggregate_function.SetReturnType(*logical_type);
}

void duckdb_aggregate_function_set_functions(duckdb_aggregate_function function, duckdb_aggregate_state_size state_size,
                                             duckdb_aggregate_init_t state_init, duckdb_aggregate_update_t update,
                                             duckdb_aggregate_combine_t combine, duckdb_aggregate_finalize_t finalize) {
	if (!function || !state_size || !state_init || !update || !combine || !finalize) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	auto &function_info = aggregate_function.GetExtraFunctionInfo().Cast<duckdb::CAggregateFunctionInfo>();
	function_info.state_size = state_size;
	function_info.state_init = state_init;
	function_info.update = update;
	function_info.combine = combine;
	function_info.finalize = finalize;
}

void duckdb_aggregate_function_set_destructor(duckdb_aggregate_function function, duckdb_aggregate_destroy_t destroy) {
	if (!function || !destroy) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	auto &function_info = aggregate_function.GetExtraFunctionInfo().Cast<duckdb::CAggregateFunctionInfo>();
	function_info.destroy = destroy;
	aggregate_function.SetStateDestructorCallback(duckdb::CAPIAggregateDestructor);
}

duckdb_state duckdb_register_aggregate_function(duckdb_connection connection, duckdb_aggregate_function function) {
	if (!connection || !function) {
		return DuckDBError;
	}

	auto &aggregate_function = GetCAggregateFunction(function);
	duckdb::AggregateFunctionSet set {aggregate_function.name};
	set.AddFunction(aggregate_function);
	return duckdb_register_aggregate_function_set(connection, reinterpret_cast<duckdb_aggregate_function_set>(&set));
}

void duckdb_aggregate_function_set_special_handling(duckdb_aggregate_function function) {
	if (!function) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	aggregate_function.SetNullHandling(duckdb::FunctionNullHandling::SPECIAL_HANDLING);
}

void duckdb_aggregate_function_set_extra_info(duckdb_aggregate_function function, void *extra_info,
                                              duckdb_delete_callback_t destroy) {
	if (!function || !extra_info) {
		return;
	}
	auto &aggregate_function = GetCAggregateFunction(function);
	auto &function_info = aggregate_function.GetExtraFunctionInfo().Cast<duckdb::CAggregateFunctionInfo>();
	function_info.extra_info = static_cast<duckdb_function_info>(extra_info);
	function_info.delete_callback = destroy;
}

duckdb::CAggregateExecuteInfo &GetCAggregateExecuteInfo(duckdb_function_info info) {
	D_ASSERT(info);
	return *reinterpret_cast<duckdb::CAggregateExecuteInfo *>(info);
}

void *duckdb_aggregate_function_get_extra_info(duckdb_function_info info_p) {
	auto &exec_info = GetCAggregateExecuteInfo(info_p);
	return exec_info.info.extra_info;
}

void duckdb_aggregate_function_set_error(duckdb_function_info info_p, const char *error) {
	auto &exec_info = GetCAggregateExecuteInfo(info_p);
	exec_info.error = error;
	exec_info.success = false;
}

duckdb_aggregate_function_set duckdb_create_aggregate_function_set(const char *name) {
	if (!name || !*name) {
		return nullptr;
	}
	try {
		auto function_set = new duckdb::AggregateFunctionSet(name);
		return reinterpret_cast<duckdb_aggregate_function_set>(function_set);
	} catch (...) {
		return nullptr;
	}
}

void duckdb_destroy_aggregate_function_set(duckdb_aggregate_function_set *set) {
	if (set && *set) {
		auto aggregate_function_set = reinterpret_cast<duckdb::AggregateFunctionSet *>(*set);
		delete aggregate_function_set;
		*set = nullptr;
	}
}

duckdb_state duckdb_add_aggregate_function_to_set(duckdb_aggregate_function_set set,
                                                  duckdb_aggregate_function function) {
	if (!set || !function) {
		return DuckDBError;
	}
	auto &aggregate_function_set = duckdb::GetCAggregateFunctionSet(set);
	auto &aggregate_function = GetCAggregateFunction(function);
	aggregate_function_set.AddFunction(aggregate_function);
	return DuckDBSuccess;
}

duckdb_state duckdb_register_aggregate_function_set(duckdb_connection connection,
                                                    duckdb_aggregate_function_set function_set) {
	if (!connection || !function_set) {
		return DuckDBError;
	}
	auto &set = duckdb::GetCAggregateFunctionSet(function_set);
	for (idx_t idx = 0; idx < set.Size(); idx++) {
		const auto &aggregate_function = *set.GetFunctionByOffset(idx);
		auto &info = aggregate_function.GetExtraFunctionInfo().Cast<duckdb::CAggregateFunctionInfo>();

		if (aggregate_function.name.empty() || !info.update || !info.combine || !info.finalize) {
			return DuckDBError;
		}
		if (duckdb::TypeVisitor::Contains(aggregate_function.GetReturnType(), duckdb::LogicalTypeId::INVALID) ||
		    duckdb::TypeVisitor::Contains(aggregate_function.GetReturnType(), duckdb::LogicalTypeId::ANY)) {
			return DuckDBError;
		}
		for (const auto &argument : aggregate_function.GetSignature().GetParameters()) {
			if (duckdb::TypeVisitor::Contains(argument.GetType(), duckdb::LogicalTypeId::INVALID)) {
				return DuckDBError;
			}
		}
	}

	try {
		auto con = reinterpret_cast<duckdb::Connection *>(connection);
		con->context->RunFunctionInTransaction([&]() {
			auto &catalog = duckdb::Catalog::GetSystemCatalog(*con->context);
			duckdb::CreateAggregateFunctionInfo sf_info(set);
			sf_info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			catalog.CreateFunction(*con->context, sf_info);
		});
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}
