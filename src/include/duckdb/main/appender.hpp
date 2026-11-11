//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/appender.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/table_description.hpp"

namespace duckdb {

class ClientContext;
class ColumnDataCollection;
class Connection;
class DuckDB;
class SQLStatement;
class TableCatalogEntry;

enum class AppenderType : uint8_t {
	LOGICAL, // Cast input -> LogicalType
	PHYSICAL // Cast input -> PhysicalType
};

//! The Appender class can be used to append elements to a table.
class BaseAppender {
public:
	//! The amount of tuples that are gathered in the column data collection before flushing.
	static constexpr const idx_t DEFAULT_FLUSH_COUNT = STANDARD_VECTOR_SIZE * 100ULL;

public:
	//! Returns a table reference to the appended data.
	static unique_ptr<TableRef> GetColumnDataTableRef(ColumnDataCollection &collection, const Identifier &table_name,
	                                                  const vector<Identifier> &expected_names);
	//! Parses the statement to append data.
	static unique_ptr<SQLStatement> ParseStatement(ClientContext &context, unique_ptr<TableRef> table_ref,
	                                               const string &query, const string &table_name);

protected:
	//! The allocator for the column data collection.
	Allocator &allocator;
	//! The column types of the associated table.
	vector<LogicalType> types;
	//! The active column types.
	vector<LogicalType> active_types;
	//! The buffered to-be-appended data.
	unique_ptr<ColumnDataCollection> collection;
	//! The active chunk for row-based appends.
	DataChunk chunk;
	//! The currently active column of row-based appends.
	idx_t column = 0;
	//! The type of the appender.
	AppenderType appender_type;
	//! The amount of rows after which the appender flushes automatically.
	idx_t flush_count = DEFAULT_FLUSH_COUNT;
	//! Peak allocation threshold at which to flush the allocator when appender flushes chunk.
	optional_idx flush_memory_threshold;

protected:
	DUCKDB_API BaseAppender(Allocator &allocator, const AppenderType type);
	DUCKDB_API BaseAppender(Allocator &allocator, vector<LogicalType> types, const AppenderType type,
	                        const idx_t flush_count = DEFAULT_FLUSH_COUNT);

public:
	DUCKDB_API virtual ~BaseAppender();

	//! Begins a new row append, after calling this the other AppendX() functions
	//! should be called the correct amount of times. After that,
	//! EndRow() should be called.
	DUCKDB_API void BeginRow();
	//! Finishes appending the current row.
	DUCKDB_API void EndRow();

	// Append functions
	template <class T>
	void Append(T value) = delete;
	DUCKDB_API void Append(DataChunk &target, const Value &value, idx_t col, idx_t row);

	DUCKDB_API void Append(const char *value, uint32_t length);

	// prepared statements
	template <typename... ARGS>
	void AppendRow(ARGS... args) {
		BeginRow();
		AppendRowRecursive(args...);
	}

	//! Commit the changes made by the appender.
	DUCKDB_API void Flush();
	//! Flush the changes made by the appender and close it. The appender cannot be used after this point
	DUCKDB_API void Close();
	//! Clears any appended data (without flushing).
	DUCKDB_API void Clear();
	//! Returns the active types of the appender.
	const vector<LogicalType> &GetActiveTypes() const;

	idx_t CurrentColumn() const {
		return column;
	}
	DUCKDB_API void AppendDataChunk(DataChunk &value);

	virtual void AppendDefault();
	virtual void AppendDefault(DataChunk &chunk, idx_t col, idx_t row);
	//! Appends a column to the active column list.
	//! Immediately flushes all previous data.
	virtual void AddColumn(const Identifier &name);
	//! Removes all columns from the active column list.
	//! Immediately flushes all previous data.
	virtual void ClearColumns();

protected:
	void Destructor();
	virtual void FlushInternal(ColumnDataCollection &collection) = 0;
	void InitializeChunk();
	void FlushChunk();

	bool ShouldFlushChunk() const;
	bool ShouldFlush() const;

	template <class T>
	void AppendValueInternal(T value);
	template <class SRC, class DST>
	void AppendValueInternal(Vector &vector, SRC input);
	template <class SRC, class DST>
	void AppendDecimalValueInternal(Vector &vector, SRC input);

	void AppendRowRecursive() {
		EndRow();
	}

	template <typename T, typename... ARGS>
	void AppendRowRecursive(T value, ARGS... args) {
		Append<T>(value);
		AppendRowRecursive(args...);
	}

