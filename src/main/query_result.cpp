#include <list>
#include "duckdb/main/query_result.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/arrow.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

QueryResult::QueryResult(QueryResultType type, StatementType statement_type)
    : type(type), statement_type(statement_type), success(true) {
}

QueryResult::QueryResult(QueryResultType type, StatementType statement_type, vector<LogicalType> types_p,
                         vector<string> names_p)
    : type(type), statement_type(statement_type), types(move(types_p)), names(move(names_p)), success(true) {
	D_ASSERT(types.size() == names.size());
}

QueryResult::QueryResult(QueryResultType type, string error) : type(type), success(false), error(move(error)) {
}

unique_ptr<DataChunk> QueryResult::Fetch() {
	auto chunk = FetchRaw();
	if (!chunk) {
		return nullptr;
	}
	chunk->Normalify();
	return chunk;
}

bool QueryResult::Equals(QueryResult &other) {
	// first compare the success state of the results
	if (success != other.success) {
		return false;
	}
	if (!success) {
		return error == other.error;
	}
	// compare names
	if (names != other.names) {
		return false;
	}
	// compare types
	if (types != other.types) {
		return false;
	}
	// now compare the actual values
	// fetch chunks
	while (true) {
		auto lchunk = Fetch();
		auto rchunk = other.Fetch();
		if (!lchunk && !rchunk) {
			return true;
		}
		if (!lchunk || !rchunk) {
			return false;
		}
		if (lchunk->size() == 0 && rchunk->size() == 0) {
			return true;
		}
		if (lchunk->size() != rchunk->size()) {
			return false;
		}
		D_ASSERT(lchunk->ColumnCount() == rchunk->ColumnCount());
		for (idx_t col = 0; col < rchunk->ColumnCount(); col++) {
			for (idx_t row = 0; row < rchunk->size(); row++) {
				auto lvalue = lchunk->GetValue(col, row);
				auto rvalue = rchunk->GetValue(col, row);
				if (lvalue != rvalue) {
					return false;
				}
			}
		}
	}
}

void QueryResult::Print() {
	Printer::Print(ToString());
}

string QueryResult::HeaderToString() {
	string result;
	for (auto &name : names) {
		result += name + "\t";
	}
	result += "\n";
	for (auto &type : types) {
		result += type.ToString() + "\t";
	}
	result += "\n";
	return result;
}

struct DuckDBArrowSchemaHolder {
	// unused in children
	vector<ArrowSchema> children;
	// unused in children
	vector<ArrowSchema *> children_ptrs;
	//! used for nested structures
	std::list<std::vector<ArrowSchema>> nested_children;
	std::list<std::vector<ArrowSchema *>> nested_children_ptr;
	//! This holds strings created to represent decimal types
	vector<unique_ptr<char[]>> owned_type_names;
};

static void ReleaseDuckDBArrowSchema(ArrowSchema *schema) {
	if (!schema || !schema->release) {
		return;
	}
	schema->release = nullptr;
	auto holder = static_cast<DuckDBArrowSchemaHolder *>(schema->private_data);
	delete holder;
}

void InitializeChild(ArrowSchema &child, const string &name = "") {
	//! Child is cleaned up by parent
	child.private_data = nullptr;
	child.release = ReleaseDuckDBArrowSchema;

	//! Store the child schema
	child.flags = ARROW_FLAG_NULLABLE;
	child.name = name.c_str();
	child.n_children = 0;
	child.children = nullptr;
	child.flags = 0;
	child.metadata = nullptr;
	child.dictionary = nullptr;
}
void SetArrowFormat(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &child, const LogicalType &type);

void SetArrowMapFormat(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &child, const LogicalType &type) {
	child.format = "+m";
	//! Map has one child which is a struct
	child.n_children = 1;
	root_holder.nested_children.emplace_back();
	root_holder.nested_children.back().resize(1);
	root_holder.nested_children_ptr.emplace_back();
	root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
	InitializeChild(root_holder.nested_children.back()[0]);
	child.children = &root_holder.nested_children_ptr.back()[0];
	child.children[0]->name = "entries";
	child_list_t<LogicalType> struct_child_types;
	struct_child_types.push_back(std::make_pair("key", ListType::GetChildType(StructType::GetChildType(type, 0))));
	struct_child_types.push_back(std::make_pair("value", ListType::GetChildType(StructType::GetChildType(type, 1))));
	auto struct_type = LogicalType::STRUCT(move(struct_child_types));
	SetArrowFormat(root_holder, *child.children[0], struct_type);
}

