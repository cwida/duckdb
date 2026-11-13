#include "duckdb/main/appender.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/tableref/column_data_ref.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

BaseAppender::BaseAppender(Allocator &allocator, const AppenderType type_p)
    : allocator(allocator), column(0), appender_type(type_p) {
}

BaseAppender::BaseAppender(Allocator &allocator_p, vector<LogicalType> types_p, const AppenderType type_p,
                           const idx_t flush_count_p)
    : allocator(allocator_p), types(std::move(types_p)), collection(make_uniq<ColumnDataCollection>(allocator, types)),
      column(0), appender_type(type_p), flush_count(flush_count_p) {
	InitializeChunk();
}

BaseAppender::~BaseAppender() {
}

void BaseAppender::Destructor() {
	if (Exception::UncaughtException()) {
		return;
	}
	// Flush any remaining chunks, if we are not cleaning up as part of an exception stack unwind wrapped in a
	// try/catch. Close() can throw if the table was dropped in the meantime.
	try {
		Close();
	} catch (...) { // NOLINT
		            // FIXME: Make any log context available here.
	}
}

const vector<LogicalType> &BaseAppender::GetActiveTypes() const {
	if (active_types.empty()) {
		return types;
	}
	return active_types;
}

void BaseAppender::InitializeChunk() {
	chunk.Destroy();
	chunk.Initialize(allocator, GetActiveTypes());
}

void BaseAppender::BeginRow() {
}

void BaseAppender::EndRow() {
	// Ensure that all columns have been appended to.
	if (column != chunk.ColumnCount()) {
		throw InvalidInputException("Call to EndRow before all columns have been appended to!");
	}
	column = 0;
	chunk.SetCardinalityUnsafe(chunk.size() + 1);
	if (ShouldFlushChunk()) {
		FlushChunk();
	}
}

template <class SRC, class DST>
void BaseAppender::AppendValueInternal(Vector &col, SRC input) {
	FlatVector::GetDataMutable<DST>(col)[chunk.size()] = Cast::Operation<SRC, DST>(input);
}

template <class SRC, class DST>
void BaseAppender::AppendDecimalValueInternal(Vector &col, SRC input) {
	switch (appender_type) {
	case AppenderType::LOGICAL: {
		auto &type = col.GetType();
		D_ASSERT(type.id() == LogicalTypeId::DECIMAL);
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		CastParameters parameters;
		auto &result = FlatVector::GetDataMutable<DST>(col)[chunk.size()];
		TryCastToDecimal::Operation<SRC, DST>(input, result, parameters, width, scale);
		return;
	}
	case AppenderType::PHYSICAL: {
		AppendValueInternal<SRC, DST>(col, input);
		return;
	}
	default:
		InternalException::Throw("Type not implemented for AppenderType");
	}
}

template <class SRC>
void BaseAppender::AppendWideDecimalValueInternal(Vector &col, SRC input) {
	auto &type = col.GetType();
	D_ASSERT(type.id() == LogicalTypeId::DECIMAL && type.InternalType() == PhysicalType::VARCHAR);
	// every source type is converted with its regular cast to the column type
	auto value = Value::CreateValue<SRC>(input).DefaultCastAs(type);
	FlatVector::GetDataMutable<string_t>(col)[chunk.size()] =
	    StringVector::AddStringOrBlob(col, StringValue::Get(value));
}

