#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/profiler/gathered_metrics.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

class EnableProfilingBindData : public TableFunctionData {
public:
	EnableProfilingBindData() {
	}

	Value format;
	Value coverage;
	Value save_location;
	Value mode;
	Value metrics;
};

static void EnableProfiling(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto bind_data = data.bind_data->Cast<EnableProfilingBindData>();

	auto &client_config = ClientConfig::GetConfig(context);
	client_config.enable_profiler = true;

	if (!bind_data.format.IsNull() && !bind_data.save_location.IsNull()) {
		auto &file_system = FileSystem::GetFileSystem(context);
		const auto file_type = file_system.ExtractExtension(bind_data.save_location.ToString());
		if (file_type != "txt" && file_type != bind_data.format.ToString()) {
			throw InvalidInputException(
			    "EnableProfiling: the save_location must be a .txt file or match the specified format.");
		}

		EnableProfilingSetting::ResetLocal(context);
		ProfilingOutputSetting::ResetLocal(context);
	}

	if (!bind_data.format.IsNull()) {
		EnableProfilingSetting::SetLocal(context, bind_data.format);
	}

	if (!bind_data.coverage.IsNull()) {
		ProfilingCoverageSetting::SetLocal(context, bind_data.coverage);
	}

	if (!bind_data.save_location.IsNull()) {
		ProfilingOutputSetting::SetLocal(context, bind_data.save_location);
	}

	if (!bind_data.mode.IsNull()) {
		ProfilingModeSetting::SetLocal(context, bind_data.mode);
	}

	if (!bind_data.metrics.IsNull()) {
		Value metrics_value = bind_data.metrics;
		if (metrics_value.type().id() == LogicalTypeId::VARCHAR) {
			metrics_value = Value::LIST(LogicalType::VARCHAR, {metrics_value});
		}
		TrackedMetricsSetting::SetLocal(context, metrics_value);
	}
}

static unique_ptr<FunctionData> BindEnableProfiling(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<EnableProfilingBindData>();

	for (const auto &named_param : input.named_parameters) {
		const auto key = EnumUtil::FromString<ProfilingParameterNames>(named_param.first.GetIdentifierName());
		switch (key) {
		case ProfilingParameterNames::FORMAT:
			bind_data->format = StringUtil::Lower(named_param.second.ToString());
			break;
		case ProfilingParameterNames::COVERAGE:
			bind_data->coverage = StringUtil::Lower(named_param.second.ToString());
			break;
		case ProfilingParameterNames::SAVE_LOCATION:
			bind_data->save_location = named_param.second.ToString();
			break;
		case ProfilingParameterNames::MODE:
			bind_data->mode = StringUtil::Lower(named_param.second.ToString());
			break;
		case ProfilingParameterNames::METRICS:
			throw InternalException("enable_profiling: metrics is placed by position");
		}
	}

	// The metrics are placed by position however they were passed; the overload settled pattern or list of names
	bind_data->metrics = input.inputs[0];

	auto &config = DBConfig::GetConfig(context);
	config.CheckLock(EnableProfilingSetting::Name);
	if (!bind_data->coverage.IsNull()) {
		config.CheckLock(ProfilingCoverageSetting::Name);
	}
	if (!bind_data->save_location.IsNull()) {
		config.CheckLock(ProfilingOutputSetting::Name);
	}
	if (!bind_data->mode.IsNull()) {
		config.CheckLock(ProfilingModeSetting::Name);
	}
	if (!bind_data->metrics.IsNull()) {
		config.CheckLock(TrackedMetricsSetting::Name);
	}

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return std::move(bind_data);
}

static void DisableProfiling(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &client_config = ClientConfig::GetConfig(context);
	client_config.enable_profiler = false;
}

static unique_ptr<FunctionData> BindDisableProfiling(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	DBConfig::GetConfig(context).CheckLock(EnableProfilingSetting::Name);
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	return nullptr;
}

void EnableProfilingFun::RegisterFunction(BuiltinFunctions &set) {
	// The metrics are either a single pattern or a list of names
	TableFunctionSet enable_set("enable_profiling");

	enable_set.AddFunction(TableFunction(FunctionSignature()
	                                         .AddParameter("metrics", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))
	                                         .AddTypedKwargs("options", TypedKwargs()
	                                                                        .Add("format", LogicalType::VARCHAR)
	                                                                        .Add("coverage", LogicalType::VARCHAR)
	                                                                        .Add("save_location", LogicalType::VARCHAR)
	                                                                        .Add("mode", LogicalType::VARCHAR)),
	                                     EnableProfiling, BindEnableProfiling));

	enable_set.AddFunction(TableFunction(FunctionSignature()
	                                         .AddParameter("metrics", LogicalType::LIST(LogicalType::VARCHAR))
	                                         .AddTypedKwargs("options", TypedKwargs()
	                                                                        .Add("format", LogicalType::VARCHAR)
	                                                                        .Add("coverage", LogicalType::VARCHAR)
	                                                                        .Add("save_location", LogicalType::VARCHAR)
	                                                                        .Add("mode", LogicalType::VARCHAR)),
	                                     EnableProfiling, BindEnableProfiling));

	set.AddFunction(std::move(enable_set));

	auto disable_fun = TableFunction("disable_profiling", {}, DisableProfiling, BindDisableProfiling, nullptr, nullptr);
	set.AddFunction(std::move(disable_fun));
}

} // namespace duckdb
