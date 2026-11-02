#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/binder.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"

#include <algorithm>

namespace duckdb {

struct PragmaTableFunctionData : public TableFunctionData {
	explicit PragmaTableFunctionData(CatalogEntry &entry_p, bool is_table_info)
	    : entry(entry_p), is_table_info(is_table_info) {
	}

	CatalogEntry &entry;
	bool is_table_info;
};

struct PragmaTableOperatorData : public GlobalTableFunctionState {
	PragmaTableOperatorData() : offset(0) {
	}
	idx_t offset;
};

struct ColumnConstraintInfo {
	bool not_null = false;
	bool pk = false;
	bool unique = false;
};

static Value DefaultValue(const ColumnDefinition &def) {
	if (def.Generated()) {
		return Value(def.GeneratedExpression().ToString());
	}
	if (!def.HasDefaultValue()) {
		return Value();
	}
	auto &value = def.DefaultValue();
	return Value(value.ToString());
}

struct PragmaTableInfoHelper {
	static void GetSchema(vector<LogicalType> &return_types, vector<Identifier> &names) {
		names.emplace_back("cid");
		return_types.emplace_back(LogicalType::INTEGER);

		names.emplace_back("name");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("type");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("notnull");
		return_types.emplace_back(LogicalType::BOOLEAN);

		names.emplace_back("dflt_value");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("pk");
		return_types.emplace_back(LogicalType::BOOLEAN);
	}

	static void GetTableColumns(const ColumnDefinition &column, ColumnConstraintInfo constraint_info,
	                            DataChunk &output) {
		// "cid", INTEGER
		output.data[0].Append(Value::INTEGER((int32_t)column.Oid()));
		// "name", VARCHAR
		output.data[1].Append(Value(column.Name()));
		// "type", VARCHAR
		output.data[2].Append(Value(column.Type().ToString()));
		// "notnull", BOOLEAN
		output.data[3].Append(Value::BOOLEAN(constraint_info.not_null));
		// "dflt_value", VARCHAR
		output.data[4].Append(DefaultValue(column));
		// "pk", BOOLEAN
		output.data[5].Append(Value::BOOLEAN(constraint_info.pk));
	}

	static void GetViewColumns(idx_t i, const Identifier &name, const LogicalType &type, DataChunk &output) {
		// "cid", INTEGER
		output.data[0].Append(Value::INTEGER((int32_t)i));
		// "name", VARCHAR
		output.data[1].Append(Value(name));
		// "type", VARCHAR
		output.data[2].Append(Value(type.ToString()));
		// "notnull", BOOLEAN
		output.data[3].Append(Value::BOOLEAN(false));
		// "dflt_value", VARCHAR
		output.data[4].Append(Value());
		// "pk", BOOLEAN
		output.data[5].Append(Value::BOOLEAN(false));
	}
};

struct PragmaShowHelper {
	static void GetSchema(vector<LogicalType> &return_types, vector<Identifier> &names) {
		names.emplace_back("column_name");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("column_type");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("null");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("key");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("default");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("extra");
		return_types.emplace_back(LogicalType::VARCHAR);
	}

	static void GetTableColumns(const ColumnDefinition &column, ColumnConstraintInfo constraint_info,
	                            DataChunk &output) {
		// "column_name", VARCHAR
		output.data[0].Append(Value(column.Name()));
		// "column_type", VARCHAR
		output.data[1].Append(Value(column.Type().ToString()));
		// "null", VARCHAR
		output.data[2].Append(Value(constraint_info.not_null ? "NO" : "YES"));
		// "key", VARCHAR
		Value key;
		if (constraint_info.pk || constraint_info.unique) {
			key = Value(constraint_info.pk ? "PRI" : "UNI");
		}
		output.data[3].Append(key);
		// "default", VARCHAR
		output.data[4].Append(DefaultValue(column));
		// "extra", VARCHAR
		output.data[5].Append(Value());
	}

	static void GetViewColumns(idx_t i, const Identifier &name, const LogicalType &type, DataChunk &output) {
		// "column_name", VARCHAR
		output.data[0].Append(Value(name));
		// "column_type", VARCHAR
		output.data[1].Append(Value(type.ToString()));
		// "null", VARCHAR
		output.data[2].Append(Value("YES"));
		// "key", VARCHAR
		output.data[3].Append(Value());
		// "default", VARCHAR
		output.data[4].Append(Value());
		// "extra", VARCHAR
		output.data[5].Append(Value());
	}
};

template <bool IS_PRAGMA_TABLE_INFO>
static unique_ptr<FunctionData> PragmaTableInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (IS_PRAGMA_TABLE_INFO) {
		PragmaTableInfoHelper::GetSchema(return_types, names);
	} else {
		PragmaShowHelper::GetSchema(return_types, names);
	}

	QualifiedName qname;
	string error;
	if (!QualifiedName::TryParse(input.inputs[0].GetValue<string>(), qname, error)) {
		throw ParserException(error);
	}

