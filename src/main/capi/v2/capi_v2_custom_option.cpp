#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb::capiv2 {

class CV2CustomOptionUpdateInfo {
public:
	CV2CustomOptionUpdateInfo(ClientContext &context, const LogicalType &type, Value &value, SetScope scope,
	                          bool is_reset, void *user_data)
	    : context(context), type(type), value(value), scope(scope), is_reset(is_reset), in_user_data(user_data) {
	}

	ClientContext &context;
	const LogicalType &type;
	//! The value the option is updated to, replaceable by the callback
	Value &value;
	SetScope scope;
	bool is_reset;
	void *in_user_data;
};

static auto Convert(duckdb_v2_custom_option_update_info_handle info) -> CV2CustomOptionUpdateInfo * {
	return reinterpret_cast<CV2CustomOptionUpdateInfo *>(info);
}
static auto Convert(CV2CustomOptionUpdateInfo *info) -> duckdb_v2_custom_option_update_info_handle {
	return reinterpret_cast<duckdb_v2_custom_option_update_info_handle>(info);
}

class CV2CustomOptionInfo final : public ExtensionOptionInfo {
public:
	void OnSet(ClientContext &context, SetScope scope, Value &parameter, bool is_reset) const override {
		if (!update_cb) {
			return;
		}
		CV2CustomOptionUpdateInfo info(context, type, parameter, scope, is_reset,
		                               user_data ? user_data->GetData() : nullptr);

		CV2ErrorInfo err = {};
		auto err_ptr = Convert(&err);
		CV2CallbackContext callback_context(context);
		update_cb(Convert(&info), Convert(&callback_context), &err_ptr);

		if (err.HasError()) {
			err.ThrowAsException();
		}
	}

public:
	duckdb_v2_custom_option_update_callback_fn update_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
	LogicalType type;
};