void SetArrowFormat(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &child, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		child.format = "b";
		break;
	case LogicalTypeId::TINYINT:
		child.format = "c";
		break;
	case LogicalTypeId::SMALLINT:
		child.format = "s";
		break;
	case LogicalTypeId::INTEGER:
		child.format = "i";
		break;
	case LogicalTypeId::BIGINT:
		child.format = "l";
		break;
	case LogicalTypeId::UTINYINT:
		child.format = "C";
		break;
	case LogicalTypeId::USMALLINT:
		child.format = "S";
		break;
	case LogicalTypeId::UINTEGER:
		child.format = "I";
		break;
	case LogicalTypeId::UBIGINT:
		child.format = "L";
		break;
	case LogicalTypeId::FLOAT:
		child.format = "f";
		break;
	case LogicalTypeId::HUGEINT:
		child.format = "d:38,0";
		break;
	case LogicalTypeId::DOUBLE:
		child.format = "g";
		break;
	case LogicalTypeId::VARCHAR:
		child.format = "u";
		break;
	case LogicalTypeId::DATE:
		child.format = "tdD";
		break;
	case LogicalTypeId::TIME:
		child.format = "ttu";
		break;
	case LogicalTypeId::TIMESTAMP:
		child.format = "tsu:";
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
		child.format = "tss:";
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		child.format = "tsn:";
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		child.format = "tsm:";
		break;
	case LogicalTypeId::INTERVAL:
		child.format = "tDm";
		break;
	case LogicalTypeId::DECIMAL: {
		uint8_t width, scale;
		type.GetDecimalProperties(width, scale);
		string format = "d:" + to_string(width) + "," + to_string(scale);
		unique_ptr<char[]> format_ptr = unique_ptr<char[]>(new char[format.size() + 1]);
		for (size_t i = 0; i < format.size(); i++) {
			format_ptr[i] = format[i];
		}
		format_ptr[format.size()] = '\0';
		root_holder.owned_type_names.push_back(move(format_ptr));
		child.format = root_holder.owned_type_names.back().get();
		break;
	}
	case LogicalTypeId::SQLNULL: {
		child.format = "n";
		break;
	}
	case LogicalTypeId::BLOB: {
		child.format = "z";
		break;
	}
	case LogicalTypeId::LIST: {
		child.format = "+l";
		child.n_children = 1;
		root_holder.nested_children.emplace_back();
		root_holder.nested_children.back().resize(1);
		root_holder.nested_children_ptr.emplace_back();
		root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
		InitializeChild(root_holder.nested_children.back()[0]);
		child.children = &root_holder.nested_children_ptr.back()[0];
		child.children[0]->name = "l";
		SetArrowFormat(root_holder, **child.children, ListType::GetChildType(type));
		break;
	}
	case LogicalTypeId::STRUCT: {
		child.format = "+s";
		auto &child_types = StructType::GetChildTypes(type);
		child.n_children = child_types.size();
		root_holder.nested_children.emplace_back();
		root_holder.nested_children.back().resize(child_types.size());
		root_holder.nested_children_ptr.emplace_back();
		root_holder.nested_children_ptr.back().resize(child_types.size());
		for (idx_t type_idx = 0; type_idx < child_types.size(); type_idx++) {
			root_holder.nested_children_ptr.back()[type_idx] = &root_holder.nested_children.back()[type_idx];
		}
		child.children = &root_holder.nested_children_ptr.back()[0];
		for (size_t type_idx = 0; type_idx < child_types.size(); type_idx++) {
			InitializeChild(*child.children[type_idx]);

			auto &struct_col_name = child_types[type_idx].first;
			unique_ptr<char[]> name_ptr = unique_ptr<char[]>(new char[struct_col_name.size() + 1]);
			for (size_t i = 0; i < struct_col_name.size(); i++) {
				name_ptr[i] = struct_col_name[i];
			}
			name_ptr[struct_col_name.size()] = '\0';
			root_holder.owned_type_names.push_back(move(name_ptr));

			child.children[type_idx]->name = root_holder.owned_type_names.back().get();
			SetArrowFormat(root_holder, *child.children[type_idx], child_types[type_idx].second);
		}
		break;
	}
	case LogicalTypeId::MAP: {
		SetArrowMapFormat(root_holder, child, type);
		break;
	}
	default:
		throw InternalException("Unsupported Arrow type " + type.ToString());
	}
}

void QueryResult::ToArrowSchema(ArrowSchema *out_schema) {
	D_ASSERT(out_schema);

	// Allocate as unique_ptr first to cleanup properly on error
	auto root_holder = make_unique<DuckDBArrowSchemaHolder>();

	// Allocate the children
	root_holder->children.resize(ColumnCount());
	root_holder->children_ptrs.resize(ColumnCount(), nullptr);
	for (size_t i = 0; i < ColumnCount(); ++i) {
		root_holder->children_ptrs[i] = &root_holder->children[i];
	}
	out_schema->children = root_holder->children_ptrs.data();
	out_schema->n_children = ColumnCount();

	// Store the schema
	out_schema->format = "+s"; // struct apparently
	out_schema->flags = 0;
	out_schema->metadata = nullptr;
	out_schema->name = "duckdb_query_result";
	out_schema->dictionary = nullptr;

	// Configure all child schemas
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		auto &child = root_holder->children[col_idx];
		InitializeChild(child, names[col_idx]);
		SetArrowFormat(*root_holder, child, types[col_idx]);
	}

	// Release ownership to caller
	out_schema->private_data = root_holder.release();
	out_schema->release = ReleaseDuckDBArrowSchema;
}

} // namespace duckdb