template <class T>
void BaseAppender::AppendValueInternal(T input) {
	if (column >= GetActiveTypes().size()) {
		InvalidInputException::Throw("Too many appends for chunk!");
	}
	auto &col = chunk.data[column];
	switch (col.GetType().id()) {
	case LogicalTypeId::BOOLEAN:
		AppendValueInternal<T, bool>(col, input);
		break;
	case LogicalTypeId::UTINYINT:
		AppendValueInternal<T, uint8_t>(col, input);
		break;
	case LogicalTypeId::TINYINT:
		AppendValueInternal<T, int8_t>(col, input);
		break;
	case LogicalTypeId::USMALLINT:
		AppendValueInternal<T, uint16_t>(col, input);
		break;
	case LogicalTypeId::SMALLINT:
		AppendValueInternal<T, int16_t>(col, input);
		break;
	case LogicalTypeId::UINTEGER:
		AppendValueInternal<T, uint32_t>(col, input);
		break;
	case LogicalTypeId::INTEGER:
		AppendValueInternal<T, int32_t>(col, input);
		break;
	case LogicalTypeId::UBIGINT:
		AppendValueInternal<T, uint64_t>(col, input);
		break;
	case LogicalTypeId::BIGINT:
		AppendValueInternal<T, int64_t>(col, input);
		break;
	case LogicalTypeId::HUGEINT:
		AppendValueInternal<T, hugeint_t>(col, input);
		break;
	case LogicalTypeId::UHUGEINT:
		AppendValueInternal<T, uhugeint_t>(col, input);
		break;
	case LogicalTypeId::FLOAT:
		AppendValueInternal<T, float>(col, input);
		break;
	case LogicalTypeId::DOUBLE:
		AppendValueInternal<T, double>(col, input);
		break;
	case LogicalTypeId::DECIMAL:
		switch (col.GetType().InternalType()) {
		case PhysicalType::INT16:
			AppendDecimalValueInternal<T, int16_t>(col, input);
			break;
		case PhysicalType::INT32:
			AppendDecimalValueInternal<T, int32_t>(col, input);
			break;
		case PhysicalType::INT64:
			AppendDecimalValueInternal<T, int64_t>(col, input);
			break;
		case PhysicalType::INT128:
			AppendDecimalValueInternal<T, hugeint_t>(col, input);
			break;
		case PhysicalType::VARCHAR:
			AppendWideDecimalValueInternal<T>(col, input);
			break;
		default:
			InternalException::Throw("Internal type not recognized for Decimal");
		}
		break;
	case LogicalTypeId::DATE:
		AppendValueInternal<T, date_t>(col, input);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		AppendValueInternal<T, timestamp_t>(col, input);
		break;
	case LogicalTypeId::TIME:
		AppendValueInternal<T, dtime_t>(col, input);
		break;
	case LogicalTypeId::TIME_NS:
		AppendValueInternal<T, dtime_ns_t>(col, input);
		break;
	case LogicalTypeId::TIME_TZ:
		AppendValueInternal<T, dtime_tz_t>(col, input);
		break;
	case LogicalTypeId::INTERVAL:
		AppendValueInternal<T, interval_t>(col, input);
		break;
	case LogicalTypeId::VARCHAR:
		FlatVector::GetDataMutable<string_t>(col)[chunk.size()] =
		    StringCast::Operation<T>(input, StringVector::GetStringHeap(col));
		break;
	default:
		AppendValue(Value::CreateValue<T>(input));
		return;
	}
	column++;
}

template <>
void BaseAppender::Append(bool value) {
	AppendValueInternal<bool>(value);
}

template <>
void BaseAppender::Append(int8_t value) {
	AppendValueInternal<int8_t>(value);
}

template <>
void BaseAppender::Append(int16_t value) {
	AppendValueInternal<int16_t>(value);
}

template <>
void BaseAppender::Append(int32_t value) {
	AppendValueInternal<int32_t>(value);
}

template <>
void BaseAppender::Append(int64_t value) {
	AppendValueInternal<int64_t>(value);
}

template <>
void BaseAppender::Append(hugeint_t value) {
	AppendValueInternal<hugeint_t>(value);
}

template <>
void BaseAppender::Append(uhugeint_t value) {
	AppendValueInternal<uhugeint_t>(value);
}

template <>
void BaseAppender::Append(uint8_t value) {
	AppendValueInternal<uint8_t>(value);
}

template <>
void BaseAppender::Append(uint16_t value) {
	AppendValueInternal<uint16_t>(value);
}

template <>
void BaseAppender::Append(uint32_t value) {
	AppendValueInternal<uint32_t>(value);
}

template <>
void BaseAppender::Append(uint64_t value) {
	AppendValueInternal<uint64_t>(value);
}

template <>
void BaseAppender::Append(const char *value) {
	AppendValueInternal<string_t>(string_t(value));
}

void BaseAppender::Append(const char *value, uint32_t length) {
	AppendValueInternal<string_t>(string_t(value, length));
}

template <>
void BaseAppender::Append(string_t value) {
	AppendValueInternal<string_t>(value);
}

template <>
void BaseAppender::Append(float value) {
	AppendValueInternal<float>(value);
}

template <>
void BaseAppender::Append(double value) {
	AppendValueInternal<double>(value);
}