class CV2CustomOption {
public:
	explicit CV2CustomOption(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and registers the option on `target`.
	void Register(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "custom option");
		if (name.empty()) {
			throw InvalidInputException("Option name cannot be empty.");
		}
		auto option_type = type;
		if (option_type.id() == LogicalTypeId::INVALID) {
			option_type = default_value.type();
		}
		if (option_type.id() == LogicalTypeId::INVALID || option_type.id() == LogicalTypeId::SQLNULL) {
			throw InvalidInputException(
			    "A type, or a default value to take the type from, must be set for the option.");
		}
		if (!option_type.IsComplete()) {
			throw InvalidInputException("The type of the option must be a fully defined concrete type.");
		}
		if (!supports_global && !supports_session) {
			throw InvalidInputException("The option must support at least one of the GLOBAL and SESSION scopes.");
		}
		auto scope = default_scope;
		if (scope == SetScope::AUTOMATIC) {
			scope = supports_session ? SetScope::SESSION : SetScope::GLOBAL;
		}
		if ((scope == SetScope::GLOBAL && !supports_global) || (scope == SetScope::SESSION && !supports_session)) {
			throw InvalidInputException("The default scope of the option must be a scope it supports.");
		}

		auto &config = DBConfig::GetConfig(target);
		if (DBConfig::GetOptionByName(name) || config.HasExtensionOption(name)) {
			throw InvalidInputException("An option named %s already exists.", name);
		}

		ExtensionOption option;
		option.description = description;
		option.type = option_type;
		option.default_value = default_value.IsNull() ? Value(option_type) : default_value.DefaultCastAs(option_type);
		option.default_scope = scope;
		option.supports_global = supports_global;
		option.supports_session = supports_session;
		auto option_info = make_shared_ptr<CV2CustomOptionInfo>();
		option_info->update_cb = update_cb;
		option_info->user_data = user_data;
		option_info->type = option_type;
		option.info = std::move(option_info);
		config.AddExtensionOption(name, std::move(option));
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	Identifier name;
	LogicalType type;
	Value default_value;
	string description;
	//! AUTOMATIC until set: resolved at registration from the supported scopes
	SetScope default_scope = SetScope::AUTOMATIC;
	bool supports_global = true;
	bool supports_session = true;
	duckdb_v2_custom_option_update_callback_fn update_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

static auto Convert(duckdb_v2_custom_option_handle option) -> CV2CustomOption * {
	return reinterpret_cast<CV2CustomOption *>(option);
}
static auto Convert(CV2CustomOption *option) -> duckdb_v2_custom_option_handle {
	return reinterpret_cast<duckdb_v2_custom_option_handle>(option);
}

static auto ConvertWriteScope(DUCKDB_V2_SETTING_SCOPE scope) -> SetScope {
	switch (scope) {
	case DUCKDB_V2_SETTING_SCOPE_GLOBAL:
		return SetScope::GLOBAL;
	case DUCKDB_V2_SETTING_SCOPE_SESSION:
		return SetScope::SESSION;
	default:
		throw InvalidInputException("The scope must be GLOBAL or SESSION.");
	}
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_custom_option_create(duckdb_v2_factory_handle factory, duckdb_v2_custom_option_handle *option,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(option);
	*option = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2CustomOption>(Convert(factory)->GetDatabase());
		*option = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_name(duckdb_v2_custom_option_handle option,
                                                 const duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(option)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_type(duckdb_v2_custom_option_handle option,
                                                 duckdb_v2_logical_type_handle type, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(type);
	return WithErrorHandler(err, [&]() {
		auto option_type = Convert(type);
		Convert(option)->type = *option_type;
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_default_value(duckdb_v2_custom_option_handle option,
                                                          duckdb_v2_value_handle value,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() { Convert(option)->default_value = *Convert(value); });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_description(duckdb_v2_custom_option_handle option,
                                                        const duckdb_v2_str *description,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(description);
	return WithErrorHandler(err, [&]() { Convert(option)->description = duckdb::string(Convert(description)); });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_default_scope(duckdb_v2_custom_option_handle option,
                                                          DUCKDB_V2_SETTING_SCOPE scope,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	return WithErrorHandler(err, [&]() { Convert(option)->default_scope = ConvertWriteScope(scope); });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_supports_scope(duckdb_v2_custom_option_handle option,
                                                           DUCKDB_V2_SETTING_SCOPE scope, bool supported,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(option);
		if (ConvertWriteScope(scope) == duckdb::SetScope::GLOBAL) {
			self.supports_global = supported;
		} else {
			self.supports_session = supported;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_user_data(duckdb_v2_custom_option_handle option, duckdb_v2_opaque *data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(option)->user_data = duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_set_update_callback(duckdb_v2_custom_option_handle option,
                                                            duckdb_v2_custom_option_update_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	return WithErrorHandler(err, [&]() { Convert(option)->update_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_update_get_user_data(duckdb_v2_custom_option_update_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_update_get_value(duckdb_v2_custom_option_update_info_handle info,
                                                         duckdb_v2_value_handle *value,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(err, [&]() { *value = Convert(new duckdb::Value(Convert(info)->value)); });
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_update_set_value(duckdb_v2_custom_option_update_info_handle info,
                                                         duckdb_v2_value_handle value,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &update = *Convert(info);
		update.value = Convert(value)->CastAs(update.context, update.type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_update_get_scope(duckdb_v2_custom_option_update_info_handle info,
                                                         DUCKDB_V2_SETTING_SCOPE *scope,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(scope);
	return WithErrorHandler(err, [&]() {
		*scope = Convert(info)->scope == duckdb::SetScope::GLOBAL ? DUCKDB_V2_SETTING_SCOPE_GLOBAL
		                                                          : DUCKDB_V2_SETTING_SCOPE_SESSION;
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_update_is_reset(duckdb_v2_custom_option_update_info_handle info, bool *is_reset,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(is_reset);
	return WithErrorHandler(err, [&]() { *is_reset = Convert(info)->is_reset; });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_custom_option(duckdb_v2_connection_handle conn,
                                                            duckdb_v2_custom_option_handle option,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(option);
	return WithErrorHandler(err, [&]() { Convert(option)->Register(*Convert(conn)->context->db); });
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_custom_option(duckdb_v2_extension_handle extension,
                                                           duckdb_v2_custom_option_handle option,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(option);
	return WithErrorHandler(err, [&]() {
		auto &loader = GetExtensionLoader(extension);
		Convert(option)->Register(loader.GetDatabaseInstance());
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_option_destroy(duckdb_v2_custom_option_handle *option) {
	return WithErrorHandler(nullptr, [&]() {
		if (!option) {
			return;
		}
		if (*option) {
			delete Convert(*option);
			*option = nullptr;
		}
	});
}
