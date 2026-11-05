#include "duckdb/main/query_result.hpp"

#include "duckdb/common/box_renderer.hpp"
#include "duckdb/common/column_data_collection_render_interface.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/retained_result_collection.hpp"

namespace duckdb {

BaseQueryResult::BaseQueryResult(StatementType statement_type, StatementProperties properties_p,
                                 vector<LogicalType> types_p, vector<Identifier> names_p)
    : statement_type(statement_type), properties(std::move(properties_p)), types(std::move(types_p)),
      names(std::move(names_p)), success(true) {
	D_ASSERT(types.size() == names.size());
}

BaseQueryResult::BaseQueryResult(ErrorData error) : success(false), error(std::move(error)) {
	// Assert that the error object is initialized
	D_ASSERT(this->error.HasError());
}

BaseQueryResult::BaseQueryResult(StatementType statement_type)
    : statement_type(statement_type), metadata_available(false), success(true) {
}

void BaseQueryResult::SetMetadata(StatementProperties properties_p, vector<LogicalType> types_p,
                                  vector<Identifier> names_p) {
	D_ASSERT(!metadata_available);
	D_ASSERT(types_p.size() == names_p.size());
	properties = std::move(properties_p);
	types = std::move(types_p);
	names = std::move(names_p);
	metadata_available = true;
}

void BaseQueryResult::RequireMetadata() const {
	// A result that failed reports the empty metadata of every failed result
	if (!MetadataAvailable()) {
		throw InvalidInputException("The metadata of this result is not available yet: the statement expands into "
		                            "several, and the one that produces the result has not been bound");
	}
}

bool BaseQueryResult::MetadataAvailable() const {
	return metadata_available || !success;
}

BaseQueryResult::~BaseQueryResult() {
}

void BaseQueryResult::ThrowError(const string &prepended_message) const {
	D_ASSERT(HasError());
	error.Throw(prepended_message);
}

void BaseQueryResult::ThrowIfError(const string &prepended_message) const {
	if (HasError()) {
		error.Throw(prepended_message);
	}
}

void BaseQueryResult::SetError(ErrorData error) {
	success = !error.HasError();
	this->error = std::move(error);
}

bool BaseQueryResult::HasError() const {
	D_ASSERT(error.HasError() == !success);
	return !success;
}

const ExceptionType &BaseQueryResult::GetErrorType() const {
	return error.Type();
}

const std::string &BaseQueryResult::GetError() const {
	D_ASSERT(HasError());
	return error.Message();
}

ErrorData &BaseQueryResult::GetErrorObject() {
	return error;
}

const ErrorData &BaseQueryResult::GetErrorObject() const {
	return error;
}

idx_t BaseQueryResult::ColumnCount() const {
	RequireMetadata();
	return types.size();
}

StatementType BaseQueryResult::GetStatementType() const {
	return statement_type;
}

const StatementProperties &BaseQueryResult::GetStatementProperties() const {
	RequireMetadata();
	return properties;
}

const vector<LogicalType> &BaseQueryResult::GetTypes() const {
	RequireMetadata();
	return types;
}

const vector<Identifier> &BaseQueryResult::GetNames() const {
	RequireMetadata();
	return names;
}

//===--------------------------------------------------------------------===//
// Construction
//===--------------------------------------------------------------------===//
QueryResult::QueryResult(shared_ptr<ClientContext> context_p, PreparedStatementData &statement,
                         vector<LogicalType> types_p, ClientProperties client_properties_p,
                         shared_ptr<BufferedData> buffer_p)
    : BaseQueryResult(statement.statement_type, statement.properties, std::move(types_p), statement.names),
      client_properties(std::move(client_properties_p)), context(std::move(context_p)), buffer(std::move(buffer_p)) {
	if (buffer) {
		format = buffer->SharedFormat();
		format_state = buffer->SharedFormatState();
	} else {
		format = ChunkFormat::InMemory();
		InitializeChunkFormatState();
	}
}

QueryResult::QueryResult(shared_ptr<ClientContext> context_p, StatementType statement_type,
                         shared_ptr<ResultFormat> format_p, ClientProperties client_properties_p)
    : BaseQueryResult(statement_type), client_properties(std::move(client_properties_p)), context(std::move(context_p)),
      format(std::move(format_p)) {
	if (!format) {
		format = ChunkFormat::InMemory();
	}
	// The state of a result without columns, which a failure leaves in place; the statement producing the result
	// replaces it
	ResultFormatContext format_context {vector<LogicalType>(), vector<Identifier>(), client_properties,
	                                    ResultOrdering::UNORDERED};
	format_state = format->InitGlobal(format_context);
}

void QueryResult::AdoptResultStatement(StatementProperties properties, PreparedStatementData &statement,
                                       shared_ptr<BufferedData> buffer_p) {
	D_ASSERT(!buffer && buffer_p);
	SetMetadata(std::move(properties), statement.types, statement.names);
	buffer = std::move(buffer_p);
	format = buffer->SharedFormat();
	format_state = buffer->SharedFormatState();
}

void QueryResult::AdoptRowlessResult(QueryResult &produced) {
	SetMetadata(produced.GetStatementProperties(), produced.GetTypes(), produced.GetNames());
	format_state = produced.format_state;
	AdoptCollected(produced);
}

QueryResult::QueryResult(StatementType statement_type, StatementProperties properties, vector<Identifier> names_p,
                         unique_ptr<ColumnDataCollection> collection_p, ClientProperties client_properties_p)
    : BaseQueryResult(statement_type, std::move(properties), collection_p->Types(), std::move(names_p)),
      client_properties(std::move(client_properties_p)), format(ChunkFormat::InMemory()),
      collection(make_uniq<ChunkRetainedCollection>(std::move(collection_p))) {
	InitializeChunkFormatState();
}

QueryResult::QueryResult(StatementType statement_type, StatementProperties properties, vector<LogicalType> types_p,
                         vector<Identifier> names_p, unique_ptr<RetainedResultCollection> collection_p,
                         shared_ptr<ResultFormat> format_p, shared_ptr<ResultFormatGlobalState> format_state_p,
                         ClientProperties client_properties_p)
    : BaseQueryResult(statement_type, std::move(properties), std::move(types_p), std::move(names_p)),
      client_properties(std::move(client_properties_p)), format(std::move(format_p)),
      format_state(std::move(format_state_p)), collection(std::move(collection_p)) {
	D_ASSERT(format && format_state && collection);
}

QueryResult::QueryResult(ErrorData error)
    : BaseQueryResult(std::move(error)),
      client_properties("UTC", ArrowOffsetSize::REGULAR, false, false, false, ArrowFormatVersion::V1_0, nullptr),
      format(ChunkFormat::InMemory()) {
	InitializeChunkFormatState();
}

QueryResult::~QueryResult() {
	Close();
}

void QueryResult::DeduplicateColumns(vector<string> &names) {
	auto identifiers = StringsToIdentifiers(names);
	DeduplicateColumns(identifiers);
	names = IdentifiersToStrings(identifiers);
}

void QueryResult::DeduplicateColumns(vector<Identifier> &names) {
	identifier_map_t<idx_t> name_map;
	for (auto &column_name : names) {
		if (name_map.find(column_name) == name_map.end()) {
			// Name does not exist yet
			name_map[column_name]++;
		} else {
			// Name already exists, we add _x where x is the repetition number
			Identifier new_column_name(column_name + "_" + std::to_string(name_map[column_name]));
			while (name_map.find(new_column_name) != name_map.end()) {
				// This name is already here due to a previous definition
				name_map[column_name]++;
				new_column_name = Identifier(column_name + "_" + std::to_string(name_map[column_name]));
			}
			column_name = new_column_name;
			name_map[new_column_name]++;
		}
	}
}

const Identifier &QueryResult::ColumnName(idx_t index) const {
	auto &names = GetNames();
	D_ASSERT(index < names.size());
	return names[index];
}

//===--------------------------------------------------------------------===//
// Execution
//===--------------------------------------------------------------------===//
unique_ptr<ClientContextLock> QueryResult::LockContext() {
	if (!context) {
		string error_str = "Attempting to execute an unsuccessful or closed query result";
		if (HasError()) {
			error_str += StringUtil::Format("\nError: %s", GetError());
		}
		throw InvalidInputException(error_str);
	}
	return context->LockContext();
}

bool QueryResult::IsOpenInternal(ClientContextLock &lock) {
	if (HasError() || !context) {
		return false;
	}
	return context->IsActiveResult(lock, *this);
}

void QueryResult::CheckExecutableInternal(ClientContextLock &lock) {
	if (!IsOpenInternal(lock)) {
		string error_str = "Attempting to execute an unsuccessful or closed query result";
		if (HasError()) {
			error_str += StringUtil::Format("\nError: %s", GetError());
		}
		throw InvalidInputException(error_str);
	}
}

bool QueryResult::IsOpen() {
	if (HasError() || !context) {
		return false;
	}
	auto lock = LockContext();
	return IsOpenInternal(*lock);
}

QueryResultState QueryResult::Cancelled() {
	if (!HasError()) {
		SetError(
		    ErrorData(ExceptionType::INTERRUPT, "The execution of the query was cancelled before it could finish"));
	}
	return QueryResultState::EXECUTION_ERROR;
}

QueryResultState QueryResult::Poll() {
	if (HasError()) {
		return QueryResultState::EXECUTION_ERROR;
	}
	if (IsCollected() || !context) {
		// The result was collected, or the query already ended: keep reporting the terminal state
		return QueryResultState::FINISHED;
	}
	auto lock = LockContext();
	if (!IsOpenInternal(*lock)) {
		return Cancelled();
	}
	return context->PollInternal(*lock, *this);
}

QueryResultState QueryResult::ExecuteTask() {
	if (IsCollected()) {
		// An earlier call ended the query: keep reporting the terminal state
		return QueryResultState::FINISHED;
	}
	// Ending the query drops the handle's reference to the context, which the lock below outlives
	auto keep_alive = context;
	auto lock = LockContext();
	CheckExecutableInternal(*lock);
	auto state = context->ExecuteTaskInternal(*lock, *this);
	// Only a statement that sank no row finishes undecided, since producers park until the decision. A statement
	// that expands into several without returning rows never has a buffer
	if (state == QueryResultState::FINISHED && (!buffer || buffer->Lifetime() != ResultLifetime::DRAINING)) {
		return EndFinishedInternal(*lock);
	}
	return state;
}

void QueryResult::WaitForTask() {
	if (!context) {
		return;
	}
	auto lock = LockContext();
	if (!IsOpenInternal(*lock)) {
		return;
	}
	context->WaitForTask(*lock, *this);
}

void QueryResult::Close() {
	if (buffer) {
		buffer->Close();
	}
	if (context) {
		auto lock = context->LockContext();
		if (context->IsActiveResult(*lock, *this)) {
			// No call ended the query, so it was abandoned and nothing it ran may be committed
			context->AbortInternal(*lock);
		}
	}
	context.reset();
}

//===--------------------------------------------------------------------===//
// Format
//===--------------------------------------------------------------------===//
void QueryResult::InitializeChunkFormatState() {
	ResultFormatContext format_context {GetTypes(), GetNames(), client_properties, ResultOrdering::UNORDERED};
	format_state = format->InitGlobal(format_context);
}

const ResultFormat &QueryResult::Format() const {
	D_ASSERT(format);
	return *format;
}

void QueryResult::AdoptCollected(QueryResult &produced) {
	collection = std::move(produced.collection);
}

void QueryResult::ThrowFormatMismatch(const char *expected) const {
	throw InvalidInputException("This query result is in the \"%s\" format, but it was asked for the \"%s\" format",
	                            Format().Name(), expected);
}

const ResultFormatGlobalState &QueryResult::CheckedFormatState(const char *expected) const {
	if (!StringUtil::Equals(Format().Name(), expected)) {
		ThrowFormatMismatch(expected);
	}
	RequireMetadata();
	D_ASSERT(format_state);
	return *format_state;
}

void QueryResult::PrepareCollected(const char *expected) {
	Complete();
	if (HasError()) {
		throw InvalidInputException("Attempting to get collection from an unsuccessful query result\n: Error %s",
		                            GetError());
	}
	if (!StringUtil::Equals(Format().Name(), expected)) {
		ThrowFormatMismatch(expected);
	}
	if (!IsCollected()) {
		ThrowNoCollection();
	}
}

//===--------------------------------------------------------------------===//
// Retention
//===--------------------------------------------------------------------===//
void QueryResult::Materialize() {
	if (IsCollected() || HasError() || !context) {
		return;
	}
	auto lock = LockContext();
	if (!IsOpenInternal(*lock)) {
		Cancelled();
		context.reset();
		return;
	}
	SettleRetainedInternal(*lock);
}

void QueryResult::SettleRetainedInternal(ClientContextLock &lock) {
	if (buffer) {
		buffer->Decide(ResultLifetime::RETAINED);
		return;
	}
	// The statement that produces the result has not started: its buffer takes the decision when it is created
	context->DecideResultLifetimeInternal(lock, ResultLifetime::RETAINED);
}

void QueryResult::Complete() {
	if (IsCollected() || HasError() || !context) {
		return;
	}
	// The handle may hold the last reference to the context, which the lock below outlives
	auto keep_alive = context;
	auto lock = keep_alive->LockContext();
	CompleteInternal(*lock);
}

void QueryResult::CompleteInternal(ClientContextLock &lock) {
	if (IsCollected() || HasError() || !context) {
		return;
	}
	if (!IsOpenInternal(lock)) {
		Cancelled();
		context.reset();
		return;
	}
	SettleRetainedInternal(lock);
	try {
		QueryResultState state;
		while (!IsTerminal(state = context->ExecuteTaskInternal(lock, *this))) {
			// NO_TASKS_AVAILABLE: the remaining tasks run on other threads. Wait (bounded, woken when a task
			// completes) as RunToResultStatementInternal does, instead of spinning on the executor lock
			if (state == QueryResultState::BLOCKED || state == QueryResultState::READY ||
			    state == QueryResultState::NO_TASKS_AVAILABLE) {
				context->WaitForTask(lock, *this);
			}
		}
		if (state == QueryResultState::FINISHED) {
			EndFinishedInternal(lock);
		}
	} catch (std::exception &ex) {
		AbandonAfterThrowInternal(lock, ErrorData(ex));
		throw;
	} catch (...) { // LCOV_EXCL_START
		AbandonAfterThrowInternal(lock, ErrorData("Unhandled exception while completing the query"));
		throw;
	} // LCOV_EXCL_STOP
	context.reset();
}

void QueryResult::AbandonAfterThrowInternal(ClientContextLock &lock, ErrorData error) {
	// the caller holds the context lock - clean up with it here, as closing this result would lock it again
	try {
		if (context->IsActiveResult(lock, *this)) {
			if (!HasError()) {
				// Ended as failed, so nothing the query ran is committed
				SetError(std::move(error));
			}
			context->CleanupInternal(lock, this, true);
		}
	} catch (...) { // NOLINT
	}
	context.reset();
}

QueryResultState QueryResult::EndFinishedInternal(ClientContextLock &lock) {
	auto produced = context->TakeFinishedResultInternal(lock);
	// Cleanup can fail on an autocommit commit; it records the error on this result
	context->CleanupInternal(lock, this, false);
	if (!HasError()) {
		if (MetadataAvailable()) {
			AdoptCollected(*produced);
		} else {
			AdoptRowlessResult(*produced);
		}
	}
	context.reset();
	return HasError() ? QueryResultState::EXECUTION_ERROR : QueryResultState::FINISHED;
}

void QueryResult::ThrowNoCollection() const {
	throw InvalidInputException("This query result no longer holds a collection: it was taken with TakeCollection, or "
	                            "the result was closed before it was collected");
}

idx_t QueryResult::RowCount() {
	Complete();
	if (HasError()) {
		return 0;
	}
	if (!IsCollected()) {
		ThrowNoCollection();
	}
	return collection->Count();
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void QueryResult::EndQuery(ClientContextLock &lock, bool invalidate_transaction) {
	context->CleanupInternal(lock, this, invalidate_transaction);
}

void QueryResult::HandleFetchFailure(ClientContextLock &lock, ErrorData error) {
	if (!context || !context->IsActiveResult(lock, *this)) {
		// The query already ended, so the failure is only recorded, unless an earlier one is
		if (!HasError()) {
			SetError(std::move(error));
		}
		return;
	}
	bool invalidate_query = true;
	if (!context->ErrorInvalidatesQueryTransaction(error.Type())) {
		// standard exceptions do not invalidate the current transaction
		invalidate_query = false;
	} else if (Exception::InvalidatesDatabase(error.Type())) {
		// fatal exceptions invalidate the entire database
		auto &db_instance = DatabaseInstance::GetDatabase(*context);
		ValidChecker::Invalidate(db_instance, error.RawMessage());
	}
	context->ProcessError(error, context->GetCurrentQuery());
	SetError(std::move(error));
	context->CleanupInternal(lock, this, invalidate_query);
}

unique_ptr<DataChunk> QueryResult::FetchRaw() {
	Complete();
	if (HasError()) {
		throw InvalidInputException("Attempting to fetch from an unsuccessful query result\nError: %s", GetError());
	}
	if (!Format().Is<ChunkFormat>()) {
		ThrowFormatMismatch(ChunkFormat::NAME);
	}
	if (!collection) {
		ThrowNoCollection();
	}
	return collection->Cast<ChunkRetainedCollection>().FetchRaw();
}

//===--------------------------------------------------------------------===//
// Rendering
//===--------------------------------------------------------------------===//
string QueryResult::ToString() {
	// The metadata of a statement that expands into several is known once it runs
	Complete();
	if (HasError()) {
		return GetError() + "\n";
	}
	string result = HeaderToString();
	if (!Format().Is<ChunkFormat>()) {
		return result + "[ Rows: " + to_string(RowCount()) + "]\n\n";
	}
	auto &coll = Collection();
	result += "[ Rows: " + to_string(coll.Count()) + "]\n";
	for (auto &row : coll.Rows()) {
		for (idx_t col_idx = 0; col_idx < coll.ColumnCount(); col_idx++) {
			if (col_idx > 0) {
				result += "\t";
			}
			auto val = row.GetValue(col_idx);
			result += val.IsNull() ? "NULL" : StringUtil::Replace(val.ToString(), string("\0", 1), "\\0");
		}
		result += "\n";
	}
	result += "\n";
	return result;
}

string QueryResult::ToBox(BoxRendererContext &context_p, const BoxRendererConfig &config) {
	Complete();
	if (HasError()) {
		return GetError() + "\n";
	}
	if (!Format().Is<ChunkFormat>()) {
		return HeaderToString() + "[ Rows: " + to_string(RowCount()) + "]\n\n";
	}
	BoxRenderer renderer(config);
	ColumnDataCollectionWrapper wrapper(Collection());
	return renderer.ToString(context_p, IdentifiersToStrings(GetNames()), wrapper);
}

bool QueryResult::Equals(QueryResult &other, bool compare_names) { // LCOV_EXCL_START
	Complete();
	other.Complete();
	// first compare the success state of the results
	if (HasError() != other.HasError()) {
		return false;
	}
	if (HasError()) {
		return GetErrorObject() == other.GetErrorObject();
	}
	if (!Format().Is<ChunkFormat>() || !other.Format().Is<ChunkFormat>()) {
		throw InvalidInputException("Query results can only be compared in the chunk format");
	}
	// compare names
	if (compare_names && GetNames() != other.GetNames()) {
		return false;
	}
	// compare types
	if (GetTypes() != other.GetTypes()) {
		return false;
	}
	// now compare the actual values
	// fetch chunks
	unique_ptr<DataChunk> lchunk, rchunk;
	idx_t lindex = 0, rindex = 0;
	while (true) {
		if (!lchunk || lindex == lchunk->size()) {
			lchunk = Fetch();
			lindex = 0;
		}
		if (!rchunk || rindex == rchunk->size()) {
			rchunk = other.Fetch();
			rindex = 0;
		}
		if (!lchunk && !rchunk) {
			return true;
		}
		if (!lchunk || !rchunk) {
			return false;
		}
		if (lchunk->size() == 0 && rchunk->size() == 0) {
			return true;
		}
		D_ASSERT(lchunk->ColumnCount() == rchunk->ColumnCount());
		for (; lindex < lchunk->size() && rindex < rchunk->size(); lindex++, rindex++) {
			for (idx_t col = 0; col < rchunk->ColumnCount(); col++) {
				auto lvalue = lchunk->GetValue(col, lindex);
				auto rvalue = rchunk->GetValue(col, rindex);
				if (lvalue.IsNull() && rvalue.IsNull()) {
					continue;
				}
				if (lvalue.IsNull() != rvalue.IsNull()) {
					return false;
				}
				if (lvalue != rvalue) {
					return false;
				}
			}
		}
	}
} // LCOV_EXCL_STOP

void QueryResult::Print() {
	Printer::Print(ToString());
}

string QueryResult::HeaderToString() {
	string result;
	for (auto &name : GetNames()) {
		result += name + "\t";
	}
	result += "\n";
	for (auto &type : GetTypes()) {
		result += type.ToString() + "\t";
	}
	result += "\n";
	return result;
}

} // namespace duckdb