template <>
void BaseAppender::Append(date_t value) {
	AppendValueInternal<date_t>(value);
}

template <>
void BaseAppender::Append(dtime_t value) {
	AppendValueInternal<dtime_t>(value);
}

template <>
void BaseAppender::Append(timestamp_t value) {
	AppendValueInternal<timestamp_t>(value);
}

template <>
void BaseAppender::Append(interval_t value) {
	AppendValueInternal<interval_t>(value);
}

template <>
void BaseAppender::Append(Value value) { // NOLINT: template stuff
	if (column >= chunk.ColumnCount()) {
		InvalidInputException::Throw("Too many appends for chunk!");
	}
	AppendValue(value);
}

void duckdb::BaseAppender::Append(DataChunk &target, const Value &value, idx_t col, idx_t row) {
	if (col >= target.ColumnCount()) {
		InvalidInputException::Throw("Too many appends for chunk!");
	}
	if (value.type() == target.GetTypes()[col]) {
		target.data[col].SetValue(row, value);
	} else {
		string error_msg;
		auto new_value = value.DefaultTryCastAs(target.GetTypes()[col], &error_msg);
		if (new_value) {
			target.data[col].SetValue(row, *new_value);
		} else {
			throw InvalidInputException("type mismatch in Append, expected %s, got %s for column %d",
			                            target.GetTypes()[col], value.type(), col);
		}
	}
}

template <>
void BaseAppender::Append(std::nullptr_t value) {
	if (column >= chunk.ColumnCount()) {
		InvalidInputException::Throw("Too many appends for chunk!");
	}
	auto &col = chunk.data[column++];
	FlatVector::SetNull(col, chunk.size(), true);
}

void BaseAppender::AppendValue(const Value &value) {
	chunk.data[column].SetValue(chunk.size(), value);
	column++;
}

void BaseAppender::AppendDataChunk(DataChunk &chunk_p) {
	auto chunk_types = chunk_p.GetTypes();
	auto &appender_types = GetActiveTypes();

	// Early-out, if types match.
	if (chunk_types == appender_types) {
		collection->Append(chunk_p);
		if (ShouldFlush()) {
			Flush();
		}
		return;
	}

	auto count = chunk_p.ColumnCount();
	if (count != appender_types.size()) {
		throw InvalidInputException("incorrect column count in AppendDataChunk, expected %d, got %d",
		                            appender_types.size(), count);
	}

	// We try to cast the chunk.
	auto size = chunk_p.size();
	DataChunk cast_chunk;
	// Chunks converted from Arrow can hold more than STANDARD_VECTOR_SIZE rows
	cast_chunk.Initialize(allocator, appender_types, MaxValue<idx_t>(size, STANDARD_VECTOR_SIZE));
	cast_chunk.SetChildCardinality(size);

	for (idx_t i = 0; i < count; i++) {
		if (chunk_p.data[i].GetType() == appender_types[i]) {
			cast_chunk.data[i].Reference(chunk_p.data[i]);
			continue;
		}

		string error_msg;
		auto success = VectorOperations::DefaultTryCast(chunk_p.data[i], cast_chunk.data[i], size, &error_msg);
		if (!success) {
			throw InvalidInputException("type mismatch in AppendDataChunk, expected %s, got %s for column %d",
			                            appender_types[i].ToString(), chunk_p.data[i].GetType().ToString(), i);
		}
	}

	collection->Append(cast_chunk);
	if (ShouldFlush()) {
		Flush();
	}
}

void BaseAppender::FlushChunk() {
	if (chunk.size() == 0) {
		return;
	}
	chunk.SetChildCardinality(chunk.size());
	collection->Append(chunk);
	chunk.Reset();
	if (ShouldFlush()) {
		Flush();
	}
}

void BaseAppender::Flush() {
	// Check that all vectors have the same length before appending.
	if (column != 0) {
		throw InvalidInputException("Failed to Flush appender: incomplete append to row!");
	}

	FlushChunk();
	if (collection->Count() == 0) {
		return;
	}

	try {
		FlushInternal(*collection);
	} catch (...) {
		// Reset so the destructor does not re-attempt flushing the same data,
		// which would hit assertions in the storage layer if it is inconsistent.
		collection->Reset();
		throw;
	}
	collection->Reset();
	column = 0;
}

