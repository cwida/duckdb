#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/common/atomic.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/query_result.hpp"

#include <set>

namespace duckdb {

struct DuckDBColumnsData : public GlobalTableFunctionState {
	DuckDBColumnsData() : offset(0), column_offset(0) {
	}

	vector<reference<CatalogEntry>> entries;
	atomic<idx_t> offset;
	idx_t column_offset;
};

static unique_ptr<FunctionData> DuckDBColumnsBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("table_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("column_index");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("column_default");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("is_nullable");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("data_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("data_type_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("character_maximum_length");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("numeric_precision");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("numeric_precision_radix");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("numeric_scale");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("is_generated");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("generation_expression");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBColumnsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBColumnsData>();

	// scan all the schemas for tables and views and collect them
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry); });
	}
	return std::move(result);
}

class ColumnHelper {
public:
	static unique_ptr<ColumnHelper> Create(ClientContext &context, CatalogEntry &entry);

	virtual ~ColumnHelper() {
	}

	virtual StandardEntry &Entry() = 0;
	virtual idx_t NumColumns() = 0;
	virtual Value ColumnName(idx_t col) = 0;
	virtual const LogicalType &ColumnType(idx_t col) = 0;
	virtual const Value ColumnDefault(idx_t col) = 0;
	virtual bool IsNullable(idx_t col) = 0;
	virtual const Value ColumnComment(idx_t col) = 0;
	virtual const Value ColumnTags(idx_t col) = 0;
	virtual bool IsGenerated(idx_t col) = 0;
	virtual const Value GenerationExpression(idx_t col) = 0;

	void WriteColumns(idx_t start_col, idx_t end_col, DataChunk &output);
};

class TableColumnHelper : public ColumnHelper {
public:
	explicit TableColumnHelper(TableCatalogEntry &entry) : entry(entry) {
		for (auto &constraint : entry.GetConstraints()) {
			if (constraint->type == ConstraintType::NOT_NULL) {
				auto &not_null = constraint->Cast<NotNullConstraint>();
				not_null_cols.insert(not_null.index.index);
			}
		}
	}

	StandardEntry &Entry() override {
		return entry;
	}
	idx_t NumColumns() override {
		return entry.GetColumns().LogicalColumnCount();
	}
	Value ColumnName(idx_t col) override {
		return Value(entry.GetColumn(LogicalIndex(col)).Name());
	}
	const LogicalType &ColumnType(idx_t col) override {
		return entry.GetColumn(LogicalIndex(col)).Type();
	}
	const Value ColumnDefault(idx_t col) override {
		auto &column = entry.GetColumn(LogicalIndex(col));
		if (column.Generated()) {
			return Value(column.GeneratedExpression().ToString());
		} else if (column.HasDefaultValue()) {
			return Value(column.DefaultValue().ToString());
		}
		return Value();
	}
	bool IsNullable(idx_t col) override {
		return not_null_cols.find(col) == not_null_cols.end();
	}
	const Value ColumnComment(idx_t col) override {
		return entry.GetColumn(LogicalIndex(col)).Comment();
	}
	const Value ColumnTags(idx_t col) override {
		return Value::MAP(entry.GetColumn(LogicalIndex(col)).Tags());
	}
	bool IsGenerated(idx_t col) override {
		return entry.GetColumn(LogicalIndex(col)).Generated();
	}
	const Value GenerationExpression(idx_t col) override {
		auto &column = entry.GetColumn(LogicalIndex(col));
		if (!column.Generated()) {
			return Value();
		}
		return Value(column.GeneratedExpression().ToString());
	}

private:
	TableCatalogEntry &entry;
	std::set<idx_t> not_null_cols;
};

class ViewColumnHelper : public ColumnHelper {
public:
	explicit ViewColumnHelper(ClientContext &context, ViewCatalogEntry &entry) : entry(entry) {
		try {
			// try to bind the view if it is not yet bound
			entry.BindView(context);
		} catch (std::exception &ex) {
		}
		auto view_columns = entry.GetColumnInfo();
		if (view_columns) {
			column_names = view_columns->names;
			types = view_columns->types;
			QueryResult::DeduplicateColumns(column_names);
			bound_view = true;
		} else {
			// view is not bound - emit a single placeholder column
			types.push_back(LogicalTypeId::INVALID);
		}
	}

