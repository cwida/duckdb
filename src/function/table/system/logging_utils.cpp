#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog_entry/collate_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/logging.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

class EnableLoggingBindData : public TableFunctionData {
public:
	EnableLoggingBindData() {
	}

	case_insensitive_map_t<Value> storage_config;
	LogConfig config;
	vector<string> log_types_to_set;
};

//! File logging requires a path. Reject switching to file storage without one before mutating any state, so the
//! active storage is preserved instead of becoming a path-less storage that throws on every later flush (end-of-query
//! and shutdown included).
static void VerifyFileStoragePath(LogManager &log_manager, const string &storage,
                                  const case_insensitive_map_t<Value> &storage_config) {
	if (StringUtil::Lower(storage) != LogConfig::FILE_STORAGE_NAME) {
		return;
	}
	auto current_storage = StringUtil::Lower(log_manager.GetConfig().storage);
	// Already-active file storage keeps its existing path, only a fresh switch is guarded.
	if (current_storage == LogConfig::FILE_STORAGE_NAME) {
		return;
	}
	auto path_entry = storage_config.find("path");
	bool has_usable_path =
	    path_entry != storage_config.end() && !path_entry->second.IsNull() && !path_entry->second.ToString().empty();
	if (!has_usable_path) {
		throw InvalidInputException(
		    "Cannot enable 'file' log storage without a valid path. Provide one via storage_path, "
		    "e.g. CALL enable_logging(storage='file', storage_path='mylog.csv');");
	}
}

static void EnableLogging(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto bind_data = data.bind_data->Cast<EnableLoggingBindData>();

	DUCKDB_LOG_WARNING(context, "The logging settings have been changed so you may lose warnings printed in the CLI.\n"
	                            "To continue printing warnings to the console, set storage='shell_log_storage'.\n"
	                            "For more info see https://duckdb.org/docs/current/operations_manual/logging/overview.")

	auto &log_manager = context.db->GetLogManager();

	// a prepared call can run after the storage it was bound against changed
	VerifyFileStoragePath(log_manager, bind_data.config.storage, bind_data.storage_config);

	// Apply the config generated from the input - restoring the previous config if that fails part-way
	auto previous = log_manager.GetConfigAndStorage();
	try {
		log_manager.SetConfig(*context.db, bind_data.config);

		if (bind_data.log_types_to_set.empty()) {
			log_manager.SetEnableLogging(true);
			log_manager.SetLogMode(LogMode::LEVEL_ONLY);
		} else {
			log_manager.SetEnableStructuredLoggers(bind_data.log_types_to_set);
		}

		if (!bind_data.storage_config.empty()) {
			log_manager.UpdateLogStorageConfig(*context.db, bind_data.storage_config);
		}
	} catch (...) {
		// the previous storage itself, as a new one of its kind would lack its storage config
		log_manager.RestoreConfig(previous.first, std::move(previous.second));
		throw;
	}
}

//! Log types are registered by extensions when they are loaded, so an unknown type may just belong to an extension
//! that is not loaded yet
static void TryAutoloadLogTypeExtension(ClientContext &context, const string &log_type) {
	auto &db = *context.db;
	if (db.GetLogManager().LookupLogType(log_type)) {
		return;
	}
	ExtensionHelper::TryAutoloadFromEntry(db, Identifier(log_type), EXTENSION_LOG_TYPES);
}

