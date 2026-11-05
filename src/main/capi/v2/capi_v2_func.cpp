#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"

namespace duckdb::capiv2 {

const Identifier &CV2FunctionBindInfo::GetArgName(idx_t index) const {
	static const Identifier variadic_name;
	const auto positional_count = function.GetPositionalArgumentCount();
	if (index >= positional_count) {
		return function.GetNamedArguments()[index - positional_count];
	}
	if (index < signature.GetPositionalParameterCount()) {
		return signature.GetParameter(index).GetName();
	}
	return variadic_name;
}

optional_idx CV2FunctionBindInfo::FindArg(const Identifier &name) const {
	for (idx_t i = signature.GetPositionalOnlyParameterCount(); i < GetArgCount(); i++) {
		const auto &arg_name = GetArgName(i);
		if (!arg_name.empty() && arg_name == name) {
			return optional_idx(i);
		}
	}
	return optional_idx();
}

LogicalType CV2ExpressionBindInfo::GetArgType(idx_t index) const {
	return input.GetArguments()[index]->GetReturnType();
}

Value CV2ExpressionBindInfo::GetArgValue(idx_t index) const {
	return input.GetConstant(index);
}

CV2ConstantBindInfo::CV2ConstantBindInfo(const BoundSimpleFunction &function, const FunctionSignature &signature,
                                         void *user_data, const vector<Value> &positional,
                                         const named_argument_map_t &named)
    : CV2FunctionBindInfo(function, signature, user_data), positional(positional), named(named) {
	D_ASSERT(function.GetPositionalArgumentCount() == positional.size());
}

const Value &CV2ConstantBindInfo::GetArg(idx_t index) const {
	if (index < positional.size()) {
		return positional[index];
	}
	const auto &name = function.GetNamedArguments()[index - positional.size()];
	auto entry = named.find(name);
	if (entry == named.end()) {
		throw InternalException("Named argument %s of a function call has no value", name);
	}
	return entry->second;
}

LogicalType CV2ConstantBindInfo::GetArgType(idx_t index) const {
	return GetArg(index).type();
}

Value CV2ConstantBindInfo::GetArgValue(idx_t index) const {
	return GetArg(index);
}

static void CheckArgIndex(const CV2FunctionBindInfo &info, idx_t index, const char *function_name) {
	if (index >= info.GetArgCount()) {
		throw InvalidInputException("Index out of bounds in %s", function_name);
	}
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_function_bind_get_user_data(duckdb_v2_function_bind_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_function_bind_set_bind_data(duckdb_v2_function_bind_info_handle info, duckdb_v2_opaque *data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		auto &bind_data_error = Convert(info)->bind_data_error;
		if (!bind_data_error.empty()) {
			throw duckdb::InvalidInputException(bind_data_error);
		}
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_bind_get_arg_count(duckdb_v2_function_bind_info_handle info, idx_t *positional_fixed,
                                                      idx_t *positional_variadic, idx_t *named_fixed,
                                                      idx_t *named_variadic, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &function = Convert(info)->function;
		auto &signature = Convert(info)->signature;
		if (positional_fixed) {
			*positional_fixed = function.GetStandardArgumentCount(signature);
		}
		if (positional_variadic) {
			*positional_variadic = function.GetVarArgsCount(signature);
		}
		if (named_fixed) {
			*named_fixed = function.GetKeywordOnlyArgumentCount(signature);
		}
		if (named_variadic) {
			*named_variadic = function.GetKwargsCount(signature);
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_bind_get_arg_type(duckdb_v2_function_bind_info_handle info, idx_t index,
                                                     duckdb_v2_logical_type_handle *type,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		CheckArgIndex(bind_info, index, "duckdb_v2_function_bind_get_arg_type");
		*type = Convert(bind_info.GetArgType(index));
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_bind_get_arg_value(duckdb_v2_function_bind_info_handle info, idx_t index,
                                                      duckdb_v2_value_handle *value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		CheckArgIndex(bind_info, index, "duckdb_v2_function_bind_get_arg_value");
		*value = Convert(new duckdb::Value(bind_info.GetArgValue(index)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_bind_get_arg_name(duckdb_v2_function_bind_info_handle info, idx_t index,
                                                     duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		CheckArgIndex(bind_info, index, "duckdb_v2_function_bind_get_arg_name");
		*name = Convert(bind_info.GetArgName(index));
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_bind_get_arg_index(duckdb_v2_function_bind_info_handle info,
                                                      const duckdb_v2_identifier_t *name, idx_t *index, bool *found,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(index);
	DUCKDB_CHECK_ARG(found);
	return WithErrorHandler(err, [&]() {
		auto entry = Convert(info)->FindArg(duckdb::Identifier(ConvertIdentifierName(name)));
		*found = entry.IsValid();
		if (entry.IsValid()) {
			*index = entry.GetIndex();
		}
	});
}
