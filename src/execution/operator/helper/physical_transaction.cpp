#include "duckdb/execution/operator/helper/physical_transaction.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {

SourceResultType PhysicalTransaction::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                      OperatorSourceInput &input) const {
	context.client.RunTransactionStatementInternal(*info);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
