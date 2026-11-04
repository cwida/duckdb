#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

namespace duckdb {
namespace capiv2 {

//----------------------------------------------------------------------------------------------------------------------
// Instance Config
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_SETTING_SCOPE CV2InstanceConfig::ReadValue(std::string_view name, Value &result) {
	// Legacy options are read through a client context; a fresh one has no SESSION settings to shadow GLOBAL.
	auto connection = Connection::CreateInternal(*instance.GetDatabase().instance);
	CV2OptionSource(*connection.context).ReadValue(name, result);
	return DUCKDB_V2_SETTING_SCOPE_GLOBAL;
}

void CV2InstanceConfig::Write(const Identifier &name, const Value &value, DUCKDB_V2_SETTING_SCOPE scope) {
	if (scope == DUCKDB_V2_SETTING_SCOPE_SESSION) {
		throw InvalidInputException("an instance has no SESSION scope: write the option through a connection");
	}
	auto connection = Connection::CreateInternal(*instance.GetDatabase().instance);
	PhysicalSet::SetVariable(*connection.context, name, SetScope::GLOBAL, value);
}

unique_ptr<CV2Option> CV2InstanceConfig::GetOption(std::string_view name) {
	return CV2Option::FromName(CV2OptionSource(*instance.GetDatabase().instance), name);
}

unique_ptr<CV2Option> CV2InstanceConfig::GetOptionByIndex(idx_t index) {
	return CV2Option::FromIndex(CV2OptionSource(*instance.GetDatabase().instance), index);
}

idx_t CV2InstanceConfig::GetOptionCount() {
	return CV2Option::Count(CV2OptionSource(*instance.GetDatabase().instance));
}

//----------------------------------------------------------------------------------------------------------------------
// Client Config
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_SETTING_SCOPE CV2ClientConfig::ReadValue(std::string_view name, Value &result) {
	return CV2OptionSource(context).ReadValue(name, result);
}

void CV2ClientConfig::Write(const Identifier &name, const Value &value, DUCKDB_V2_SETTING_SCOPE scope) {
	PhysicalSet::SetVariable(context, name, ConvertSetScope(scope), value);
}

unique_ptr<CV2Option> CV2ClientConfig::GetOption(std::string_view name) {
	return CV2Option::FromName(CV2OptionSource(context), name);
}

unique_ptr<CV2Option> CV2ClientConfig::GetOptionByIndex(idx_t index) {
	return CV2Option::FromIndex(CV2OptionSource(context), index);
}

idx_t CV2ClientConfig::GetOptionCount() {
	return CV2Option::Count(CV2OptionSource(context));
}

} // namespace capiv2
} // namespace duckdb

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_instance_get_config(duckdb_v2_instance_handle instance, duckdb_v2_config_handle *out_config,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(out_config);
	*out_config = nullptr;
	return WithErrorHandler(err, [&]() { *out_config = Convert(&Convert(instance)->config_handle); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_get_config(duckdb_v2_connection_handle conn, duckdb_v2_config_handle *out_config,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(out_config);
	*out_config = nullptr;
	return WithErrorHandler(err, [&]() { *out_config = Convert(&Convert(conn)->context_handle.config_handle); });
}

DUCKDB_V2_ERROR duckdb_v2_context_get_config(duckdb_v2_context_handle ctx, duckdb_v2_config_handle *out_config,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(ctx);
	DUCKDB_CHECK_ARG(out_config);
	*out_config = nullptr;
	return WithErrorHandler(err, [&]() { *out_config = Convert(&Convert(ctx)->config_handle); });
}

DUCKDB_V2_ERROR duckdb_v2_config_get_option_value(duckdb_v2_config_handle config, const duckdb_v2_identifier_t *name,
                                                  duckdb_v2_value_handle *out_value, DUCKDB_V2_SETTING_SCOPE *out_scope,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(config);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_value);
	DUCKDB_CHECK_ARG(out_scope);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() {
		duckdb::Value value;
		*out_scope = Convert(config)->ReadValue(ConvertIdentifierName(name), value);
		*out_value = Convert(new duckdb::Value(std::move(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_config_set_option_value(duckdb_v2_config_handle config, const duckdb_v2_identifier_t *name,
                                                  duckdb_v2_value_handle value, DUCKDB_V2_SETTING_SCOPE scope,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(config);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		Convert(config)->Write(duckdb::Identifier(ConvertIdentifierName(name)), *Convert(value), scope);
	});
}

DUCKDB_V2_ERROR duckdb_v2_config_set_option_text(duckdb_v2_config_handle config, const duckdb_v2_identifier_t *name,
                                                 const duckdb_v2_str *setting, DUCKDB_V2_SETTING_SCOPE scope,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(config);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(setting);
	return WithErrorHandler(err, [&]() {
		Convert(config)->Write(duckdb::Identifier(ConvertIdentifierName(name)),
		                       duckdb::Value(duckdb::string(Convert(setting))), scope);
	});
}

DUCKDB_V2_ERROR duckdb_v2_config_get_option_by_name(duckdb_v2_config_handle config, const duckdb_v2_identifier_t *name,
                                                    duckdb_v2_option_handle *out_option,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(config);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_option);
	*out_option = nullptr;
	return WithErrorHandler(
	    err, [&]() { *out_option = Convert(Convert(config)->GetOption(ConvertIdentifierName(name)).release()); });
}

DUCKDB_V2_ERROR duckdb_v2_config_get_option_count(duckdb_v2_config_handle config, idx_t *out_count,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(config);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = Convert(config)->GetOptionCount(); });
}

DUCKDB_V2_ERROR duckdb_v2_config_get_option_by_index(duckdb_v2_config_handle config, idx_t index,
                                                     duckdb_v2_option_handle *out_option,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(config);
	DUCKDB_CHECK_ARG(out_option);
	*out_option = nullptr;
	return WithErrorHandler(err, [&]() { *out_option = Convert(Convert(config)->GetOptionByIndex(index).release()); });
}