	// look up the table name in the catalog
	CatalogEntryRetriever retriever(context);
	qname = Binder::BindTableName(retriever, qname);
	auto &entry = Catalog::GetEntry(context, CatalogType::TABLE_ENTRY, qname);
	Binder::RegisterEntryRead(input.binder, context, entry);
	return make_uniq<PragmaTableFunctionData>(entry, IS_PRAGMA_TABLE_INFO);
}

unique_ptr<GlobalTableFunctionState> PragmaTableInfoInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<PragmaTableOperatorData>();
}

static ColumnConstraintInfo CheckConstraints(TableCatalogEntry &table, const ColumnDefinition &column) {
	ColumnConstraintInfo result;
	// check all constraints
	for (auto &constraint : table.GetConstraints()) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &not_null = constraint->Cast<NotNullConstraint>();
			if (not_null.index == column.Logical()) {
				result.not_null = true;
			}
			break;
		}
		case ConstraintType::UNIQUE: {
			auto &unique = constraint->Cast<UniqueConstraint>();
			bool &constraint_info = unique.IsPrimaryKey() ? result.pk : result.unique;
			if (unique.HasIndex()) {
				if (unique.GetIndex() == column.Logical()) {
					constraint_info = true;
				}
			} else {
				auto &columns = unique.GetColumnNames();
				if (std::find(columns.begin(), columns.end(), column.GetName()) != columns.end()) {
					constraint_info = true;
				}
			}
			break;
		}
		default:
			break;
		}
	}
	return result;
}

void PragmaTableInfo::GetColumnInfo(TableCatalogEntry &table, const ColumnDefinition &column, DataChunk &output) {
	auto constraint_info = CheckConstraints(table, column);
	PragmaShowHelper::GetTableColumns(column, constraint_info, output);
}

static void PragmaTableInfoTable(PragmaTableOperatorData &data, TableCatalogEntry &table, DataChunk &output,
                                 bool is_table_info) {
	if (data.offset >= table.GetColumns().LogicalColumnCount()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t next = MinValue<idx_t>(data.offset + STANDARD_VECTOR_SIZE, table.GetColumns().LogicalColumnCount());

	for (idx_t i = data.offset; i < next; i++) {
		auto &column = table.GetColumn(LogicalIndex(i));
		D_ASSERT(column.Oid() < (idx_t)NumericLimits<int32_t>::Maximum());
		auto constraint_info = CheckConstraints(table, column);

		if (is_table_info) {
			PragmaTableInfoHelper::GetTableColumns(column, constraint_info, output);
		} else {
			PragmaShowHelper::GetTableColumns(column, constraint_info, output);
		}
	}
	data.offset = next;
}

static void PragmaTableInfoView(ClientContext &context, PragmaTableOperatorData &data, ViewCatalogEntry &view,
                                DataChunk &output, bool is_table_info) {
	// force rebind the view to ensure the names / types are up to date
	view.BindView(context, BindViewAction::FORCE_REBIND);
	auto columns = view.GetColumnInfo();
	auto &view_names = columns->names;
	auto &view_types = columns->types;
	if (data.offset >= view_types.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t next = MinValue<idx_t>(data.offset + STANDARD_VECTOR_SIZE, view_types.size());

	for (idx_t i = data.offset; i < next; i++) {
		auto type = view_types[i];
		auto name = i < view.aliases.size() ? view.aliases[i] : view_names[i];

		if (is_table_info) {
			PragmaTableInfoHelper::GetViewColumns(i, name, type, output);
		} else {
			PragmaShowHelper::GetViewColumns(i, name, type, output);
		}
	}
	data.offset = next;
}

static void PragmaTableInfoFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<PragmaTableFunctionData>();
	auto &state = data_p.global_state->Cast<PragmaTableOperatorData>();
	switch (bind_data.entry.type) {
	case CatalogType::TABLE_ENTRY:
		PragmaTableInfoTable(state, bind_data.entry.Cast<TableCatalogEntry>(), output, bind_data.is_table_info);
		break;
	case CatalogType::VIEW_ENTRY:
		PragmaTableInfoView(context, state, bind_data.entry.Cast<ViewCatalogEntry>(), output, bind_data.is_table_info);
		break;
	default:
		throw NotImplementedException("Unimplemented catalog type for pragma_table_info");
	}
}

void PragmaTableInfo::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("pragma_table_info",
	                              FunctionSignature().AddPositionalOnly("table_name", LogicalType::VARCHAR),
	                              PragmaTableInfoFunction, PragmaTableInfoBind<true>, PragmaTableInfoInit));
	set.AddFunction(TableFunction("pragma_show",
	                              FunctionSignature().AddPositionalOnly("table_name", LogicalType::VARCHAR),
	                              PragmaTableInfoFunction, PragmaTableInfoBind<false>, PragmaTableInfoInit));
}

} // namespace duckdb