void BaseAppender::AppendDefault() {
	throw NotImplementedException("AppendDefault is only supported when directly appending to a table");
}

void BaseAppender::AppendDefault(DataChunk &chunk, idx_t col, idx_t row) {
	throw NotImplementedException("AppendDefault is only supported when directly appending to a table");
}

void BaseAppender::AddColumn(const Identifier &name) {
	throw NotImplementedException("AddColumn is only supported when directly appending to a table");
}

void BaseAppender::ClearColumns() {
	throw NotImplementedException("ClearColumns is only supported when directly appending to a table");
}

unique_ptr<TableRef> BaseAppender::GetColumnDataTableRef(ColumnDataCollection &collection, const Identifier &table_name,
                                                         const vector<Identifier> &expected_names) {
	auto column_data_ref = make_uniq<ColumnDataRef>(collection);
	column_data_ref->alias = table_name.empty() ? "appended_data" : table_name;
	;
	column_data_ref->expected_names = expected_names;
	return std::move(column_data_ref);
}

CommonTableExpressionMap &GetCTEMap(SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::INSERT_STATEMENT:
		return statement.Cast<InsertStatement>().node->cte_map;
	case StatementType::DELETE_STATEMENT:
		return statement.Cast<DeleteStatement>().node->cte_map;
	case StatementType::UPDATE_STATEMENT:
		return statement.Cast<UpdateStatement>().node->cte_map;
	case StatementType::MERGE_INTO_STATEMENT:
		return statement.Cast<MergeIntoStatement>().node->cte_map;
	default:
		throw InvalidInputException(
		    "Unsupported statement type for appender: expected INSERT, DELETE, UPDATE or MERGE INTO");
	}
}

unique_ptr<SQLStatement> BaseAppender::ParseStatement(ClientContext &context, unique_ptr<TableRef> table_ref,
                                                      const string &query, const string &table_name) {
	// Parse the query.
	Parser parser(context);
	parser.ParseQuery(query);

	// Must be a single statement.
	if (parser.statements.size() != 1) {
		throw InvalidInputException("Expected exactly one query for appending data.");
	}

	// Create the CTE for the appender.
	auto cte = make_uniq<SelectNode>();
	cte->select_list.push_back(make_uniq<StarExpression>());
	cte->from_table = std::move(table_ref);

	// Create the SELECT CTE.
	auto cte_select = make_uniq<SelectStatement>();
	cte_select->node = std::move(cte);

	// Create the CTE info.
	auto cte_info = make_uniq<CommonTableExpressionInfo>();
	cte_info->query_node = std::move(cte_select->node);
	cte_info->materialized = CTEMaterialize::CTE_MATERIALIZE_NEVER;

	// Add the appender data as a CTE to the CTE map of the statement.
	string alias = table_name.empty() ? "appended_data" : table_name;
	auto &cte_map = GetCTEMap(*parser.statements[0]);
	cte_map.map.insert(Identifier(alias), std::move(cte_info));

	return std::move(parser.statements[0]);
}

//===--------------------------------------------------------------------===//
// Table Appender
//===--------------------------------------------------------------------===//
Appender::Appender(Connection &con, const Identifier &database_name, const Identifier &schema_name,
                   const Identifier &table_name, const idx_t flush_memory_threshold_p)
    : BaseAppender(Allocator::DefaultAllocator(), AppenderType::LOGICAL), context(con.context) {
	flush_memory_threshold = (flush_memory_threshold_p == DConstants::INVALID_INDEX)
	                             ? optional_idx::Invalid()
	                             : optional_idx(flush_memory_threshold_p);

	description = con.TableInfo(database_name, schema_name, table_name);
	if (!description) {
		throw CatalogException(StringUtil::Format("Table '%s.%s.%s' could not be found", SQLIdentifier(database_name),
		                                          SQLIdentifier(schema_name), SQLIdentifier(table_name)));
	}
	if (description->readonly) {
		throw InvalidInputException("Cannot append to a readonly database.");
	}

	vector<optional_ptr<const ParsedExpression>> defaults;
	for (auto &column : description->columns) {
		if (column.Generated()) {
			continue;
		}
		types.push_back(column.Type());
		defaults.push_back(column.HasDefaultValue() ? &column.DefaultValue() : nullptr);
	}
	auto &context_ref = *con.context;
	auto binder = Binder::CreateBinder(context_ref);
	context_ref.RunFunctionInTransaction([&]() {
		for (idx_t i = 0; i < types.size(); i++) {
			auto &type = types[i];
			auto &expr = defaults[i];

			if (!expr) {
				// The default value is NULL.
				default_values[i] = Value(type);
				continue;
			}

			auto default_copy = expr->Copy();
			D_ASSERT(!default_copy->HasParameter());

			ConstantBinder default_binder(*binder, context_ref, "DEFAULT value");
			default_binder.target_type = type;
			auto bound_default = default_binder.Bind(default_copy);

			if (!bound_default->IsFoldable()) {
				// Not supported yet.
				continue;
			}

			Value result_value;
			auto eval_success = ExpressionExecutor::TryEvaluateScalar(context_ref, *bound_default, result_value);
			// Insert the default Value.
			if (eval_success) {
				default_values[i] = result_value;
			}
		}
	});

	InitializeChunk();
	collection = make_uniq<ColumnDataCollection>(allocator, GetActiveTypes());
}

