#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/database_path_and_type.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

namespace duckdb {
namespace capiv2 {

// Routed through the env's DBInstanceCache so every instance shares its path manager: opening one file twice is
// detected across instances, while no instance is memoized.
CV2Instance::CV2Instance(CV2Environment &env, DBConfig &startup_config)
    : database(env.cache->CreateEmptyInstance(startup_config)), env(env), factory(*this), config_handle(*this),
      file_system(FileSystem::GetFileSystem(*database->instance), QueryContext()) {
}

DatabaseInstance &CV2InstanceFactory::GetDatabase() {
	return *instance.GetDatabase().instance;
}

void CV2InstanceFactory::WithTransaction(const std::function<void(ClientContext &)> &fn) {
	throw InvalidInputException("this requires a catalog: use the factory of a connection or a context");
}

//! Runs `action` inside a transaction on a short-lived connection, the way the ATTACH / DETACH operators run inside
//! their statement's transaction.
template <class T>
static void WithTransaction(DuckDB &database, T action) {
	auto connection = Connection::CreateInternal(*database.instance);
	connection.BeginTransaction();
	try {
		action(*connection.context);
	} catch (...) {
		connection.Rollback();
		throw;
	}
	connection.Commit();
}

void CV2Instance::Attach(const string &path, const Identifier &name,
                         optional_ptr<const CV2AttachOptions> attach_options, bool make_default) {
	if (attach_options && &attach_options->instance != this) {
		throw InvalidInputException("the attach options were created from a different instance handle");
	}
	WithTransaction(*database, [&](ClientContext &context) {
		// Mirrors PhysicalAttach for `ATTACH 'path' AS name (options)`.
		auto &instance = *database->instance;
		AttachInfo info;
		info.path = path;
		info.name = name;
		if (attach_options) {
			info.options = attach_options->options;
		}
		AttachOptions options(info.options, instance.config.options.access_mode);
		options.original_path = path;
		if (options.db_type.empty()) {
			DBPathAndType::ExtractExtensionPrefix(info.path, options.db_type);
		}
		if (info.name.empty()) {
			info.name = AttachedDatabase::ExtractDatabaseName(info.path, FileSystem::GetFileSystem(instance));
		}
		// The host opening a file is not external access: allow it the way the main database path is allowed.
		if (options.db_type.empty() && !DBConfig::IsInMemoryDatabase(info.path.c_str()) &&
		    !FileSystem::IsRemoteFile(info.path) && !Settings::Get<EnableExternalAccessSetting>(instance)) {
			instance.config.AddAllowedDatabasePath(info.path);
		}
		auto &db_manager = DatabaseManager::Get(instance);
		auto attached = db_manager.AttachDatabase(context, info, options);
		if (make_default) {
			db_manager.SetDefaultDatabase(attached->GetName());
		}
	});
}

//! Throws unless a user database is attached under `name`.
static void CheckAttached(DatabaseInstance &instance, const Identifier &name) {
	auto attached = DatabaseManager::Get(instance).GetDatabase(name);
	if (!attached || attached->IsSystem()) {
		throw InvalidInputException("no database is attached under the name '%s'", name.GetIdentifierName());
	}
}

void CV2Instance::Detach(const Identifier &name) {
	auto &instance = *database->instance;
	CheckAttached(instance, name);
	WithTransaction(*database, [&](ClientContext &context) {
		DatabaseManager::Get(instance).DetachDatabase(context, name, OnEntryNotFound::THROW_EXCEPTION, true);
	});
}

void CV2Instance::SetDefault(const Identifier &name) {
	auto &instance = *database->instance;
	CheckAttached(instance, name);
	DatabaseManager::Get(instance).SetDefaultDatabase(name);
}

} // namespace capiv2
} // namespace duckdb

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_instance_create(duckdb_v2_environment_handle env, duckdb_v2_instance_handle *out_instance,
                                          duckdb_v2_error_info_handle *err) {
	return duckdb_v2_instance_create_with_options(env, nullptr, nullptr, nullptr, nullptr, 0, out_instance, err);
}

