#include "core_functions/scalar/debug_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {

namespace {

constexpr idx_t INDEX_KEY_FIXED_ARGS = 1;

//! Selects either a user-defined index by name, or a constraint index by constraint type and columns.
struct IndexKeyPath {
	QualifiedName table;
	Identifier index_name;
	IndexConstraintType constraint_type = IndexConstraintType::NONE;
	vector<Identifier> columns;
};

IndexConstraintType ParseConstraintType(const string &value) {
	if (StringUtil::CIEquals(value, "primary")) {
		return IndexConstraintType::PRIMARY;
	}
	if (StringUtil::CIEquals(value, "unique")) {
		return IndexConstraintType::UNIQUE;
	}
	if (StringUtil::CIEquals(value, "foreign")) {
		return IndexConstraintType::FOREIGN;
	}
	throw BinderException("index_key: constraint must be one of 'primary', 'unique' or 'foreign'");
}

IndexKeyPath EvaluateIndexKeyPath(const Value &input_struct) {
	if (input_struct.IsNull()) {
		throw BinderException("index_key: path parameter cannot be NULL");
	}
	auto &field_types = StructType::GetChildTypes(input_struct.type());
	auto &field_values = StructValue::GetChildren(input_struct);

	unordered_map<string, string> fields;
	fields["catalog"] = INVALID_CATALOG;
	fields["schema"] = DEFAULT_SCHEMA;
	fields["table"] = "";
	fields["index"] = "";
	fields["constraint"] = "";

	IndexKeyPath path;
	for (idx_t i = 0; i < field_types.size(); i++) {
		auto field_name = StringUtil::Lower(field_types[i].first.GetIdentifierName());
		auto &field_value = field_values[i];
		if (field_value.IsNull()) {
			throw BinderException("index_key: path field %s cannot be NULL", field_types[i].first);
		}

		if (field_name == "columns") {
			if (field_value.type() != LogicalType::LIST(LogicalType::VARCHAR)) {
				throw BinderException("index_key: path field %s must be VARCHAR[]", field_types[i].first);
			}
			for (auto &column : ListValue::GetChildren(field_value)) {
				if (column.IsNull()) {
					throw BinderException("index_key: path field %s cannot contain NULL", field_types[i].first);
				}
				path.columns.emplace_back(StringValue::Get(column));
			}
			continue;
		}

		if (fields.find(field_name) == fields.end()) {
			throw BinderException("index_key: unknown field %s in path", field_types[i].first);
		}
		if (field_value.type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("index_key: path field %s must be VARCHAR", field_types[i].first);
		}
		auto value = StringValue::Get(field_value);
		if (value.empty()) {
			throw BinderException("index_key: path field %s cannot be empty", field_types[i].first);
		}
		fields[field_name] = value;
	}

	if (fields["table"].empty()) {
		throw BinderException("index_key: path must contain a 'table' field");
	}
	if (fields["index"].empty() == fields["constraint"].empty()) {
		throw BinderException("index_key: path must contain exactly one of the 'index' or 'constraint' fields");
	}
	path.table =
	    QualifiedName(Identifier(fields["catalog"]), Identifier(fields["schema"]), Identifier(fields["table"]));

	if (!fields["index"].empty()) {
		if (!path.columns.empty()) {
			throw BinderException("index_key: path field 'columns' requires a 'constraint' field");
		}
		path.index_name = Identifier(fields["index"]);
		return path;
	}

	path.constraint_type = ParseConstraintType(fields["constraint"]);
	return path;
}

//! User-defined indexes are catalog entries and constraint indexes are not, so their names cannot clash.
idx_t FindUserIndexOid(ClientContext &context, TableCatalogEntry &table_entry, const Identifier &index_name,
                       const string &qualified_table) {
	auto &schema = table_entry.schema;
	auto entry = schema.GetEntry(schema.GetCatalogTransaction(context), CatalogType::INDEX_ENTRY, index_name);
	if (!entry || entry->Cast<IndexCatalogEntry>().GetTableName() != table_entry.name) {
		throw CatalogException("index_key: index %s was not found on table %s", index_name, qualified_table);
	}
	return entry->oid;
}

idx_t FindConstraintIndexOid(TableCatalogEntry &table_entry, const IndexKeyPath &path, const string &qualified_table) {
	auto &table_columns = table_entry.GetColumns();
	vector<idx_t> index_oids;
	for (auto &constraint : table_entry.GetConstraints()) {
		IndexConstraintType constraint_type;
		vector<Identifier> constraint_columns;
		if (constraint->type == ConstraintType::UNIQUE) {
			auto &unique = constraint->Cast<UniqueConstraint>();
			constraint_type = unique.GetIndexConstraintType();
			for (auto &logical_index : unique.GetLogicalIndexes(table_columns)) {
				constraint_columns.push_back(table_columns.GetColumn(logical_index).Name());
			}
		} else if (constraint->type == ConstraintType::FOREIGN_KEY) {
			auto &foreign_key = constraint->Cast<ForeignKeyConstraint>();
			// The referenced side of a foreign key uses the referenced key's index.
			if (!foreign_key.info.IsAppendConstraint()) {
				continue;
			}
			constraint_type = IndexConstraintType::FOREIGN;
			for (auto &physical_index : foreign_key.info.fk_keys) {
				constraint_columns.push_back(table_columns.GetColumn(physical_index).Name());
			}
		} else {
			continue;
		}

		if (constraint_type == path.constraint_type && (path.columns.empty() || path.columns == constraint_columns)) {
			index_oids.push_back(constraint->GetBackingIndexOid());
		}
	}
	if (index_oids.empty()) {
		throw CatalogException("index_key: %s constraint was not found on table %s",
		                       EnumUtil::ToString(path.constraint_type), qualified_table);
	}
	if (index_oids.size() > 1) {
		throw BinderException("index_key: table %s has multiple %s constraints, specify 'columns' to select one",
		                      qualified_table, EnumUtil::ToString(path.constraint_type));
	}
	return index_oids[0];
}

struct IndexKeyBindData : public FunctionData {
	IndexKeyBindData(shared_ptr<IndexEntry> index_entry, vector<LogicalType> key_types)
	    : index_entry(std::move(index_entry)), key_types(std::move(key_types)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<IndexKeyBindData>(index_entry, key_types);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<IndexKeyBindData>();
		return index_entry.get() == other.index_entry.get() && key_types == other.key_types;
	}