Appender::Appender(Connection &con, const Identifier &schema_name, const Identifier &table_name,
                   const idx_t flush_memory_threshold_p)
    : Appender(con, Identifier::InvalidCatalog(), schema_name, table_name, flush_memory_threshold_p) {
}

Appender::Appender(Connection &con, const Identifier &table_name, const idx_t flush_memory_threshold_p)
    : Appender(con, Identifier::InvalidCatalog(), Identifier::DefaultSchema(), table_name, flush_memory_threshold_p) {
}

Appender::~Appender() {
	Destructor();
}

vector<Identifier> Appender::GetExpectedNames() {
	vector<Identifier> expected_names;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto &col_name = description->columns[column_ids[i].index].Name();
		expected_names.emplace_back(col_name);
	}
	return expected_names;
}

string Appender::ConstructQuery(TableDescription &description_p, const Identifier &table_name,
                                const vector<Identifier> &expected_names) {
	string query = "INSERT INTO ";
	if (!description_p.qualified_name.Catalog().empty()) {
		query += StringUtil::Format("%s.", SQLIdentifier(description_p.qualified_name.Catalog()));
	}
	if (!description_p.qualified_name.Schema().empty()) {
		query += StringUtil::Format("%s.", SQLIdentifier(description_p.qualified_name.Schema()));
	}
	query += StringUtil::Format("%s", SQLIdentifier(description_p.qualified_name.Name()));
	if (!expected_names.empty()) {
		query += "(";
		for (idx_t i = 0; i < expected_names.size(); i++) {
			if (i > 0) {
				query += ", ";
			}
			query += StringUtil::Format("%s", SQLIdentifier(expected_names[i]));
		}
		query += ")";
	}
	query += " FROM ";
	query += table_name;
	return query;
}

void Appender::FlushInternal(ColumnDataCollection &collection) {
	auto context_ref = context.lock();
	if (!context_ref) {
		throw InvalidInputException("Appender: Attempting to flush data to a closed connection");
	}

	Identifier table_name("__duckdb_internal_appended_data");
	auto expected_names = GetExpectedNames();
	auto query = ConstructQuery(*description, table_name, expected_names);

	auto table_ref = GetColumnDataTableRef(collection, table_name, expected_names);
	auto stmt = ParseStatement(*context_ref, std::move(table_ref), query, table_name.GetIdentifierName());
	context_ref->Append(std::move(stmt));
}

void Appender::AppendDefault() {
	auto value = GetDefaultValue(column);
	Append(value);
}

void duckdb::Appender::AppendDefault(DataChunk &chunk, idx_t col, idx_t row) {
	auto value = GetDefaultValue(col);
	Append(chunk, value, col, row);
}

Value Appender::GetDefaultValue(idx_t column) {
	auto index = column;

	if (!column_ids.empty()) {
		if (column >= column_ids.size()) {
			throw InvalidInputException("Column index out of bounds");
		}
		index = column_ids[column].index;
	}

	auto it = default_values.find(index);
	if (it == default_values.end()) {
		auto &name = description->columns[index].Name();
		throw NotImplementedException(
		    "AppendDefault is not supported for column %s: not a foldable default expressions.", name);
	}
	return it->second;
}