DUCKDB_V2_ERROR duckdb_v2_instance_create_with_options(duckdb_v2_environment_handle env, const char **option_names,
                                                       const idx_t *option_names_lengths, const char **option_values,
                                                       const idx_t *option_values_lengths, idx_t option_count,
                                                       duckdb_v2_instance_handle *out_instance,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(env);
	DUCKDB_CHECK_ARG(out_instance);
	*out_instance = nullptr;
	if (option_count > 0 && !option_names) {
		return NullArgumentError(err, __func__, "option_names");
	}
	if (option_count > 0 && !option_values) {
		return NullArgumentError(err, __func__, "option_values");
	}
	return WithErrorHandler(err, [&]() {
		duckdb::DBConfig startup_config;
		string_view_array names(option_names, option_names_lengths, option_count);
		string_view_array values(option_values, option_values_lengths, option_count);
		for (idx_t i = 0; i < option_count; i++) {
			startup_config.SetOptionByName(duckdb::Identifier(duckdb::string(names[i])),
			                               duckdb::Value(duckdb::string(values[i])));
		}
		auto *env_wrapper = Convert(env);
		auto wrapper = duckdb::make_uniq<CV2Instance>(*env_wrapper, startup_config);
		env_wrapper->instance_count.fetch_add(1, std::memory_order_release);
		*out_instance = Convert(wrapper.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_destroy(duckdb_v2_instance_handle *instance) {
	return WithErrorHandler(nullptr, [&]() {
		if (!instance) {
			return;
		}
		if (*instance) {
			const auto *wrapper = Convert(*instance);
			auto &env = wrapper->env;
			delete wrapper;
			env.instance_count.fetch_sub(1, std::memory_order_release);
			*instance = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_attach(duckdb_v2_instance_handle instance, const char *name, idx_t name_length,
                                          const char *path, idx_t path_length, duckdb_v2_attach_options_handle options,
                                          bool make_default, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_STR_ARG(path, path_length);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(err, [&]() {
		auto &wrapper = *Convert(instance);
		duckdb::Identifier attach_name(ConvertIdentifierName(name, name_length));
		wrapper.Attach(duckdb::string(Convert(path, path_length)), attach_name, Convert(options), make_default);
	});
}

DUCKDB_V2_ERROR duckdb_v2_attach_options_create(duckdb_v2_instance_handle instance,
                                                duckdb_v2_attach_options_handle *out_options,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(out_options);
	*out_options = nullptr;
	return WithErrorHandler(err, [&]() {
		auto options = duckdb::make_uniq<CV2AttachOptions>(*Convert(instance));
		*out_options = Convert(options.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_attach_options_set(duckdb_v2_attach_options_handle options, const char *key, idx_t key_length,
                                             const char *setting, idx_t setting_length,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(options);
	DUCKDB_CHECK_STR_ARG(key, key_length);
	DUCKDB_CHECK_STR_ARG(setting, setting_length);
	return WithErrorHandler(err, [&]() {
		// Keys are unquoted SQL identifiers, which the parser lowercases.
		auto lowered = duckdb::StringUtil::Lower(duckdb::string(ConvertIdentifierName(key, key_length)));
		Convert(options)->options[lowered] = duckdb::Value(duckdb::string(Convert(setting, setting_length)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_attach_options_destroy(duckdb_v2_attach_options_handle *options) {
	return WithErrorHandler(nullptr, [&]() {
		if (!options) {
			return;
		}
		if (*options) {
			delete Convert(*options);
			*options = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_detach(duckdb_v2_instance_handle instance, const char *name, idx_t name_length,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(err, [&]() {
		auto &wrapper = *Convert(instance);
		wrapper.Detach(duckdb::Identifier(ConvertIdentifierName(name, name_length)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_set_default(duckdb_v2_instance_handle instance, const char *name, idx_t name_length,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(err, [&]() {
		auto &wrapper = *Convert(instance);
		wrapper.SetDefault(duckdb::Identifier(ConvertIdentifierName(name, name_length)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_library_version(const char **out_version, idx_t *out_version_length,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_version);
	DUCKDB_CHECK_ARG(out_version_length);
	return WithErrorHandler(err, [&]() {
		const auto version = duckdb::DuckDB::LibraryVersion();
		SetStringOut(out_version, out_version_length, std::string_view(version));
	});
}