static unique_ptr<FunctionData> BindEnableLogging(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() > 1) {
		throw InvalidInputException("EnableLogging: expected 0 or 1 parameter");
	}

	auto &config = DBConfig::GetConfig(context);
	config.CheckLock(EnableLogging::Name);
	config.CheckLock(EnabledLogTypes::Name);
	config.CheckLock(LoggingLevel::Name);
	config.CheckLock(LoggingMode::Name);
	config.CheckLock(LoggingStorage::Name);

	auto result = make_uniq<EnableLoggingBindData>();

	bool storage_isset = false;

	for (const auto &param : input.named_parameters) {
		auto &key = param.first;
		if (key == "level") {
			if (param.second.IsNull()) {
				throw InvalidInputException("EnableLogging: level cannot be NULL");
			}
			result->config.level = EnumUtil::FromString<LogLevel>(param.second.ToString());
		} else if (key == "storage") {
			storage_isset = true;
			result->config.storage = param.second.ToString();
		} else if (key == "storage_config") {
			if (param.second.type().id() != LogicalTypeId::STRUCT) {
				throw InvalidInputException("EnableLogging: storage_config must be a struct");
			}
			auto &children = StructValue::GetChildren(param.second);
			for (idx_t i = 0; i < children.size(); i++) {
				result->storage_config[StructType::GetChildName(param.second.type(), i).GetIdentifierName()] =
				    children[i];
			}
		} else if (key == "storage_path") {
			result->storage_config["path"] = param.second;
		} else if (key == "storage_normalize") {
			result->storage_config["normalize"] = param.second;
		} else if (key == "storage_buffer_size") {
			result->storage_config["buffer_size"] = param.second;
		} else {
			throw InvalidInputException("EnableLogging: unknown named parameter: %s", param.first.c_str());
		}
	}

	// If the user didn't explicitly set storage=, infer it. A 'path' in storage_config (provided
	// either via storage_path= or storage_config={path:...}) implies file storage. Otherwise
	// preserve the currently configured storage so logging_storage isn't silently reset.
	if (!storage_isset) {
		if (result->storage_config.find("path") != result->storage_config.end()) {
			result->config.storage = LogConfig::FILE_STORAGE_NAME;
		} else {
			result->config.storage = context.db->GetLogManager().GetConfig().storage;
		}
	}

	VerifyFileStoragePath(context.db->GetLogManager(), result->config.storage, result->storage_config);

	// Process positional params
	if (!input.inputs.empty()) {
		if (input.inputs[0].type() == LogicalType::VARCHAR) {
			result->log_types_to_set.push_back(input.inputs[0].GetValue<string>());
		} else if (input.inputs[0].type() == LogicalType::LIST(LogicalType::VARCHAR)) {
			for (const auto &child : ListValue::GetChildren(input.inputs[0])) {
				result->log_types_to_set.push_back(child.GetValue<string>());
			}
		} else {
			throw BinderException("Unexpected type positional parameter to enable_logging");
		}
	}

	for (const auto &log_type : result->log_types_to_set) {
		TryAutoloadLogTypeExtension(context, log_type);
		// verify the log types before changing any of the logging settings
		if (!context.db->GetLogManager().LookupLogType(log_type)) {
			throw InvalidInputException("Unknown log type: '%s'", log_type);
		}
	}

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return std::move(result);
}

//! Reset the logmanager to the defaults
static void DisableLogging(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	context.db->GetLogManager().SetEnableLogging(false);
}

//! Truncate the current log storage
static void TruncateLogs(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	context.db->GetLogManager().TruncateLogStorage();
}

static unique_ptr<FunctionData> BindDisableLogging(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	DBConfig::GetConfig(context).CheckLock(EnableLogging::Name);
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return std::move(make_uniq<EnableLoggingBindData>());
}

static unique_ptr<FunctionData> BindTruncateLogs(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return make_uniq<EnableLoggingBindData>();
}

void EnableLoggingFun::RegisterFunction(BuiltinFunctions &set) {
	auto enable_fun = TableFunction("enable_logging", {}, EnableLogging, BindEnableLogging, nullptr, nullptr);

	enable_fun.GetSignature()
	    .AddArgs("args", LogicalType::ANY)
	    .AddKeywordOnly("level", LogicalType::VARCHAR, Value("INFO"))
	    .WithTypedKwargs("options", [](TypedKwargs &options) {
		    options.Add("storage", LogicalType::VARCHAR)
		        .Add("storage_config", LogicalType::ANY)
		        // forwarded to the storage_config struct as syntactic sugar
		        .Add("storage_path", LogicalType::VARCHAR)
		        .Add("storage_normalize", LogicalType::BOOLEAN)
		        .Add("storage_buffer_size", LogicalType::UBIGINT);
	    });

	set.AddFunction(enable_fun);

	auto disable_fun = TableFunction("disable_logging", {}, DisableLogging, BindDisableLogging, nullptr, nullptr);
	set.AddFunction(disable_fun);

	auto truncate_fun = TableFunction("truncate_duckdb_logs", {}, TruncateLogs, BindTruncateLogs, nullptr, nullptr);
	set.AddFunction(truncate_fun);
}

} // namespace duckdb
