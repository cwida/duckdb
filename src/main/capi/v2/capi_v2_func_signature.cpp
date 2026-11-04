#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi/capi_function_signature.hpp"

using namespace duckdb::capiv2;

static auto ConvertParameterKind(DUCKDB_V2_FUNCTION_PARAMETER_KIND kind) -> duckdb::FunctionParameterKind {
	switch (kind) {
	case DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_ONLY:
		return duckdb::FunctionParameterKind::POSITIONAL_ONLY;
	case DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD:
		return duckdb::FunctionParameterKind::STANDARD;
	case DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC:
		return duckdb::FunctionParameterKind::VAR_POSITIONAL;
	case DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_ONLY:
		return duckdb::FunctionParameterKind::KEYWORD_ONLY;
	case DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_VARIADIC:
		return duckdb::FunctionParameterKind::VAR_KEYWORD;
	default:
		throw duckdb::InvalidInputException("Invalid function parameter kind %d", static_cast<int>(kind));
	}
}

DUCKDB_V2_ERROR
duckdb_v2_function_signature_add_parameter(duckdb_v2_function_signature_handle sig, const duckdb_v2_identifier_t *name,
                                           duckdb_v2_logical_type_handle type, duckdb_v2_value_handle value,
                                           DUCKDB_V2_FUNCTION_PARAMETER_KIND kind, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(sig);
	DUCKDB_CHECK_ARG(type);

	return WithErrorHandler(err, [&]() {
		auto &signature = *Convert(sig);
		auto param_kind = ConvertParameterKind(kind);
		auto param_name = duckdb::Identifier(ConvertIdentifierName(name));
		duckdb::optional<duckdb::Value> default_value;
		if (value) {
			default_value = *Convert(value);
		}
		auto param_type = Convert(type);
		// parameters of different kinds can be added in any order, the signature orders them by kind
		duckdb::CAPIFunctionSignature::AddParameter(
		    signature,
		    duckdb::FunctionParameter(std::move(param_name), *param_type, std::move(default_value), param_kind));
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_signature_set_return_type(duckdb_v2_function_signature_handle sig,
                                                             duckdb_v2_logical_type_handle type,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(sig);
	DUCKDB_CHECK_ARG(type);

	return WithErrorHandler(err, [&]() {
		auto &signature = *Convert(sig);
		auto return_type = Convert(type);
		signature.SetReturnType(*return_type);
	});
}