	shared_ptr<IndexEntry> index_entry;
	vector<LogicalType> key_types;
};

// index_key resolves index metadata during binding.
// Register the owning catalog so cached prepared statements rebind after catalog changes.
void RegisterIndexKeyRead(BindScalarFunctionInput &input, TableCatalogEntry &table_entry) {
	if (input.HasBinder()) {
		auto &binder = input.GetBinder();
		binder.GetStatementProperties().RegisterDBRead(table_entry.ParentCatalog(), input.GetClientContext());
	}
}

void RegisterIndexKeyRead(ResolveScalarFunctionTypesInput &input, TableCatalogEntry &table_entry) {
}

//! Called from both resolve_types and bind - the key types and the bind data both come from the index
template <class INPUT>
unique_ptr<FunctionData> IndexKeyBind(INPUT &input) {
	ClientContext &context = input.GetClientContext();
	BoundScalarFunction &bound_function = input.GetBoundFunction();
	const vector<unique_ptr<Expression>> &arguments = input.GetArguments();
	if (arguments.size() < INDEX_KEY_FIXED_ARGS) {
		throw BinderException("index_key: requires a path (STRUCT) argument");
	}

	const Value path_value = input.GetConstant(0);
	auto path = EvaluateIndexKeyPath(path_value);

	auto qualified_table = path.table.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context, path.table);
	auto duck_table = table_entry.TryGetDuckTableEntry();
	if (!duck_table) {
		throw BinderException("index_key: table '%s' is not a DuckDB table", qualified_table);
	}