	StandardEntry &Entry() override {
		return entry;
	}
	idx_t NumColumns() override {
		return types.size();
	}
	Value ColumnName(idx_t col) override {
		if (types[0].id() == LogicalTypeId::INVALID) {
			return Value();
		}
		return Value(col < entry.aliases.size() ? entry.aliases[col] : column_names[col]);
	}
	const LogicalType &ColumnType(idx_t col) override {
		return types[col];
	}
	const Value ColumnDefault(idx_t col) override {
		return Value();
	}
	bool IsNullable(idx_t col) override {
		return true;
	}
	const Value ColumnComment(idx_t col) override {
		return bound_view ? entry.GetColumnComment(col) : Value();
	}
	const Value ColumnTags(idx_t col) override {
		InsertionOrderPreservingMap<string> empty;
		return Value::MAP(empty);
	}
	bool IsGenerated(idx_t col) override {
		return false;
	}
	const Value GenerationExpression(idx_t col) override {
		return Value();
	}

private:
	ViewCatalogEntry &entry;
	vector<Identifier> column_names;
	vector<LogicalType> types;
	bool bound_view = false;
};

unique_ptr<ColumnHelper> ColumnHelper::Create(ClientContext &context, CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::TABLE_ENTRY:
		return make_uniq<TableColumnHelper>(entry.Cast<TableCatalogEntry>());
	case CatalogType::VIEW_ENTRY:
		return make_uniq<ViewColumnHelper>(context, entry.Cast<ViewCatalogEntry>());
	default:
		throw NotImplementedException({{"catalog_type", CatalogTypeToString(entry.type)}},
		                              "Unsupported catalog type for duckdb_columns");
	}
}

void ColumnHelper::WriteColumns(idx_t start_col, idx_t end_col, DataChunk &output) {
	// database_name, VARCHAR
	auto &database_name = output.data[0];
	// database_oid, BIGINT
	auto &database_oid = output.data[1];
	// schema_name, VARCHAR
	auto &schema_name = output.data[2];
	// schema_oid, BIGINT
	auto &schema_oid = output.data[3];
	// table_name, VARCHAR
	auto &table_name = output.data[4];
	// table_oid, BIGINT
	auto &table_oid = output.data[5];
	// column_name, VARCHAR
	auto &column_name = output.data[6];
	// column_index, INTEGER
	auto &column_index = output.data[7];
	// comment, VARCHAR
	auto &comment = output.data[8];
	// internal, BOOLEAN
	auto &internal = output.data[9];
	// column_default, VARCHAR
	auto &column_default = output.data[10];
	// is_nullable, BOOLEAN
	auto &is_nullable = output.data[11];
	// data_type, VARCHAR
	auto &data_type = output.data[12];
	// data_type_id, BIGINT
	auto &data_type_id = output.data[13];
	// character_maximum_length, INTEGER
	auto &character_maximum_length = output.data[14];
	// numeric_precision, INTEGER
	auto &numeric_precision_col = output.data[15];
	// numeric_precision_radix, INTEGER
	auto &numeric_precision_radix_col = output.data[16];
	// numeric_scale, INTEGER
	auto &numeric_scale_col = output.data[17];
	// tags, MAP(VARCHAR, VARCHAR)
	auto &tags = output.data[18];
	// is_generated, BOOLEAN
	auto &is_generated = output.data[19];
	// generation_expression, VARCHAR
	auto &generation_expression = output.data[20];

	for (idx_t i = start_col; i < end_col; i++) {
		auto &entry = Entry();

		database_name.Append(Value(entry.catalog.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(entry.catalog.GetOid())));
		schema_name.Append(Value(entry.schema.name));
		schema_oid.Append(Value::BIGINT(NumericCast<int64_t>(entry.schema.oid)));
		table_name.Append(Value(entry.name));
		table_oid.Append(Value::BIGINT(NumericCast<int64_t>(entry.oid)));
		column_name.Append(ColumnName(i));
		column_index.Append(Value::INTEGER(UnsafeNumericCast<int32_t>(i + 1)));
		comment.Append(ColumnComment(i));
		internal.Append(Value::BOOLEAN(entry.internal));
		column_default.Append(ColumnDefault(i));
		is_nullable.Append(Value::BOOLEAN(IsNullable(i)));
		const LogicalType &type = ColumnType(i);
		data_type.Append(type.id() == LogicalTypeId::INVALID ? Value() : Value(type.ToString()));
		data_type_id.Append(type.id() == LogicalTypeId::INVALID ? Value() : Value::BIGINT(int(type.id())));
		// FIXME: need check constraints in place to set this correctly for VARCHAR
		character_maximum_length.Append(Value());

		Value numeric_precision, numeric_scale, numeric_precision_radix;
		switch (type.id()) {
		case LogicalTypeId::DECIMAL:
			numeric_precision = Value::INTEGER(DecimalType::GetWidth(type));
			numeric_scale = Value::INTEGER(DecimalType::GetScale(type));
			numeric_precision_radix = Value::INTEGER(10);
			break;
		case LogicalTypeId::HUGEINT:
			numeric_precision = Value::INTEGER(128);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		case LogicalTypeId::BIGINT:
			numeric_precision = Value::INTEGER(64);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		case LogicalTypeId::INTEGER:
			numeric_precision = Value::INTEGER(32);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		case LogicalTypeId::SMALLINT:
			numeric_precision = Value::INTEGER(16);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		case LogicalTypeId::TINYINT:
			numeric_precision = Value::INTEGER(8);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		case LogicalTypeId::FLOAT:
			numeric_precision = Value::INTEGER(24);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		case LogicalTypeId::DOUBLE:
			numeric_precision = Value::INTEGER(53);
			numeric_scale = Value::INTEGER(0);
			numeric_precision_radix = Value::INTEGER(2);
			break;
		default:
			numeric_precision = Value();
			numeric_scale = Value();
			numeric_precision_radix = Value();
			break;
		}

		numeric_precision_col.Append(numeric_precision);
		numeric_precision_radix_col.Append(numeric_precision_radix);
		numeric_scale_col.Append(numeric_scale);
		tags.Append(ColumnTags(i));
		is_generated.Append(Value::BOOLEAN(IsGenerated(i)));
		generation_expression.Append(GenerationExpression(i));
	}
}

