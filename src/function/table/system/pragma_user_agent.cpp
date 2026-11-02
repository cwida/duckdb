#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

struct PragmaUserAgentData : public GlobalTableFunctionState {
	PragmaUserAgentData() : finished(false) {
	}

	std::string user_agent;
	bool finished;
};

static unique_ptr<FunctionData> PragmaUserAgentBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("user_agent");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> PragmaUserAgentInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<PragmaUserAgentData>();
	auto &config = DBConfig::GetConfig(context);
	result->user_agent = config.UserAgent();

	return std::move(result);
}

void PragmaUserAgentFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<PragmaUserAgentData>();

	if (data.finished) {
		// signal end of output
		return;
	}

	output.data[0].Append(Value(data.user_agent));

	data.finished = true;
}

void PragmaUserAgent::RegisterFunction(BuiltinFunctions &set) {
	TableFunction pragma_user_agent("pragma_user_agent", {}, PragmaUserAgentFunction, PragmaUserAgentBind,
	                                PragmaUserAgentInit);
	set.AddFunction(pragma_user_agent);
}

} // namespace duckdb
