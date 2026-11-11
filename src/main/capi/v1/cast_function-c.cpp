#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

namespace duckdb {
struct CCastExecuteInfo {
	CastParameters &parameters;
	string error_message;

	explicit CCastExecuteInfo(CastParameters &parameters) : parameters(parameters), error_message() {
	}
};

struct CCastFunction {
	unique_ptr<LogicalType> source_type;
	unique_ptr<LogicalType> target_type;
	int64_t implicit_cast_cost = -1;

	duckdb_cast_function_t function;
	duckdb_function_info extra_info = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

struct CCastFunctionUserData {
	duckdb_function_info data_ptr = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;

	CCastFunctionUserData(duckdb_function_info data_ptr_p, duckdb_delete_callback_t delete_callback_p)
	    : data_ptr(data_ptr_p), delete_callback(delete_callback_p) {
	}

	~CCastFunctionUserData() {
		if (data_ptr && delete_callback) {
			delete_callback(data_ptr);
		}
		data_ptr = nullptr;
		delete_callback = nullptr;
	}
};

struct CCastFunctionData final : public BoundCastData {
	duckdb_cast_function_t function;
	shared_ptr<CCastFunctionUserData> extra_info;

	explicit CCastFunctionData(duckdb_cast_function_t function_p, shared_ptr<CCastFunctionUserData> extra_info_p)
	    : function(function_p), extra_info(std::move(extra_info_p)) {
	}

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<CCastFunctionData>(function, extra_info);
	}
};

static bool CAPICastFunction(Vector &input, Vector &output, idx_t count, CastParameters &parameters) {
	PrepareCAPIVector(output);
	const auto is_const = input.GetVectorType() == VectorType::CONSTANT_VECTOR;
	input.Flatten();
	PrepareCAPIVector(input);

	CCastExecuteInfo exec_info(parameters);
	const auto &data = parameters.cast_data->Cast<CCastFunctionData>();

	auto c_input = reinterpret_cast<duckdb_vector>(&input);
	auto c_output = reinterpret_cast<duckdb_vector>(&output);
	auto c_info = reinterpret_cast<duckdb_function_info>(&exec_info);

	const auto success = data.function(c_info, count, c_input, c_output);

	if (!success) {
		HandleCastError::AssignError(exec_info.error_message, parameters);
	}

	if (is_const && count == 1 && (success || !parameters.strict)) {
		output.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	return success;
}

} // namespace duckdb

duckdb_cast_function duckdb_create_cast_function() {
	const auto function = new duckdb::CCastFunction();
	return reinterpret_cast<duckdb_cast_function>(function);
}

void duckdb_cast_function_set_source_type(duckdb_cast_function cast_function, duckdb_logical_type source_type) {
	if (!cast_function || !source_type) {
		return;
	}
	const auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(source_type));
	auto &cast = *(reinterpret_cast<duckdb::CCastFunction *>(cast_function));
	cast.source_type = duckdb::make_uniq<duckdb::LogicalType>(logical_type);
}

void duckdb_cast_function_set_target_type(duckdb_cast_function cast_function, duckdb_logical_type target_type) {
	if (!cast_function || !target_type) {
		return;
	}
	const auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(target_type));
	auto &cast = *(reinterpret_cast<duckdb::CCastFunction *>(cast_function));
	cast.target_type = duckdb::make_uniq<duckdb::LogicalType>(logical_type);
}

void duckdb_cast_function_set_implicit_cast_cost(duckdb_cast_function cast_function, int64_t cost) {
	if (!cast_function) {
		return;
	}
	auto &custom_type = *(reinterpret_cast<duckdb::CCastFunction *>(cast_function));
	custom_type.implicit_cast_cost = cost;
}

void duckdb_cast_function_set_function(duckdb_cast_function cast_function, duckdb_cast_function_t function) {
	if (!cast_function || !function) {
		return;
	}
	auto &cast = *(reinterpret_cast<duckdb::CCastFunction *>(cast_function));
	cast.function = function;
}

duckdb_cast_mode duckdb_cast_function_get_cast_mode(duckdb_function_info info) {
	const auto &cast_info = *reinterpret_cast<duckdb::CCastExecuteInfo *>(info);
	return cast_info.parameters.error_message == nullptr ? DUCKDB_CAST_NORMAL : DUCKDB_CAST_TRY;
}

void duckdb_cast_function_set_error(duckdb_function_info info, const char *error) {
	auto &cast_info = *reinterpret_cast<duckdb::CCastExecuteInfo *>(info);
	cast_info.error_message = error;
}

void duckdb_cast_function_set_row_error(duckdb_function_info info, const char *error, idx_t row, duckdb_vector output) {
	auto &cast_info = *reinterpret_cast<duckdb::CCastExecuteInfo *>(info);
	cast_info.error_message = error;
	if (!output) {
		return;
	}
	auto &output_vector = *reinterpret_cast<duckdb::Vector *>(output);
	duckdb::FlatVector::SetNull(output_vector, row, true);
}

void duckdb_cast_function_set_extra_info(duckdb_cast_function cast_function, void *extra_info,
                                         duckdb_delete_callback_t destroy) {
	if (!cast_function || !extra_info) {
		return;
	}
	auto &cast = *reinterpret_cast<duckdb::CCastFunction *>(cast_function);
	cast.extra_info = static_cast<duckdb_function_info>(extra_info);
	cast.delete_callback = destroy;
}

void *duckdb_cast_function_get_extra_info(duckdb_function_info info) {
	if (!info) {
		return nullptr;
	}
	auto &cast_info = *reinterpret_cast<duckdb::CCastExecuteInfo *>(info);
	auto &cast_data = cast_info.parameters.cast_data->Cast<duckdb::CCastFunctionData>();
	return cast_data.extra_info->data_ptr;
}

duckdb_state duckdb_register_cast_function(duckdb_connection connection, duckdb_cast_function cast_function) {
	if (!connection || !cast_function) {
		return DuckDBError;
	}
	auto &cast = *reinterpret_cast<duckdb::CCastFunction *>(cast_function);
	if (!cast.source_type || !cast.target_type || !cast.function) {
		return DuckDBError;
	}

	const auto &source_type = *cast.source_type;
	const auto &target_type = *cast.target_type;

	if (duckdb::TypeVisitor::Contains(source_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(source_type, duckdb::LogicalTypeId::ANY)) {
		return DuckDBError;
	}

	if (duckdb::TypeVisitor::Contains(target_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(target_type, duckdb::LogicalTypeId::ANY)) {
		return DuckDBError;
	}

	try {
		const auto con = reinterpret_cast<duckdb::Connection *>(connection);
		con->context->RunFunctionInTransaction([&]() {
			auto &config = duckdb::DBConfig::GetConfig(*con->context);
			auto &casts = config.GetCastFunctions();

			auto extra_info =
			    duckdb::make_shared_ptr<duckdb::CCastFunctionUserData>(cast.extra_info, cast.delete_callback);
			auto cast_data = duckdb::make_uniq<duckdb::CCastFunctionData>(cast.function, std::move(extra_info));
			duckdb::BoundCastInfo cast_info(duckdb::CAPICastFunction, std::move(cast_data));
			casts.RegisterCastFunction(source_type, target_type, std::move(cast_info), cast.implicit_cast_cost);
		});
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

void duckdb_destroy_cast_function(duckdb_cast_function *cast_function) {
	if (!cast_function || !*cast_function) {
		return;
	}
	const auto function = reinterpret_cast<duckdb::CCastFunction *>(*cast_function);
	delete function;
	*cast_function = nullptr;
}