static void DuckDBColumnsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBColumnsData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}

	// We need to track the offset of the relation we're writing as well as the last column
	// we wrote from that relation (if any); it's possible that we can fill up the output
	// with a partial list of columns from a relation and will need to pick up processing the
	// next chunk at the same spot.
	idx_t next = data.offset;
	idx_t column_offset = data.column_offset;
	idx_t index = 0;
	while (next < data.entries.size() && index < STANDARD_VECTOR_SIZE) {
		auto column_helper = ColumnHelper::Create(context, data.entries[next].get());
		idx_t columns = column_helper->NumColumns();

		// Check to see if we are going to exceed the maximum index for a DataChunk
		if (index + (columns - column_offset) > STANDARD_VECTOR_SIZE) {
			idx_t column_limit = column_offset + (STANDARD_VECTOR_SIZE - index);
			column_helper->WriteColumns(column_offset, column_limit, output);

			// Make the current column limit the column offset when we process the next chunk
			column_offset = column_limit;
			break;
		} else {
			// Otherwise, write all of the columns from the current relation and
			// then move on to the next one.
			column_helper->WriteColumns(column_offset, columns, output);
			index += columns - column_offset;
			next++;
			column_offset = 0;
		}
	}
	// WriteColumns appends to the child vectors - record the resulting cardinality on the chunk
	data.offset.store(next, std::memory_order_relaxed);
	data.column_offset = column_offset;
}

static double DuckDBColumnsProgress(ClientContext &context, const FunctionData *bind_data,
                                    const GlobalTableFunctionState *global_state) {
	auto &data = global_state->Cast<DuckDBColumnsData>();
	if (data.entries.empty()) {
		return 100.0;
	}
	return 100.0 * static_cast<double>(data.offset) / static_cast<double>(data.entries.size());
}

void DuckDBColumnsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction columns("duckdb_columns", {}, DuckDBColumnsFunction, DuckDBColumnsBind, DuckDBColumnsInit);
	columns.table_scan_progress = DuckDBColumnsProgress;
	set.AddFunction(columns);
}

} // namespace duckdb