	RegisterIndexKeyRead(input, table_entry);
	auto &data_table = duck_table->GetStorage();
	auto &data_table_info = *data_table.GetDataTableInfo();

	// Note: It may come up in testing that we don't want to force binding here, e.g. if the test should explicitly
	// exercise a code path that binds, not forcing a bind in testing code here. In that case we may want to add an
	// option to this function to bind or not.
	data_table_info.BindIndexes(context);

	auto index_oid = path.constraint_type == IndexConstraintType::NONE
	                     ? FindUserIndexOid(context, table_entry, path.index_name, qualified_table)
	                     : FindConstraintIndexOid(table_entry, path, qualified_table);
	const auto index_entry = data_table_info.GetIndexes().FindEntry(index_oid);
	if (!index_entry) {
		throw CatalogException("index_key: no index found in storage for index oid %llu on table %s", index_oid,
		                       qualified_table);
	}
	const auto index = index_entry->GetReadHandle<BoundIndex>();

	const auto &index_type = index->GetIndexType();
	if (index_type != ART::TYPE_NAME) {
		throw NotImplementedException(
		    "index_key: index type '%s' is not yet supported (only ART indexes are supported)", index_type);
	}

	auto key_types = index->GetLogicalTypes();

	idx_t num_key_args = arguments.size() - INDEX_KEY_FIXED_ARGS;
	if (num_key_args != key_types.size()) {
		auto index_description =
		    path.constraint_type == IndexConstraintType::NONE
		        ? StringUtil::Format("index %s", path.index_name)
		        : StringUtil::Format("%s constraint index", EnumUtil::ToString(path.constraint_type));
		throw BinderException("index_key: %s on table %s expects %llu key column(s), but %llu argument(s) provided",
		                      index_description, qualified_table, key_types.size(), num_key_args);
	}

	// Set bound_function.GetArguments() to actual types for proper casting.
	// Note: In case this is ever a bottleneck for testing purposes -- currently, we retain the path argument
	// for execution even though it is only required for binding. This requires us to create a key_chunk
	// that only references the key columns during execution. We could erase the path argument here, but
	// that also requires some (de)serialization boilerplate, so for now we don't do it.
	bound_function.GetArguments().clear();
	bound_function.GetArguments().push_back(arguments[0]->GetReturnType());
	for (auto &key_type : key_types) {
		bound_function.GetArguments().push_back(key_type);
	}

	return make_uniq<IndexKeyBindData>(index_entry, std::move(key_types));
}

void IndexKeyResolveTypes(ResolveScalarFunctionTypesInput &input) {
	IndexKeyBind(input);
}

void IndexKeyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.BindInfo()->Cast<IndexKeyBindData>();

	idx_t count = args.size();

	// Create a DataChunk referencing only the key columns (skip the path).
	DataChunk key_chunk;
	key_chunk.InitializeEmpty(bind_data.key_types);
	for (idx_t i = 0; i < bind_data.key_types.size(); i++) {
		key_chunk.data[i].Reference(args.data[INDEX_KEY_FIXED_ARGS + i]);
	}

	const auto art = bind_data.index_entry->GetReadHandle<ART>();
	unsafe_vector<ARTKey> keys(count);
	ArenaAllocator allocator(Allocator::DefaultAllocator());
	art->GenerateKeys(allocator, key_chunk, keys);

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto &key = keys[i];
		if (key.Empty()) {
			result_data.WriteNull();
		} else {
			result_data.WriteValue(string_t(const_char_ptr_cast(key.data), key.len));
		}
	}
	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
	result.Verify();
}

} // namespace

ScalarFunction IndexKeyFun::GetFunction() {
	ScalarFunction fun("index_key", {}, LogicalType::BLOB, IndexKeyFunction, IndexKeyBind<BindScalarFunctionInput>);
	fun.GetSignature().AddParameter("path", LogicalTypeId::STRUCT);
	fun.GetSignature().AddArgs("args", LogicalTypeId::ANY);
	fun.SetResolveTypesCallback(IndexKeyResolveTypes);
	return fun;
}

} // namespace duckdb