	void AppendValue(const Value &value);
	void AppendValue(DataChunk target, const Value &value);
};

class Appender : public BaseAppender {
public:
	DUCKDB_API Appender(Connection &con, const Identifier &database_name, const Identifier &schema_name,
	                    const Identifier &table_name, const idx_t flush_memory_threshold = DConstants::INVALID_INDEX);
	DUCKDB_API Appender(Connection &con, const Identifier &schema_name, const Identifier &table_name,
	                    const idx_t flush_memory_threshold = DConstants::INVALID_INDEX);
	DUCKDB_API Appender(Connection &con, const Identifier &table_name,
	                    const idx_t flush_memory_threshold = DConstants::INVALID_INDEX);
	DUCKDB_API ~Appender() override;

public:
	void AppendDefault() override;
	void AppendDefault(DataChunk &chunk, idx_t col, idx_t row) override;
	void AddColumn(const Identifier &name) override;
	void ClearColumns() override;
	//! Get the expected names based on the active columns.
	vector<Identifier> GetExpectedNames();
	//! Construct a query that appends data from, typically, a column data collection.
	static string ConstructQuery(TableDescription &description_p, const Identifier &table_name,
	                             const vector<Identifier> &expected_names);

private:
	//! A shared pointer to the context of this appender.
	weak_ptr<ClientContext> context;
	//! The table description including the column names.
	unique_ptr<TableDescription> description;
	//! All table default values.
	unordered_map<column_t, Value> default_values;

	//! If not empty, then this holds all logical column IDs of columns provided by the appender.
	//! Any other columns default to NULL, or their default values.
	vector<LogicalIndex> column_ids;

protected:
	void FlushInternal(ColumnDataCollection &collection) override;
	Value GetDefaultValue(idx_t column);
};

class QueryAppender : public BaseAppender {
public:
	DUCKDB_API QueryAppender(Connection &con, string query, vector<LogicalType> types,
	                         vector<Identifier> names = vector<Identifier>(), Identifier table_name = Identifier(),
	                         const idx_t flush_memory_threshold = DConstants::INVALID_INDEX);
	DUCKDB_API ~QueryAppender() override;

private:
	//! A shared pointer to the context of this appender.
	weak_ptr<ClientContext> context;
	//! The query to run.
	string query;
	//! The column names of the to-be-appended data, or "col1, col2, ...", if empty.
	vector<Identifier> names;
	//! The table name that we can reference in the query, or "appended_data", if empty.
	Identifier table_name;

protected:
	void FlushInternal(ColumnDataCollection &collection) override;
};

class InternalAppender : public BaseAppender {
	//! The client context
	ClientContext &context;
	//! The internal table entry to append to
	TableCatalogEntry &table;

public:
	DUCKDB_API InternalAppender(ClientContext &context, TableCatalogEntry &table,
	                            const idx_t flush_count = DEFAULT_FLUSH_COUNT,
	                            const idx_t flush_memory_threshold = DConstants::INVALID_INDEX);
	DUCKDB_API ~InternalAppender() override;

protected:
	void FlushInternal(ColumnDataCollection &collection) override;
};

template <>
DUCKDB_API void BaseAppender::Append(bool value);
template <>
DUCKDB_API void BaseAppender::Append(int8_t value);
template <>
DUCKDB_API void BaseAppender::Append(int16_t value);
template <>
DUCKDB_API void BaseAppender::Append(int32_t value);
template <>
DUCKDB_API void BaseAppender::Append(int64_t value);
template <>
DUCKDB_API void BaseAppender::Append(hugeint_t value);
template <>
DUCKDB_API void BaseAppender::Append(uhugeint_t value);
template <>
DUCKDB_API void BaseAppender::Append(uint8_t value);
template <>
DUCKDB_API void BaseAppender::Append(uint16_t value);
template <>
DUCKDB_API void BaseAppender::Append(uint32_t value);
template <>
DUCKDB_API void BaseAppender::Append(uint64_t value);
template <>
DUCKDB_API void BaseAppender::Append(float value);
template <>
DUCKDB_API void BaseAppender::Append(double value);
template <>
DUCKDB_API void BaseAppender::Append(date_t value);
template <>
DUCKDB_API void BaseAppender::Append(dtime_t value);
template <>
DUCKDB_API void BaseAppender::Append(timestamp_t value);
template <>
DUCKDB_API void BaseAppender::Append(interval_t value);
template <>
DUCKDB_API void BaseAppender::Append(const char *value);
template <>
DUCKDB_API void BaseAppender::Append(string_t value);
template <>
DUCKDB_API void BaseAppender::Append(Value value);
template <>
DUCKDB_API void BaseAppender::Append(std::nullptr_t value);

} // namespace duckdb