void Appender::AddColumn(const Identifier &name) {
	Flush();

	auto exists = false;
	for (idx_t col_idx = 0; col_idx < description->columns.size(); col_idx++) {
		auto &col_def = description->columns[col_idx];
		if (col_def.Name() != name) {
			continue;
		}

		// Ensure that we are not adding a generated column.
		if (col_def.Generated()) {
			throw InvalidInputException("cannot add a generated column to the appender");
		}

		// Ensure that we haven't added this column before.
		for (const auto &column_id : column_ids) {
			if (column_id == col_def.Logical()) {
				throw InvalidInputException("cannot add the same column twice");
			}
		}

		active_types.push_back(col_def.Type());
		column_ids.push_back(col_def.Logical());
		exists = true;
		break;
	}
	if (!exists) {
		throw InvalidInputException("the column must exist in the table");
	}

	InitializeChunk();
	collection = make_uniq<ColumnDataCollection>(allocator, GetActiveTypes());
}

void Appender::ClearColumns() {
	Flush();
	column_ids.clear();
	active_types.clear();

	InitializeChunk();
	collection = make_uniq<ColumnDataCollection>(allocator, GetActiveTypes());
}

//===--------------------------------------------------------------------===//
// Query Appender
//===--------------------------------------------------------------------===//
QueryAppender::QueryAppender(Connection &con, string query_p, vector<LogicalType> types_p, vector<Identifier> names_p,
                             Identifier table_name_p, const idx_t flush_memory_threshold_p)
    : BaseAppender(Allocator::DefaultAllocator(), AppenderType::LOGICAL), context(con.context),
      query(std::move(query_p)), names(std::move(names_p)), table_name(std::move(table_name_p)) {
	types = std::move(types_p);
	InitializeChunk();
	collection = make_uniq<ColumnDataCollection>(allocator, GetActiveTypes());
	flush_memory_threshold = (flush_memory_threshold_p == DConstants::INVALID_INDEX)
	                             ? optional_idx::Invalid()
	                             : optional_idx(flush_memory_threshold_p);
}

QueryAppender::~QueryAppender() {
}

void QueryAppender::FlushInternal(ColumnDataCollection &collection) {
	auto context_ref = context.lock();
	if (!context_ref) {
		throw InvalidInputException("Attempting to flush query appender data on a closed connection");
	}
	auto table_ref = GetColumnDataTableRef(collection, table_name, names);
	auto parsed_statement = ParseStatement(*context_ref, std::move(table_ref), query, table_name.GetIdentifierName());
	context_ref->Append(std::move(parsed_statement));
}

//===--------------------------------------------------------------------===//
// Internal Appender
//===--------------------------------------------------------------------===//
InternalAppender::InternalAppender(ClientContext &context_p, TableCatalogEntry &table_p, const idx_t flush_count_p,
                                   const idx_t flush_memory_threshold_p)
    : BaseAppender(Allocator::DefaultAllocator(), table_p.GetTypes(), AppenderType::PHYSICAL, flush_count_p),
      context(context_p), table(table_p) {
	flush_memory_threshold = (flush_memory_threshold_p == DConstants::INVALID_INDEX)
	                             ? optional_idx::Invalid()
	                             : optional_idx(flush_memory_threshold_p);
}

InternalAppender::~InternalAppender() {
	Destructor();
}

void InternalAppender::FlushInternal(ColumnDataCollection &collection) {
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	table.GetStorage().LocalAppend(table.Cast<DuckTableEntry>(), context, collection, bound_constraints, nullptr);
}

void BaseAppender::Close() {
	if (column == 0 || column == GetActiveTypes().size()) {
		Flush();
	}
}

void BaseAppender::Clear() {
	chunk.Reset();

	if (collection) {
		collection->Reset();
	}

	column = 0;
}

bool BaseAppender::ShouldFlushChunk() const {
	if (chunk.size() >= STANDARD_VECTOR_SIZE) {
		return true;
	}

	if (!flush_memory_threshold.IsValid()) {
		return false;
	}

	return (collection->AllocationSize() >= flush_memory_threshold.GetIndex());
}

bool BaseAppender::ShouldFlush() const {
	if (collection->Count() >= flush_count) {
		return true;
	}

	if (!flush_memory_threshold.IsValid()) {
		return false;
	}

	return (collection->AllocationSize() >= flush_memory_threshold.GetIndex());
}

} // namespace duckdb
