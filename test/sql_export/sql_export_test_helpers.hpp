#pragma once

#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

template <class STATEMENT>
unique_ptr<QueryResult> SubmitSQLExportResult(ClientContext &context, STATEMENT &&statement,
                                              const QueryParameters &parameters) {
	auto result = context.Submit(std::forward<STATEMENT>(statement), parameters);
	while (!result->HasError()) {
		auto state = result->ExecuteTask();
		if (IsObservable(state)) {
			break;
		}
		if (state == QueryResultState::BLOCKED) {
			result->WaitForTask();
		}
	}
	return result;
}

inline unique_ptr<QueryResult> MaterializeSQLExportStream(unique_ptr<QueryResult> result) {
	auto statement_type = result->GetStatementType();
	auto properties = result->GetStatementProperties();
	auto names = result->GetNames();
	auto client_properties = result->client_properties;
	QueryResultStream stream(std::move(result));
	auto collection = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), stream.GetTypes());
	ColumnDataAppendState append_state;
	collection->InitializeAppend(append_state);
	while (auto chunk = stream.Fetch()) {
		collection->Append(append_state, *chunk);
	}
	if (stream.HasError()) {
		return make_uniq<QueryResult>(stream.GetErrorObject());
	}
	return make_uniq<QueryResult>(statement_type, properties, names, std::move(collection), client_properties);
}

} // namespace duckdb
