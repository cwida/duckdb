#include "duckdb/main/stream_query_result.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

StreamQueryResult::StreamQueryResult(StatementType statement_type, shared_ptr<ClientContext> context,
                                     vector<LogicalType> types, vector<string> names,
                                     shared_ptr<PreparedStatementData> prepared)
    : QueryResult(QueryResultType::STREAM_RESULT, statement_type, move(types), move(names)), is_open(true),
      context(move(context)), prepared(move(prepared)) {
}

StreamQueryResult::~StreamQueryResult() {
	Close();
}

string StreamQueryResult::ToString() {
	string result;
	if (success) {
		result = HeaderToString();
		result += "[[STREAM RESULT]]";
	} else {
		result = error + "\n";
	}
	return result;
}

unique_ptr<DataChunk> StreamQueryResult::FetchRaw() {
	if (!success || !is_open) {
		throw InvalidInputException(
		    "Attempting to fetch from an unsuccessful or closed streaming query result\nError: %s", error);
	}
	auto chunk = context->Fetch();
	if (!chunk || chunk->ColumnCount() == 0 || chunk->size() == 0) {
		Close();
		return nullptr;
	}
	return chunk;
}

unique_ptr<MaterializedQueryResult> StreamQueryResult::Materialize() {
	if (!success) {
		return make_unique<MaterializedQueryResult>(error);
	}
	auto result = make_unique<MaterializedQueryResult>(statement_type, types, names);
	while (true) {
		auto chunk = Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		result->collection.Append(*chunk);
	}
	if (!success) {
		return make_unique<MaterializedQueryResult>(error);
	}
	return result;
}

void StreamQueryResult::Close() {
	if (!is_open) {
		return;
	}
	is_open = false;
	context->Cleanup();
}

} // namespace duckdb
