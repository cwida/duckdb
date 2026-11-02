#include "duckdb/execution/operator/helper/physical_pragma.hpp"

namespace duckdb {

SourceResultType PhysicalPragma::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &client = context.client;
	FunctionParameters parameters {info->parameters, info->named_parameters, &info->function};
	info->function.function(client, parameters);

	return SourceResultType::FINISHED;
}

} // namespace duckdb
