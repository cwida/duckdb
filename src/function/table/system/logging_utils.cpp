#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog_entry/collate_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/logging/logging.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

class EnableLoggingBindData : public TableFunctionData {
public:
	EnableLoggingBindData() {
	}

	case_insensitive_map_t<Value> sink_config;
	LogConfig config;
	vector<string> log_types_to_set;
	//! The deprecated parameters that were used, to warn about them
	vector<string> deprecated_parameters;
};

//! The sink parameter that replaces a deprecated storage parameter, e.g. storage_path -> sink_path
static string GetSinkParameterName(const string &storage_parameter) {
	return "sink" + storage_parameter.substr(strlen("storage"));
}

static void EnableLogging(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto bind_data = data.bind_data->Cast<EnableLoggingBindData>();

	for (auto &parameter : bind_data.deprecated_parameters) {
		DUCKDB_LOG_WARNING(*context.db, "the %s parameter of enable_logging is deprecated: use %s instead", parameter,
		                   GetSinkParameterName(parameter));
	}

	DUCKDB_LOG_WARNING(context, "The logging settings have been changed so you may lose warnings printed in the CLI.\n"
	                            "To continue printing warnings to the console, set sink='shell_log_sink'.\n"
	                            "For more info see https://duckdb.org/docs/current/operations_manual/logging/overview.")

	auto &log_manager = context.db->GetLogManager();

	// Apply the config generated from the input - restoring the previous config if that fails part-way
	auto previous_config = log_manager.GetConfig();
	try {
		log_manager.SetConfig(*context.db, bind_data.config);

		if (bind_data.log_types_to_set.empty()) {
			log_manager.SetEnableLogging(true);
			log_manager.SetLogMode(LogMode::LEVEL_ONLY);
		} else {
			log_manager.SetEnableStructuredLoggers(bind_data.log_types_to_set);
		}

		if (!bind_data.sink_config.empty()) {
			log_manager.UpdateMainLogSinkConfig(*context.db, bind_data.sink_config);
		}
	} catch (...) {
		log_manager.SetConfig(*context.db, previous_config);
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

	auto result = make_uniq<EnableLoggingBindData>();

	bool sink_isset = false;
	case_insensitive_set_t given_parameters;

	for (const auto &param : input.named_parameters) {
		auto &name = param.first.GetIdentifierName();
		auto key = name;
		if (StringUtil::StartsWith(key, "storage")) {
			result->deprecated_parameters.push_back(name);
			key = GetSinkParameterName(key);
		}
		if (!given_parameters.insert(key).second) {
			throw InvalidInputException("EnableLogging: %s cannot be combined with its deprecated name storage%s", key,
			                            key.substr(strlen("sink")));
		}
		if (key == "level") {
			if (param.second.IsNull()) {
				throw InvalidInputException("EnableLogging: level cannot be NULL");
			}
			result->config.level = EnumUtil::FromString<LogLevel>(param.second.ToString());
		} else if (key == "sink") {
			sink_isset = true;
			result->config.storage = param.second.ToString();
		} else if (key == "sink_config") {
			if (param.second.type().id() != LogicalTypeId::STRUCT) {
				throw InvalidInputException("EnableLogging: %s must be a struct", name);
			}
			auto &children = StructValue::GetChildren(param.second);
			for (idx_t i = 0; i < children.size(); i++) {
				result->sink_config[StructType::GetChildName(param.second.type(), i).GetIdentifierName()] = children[i];
			}
		} else if (key == "sink_path") {
			result->sink_config["path"] = param.second;
		} else if (key == "sink_normalize") {
			result->sink_config["normalize"] = param.second;
		} else if (key == "sink_buffer_size") {
			result->sink_config["buffer_size"] = param.second;
		} else {
			throw InvalidInputException("EnableLogging: unknown named parameter: %s", param.first.c_str());
		}
	}

	// If the user didn't explicitly set sink=, infer it. A 'path' in sink_config (provided
	// either via sink_path= or sink_config={path:...}) implies the file sink. Otherwise
	// preserve the currently configured sink so logging_sink isn't silently reset.
	if (!sink_isset) {
		if (result->sink_config.find("path") != result->sink_config.end()) {
			result->config.storage = LogConfig::FILE_STORAGE_NAME;
		} else {
			result->config.storage = context.db->GetLogManager().GetConfig().storage;
		}
	}

	// File logging requires a path. Reject switching to the file sink without one before mutating any
	// state, so the active sink is preserved instead of becoming a path-less sink that throws
	// on every later flush (end-of-query and shutdown included).
	if (StringUtil::Lower(result->config.storage) == LogConfig::FILE_STORAGE_NAME) {
		// An enabled file sink keeps its existing path; only guard a fresh switch.
		if (!context.db->GetLogManager().IsLogSinkEnabled(LogConfig::FILE_STORAGE_NAME)) {
			auto path_entry = result->sink_config.find("path");
			bool has_usable_path = path_entry != result->sink_config.end() && !path_entry->second.IsNull() &&
			                       !path_entry->second.ToString().empty();
			if (!has_usable_path) {
				throw InvalidInputException(
				    "Cannot enable the 'file' log sink without a valid path. Provide one via sink_path, "
				    "e.g. CALL enable_logging(sink='file', sink_path='mylog.csv');");
			}
		}
	}

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

//! Truncate the main log sink
static void TruncateLogs(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	context.db->GetLogManager().TruncateMainLogSink();
}

static unique_ptr<FunctionData> BindDisableLogging(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
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

class LogSinkNameBindData : public TableFunctionData {
public:
	explicit LogSinkNameBindData(string name_p) : name(std::move(name_p)) {
	}

	string name;
	case_insensitive_map_t<Value> sink_config;
};

static unique_ptr<FunctionData> BindLogSinkName(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("The log sink name cannot be NULL");
	}
	auto name = input.inputs[0].GetValue<string>();
	if (name.empty()) {
		throw InvalidInputException("The log sink name cannot be empty");
	}

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return make_uniq<LogSinkNameBindData>(std::move(name));
}

static unique_ptr<FunctionData> BindAddLogSink(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = BindLogSinkName(context, input, return_types, names);
	auto &bind_data = result->Cast<LogSinkNameBindData>();
	for (const auto &param : input.named_parameters) {
		auto &name = param.first.GetIdentifierName();
		if (name == "config") {
			if (param.second.type().id() != LogicalTypeId::STRUCT) {
				throw InvalidInputException("add_log_sink: config must be a struct");
			}
			auto &children = StructValue::GetChildren(param.second);
			for (idx_t i = 0; i < children.size(); i++) {
				bind_data.sink_config[StructType::GetChildName(param.second.type(), i).GetIdentifierName()] =
				    children[i];
			}
		} else if (name == "path" || name == "normalize" || name == "buffer_size") {
			bind_data.sink_config[name] = param.second;
		} else {
			throw InvalidInputException("add_log_sink: unknown named parameter: %s", name);
		}
	}
	return result;
}

//! Also send the log entries to this sink
static void AddLogSink(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<LogSinkNameBindData>();
	context.db->GetLogManager().AddLogSink(*context.db, bind_data.name, bind_data.sink_config);
}

static void RemoveLogSink(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<LogSinkNameBindData>();
	context.db->GetLogManager().RemoveLogSink(*context.db, bind_data.name);
}

void EnableLoggingFun::RegisterFunction(BuiltinFunctions &set) {
	auto enable_fun = TableFunction("enable_logging", {}, EnableLogging, BindEnableLogging, nullptr, nullptr);

	enable_fun.GetSignature()
	    .AddArgs("args", LogicalType::ANY)
	    .AddKeywordOnly("level", LogicalType::VARCHAR, Value("INFO"))
	    .WithTypedKwargs("options", [](TypedKwargs &options) {
		    options.Add("sink", LogicalType::VARCHAR)
		        .Add("sink_config", LogicalType::ANY)
		        // forwarded to the sink_config struct as syntactic sugar
		        .Add("sink_path", LogicalType::VARCHAR)
		        .Add("sink_normalize", LogicalType::BOOLEAN)
		        .Add("sink_buffer_size", LogicalType::UBIGINT)
		        // deprecated names of the sink parameters
		        .Add("storage", LogicalType::VARCHAR)
		        .Add("storage_config", LogicalType::ANY)
		        .Add("storage_path", LogicalType::VARCHAR)
		        .Add("storage_normalize", LogicalType::BOOLEAN)
		        .Add("storage_buffer_size", LogicalType::UBIGINT);
	    });

	set.AddFunction(enable_fun);

	auto disable_fun = TableFunction("disable_logging", {}, DisableLogging, BindDisableLogging, nullptr, nullptr);
	set.AddFunction(disable_fun);

	auto truncate_fun = TableFunction("truncate_duckdb_logs", {}, TruncateLogs, BindTruncateLogs, nullptr, nullptr);
	set.AddFunction(truncate_fun);

	TableFunction add_sink_fun("add_log_sink", FunctionSignature().AddPositionalOnly("sink", LogicalType::VARCHAR),
	                           AddLogSink, BindAddLogSink);
	add_sink_fun.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		// path, normalize and buffer_size are forwarded to the config struct as syntactic sugar
		options.Add("config", LogicalType::ANY)
		    .Add("path", LogicalType::VARCHAR)
		    .Add("normalize", LogicalType::BOOLEAN)
		    .Add("buffer_size", LogicalType::UBIGINT);
	});
	set.AddFunction(add_sink_fun);

	auto remove_sink_fun =
	    TableFunction("remove_log_sink", {LogicalType::VARCHAR}, RemoveLogSink, BindLogSinkName, nullptr, nullptr);
	set.AddFunction(remove_sink_fun);
}

} // namespace duckdb
