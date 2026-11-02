#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/multi_file/multi_file_column_mapper.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/multi_file/union_by_name.hpp"
#include <algorithm>

namespace duckdb {

constexpr column_t MultiFileReader::COLUMN_IDENTIFIER_FILENAME;
constexpr column_t MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER;
constexpr column_t MultiFileReader::COLUMN_IDENTIFIER_FILE_INDEX;
constexpr int32_t MultiFileReader::ORDINAL_FIELD_ID;
constexpr int32_t MultiFileReader::FILENAME_FIELD_ID;
constexpr int32_t MultiFileReader::ROW_ID_FIELD_ID;
constexpr int32_t MultiFileReader::LAST_UPDATED_SEQUENCE_NUMBER_ID;

MultiFileReaderGlobalState::~MultiFileReaderGlobalState() {
}

MultiFileReader::~MultiFileReader() {
}

unique_ptr<MultiFileReader> MultiFileReader::Create(const BoundTableFunction &table_function) {
	unique_ptr<MultiFileReader> res;
	if (table_function.get_multi_file_reader) {
		res = table_function.get_multi_file_reader(table_function);
		res->function_name = table_function.GetName();
	} else {
		res = make_uniq<MultiFileReader>();
		res->function_name = table_function.GetName();
	}
	return res;
}

unique_ptr<MultiFileReader> MultiFileReader::Copy() const {
	return CreateDefault(function_name.GetIdentifierName());
}

MultiFileBindData::~MultiFileBindData() {
}

unique_ptr<FunctionData> MultiFileBindData::Copy() const {
	auto result = make_uniq<MultiFileBindData>();
	if (bind_data) {
		result->bind_data = unique_ptr_cast<FunctionData, TableFunctionData>(bind_data->Copy());
	}
	result->file_list = file_list->Copy();
	result->multi_file_reader = multi_file_reader->Copy();
	result->interface = interface->Copy();
	result->columns = columns;
	result->reader_bind = reader_bind;
	result->file_options = file_options;
	result->types = types;
	result->names = names;
	result->virtual_columns = virtual_columns;
	result->table_columns = table_columns;
	return std::move(result);
}

bool MultiFileBindData::IsGeneratedColumn(idx_t column_idx) const {
	if ((reader_bind.filename_idx.IsValid() && reader_bind.filename_idx.GetIndex() == column_idx) ||
	    (reader_bind.file_row_number_idx.IsValid() && reader_bind.file_row_number_idx.GetIndex() == column_idx)) {
		return true;
	}
	for (const auto &partition : reader_bind.hive_partitioning_indexes) {
		if (partition.index == column_idx) {
			return true;
		}
	}
	return false;
}

bool MultiFileBindData::MatchesPhysicalColumn(idx_t column_idx, const Identifier &physical_name,
                                              const LogicalType &physical_type) const {
	return column_idx < names.size() && column_idx < types.size() && !IsGeneratedColumn(column_idx) &&
	       names[column_idx] == physical_name && types[column_idx].EqualsIncludingCollation(physical_type);
}

bool MultiFileBindData::HasIdentityMapping(const vector<MultiFileColumnDefinition> &physical_columns) const {
	if (physical_columns.size() != names.size() || physical_columns.size() != types.size()) {
		return false;
	}
	for (idx_t i = 0; i < physical_columns.size(); i++) {
		if (!MatchesPhysicalColumn(i, physical_columns[i].name, physical_columns[i].type)) {
			return false;
		}
	}
	return true;
}

unique_ptr<MultiFileReader> MultiFileReader::CreateDefault(const string &function_name) {
	auto res = make_uniq<MultiFileReader>();
	res->function_name = Identifier(function_name);
	return res;
}

Value MultiFileReader::CreateValueFromFileList(const vector<string> &file_list) {
	vector<Value> files;
	for (auto &file : file_list) {
		files.push_back(file);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(files));
}

void MultiFileReader::AddParameters(TableFunction &table_function, MultiFileParameters which) {
	auto add_options = [&](TypedKwargs &options) {
		if (which == MultiFileParameters::ALL) {
			// "filename" is a boolean, or the name of the column to hold the file name
			options.Add("filename", LogicalType::ANY)
			    .Add("hive_partitioning", LogicalType::BOOLEAN)
			    .Add("union_by_name", LogicalType::BOOLEAN)
			    .Add("hive_types", LogicalType::ANY)
			    .Add("hive_types_autocast", LogicalType::BOOLEAN)
			    .Add("schema", LogicalTypeId::ANY)
			    .Add("file_row_number", LogicalType::BOOLEAN)
			    .Add("column_statistics", LogicalTypeId::ANY);
		}
		options.Add("allow_empty", LogicalType::BOOLEAN);
	};
	// a reader can declare its own options before or after these
	auto &signature = table_function.GetSignature();
	if (signature.GetTypedKwargs()) {
		signature.ExtendTypedKwargs(add_options);
	} else {
		signature.WithTypedKwargs("options", add_options);
	}
}

MultiFileColumnDefinition MultiFileColumnDefinition::CreateNested(const Identifier &name, const LogicalType &type) {
	MultiFileColumnDefinition result(name, type);
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			result.children.push_back(CreateNested(child.first, child.second));
		}
		break;
	case LogicalTypeId::LIST:
		result.children.push_back(CreateNested(Identifier("list"), ListType::GetChildType(type)));
		break;
	case LogicalTypeId::ARRAY:
		result.children.push_back(CreateNested(Identifier("list"), ArrayType::GetChildType(type)));
		break;
	case LogicalTypeId::MAP: {
		// the multi-file reader expects the entries of a MAP as a "key_value" STRUCT of the keys and the values
		MultiFileColumnDefinition key_value(Identifier("key_value"), ListType::GetChildType(type));
		key_value.children.push_back(CreateNested(Identifier("key"), MapType::KeyType(type)));
		key_value.children.push_back(CreateNested(Identifier("value"), MapType::ValueType(type)));
		result.children.push_back(std::move(key_value));
		break;
	}
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			result.children.push_back(
			    CreateNested(UnionType::GetMemberName(type, i), UnionType::GetMemberType(type, i)));
		}
		break;
	default:
		break;
	}
	return result;
}

MultiFileColumnDefinition &MultiFileColumnDefinition::ResolveChildPath(const vector<idx_t> &child_path) {
	reference<MultiFileColumnDefinition> current(*this);
	for (auto child_index : child_path) {
		auto &definition = current.get();
		if (definition.type.id() == LogicalTypeId::MAP) {
			// the keys and the values are the children of the "key_value" entry
			if (child_index > 1) {
				throw InvalidInputException("A MAP has two children: its keys (0) and its values (1), not %llu",
				                            child_index);
			}
			current = definition.children[0].children[child_index];
			continue;
		}
		if (child_index >= definition.children.size()) {
			throw InvalidInputException("Child index %llu is out of range for a field of type %s", child_index,
			                            definition.type.ToString());
		}
		current = definition.children[child_index];
	}
	return current.get();
}

OpenFileInfo MultiFileReader::ParseFileEntry(const Value &input) {
	return OpenFileInfo::FromValue(input, function_name);
}

vector<OpenFileInfo> MultiFileReader::ParseFileList(const Value &input) {
	if (input.IsNull()) {
		throw ParserException("%s cannot take NULL list as parameter", function_name);
	}
	if (input.type().id() != LogicalTypeId::LIST) {
		return {ParseFileEntry(input)};
	}
	vector<OpenFileInfo> files;
	for (auto &val : ListValue::GetChildren(input)) {
		files.push_back(ParseFileEntry(val));
	}
	return files;
}

shared_ptr<MultiFileList> MultiFileReader::CreateFileList(ClientContext &context, const vector<string> &paths,
                                                          const FileGlobInput &glob_input) {
	auto res = make_uniq<GlobMultiFileList>(context, paths, glob_input);
	if (res->GetExpandResult() == FileExpandResult::NO_FILES && !glob_input.AllowsEmpty()) {
		throw IOException("%s needs at least one file to read", function_name);
	}
	return std::move(res);
}

shared_ptr<MultiFileList> MultiFileReader::CreateFileList(ClientContext &context, vector<OpenFileInfo> files,
                                                          const FileGlobInput &glob_input) {
	bool has_open_options = false;
	for (auto &file : files) {
		if (file.extended_info) {
			has_open_options = true;
			break;
		}
	}
	if (!has_open_options) {
		// no per-file open options - dispatch to the path based method so any overrides of it are used
		vector<string> paths;
		paths.reserve(files.size());
		for (auto &file : files) {
			paths.push_back(std::move(file.path));
		}
		return CreateFileList(context, paths, glob_input);
	}
	auto res = make_uniq<GlobMultiFileList>(context, std::move(files), glob_input);
	if (res->GetExpandResult() == FileExpandResult::NO_FILES && !glob_input.AllowsEmpty()) {
		throw IOException("%s needs at least one file to read", function_name);
	}
	return std::move(res);
}

shared_ptr<MultiFileList> MultiFileReader::CreateFileList(ClientContext &context, const Value &input,
                                                          const FileGlobInput &glob_input) {
	return CreateFileList(context, ParseFileList(input), glob_input);
}

static void VerifySchemaDefinitionType(const LogicalType &definition_type, bool is_root) {
	if (definition_type.id() != LogicalTypeId::STRUCT) {
		if (is_root) {
			throw InvalidInputException("'schema' expects a STRUCT as the value type of the map");
		}
		throw BinderException("'schema' 'children' expects a STRUCT as the value type of the map, not %s",
		                      definition_type.ToString());
	}
	auto &fields = StructType::GetChildTypes(definition_type);
	if (fields.size() != 3 && fields.size() != 4) {
		throw InvalidInputException(
		    "'schema' expects the STRUCT to have 3 or 4 fields, 'name', 'type', 'default_value' and optionally "
		    "'children', not %d",
		    fields.size());
	}
	if (fields[0].first != "name") {
		throw InvalidInputException("'schema' expects the first field of the struct to be called 'name'");
	}
	if (fields[0].second.id() != LogicalTypeId::VARCHAR) {
		throw InvalidInputException("'schema' expects the 'name' field to be of type VARCHAR, not %s",
		                            LogicalTypeIdToString(fields[0].second.id()));
	}
	if (fields[1].first != "type") {
		throw InvalidInputException("'schema' expects the second field of the struct to be called 'type'");
	}
	if (fields[1].second.id() != LogicalTypeId::VARCHAR) {
		throw InvalidInputException("'schema' expects the 'type' field to be of type VARCHAR, not %s",
		                            LogicalTypeIdToString(fields[1].second.id()));
	}
	if (fields[2].first != "default_value") {
		throw InvalidInputException("'schema' expects the third field of the struct to be called 'default_value'");
	}
	if (fields.size() == 4 && fields[3].first != "children") {
		throw InvalidInputException("'schema' expects the fourth field of the struct to be called 'children'");
	}
}

static void VerifySchemaChildType(const MultiFileColumnDefinition &column, const MultiFileColumnDefinition &child,
                                  const string &expected_name, const LogicalType &expected_type) {
	auto child_name = child.name.GetIdentifierName();
	const bool name_equivalent = child_name == expected_name;
	const bool type_equivalent = child.type == expected_type;
	if (name_equivalent && type_equivalent) {
		return;
	}
	string error;
	if (!name_equivalent) {
		error = StringUtil::Format("name \"%s\" (got \"%s\")", expected_name, child_name);
	}
	if (!type_equivalent) {
		const bool name_mentioned = !error.empty();
		if (name_mentioned) {
			error += " and ";
		} else {
			error += StringUtil::Format("name \"%s\" to have ", expected_name);
		}
		error += StringUtil::Format("type \"%s\" (got \"%s\")", expected_type.ToString(), child.type.ToString());
	}
	throw BinderException("'schema' column \"%s\" expects a child with %s", column.name.GetIdentifierName(), error);
}

static void VerifySchemaChildren(const MultiFileColumnDefinition &column) {
	auto column_name = column.name.GetIdentifierName();
	switch (column.type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		break;
	default:
		throw BinderException("'schema' column \"%s\" of type %s cannot define nested children", column_name,
		                      column.type.ToString());
	}
	auto expected_children = LogicalType::GetNamedChildTypes(column.type);
	if (column.children.size() != expected_children.size()) {
		throw BinderException("'schema' column \"%s\" of type %s expects %d child definitions, not %d", column_name,
		                      column.type.ToString(), expected_children.size(), column.children.size());
	}
	for (idx_t i = 0; i < expected_children.size(); i++) {
		VerifySchemaChildType(column, column.children[i], expected_children[i].first.GetIdentifierName(),
		                      expected_children[i].second);
	}
}

static vector<MultiFileColumnDefinition> ParseSchemaMap(ClientContext &context, const Value &schema_value,
                                                        const LogicalType &root_key_type, bool is_root);

static MultiFileColumnDefinition ParseSchemaDefinition(ClientContext &context, const Value &column_value,
                                                       const LogicalType &root_key_type) {
	auto &map_entry = StructValue::GetChildren(column_value);
	const auto &column_def = map_entry[1];
	if (column_def.IsNull()) {
		throw BinderException("'schema' definition cannot be NULL");
	}
	VerifySchemaDefinitionType(column_def.type(), false);

	const auto &children = StructValue::GetChildren(column_def);
	auto type = TransformStringToLogicalType(StringValue::Get(children[1]), context);
	MultiFileColumnDefinition result(StringValue::Get(children[0]), type);
	result.identifier = map_entry[0];
	string error_message;
	auto default_value = children[2].TryCastAs(context, result.type, &error_message);
	if (!default_value) {
		throw BinderException("Unable to cast 'schema' default_value \"%s\" to %s", children[2].ToString(),
		                      result.type.ToString());
	}
	result.default_expression = ConstantExpression::FromValue(*default_value);
	if (children.size() > 3 && !children[3].IsNull()) {
		result.children = ParseSchemaMap(context, children[3], root_key_type, false);
		VerifySchemaChildren(result);
	}
	return result;
}

static vector<MultiFileColumnDefinition> ParseSchemaMap(ClientContext &context, const Value &schema_value,
                                                        const LogicalType &root_key_type, bool is_root) {
	if (schema_value.type().id() != LogicalTypeId::MAP) {
		if (is_root) {
			throw InvalidInputException("'schema' expects a value of type MAP, not %s",
			                            LogicalTypeIdToString(schema_value.type().id()));
		}
		throw BinderException("'schema' 'children' expects a value of type MAP, not %s",
		                      LogicalTypeIdToString(schema_value.type().id()));
	}
	auto &map_type = schema_value.type();
	auto &key_type = MapType::KeyType(map_type);
	auto &value_type = MapType::ValueType(map_type);
	VerifySchemaDefinitionType(value_type, is_root);
	if (is_root) {
		if (key_type.id() != LogicalTypeId::INTEGER && key_type.id() != LogicalTypeId::VARCHAR) {
			throw InvalidInputException(
			    "'schema' expects the value type of the map to be either INTEGER or VARCHAR, not %s",
			    LogicalTypeIdToString(key_type.id()));
		}
	} else if (key_type != root_key_type) {
		throw BinderException("'schema' 'children' key type must match the root schema key type %s, not %s",
		                      root_key_type.ToString(), key_type.ToString());
	}

	auto &entries = ListValue::GetChildren(schema_value);
	vector<MultiFileColumnDefinition> result;
	result.reserve(entries.size());
	for (auto &entry : entries) {
		result.push_back(ParseSchemaDefinition(context, entry, root_key_type));
	}
	return result;
}

vector<MultiFileColumnDefinition> MultiFileReader::ParseSchemaOption(ClientContext &context,
                                                                     const Value &schema_value) {
	if (schema_value.type().id() != LogicalTypeId::MAP) {
		throw InvalidInputException("'schema' expects a value of type MAP, not %s",
		                            LogicalTypeIdToString(schema_value.type().id()));
	}
	return ParseSchemaMap(context, schema_value, MapType::KeyType(schema_value.type()), true);
}

void MultiFileOptions::SetBoundSchema(const Value &value, ClientContext &context) {
	if (value.IsNull() || value.type().id() != LogicalTypeId::STRUCT) {
		throw BinderException("'bound_schema' expects a non-NULL STRUCT of column names to type strings");
	}
	auto &values = StructValue::GetChildren(value);
	if (values.empty()) {
		throw BinderException("'bound_schema' cannot be empty");
	}
	for (idx_t i = 0; i < values.size(); i++) {
		if (values[i].IsNull() || values[i].type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("'bound_schema' types must be non-NULL VARCHAR values");
		}
		auto type = TransformStringToLogicalType(StringValue::Get(values[i]), context);
		bound_schema.push_back(
		    MultiFileColumnDefinition::CreateNested(StructType::GetChildName(value.type(), i), type));
	}
}

void MultiFileOptions::ValidateBoundSchema() const {
	if (bound_schema.empty()) {
		return;
	}
	if (!schema.empty()) {
		throw BinderException("'bound_schema' cannot be combined with 'schema'");
	}
	if (union_by_name || maximum_sample_files != 1) {
		throw BinderException("'bound_schema' does not support union_by_name or multi-file schema sampling");
	}
	if (auto_detect_hive_partitioning) {
		throw BinderException("'bound_schema' requires explicit hive_partitioning=true or false");
	}
	if (hive_partitioning && hive_types_autocast) {
		throw BinderException("'bound_schema' requires hive_types_autocast=false and complete hive_types");
	}
	if (!hive_partitioning && !hive_types_schema.empty()) {
		throw BinderException("'bound_schema' cannot combine hive_types with hive_partitioning=false");
	}
	case_insensitive_map_t<LogicalType> columns;
	for (auto &column : bound_schema) {
		if (!column.type.IsComplete() || !columns.emplace(column.name.GetIdentifierName(), column.type).second) {
			throw BinderException("'bound_schema' requires complete types and distinct column names");
		}
	}
	auto require_type = [&](const string &name, const LogicalType &type) {
		auto entry = columns.find(name);
		if (entry == columns.end() || !entry->second.EqualsIncludingCollation(type)) {
			throw BinderException("'bound_schema' must include column \"%s\" with type %s", name, type.ToString());
		}
	};
	if (filename) {
		require_type(filename_column, LogicalType::VARCHAR);
	}
	if (file_row_number) {
		require_type("file_row_number", LogicalType::BIGINT);
		if (filename && StringUtil::CIEquals(filename_column, "file_row_number")) {
			throw BinderException("'bound_schema' cannot assign filename and file_row_number to the same column");
		}
	}
	for (auto &partition : hive_types_schema) {
		require_type(partition.first, partition.second);
		if ((filename && StringUtil::CIEquals(filename_column, partition.first)) ||
		    (file_row_number && StringUtil::CIEquals("file_row_number", partition.first))) {
			throw BinderException("'bound_schema' cannot assign generated and Hive roles to column \"%s\"",
			                      partition.first);
		}
	}
	if (allow_empty) {
		throw BinderException("'bound_schema' does not support allow_empty");
	}
}

bool MultiFileReader::ParseCopyOption(const Identifier &key, const vector<Value> &values, MultiFileOptions &options) {
	if (key != "file_row_number") {
		return false;
	}
	// a boolean COPY option that is given without a value is set
	if (values.empty()) {
		options.file_row_number = true;
		return true;
	}
	string error_message;
	auto value = values[0].DefaultTryCastAs(LogicalType::BOOLEAN, &error_message);
	if (!value) {
		throw InvalidInputException("Unable to cast \"%s\" to BOOLEAN for boolean option %s", values[0].ToString(),
		                            key);
	}
	options.file_row_number = BooleanValue::Get(*value);
	return true;
}

bool MultiFileReader::ParseOption(const Identifier &key, const Value &val, MultiFileOptions &options,
                                  ClientContext &context) {
	if (key == "filename") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		if (val.type() == LogicalType::VARCHAR) {
			// If not, we interpret it as the name of the column containing the filename
			options.filename = true;
			options.filename_column = StringValue::Get(val);
		} else {
			string error_message;
			auto boolean_value = val.DefaultTryCastAs(LogicalType::BOOLEAN, &error_message);
			if (boolean_value) {
				// If the argument can be cast to boolean, we just interpret it as a boolean
				options.filename = BooleanValue::Get(*boolean_value);
			}
		}
	} else if (key == "hive_partitioning") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		options.hive_partitioning = BooleanValue::Get(val);
		options.auto_detect_hive_partitioning = false;
	} else if (key == "union_by_name") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		options.union_by_name = BooleanValue::Get(val);
	} else if (key == "allow_empty") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		options.allow_empty = BooleanValue::Get(val);
	} else if (key == "column_statistics") {
		options.SetColumnStatistics(key, val);
	} else if (key == "maximum_sample_files") {
		options.SetMaximumSampleFiles(key, val);
	} else if (key == "hive_types_autocast" || key == "hive_type_autocast") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		options.hive_types_autocast = BooleanValue::Get(val);
	} else if (key == "hive_types" || key == "hive_type") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		if (val.type().id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException(
			    "'hive_types' only accepts a STRUCT('name':VARCHAR, ...), but '%s' was provided",
			    val.type().ToString());
		}
		// verify that all the children of the struct value are VARCHAR
		auto &children = StructValue::GetChildren(val);
		if (children.empty()) {
			throw InvalidInputException("'hive_types' must specify the type of at least one column");
		}
		for (idx_t i = 0; i < children.size(); i++) {
			const Value &child = children[i];
			if (child.type().id() != LogicalType::VARCHAR) {
				throw InvalidInputException("hive_types: %s must be a VARCHAR, instead: '%s' was provided",
				                            StructType::GetChildName(val.type(), i), child.type().ToString());
			}
			// for every child of the struct, get the logical type
			LogicalType transformed_type = TransformStringToLogicalType(child.ToString(), context);
			const string &name = StructType::GetChildName(val.type(), i).GetIdentifierName();
			options.hive_types_schema[name] = transformed_type;
		}
		D_ASSERT(!options.hive_types_schema.empty());
	} else if (key == "bound_schema") {
		options.SetBoundSchema(val, context);
	} else if (key == "schema") {
		options.schema = ParseSchemaOption(context, val);
		if (options.schema.empty()) {
			throw BinderException("'schema' cannot be empty");
		}
		// A given schema is not detected from the paths; that is decided where the schema is bound, so
		// auto_detect_hive_partitioning keeps meaning what it says - whether the CALLER left it to us
	} else if (key == "file_row_number") {
		if (val.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for %s", key);
		}
		options.file_row_number = BooleanValue::Get(val);
	} else {
		return false;
	}
	return true;
}

unique_ptr<MultiFileList> MultiFileReader::ComplexFilterPushdown(ClientContext &context, MultiFileList &files,
                                                                 const MultiFileOptions &options,
                                                                 MultiFilePushdownInfo &info,
                                                                 vector<unique_ptr<Expression>> &filters) {
	return files.ComplexFilterPushdown(context, options, info, filters);
}

MultiFileDynamicPushdownInfo::MultiFileDynamicPushdownInfo(ClientContext &context, const MultiFileOptions &options,
                                                           const vector<Identifier> &column_names,
                                                           const vector<LogicalType> &column_types,
                                                           const vector<ColumnIndex> &column_indexes,
                                                           TableFilterSet &filters)
    : context(context), options(options), column_names(column_names), column_types(column_types),
      column_indexes(column_indexes), filters(filters) {
	for (auto &column_id : column_indexes) {
		column_ids.push_back(column_id.GetPrimaryIndex());
	}
}

unique_ptr<MultiFileList> MultiFileReader::DynamicFilterPushdown(const MultiFileList &files,
                                                                 MultiFileDynamicPushdownInfo &pushdown_info) {
	return files.DynamicFilterPushdown(pushdown_info);
}

bool MultiFileReader::Bind(MultiFileOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
                           vector<Identifier> &names, MultiFileReaderBindData &bind_data) {
	if (options.schema.empty()) {
		// the default MultiFileReader has no schema information of its own - the files have to be bound
		return false;
	}
	if (options.union_by_name) {
		// both decide the schema of the scan, from the files and from the option respectively
		throw BinderException("'schema' cannot be combined with union_by_name=true");
	}
	// the files are mapped onto the schema by field id when its columns are identified by INTEGER, by name otherwise
	const bool by_field_id = options.schema[0].identifier.type().id() == LogicalTypeId::INTEGER;
	for (auto &column : options.schema) {
		names.push_back(column.name);
		return_types.push_back(column.type);
		bind_data.schema.push_back(column);
	}
	bind_data.mapping = by_field_id ? MultiFileColumnMappingMode::BY_FIELD_ID : MultiFileColumnMappingMode::BY_NAME;
	return true;
}

void MultiFileReader::BindOptions(MultiFileOptions &options, MultiFileList &files, vector<LogicalType> &return_types,
                                  vector<Identifier> &names, MultiFileReaderBindData &bind_data) {
	const bool has_bound_schema = !options.bound_schema.empty();
	if (has_bound_schema) {
		// Roles bind directly to their final declared positions; the physical reader's schema is unchanged.
		names.clear();
		return_types.clear();
		for (auto &column : options.bound_schema) {
			names.push_back(column.name);
			return_types.push_back(column.type);
		}
	}
	auto bind_generated_column = [&](const Identifier &name, const LogicalType &type) -> idx_t {
		if (has_bound_schema) {
			auto index = StringUtil::CIFind(names, name);
			if (index == DConstants::INVALID_INDEX || !return_types[index].EqualsIncludingCollation(type)) {
				throw BinderException("'bound_schema' column \"%s\" does not match the resolved output", name);
			}
			return index;
		}
		auto index = names.size();
		names.push_back(name);
		return_types.push_back(type);
		return index;
	};
	// Add the file_row_number column - it is read from the row number virtual column of the reader
	if (options.file_row_number) {
		if (!has_bound_schema && StringUtil::CIFind(names, "file_row_number") != DConstants::INVALID_INDEX) {
			throw BinderException("Option file_row_number adds column \"file_row_number\", but a column with this "
			                      "name is also in the file");
		}
		bind_data.file_row_number_idx = bind_generated_column("file_row_number", LogicalType::BIGINT);
	}

	// Add generated constant column for filename
	if (options.filename) {
		if (!has_bound_schema && std::find(names.begin(), names.end(), options.filename_column) != names.end()) {
			throw BinderException("Option filename adds column \"%s\", but a column with this name is also in the "
			                      "file. Try setting a different name: filename='<filename column name>'",
			                      options.filename_column);
		}
		bind_data.filename_idx = bind_generated_column(Identifier(options.filename_column), LogicalType::VARCHAR);
	}

	// Add generated constant columns from hive partitioning scheme
	if (options.hive_partitioning) {
		D_ASSERT(files.GetExpandResult() != FileExpandResult::NO_FILES);
		auto partitions = HivePartitioning::Parse(files.GetFirstFile().path);
		// verify that all files have the same hive partitioning scheme
		for (const auto &file : files.Files()) {
			auto file_partitions = HivePartitioning::Parse(file.path);
			for (auto &part_info : partitions) {
				if (file_partitions.find(part_info.first) == file_partitions.end()) {
					string error = "Hive partition mismatch between file \"%s\" and \"%s\": key \"%s\" not found";
					if (options.auto_detect_hive_partitioning == true) {
						throw InternalException(error + "(hive partitioning was autodetected)",
						                        files.GetFirstFile().path, file.path, part_info.first);
					}
					throw BinderException(error.c_str(), files.GetFirstFile().path, file.path, part_info.first);
				}
			}
			if (partitions.size() != file_partitions.size()) {
				string error_msg = "Hive partition mismatch between file \"%s\" and \"%s\"";
				if (options.auto_detect_hive_partitioning == true) {
					throw InternalException(error_msg + "(hive partitioning was autodetected)",
					                        files.GetFirstFile().path, file.path);
				}
				throw BinderException(error_msg.c_str(), files.GetFirstFile().path, file.path);
			}
		}

		if (has_bound_schema && partitions.size() != options.hive_types_schema.size()) {
			throw BinderException("'bound_schema' requires hive_types for every Hive partition column");
		}
		if (!options.hive_types_schema.empty()) {
			// verify that all hive_types are existing partitions
			options.VerifyHiveTypesArePartitions(partitions);
		}

		for (auto &part : partitions) {
			if (has_bound_schema) {
				auto index = bind_generated_column(Identifier(part.first), options.GetHiveLogicalType(part.first));
				bind_data.hive_partitioning_indexes.emplace_back(part.first, index);
				continue;
			}
			idx_t hive_partitioning_index;
			auto lookup = std::find_if(names.begin(), names.end(),
			                           [&](const Identifier &col_name) { return col_name == part.first; });
			if (lookup != names.end()) {
				auto idx = NumericCast<idx_t>(lookup - names.begin());
				if (bind_data.filename_idx == idx) {
					throw BinderException(
					    "Option filename adds column \"%s\", but a hive partition column with this "
					    "name also exists. Try setting a different name: filename='<filename column name>'",
					    options.filename_column);
				}
				auto hive_type = options.GetHiveLogicalType(part.first);
				// the path does not get to change a type the schema declares
				if (!options.schema.empty() && idx < bind_data.schema.size() &&
				    !bind_data.schema[idx].type.EqualsIncludingCollation(hive_type)) {
					throw BinderException("'schema' declares column \"%s\" as %s, but its hive partition type is %s - "
					                      "set the partition type with hive_types",
					                      bind_data.schema[idx].name.GetIdentifierName(),
					                      bind_data.schema[idx].type.ToString(), hive_type.ToString());
				}
				// hive partitioning column also exists in file - override
				hive_partitioning_index = idx;
				return_types[idx] = hive_type;
			} else {
				// hive partitioning column does not exist in file - add a new column containing the key
				hive_partitioning_index = names.size();
				return_types.emplace_back(options.GetHiveLogicalType(part.first));
				names.emplace_back(part.first);
			}
			bind_data.hive_partitioning_indexes.emplace_back(part.first, hive_partitioning_index);
		}
	}
}

void MultiFileReader::GetVirtualColumns(ClientContext &context, MultiFileReaderBindData &bind_data,
                                        virtual_column_map_t &result) {
	if (!bind_data.filename_idx.IsValid() || bind_data.filename_idx == COLUMN_IDENTIFIER_FILENAME) {
		bind_data.filename_idx = COLUMN_IDENTIFIER_FILENAME;
		result.insert(make_pair(COLUMN_IDENTIFIER_FILENAME, TableColumn("filename", LogicalType::VARCHAR)));
	}
	result.insert(make_pair(COLUMN_IDENTIFIER_FILE_INDEX, TableColumn("file_index", LogicalType::UBIGINT)));
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
}

// Resolves `column_id` to a Value from info known without opening the file: filename, file_index,
// or hive partitioning columns. `hive_partitions` is a caller-owned lazy cache.
static bool TryResolvePreOpenConstant(column_t column_id, const string &file_path, idx_t file_list_idx,
                                      const MultiFileReaderBindData &reader_bind, const MultiFileOptions &file_options,
                                      ClientContext &context, std::map<string, string> &hive_partitions,
                                      bool &hive_partitions_parsed, Value &out_constant) {
	if ((reader_bind.filename_idx.IsValid() && column_id == reader_bind.filename_idx.GetIndex()) ||
	    column_id == MultiFileReader::COLUMN_IDENTIFIER_FILENAME) {
		out_constant = Value(file_path);
		return true;
	}
	if (column_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_INDEX) {
		out_constant = Value::UBIGINT(file_list_idx);
		return true;
	}
	auto &hive = reader_bind.hive_partitioning_indexes;
	auto hp = std::find_if(hive.begin(), hive.end(),
	                       [column_id](const HivePartitioningIndex &e) { return e.index == column_id; });
	if (hp == hive.end()) {
		return false;
	}
	if (!hive_partitions_parsed) {
		hive_partitions = HivePartitioning::Parse(file_path);
		hive_partitions_parsed = true;
	}
	auto entry = hive_partitions.find(hp->value);
	if (entry == hive_partitions.end()) {
		return false;
	}
	out_constant = file_options.GetHivePartitionValue(entry->second, hp->value, context);
	return true;
}

void MultiFileReader::FinalizeBind(MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
                                   const MultiFileReaderBindData &options,
                                   const vector<MultiFileColumnDefinition> &global_columns,
                                   const vector<ColumnIndex> &global_column_ids, ClientContext &context,
                                   optional_ptr<MultiFileReaderGlobalState> global_state) {
	// create a map of name -> column index
	auto &local_columns = reader_data.reader->GetColumns();
	auto &filename = reader_data.reader->GetFileName();
	case_insensitive_map_t<idx_t> name_map;
	// a column a file lacks is read as NULL when the schema is a union of the files - a schema given up front instead
	// says what such a column is read as, which the column mapper takes care of
	const bool missing_columns_are_null = file_options.SchemaIsUnion() && file_options.schema.empty();
	if (missing_columns_are_null) {
		for (idx_t col_idx = 0; col_idx < local_columns.size(); col_idx++) {
			auto &column = local_columns[col_idx];
			name_map[column.name.GetIdentifierName()] = col_idx;
		}
	}
	std::map<string, string> hive_partitions;
	bool hive_partitions_parsed = false;
	Value pre_open_constant;
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_idx = MultiFileGlobalIndex(i);
		auto &col_id = global_column_ids[i];
		auto column_id = col_id.GetPrimaryIndex();
		if (TryResolvePreOpenConstant(column_id, filename, reader_data.reader->file_list_idx.GetIndex(), options,
		                              file_options, context, hive_partitions, hive_partitions_parsed,
		                              pre_open_constant)) {
			reader_data.constant_map.Add(global_idx, pre_open_constant);
			continue;
		}
		if (IsVirtualColumn(column_id)) {
			continue;
		}
		if (missing_columns_are_null) {
			auto &column = global_columns[column_id];
			auto &name = column.name;
			auto &type = column.type;

			auto entry = name_map.find(name.GetIdentifierName());
			bool not_present_in_file = entry == name_map.end();
			if (not_present_in_file) {
				// we need to project a column with name \"global_name\" - but it does not exist in the current file
				// push a NULL value of the specified type
				auto &constant_type = col_id.HasType() ? col_id.GetScanType() : type;
				reader_data.constant_map.Add(global_idx, Value(constant_type));
				continue;
			}
		}
	}
}

unique_ptr<MultiFileReaderGlobalState>
MultiFileReader::InitializeGlobalState(ClientContext &context, const MultiFileOptions &file_options,
                                       const MultiFileReaderBindData &bind_data, const MultiFileList &file_list,
                                       const vector<MultiFileColumnDefinition> &global_columns,
                                       const vector<ColumnIndex> &global_column_ids) {
	// By default, the multifilereader does not require any global state
	return nullptr;
}

ReaderInitializeType
MultiFileReader::CreateMapping(ClientContext &context, MultiFileReaderData &reader_data,
                               const vector<MultiFileColumnDefinition> &global_columns,
                               const vector<ColumnIndex> &global_column_ids, optional_ptr<TableFilterSet> filters,
                               MultiFileList &multi_file_list, const MultiFileReaderBindData &bind_data,
                               const virtual_column_map_t &virtual_columns, MultiFileColumnMappingMode mapping_mode) {
	MultiFileColumnMapper column_mapper(context, *this, reader_data, global_columns, global_column_ids, filters,
	                                    multi_file_list, virtual_columns);
	return column_mapper.CreateMapping(mapping_mode);
}

ReaderInitializeType MultiFileReader::CreateMapping(
    ClientContext &context, MultiFileReaderData &reader_data, const vector<MultiFileColumnDefinition> &global_columns,
    const vector<ColumnIndex> &global_column_ids, optional_ptr<TableFilterSet> filters, MultiFileList &multi_file_list,
    const MultiFileReaderBindData &bind_data, const virtual_column_map_t &virtual_columns) {
	return CreateMapping(context, reader_data, global_columns, global_column_ids, filters, multi_file_list, bind_data,
	                     virtual_columns, bind_data.mapping);
}

static string GetExtendedMultiFileError(const MultiFileBindData &bind_data, const Expression &expr,
                                        BaseFileReader &reader, idx_t expr_idx, string &first_message) {
	if (expr.GetExpressionType() != ExpressionType::OPERATOR_CAST) {
		// not a cast
		return string();
	}
	auto &cast_expr = expr.Cast<BoundFunctionExpression>();
	if (BoundCastExpression::Child(cast_expr).GetExpressionType() != ExpressionType::BOUND_REF) {
		return string();
	}
	auto &ref = BoundCastExpression::Child(cast_expr).Cast<BoundReferenceExpression>();
	auto &source_type = ref.GetReturnType();
	auto &target_type = cast_expr.GetReturnType();
	auto &columns = reader.GetColumns();
	auto local_col_id = reader.column_indexes[ref.Index()].GetPrimaryIndex();
	auto &local_col = columns[local_col_id];

	auto reader_type = reader.GetReaderType();
	auto function_name = "read_" + StringUtil::Lower(reader_type);
	string extended_error;
	if (!bind_data.table_columns.empty()) {
		// COPY .. FROM
		string target_column;
		if (expr_idx < bind_data.table_columns.size()) {
			target_column = "\"" + bind_data.table_columns[expr_idx] + "\" ";
		}
		extended_error = StringUtil::Format(
		    "In file \"%s\" the column %s has type %s, but we are trying to load it into column %swith type "
		    "%s.\nThis means the %s schema does not match the schema of the table.\nPossible solutions:\n* Insert by "
		    "name instead of by position using \"INSERT INTO tbl BY NAME SELECT * FROM %s(...)\"\n* Manually specify "
		    "which columns to insert using \"INSERT INTO tbl SELECT ... FROM %s(...)\"",
		    reader.GetFileName(), local_col.name, source_type, target_column, target_type, reader_type, function_name,
		    function_name);
	} else {
		// read_parquet() with multiple files
		extended_error = StringUtil::Format(
		    "In file \"%s\" the column %s has type %s, but we are trying to read it as type %s."
		    "\nThis can happen when reading multiple %s files. The schema information is taken from "
		    "the first %s file by default. Possible solutions:\n"
		    "* Enable the union_by_name=True option to combine the schema of all %s files "
		    "(https://duckdb.org/docs/current/data/multiple_files/combining_schemas)\n"
		    "* Use a COPY statement to automatically derive types from an existing table.",
		    reader.GetFileName(), local_col.name, source_type, target_type, reader_type, reader_type, reader_type);
	}
	first_message =
	    StringUtil::Format("failed to cast column %s from type %s to %s: ", local_col.name, source_type, target_type);
	return extended_error;
}

void MultiFileReader::FinalizeChunk(ClientContext &context, const MultiFileBindData &bind_data, BaseFileReader &reader,
                                    const MultiFileReaderData &reader_data, DataChunk &input_chunk,
                                    DataChunk &output_chunk, ExpressionExecutor &executor,
                                    optional_ptr<MultiFileReaderGlobalState> global_state) {
	executor.SetChunk(input_chunk);
	for (idx_t i = 0; i < executor.expressions.size(); i++) {
		try {
			executor.ExecuteExpression(i, output_chunk.data[i]);
		} catch (std::exception &ex) {
			// error while converting - try to create a nice error message
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERNAL) {
				throw;
			}
			auto &original_error = error.RawMessage();
			string first_message;
			string extended_error =
			    GetExtendedMultiFileError(bind_data, *executor.expressions[i], reader, i, first_message);
			throw ConversionException("Error while reading file \"%s\": %s: %s\n\n%s", reader.GetFileName(),
			                          first_message, original_error, extended_error);
		}
	}
}

void MultiFileReader::GetPartitionData(ClientContext &context, const MultiFileReaderBindData &bind_data,
                                       const MultiFileReaderData &reader_data,
                                       optional_ptr<MultiFileReaderGlobalState> global_state,
                                       const OperatorPartitionInfo &partition_info,
                                       OperatorPartitionData &partition_data) {
	for (idx_t col : partition_info.partition_columns) {
		bool found_constant = false;
		for (auto &constant : reader_data.constant_map) {
			if (constant.column_idx.GetIndex() == col) {
				found_constant = true;
				partition_data.partition_data.emplace_back(constant.value);
				break;
			}
		}
		if (!found_constant) {
			throw InternalException(
			    "MultiFileReader::GetPartitionData - did not find constant for the given partition");
		}
	}
}

TablePartitionInfo MultiFileReader::GetPartitionInfo(ClientContext &context, const MultiFileReaderBindData &bind_data,
                                                     TableFunctionPartitionInput &input) {
	// check if all of the columns are in the hive partition set
	for (auto &partition_col : input.partition_ids) {
		// check if this column is in the hive partitioned set
		bool found = false;
		for (auto &partition : bind_data.hive_partitioning_indexes) {
			if (partition.index == partition_col) {
				found = true;
				break;
			}
		}
		if (!found) {
			// the column is not partitioned - hive partitioning alone can't guarantee the groups are partitioned
			return TablePartitionInfo::NOT_PARTITIONED;
		}
	}
	// if all columns are in the hive partitioning set, we know that each partition will only have a single value
	// i.e. if the hive partitioning is by (YEAR, MONTH), each partition will have a single unique (YEAR, MONTH)
	return TablePartitionInfo::SINGLE_VALUE_PARTITIONS;
}

TableFunctionSet MultiFileReader::CreateFunctionSet(TableFunction table_function) {
	TableFunctionSet function_set {table_function.name};
	function_set.AddFunction(table_function);
	D_ASSERT(table_function.GetSignature().GetPositionalParameterCount() > 0 &&
	         table_function.GetSignature().GetParameter(0).GetType() == LogicalType::VARCHAR);
	// the list variant takes ANY as its child type: a file is either a path (VARCHAR) or a STRUCT/VARIANT
	// holding the path together with the options to open the file with
	auto list_function = table_function;
	auto &list_parameter = list_function.GetSignature().GetParameter(0);
	list_parameter.SetType(LogicalType::LIST(LogicalType::ANY));
	if (list_parameter.GetName() == "path") {
		list_parameter.SetName("paths");
	}
	function_set.AddFunction(std::move(list_function));
	// a single file can also be passed as a VARIANT - without this overload it would implicitly cast to VARCHAR
	// and the stringified variant would be read as a path
	table_function.GetSignature().GetParameter(0).SetType(LogicalType::VARIANT());
	function_set.AddFunction(std::move(table_function));
	return function_set;
}

unique_ptr<Expression> MultiFileReader::GetConstantVirtualColumn(MultiFileReaderData &reader_data, idx_t column_id,
                                                                 const LogicalType &type) {
	if (column_id == COLUMN_IDENTIFIER_EMPTY || column_id == COLUMN_IDENTIFIER_FILENAME) {
		return make_uniq<BoundConstantExpression>(Value(type));
	}
	return nullptr;
}

MultiFileReaderVirtualColumnBinding
MultiFileReader::GetVirtualColumnExpression(ClientContext &, MultiFileReaderData &,
                                            const vector<MultiFileColumnDefinition> &, idx_t column_id,
                                            const LogicalType &type, MultiFileLocalIndex local_idx) {
	return MultiFileReaderVirtualColumnBinding(make_uniq<BoundReferenceExpression>(type, local_idx.GetIndex()),
	                                           {column_id});
}

MultiFileReaderBindData MultiFileReader::BindUnionReader(ClientContext &context, vector<LogicalType> &return_types,
                                                         vector<Identifier> &names, MultiFileList &files,
                                                         MultiFileBindData &result, BaseFileReaderOptions &options,
                                                         MultiFileOptions &file_options) {
	D_ASSERT(file_options.union_by_name);
	vector<Identifier> union_col_names;
	vector<LogicalType> union_col_types;

	// obtain the set of union column names + types by unifying the types of all of the files
	// note that this requires opening readers for each file and reading the metadata of each file
	// note also that it requires fully expanding the MultiFileList
	auto materialized_file_list = files.GetAllFiles();
	auto union_readers = UnionByName::UnionCols(context, materialized_file_list, union_col_types, union_col_names,
	                                            options, file_options, *this, *result.interface);

	std::move(union_readers.begin(), union_readers.end(), std::back_inserter(result.union_readers));
	// perform the binding on the obtained set of names + types
	MultiFileReaderBindData bind_data;
	BindOptions(file_options, files, union_col_types, union_col_names, bind_data);
	names = union_col_names;
	return_types = union_col_types;
	result.Initialize(context, *result.union_readers[0]);
	D_ASSERT(names.size() == return_types.size());
	return bind_data;
}

MultiFileReaderBindData MultiFileReader::BindReader(ClientContext &context, vector<LogicalType> &return_types,
                                                    vector<Identifier> &names, MultiFileList &files,
                                                    MultiFileBindData &result, BaseFileReaderOptions &options,
                                                    MultiFileOptions &file_options) {
	if (file_options.union_by_name) {
		return BindUnionReader(context, return_types, names, files, result, options, file_options);
	}
	if (file_options.maximum_sample_files > 1) {
		return BindSampledReader(context, return_types, names, files, result, options, file_options);
	}
	return BindFirstReader(context, return_types, names, files, result, options, file_options);
}

MultiFileReaderBindData MultiFileReader::BindFirstReader(ClientContext &context, vector<LogicalType> &return_types,
                                                         vector<Identifier> &names, MultiFileList &files,
                                                         MultiFileBindData &result, BaseFileReaderOptions &options,
                                                         MultiFileOptions &file_options) {
	auto reader = CreateReader(context, files.GetFirstFile(), options, file_options, *result.interface);
	auto &columns = reader->GetColumns();
	for (auto &column : columns) {
		return_types.emplace_back(column.type);
		names.emplace_back(column.name);
	}
	result.Initialize(std::move(reader));
	MultiFileReaderBindData bind_data;
	BindOptions(file_options, files, return_types, names, bind_data);
	return bind_data;
}

MultiFileReaderBindData MultiFileReader::BindSampledReader(ClientContext &context, vector<LogicalType> &return_types,
                                                           vector<Identifier> &names, MultiFileList &files,
                                                           MultiFileBindData &result, BaseFileReaderOptions &options,
                                                           MultiFileOptions &file_options) {
	// gather the files we want to sample - note that we deliberately do not expand the entire file list here
	vector<OpenFileInfo> sampled_files;
	MultiFileListScanData file_scan;
	files.InitializeScan(file_scan);
	OpenFileInfo file;
	bool sampled_all_files = true;
	while (files.Scan(file_scan, file)) {
		if (sampled_files.size() >= file_options.maximum_sample_files) {
			sampled_all_files = false;
			break;
		}
		sampled_files.push_back(std::move(file));
	}
	if (sampled_files.size() <= 1) {
		// only a single file - there is nothing to combine
		return BindFirstReader(context, return_types, names, files, result, options, file_options);
	}

	// open the sampled files and combine their schemas into one
	vector<Identifier> union_col_names;
	vector<LogicalType> union_col_types;
	auto sampled_readers = UnionByName::UnionCols(context, sampled_files, union_col_types, union_col_names, options,
	                                              file_options, *this, *result.interface);
	names = union_col_names;
	return_types = union_col_types;

	if (sampled_all_files) {
		// we have opened every file - keep the readers around so they do not need to be opened again
		std::move(sampled_readers.begin(), sampled_readers.end(), std::back_inserter(result.union_readers));
		result.Initialize(context, *result.union_readers[0]);
	} else {
		// we only sampled a subset - the readers cannot be re-used, keep only the reader of the first file
		result.Initialize(context, *sampled_readers[0]);
	}
	MultiFileReaderBindData bind_data;
	BindOptions(file_options, files, return_types, names, bind_data);
	return bind_data;
}

ReaderInitializeType MultiFileReader::InitializeReader(MultiFileReaderData &reader_data,
                                                       const MultiFileBindData &bind_data,
                                                       const vector<MultiFileColumnDefinition> &global_columns,
                                                       const vector<ColumnIndex> &global_column_ids,
                                                       optional_ptr<TableFilterSet> table_filters,
                                                       ClientContext &context, MultiFileGlobalState &gstate) {
	// the file_row_number column is not a column of the file - it is read from the row number virtual column
	auto column_ids = global_column_ids;
	auto &file_row_number_idx = bind_data.reader_bind.file_row_number_idx;
	if (file_row_number_idx.IsValid()) {
		for (auto &column_id : column_ids) {
			if (column_id.GetPrimaryIndex() == file_row_number_idx.GetIndex()) {
				column_id = ColumnIndex(COLUMN_IDENTIFIER_FILE_ROW_NUMBER);
			}
		}
	}
	FinalizeBind(reader_data, bind_data.file_options, bind_data.reader_bind, global_columns, column_ids, context,
	             gstate.multi_file_reader_state.get());
	return CreateMapping(context, reader_data, global_columns, column_ids, table_filters, gstate.file_list,
	                     bind_data.reader_bind, bind_data.virtual_columns);
}

shared_ptr<BaseFileReader> MultiFileReader::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                                         BaseUnionData &union_data,
                                                         const MultiFileBindData &bind_data) {
	return bind_data.interface->CreateReader(context, gstate, union_data, bind_data);
}

shared_ptr<BaseFileReader> MultiFileReader::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                                         const OpenFileInfo &file, idx_t file_idx,
                                                         const MultiFileBindData &bind_data) {
	return bind_data.interface->CreateReader(context, gstate, file, file_idx, bind_data);
}

shared_ptr<BaseFileReader> MultiFileReader::CreateReader(ClientContext &context, const OpenFileInfo &file,
                                                         BaseFileReaderOptions &options,
                                                         const MultiFileOptions &file_options,
                                                         MultiFileReaderInterface &interface) {
	return interface.CreateReader(context, file, options, file_options);
}

bool MultiFileReader::CanSkipFileFromFilters(ClientContext &context, const OpenFileInfo &file, idx_t file_list_idx,
                                             const MultiFileOptions &file_options,
                                             const MultiFileReaderBindData &reader_bind,
                                             const vector<ColumnIndex> &global_column_ids,
                                             optional_ptr<TableFilterSet> filters) {
	if (!filters) {
		return false;
	}

	std::map<string, string> hive_partitions;
	bool hive_partitions_parsed = false;
	Value constant;

	for (auto &it : *filters) {
		auto projection_idx = it.GetIndex().GetIndex();
		D_ASSERT(projection_idx < global_column_ids.size());
		auto column_id = global_column_ids[projection_idx].GetPrimaryIndex();
		if (!TryResolvePreOpenConstant(column_id, file.path, file_list_idx, reader_bind, file_options, context,
		                               hive_partitions, hive_partitions_parsed, constant)) {
			continue;
		}
		if (!EvaluateTableFilterAgainstConstant(context, it.Filter(), constant)) {
			return true;
		}
	}
	return false;
}

void MultiFileReader::PruneReaders(MultiFileBindData &data, MultiFileList &file_list) {
	unordered_set<string> file_set;

	// Avoid materializing the file list if there's nothing to prune
	if (!data.initial_reader && data.union_readers.empty()) {
		return;
	}

	for (const auto &file : file_list.Files()) {
		file_set.insert(file.path);
	}

	if (data.initial_reader) {
		// check if the initial reader should still be read
		auto entry = file_set.find(data.initial_reader->GetFileName());
		if (entry == file_set.end()) {
			data.initial_reader.reset();
		}
	}
	for (idx_t r = 0; r < data.union_readers.size(); r++) {
		if (!data.union_readers[r]) {
			data.union_readers.erase_at(r);
			r--;
			continue;
		}
		// check if the union reader should still be read or not
		auto entry = file_set.find(data.union_readers[r]->GetFileName());
		if (entry == file_set.end()) {
			data.union_readers.erase_at(r);
			r--;
			continue;
		}
	}
}

FileGlobInput MultiFileReader::GetGlobInput(MultiFileReaderInterface &interface) {
	return interface.GetGlobInput();
}

HivePartitioningIndex::HivePartitioningIndex(string value_p, idx_t index) : value(std::move(value_p)), index(index) {
}

void MultiFileOptions::AddBatchInfo(BindInfo &bind_info) const {
	bind_info.InsertOption("filename", Value(filename_column));
	bind_info.InsertOption("hive_partitioning", Value::BOOLEAN(hive_partitioning));
	bind_info.InsertOption("auto_detect_hive_partitioning", Value::BOOLEAN(auto_detect_hive_partitioning));
	bind_info.InsertOption("union_by_name", Value::BOOLEAN(union_by_name));
	bind_info.InsertOption("hive_types_autocast", Value::BOOLEAN(hive_types_autocast));
	bind_info.InsertOption("allow_empty", Value::BOOLEAN(allow_empty));
	bind_info.InsertOption("file_row_number", Value::BOOLEAN(file_row_number));
}

void UnionByName::CombineUnionTypes(const vector<string> &col_names, const vector<LogicalType> &sql_types,
                                    vector<LogicalType> &union_col_types, vector<Identifier> &union_col_names,
                                    identifier_map_t<idx_t> &union_names_map) {
	D_ASSERT(col_names.size() == sql_types.size());

	for (idx_t col = 0; col < col_names.size(); ++col) {
		Identifier col_name(col_names[col]);
		auto union_find = union_names_map.find(col_name);

		if (union_find != union_names_map.end()) {
			// given same name , union_col's type must compatible with col's type
			auto &current_type = union_col_types[union_find->second];
			auto compatible_type = LogicalType::DefaultForceMaxLogicalType(current_type, sql_types[col]);
			union_col_types[union_find->second] = compatible_type;
		} else {
			union_names_map[col_name] = union_col_names.size();
			union_col_names.emplace_back(col_name);
			union_col_types.emplace_back(sql_types[col]);
		}
	}
}

bool MultiFileOptions::AutoDetectHivePartitioningInternal(MultiFileList &files, ClientContext &context) {
	auto first_file = files.GetFirstFile();
	auto partitions = HivePartitioning::Parse(first_file.path);
	if (partitions.empty()) {
		// no partitions found in first file
		return false;
	}

	for (const auto &file : files.Files()) {
		auto new_partitions = HivePartitioning::Parse(file.path);
		if (new_partitions.size() != partitions.size()) {
			// partition count mismatch
			return false;
		}
		for (auto &part : new_partitions) {
			auto entry = partitions.find(part.first);
			if (entry == partitions.end()) {
				// differing partitions between files
				return false;
			}
		}
	}
	return true;
}
void MultiFileOptions::AutoDetectHiveTypesInternal(MultiFileList &files, ClientContext &context) {
	const LogicalType candidates[] = {LogicalType::DATE, LogicalType::TIMESTAMP, LogicalType::BIGINT};

	unordered_map<string, LogicalType> detected_types;
	for (const auto &file : files.Files()) {
		auto partitions = HivePartitioning::Parse(file.path);
		if (partitions.empty()) {
			return;
		}

		for (auto &part : partitions) {
			const string &name = part.first;
			if (hive_types_schema.find(name) != hive_types_schema.end()) {
				// type was explicitly provided by the user
				continue;
			}
			if (HivePartitioning::IsNull(part.second)) {
				continue; // don't update detected_types for this partition/file
			}
			LogicalType detected_type = LogicalType::VARCHAR;
			Value value(part.second);
			for (auto &candidate : candidates) {
				const bool success = value.TryCastAs(context, candidate, nullptr, true).has_value();
				if (success) {
					detected_type = candidate;
					break;
				}
			}
			auto entry = detected_types.find(name);
			if (entry == detected_types.end()) {
				// type was not yet detected - insert it
				detected_types.insert(make_pair(name, std::move(detected_type)));
			} else {
				// type was already detected - check if the type matches
				// if not promote to VARCHAR
				if (entry->second != detected_type) {
					entry->second = LogicalType::VARCHAR;
				}
			}
		}
	}
	for (auto &entry : detected_types) {
		hive_types_schema.insert(make_pair(entry.first, std::move(entry.second)));
	}
}
void MultiFileOptions::AutoDetectHivePartitioning(MultiFileList &files, ClientContext &context,
                                                  bool detect_partitioning) {
	if (files.GetExpandResult() == FileExpandResult::NO_FILES) {
		return;
	}
	const bool hp_explicitly_disabled = !auto_detect_hive_partitioning && !hive_partitioning;
	const bool ht_enabled = !hive_types_schema.empty();
	if (hp_explicitly_disabled && ht_enabled) {
		throw InvalidInputException("cannot disable hive_partitioning when hive_types is enabled");
	}
	if (ht_enabled && auto_detect_hive_partitioning && !hive_partitioning) {
		// hive_types flag implies hive_partitioning
		hive_partitioning = true;
		auto_detect_hive_partitioning = false;
	}
	if (detect_partitioning && auto_detect_hive_partitioning) {
		hive_partitioning = AutoDetectHivePartitioningInternal(files, context);
	}
	if (hive_partitioning && hive_types_autocast) {
		AutoDetectHiveTypesInternal(files, context);
	}
}
void MultiFileOptions::VerifyHiveTypesArePartitions(const std::map<string, string> &partitions) const {
	for (auto &hive_type : hive_types_schema) {
		if (partitions.find(hive_type.first) == partitions.end()) {
			throw InvalidInputException("Unknown hive_type: \"%s\" does not appear to be a partition", hive_type.first);
		}
	}
}
LogicalType MultiFileOptions::GetHiveLogicalType(const string &hive_partition_column) const {
	if (!hive_types_schema.empty()) {
		auto it = hive_types_schema.find(hive_partition_column);
		if (it != hive_types_schema.end()) {
			return it->second;
		}
	}
	return LogicalType::VARCHAR;
}

void MultiFileOptions::SetMaximumSampleFiles(const Identifier &key, const Value &val) {
	if (val.IsNull()) {
		throw BinderException("Cannot use NULL as argument to key %s", key);
	}
	if (!TrySetMaximumSampleFiles(val)) {
		throw BinderException("%s parameter must be positive, or -1 to remove the limit on the number of files "
		                      "used to determine the schema.",
		                      key);
	}
}

bool MultiFileOptions::TrySetMaximumSampleFiles(const Value &val) {
	if (val.IsNull()) {
		return false;
	}
	auto sample_files = val.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
	if (sample_files == -1) {
		maximum_sample_files = NumericLimits<idx_t>::Maximum();
		return true;
	}
	if (sample_files <= 0) {
		return false;
	}
	maximum_sample_files = NumericCast<idx_t>(sample_files);
	return true;
}

void MultiFileOptions::SetColumnStatistics(const Identifier &key, const Value &val) {
	if (val.IsNull()) {
		column_statistics = Value();
		return;
	}
	// The shape COPY ... (RETURN_STATS) reports column statistics in, so what one COPY returns can be handed to the
	// next scan as it is. Anything that casts to it (a MAP literal of other value types, say) is taken.
	auto target = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	string error;
	auto cast = val.DefaultTryCastAs(target, &error);
	if (!cast) {
		throw InvalidInputException(
		    "%s must be a MAP of column name to a MAP of statistic name ('min', 'max', 'count', 'null_count', "
		    "'distinct_count') to its value, but '%s' was provided",
		    key, val.type().ToString());
	}
	column_statistics = std::move(*cast);
}

// A key of the column_statistics option names a column of the scan - the way RETURN_STATS names what it wrote: every
// component of the path quoted, "s"."x" for field x of struct column s, "l"."element" for the elements of list column
// l, "m"."key" / "m"."value" for a map. A key without quotes is a plain top-level column name.
static vector<string> ParseColumnStatisticsKey(const string &key) {
	vector<string> path;
	if (key.find('"') == string::npos) {
		path.push_back(key);
		return path;
	}
	idx_t pos = 0;
	while (pos < key.size()) {
		if (key[pos] != '"') {
			throw InvalidInputException(
			    "column_statistics: key '%s' is not a quoted column path (\"column\".\"field\")", key);
		}
		string component;
		pos++;
		while (true) {
			if (pos >= key.size()) {
				throw InvalidInputException("column_statistics: key '%s' has an unterminated quote", key);
			}
			if (key[pos] == '"') {
				if (pos + 1 < key.size() && key[pos + 1] == '"') {
					component += '"';
					pos += 2;
					continue;
				}
				pos++;
				break;
			}
			component += key[pos++];
		}
		path.push_back(std::move(component));
		if (pos < key.size()) {
			if (key[pos] != '.') {
				throw InvalidInputException("column_statistics: key '%s' has '%c' where a '.' or the end was expected",
				                            key, key[pos]);
			}
			pos++;
		}
	}
	return path;
}

// Apply the statistics given for one (possibly nested) column to the statistics object of that column.
static void ApplyColumnStatistics(BaseStatistics &result, const Value &given, const string &key) {
	Value min, max;
	optional<idx_t> count, null_count, distinct_count;
	for (auto &stat : MapValue::GetChildren(given)) {
		auto &stat_key_value = StructValue::GetChildren(stat);
		if (stat_key_value[0].IsNull() || stat_key_value[1].IsNull()) {
			continue;
		}
		auto stat_name = StringUtil::Lower(StringValue::Get(stat_key_value[0]));
		auto &stat_value = stat_key_value[1];
		if (stat_name == "min") {
			min = stat_value;
		} else if (stat_name == "max") {
			max = stat_value;
		} else if (stat_name == "count" || stat_name == "null_count" || stat_name == "distinct_count") {
			string error;
			auto parsed = stat_value.DefaultTryCastAs(LogicalType::UBIGINT, &error);
			if (!parsed) {
				throw InvalidInputException("column_statistics: '%s' of '%s' must be a count, got '%s'", stat_name, key,
				                            stat_value.ToString());
			}
			auto parsed_count = UBigIntValue::Get(*parsed);
			if (stat_name == "count") {
				count = parsed_count;
			} else if (stat_name == "null_count") {
				null_count = parsed_count;
			} else {
				distinct_count = parsed_count;
			}
		}
		// other statistics a writer reports (column_size_bytes, has_nan, ...) say nothing the optimizer uses
	}
	// Only what is given is claimed: the rest is left unknown
	if (null_count.has_value()) {
		if (*null_count == 0) {
			result.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		}
		if (count.has_value() && *null_count >= *count) {
			result.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
		}
	}
	bool has_values = !count.has_value() || !null_count.has_value() || *null_count < *count;
	auto &type = result.GetType();
	if (has_values && !min.IsNull() && !max.IsNull() &&
	    BaseStatistics::GetStatsType(type) == StatisticsType::NUMERIC_STATS) {
		NumericStats::SetMin(result, min.DefaultCastAs(type, true));
		NumericStats::SetMax(result, max.DefaultCastAs(type, true));
	}
	if (distinct_count.has_value() && *distinct_count > 0) {
		result.SetDistinctCount(*distinct_count);
	}
}

// The statistics object a path below a column's leads to, following the RETURN_STATS naming of nested columns;
// nullptr when the path names nothing of the type.
static optional_ptr<BaseStatistics> DescendColumnStatistics(BaseStatistics &stats, const vector<string> &path,
                                                            idx_t depth) {
	if (depth >= path.size()) {
		return &stats;
	}
	auto &type = stats.GetType();
	auto &component = path[depth];
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		for (idx_t i = 0; i < StructType::GetChildCount(type); i++) {
			if (StructType::GetChildName(type, i) == component) {
				return DescendColumnStatistics(StructStats::GetChildStats(stats, i), path, depth + 1);
			}
		}
		return nullptr;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		if (component == "element") {
			return DescendColumnStatistics(ListStats::GetChildStats(stats), path, depth + 1);
		}
		return nullptr;
	case LogicalTypeId::MAP: {
		// a map's statistics are those of a list of key/value structs
		auto &entry_stats = ListStats::GetChildStats(stats);
		if (component == "key") {
			return DescendColumnStatistics(StructStats::GetChildStats(entry_stats, 0), path, depth + 1);
		}
		if (component == "value") {
			return DescendColumnStatistics(StructStats::GetChildStats(entry_stats, 1), path, depth + 1);
		}
		return nullptr;
	}
	default:
		return nullptr;
	}
}

void MultiFileOptions::VerifyColumnStatistics(const vector<Identifier> &names, const vector<LogicalType> &types) const {
	if (column_statistics.IsNull()) {
		return;
	}
	for (auto &entry : MapValue::GetChildren(column_statistics)) {
		auto &key_value = StructValue::GetChildren(entry);
		if (key_value[0].IsNull() || key_value[1].IsNull()) {
			continue;
		}
		auto &key = StringValue::Get(key_value[0]);
		auto path = ParseColumnStatisticsKey(key);
		optional_idx column;
		for (idx_t i = 0; i < names.size(); i++) {
			if (names[i] == Identifier(path[0])) {
				column = i;
				break;
			}
		}
		if (!column.IsValid()) {
			throw InvalidInputException("column_statistics: '%s' names no column of the scan", key);
		}
		// Build the statistics once here, so a key that names nothing of the column, a count that is not one or a
		// bound that does not cast to the column's type fails at bind rather than when the optimizer first asks
		auto stats = BaseStatistics::CreateUnknown(types[column.GetIndex()]);
		auto target = DescendColumnStatistics(stats, path, 1);
		if (!target) {
			throw InvalidInputException("column_statistics: '%s' names no part of column \"%s\" of type %s", key,
			                            path[0], types[column.GetIndex()].ToString());
		}
		ApplyColumnStatistics(*target, key_value[1], key);
	}
}

unique_ptr<BaseStatistics> MultiFileOptions::GetColumnStatistics(const Identifier &column_name,
                                                                 const LogicalType &type) const {
	if (column_statistics.IsNull()) {
		return nullptr;
	}
	unique_ptr<BaseStatistics> result;
	for (auto &entry : MapValue::GetChildren(column_statistics)) {
		auto &key_value = StructValue::GetChildren(entry);
		if (key_value[0].IsNull() || key_value[1].IsNull()) {
			continue;
		}
		auto &key = StringValue::Get(key_value[0]);
		auto path = ParseColumnStatisticsKey(key);
		if (Identifier(path[0]) != column_name) {
			continue;
		}
		if (!result) {
			// Nothing is claimed about the column, or any part of it, that the option does not say
			result = BaseStatistics::CreateUnknown(type).ToUnique();
		}
		auto target = DescendColumnStatistics(*result, path, 1);
		if (!target) {
			// checked at bind, see VerifyColumnStatistics
			throw InternalException("column_statistics: '%s' names no part of column \"%s\"", key,
			                        column_name.GetIdentifierName());
		}
		ApplyColumnStatistics(*target, key_value[1], key);
	}
	return result;
}

bool MultiFileOptions::AnySet() const {
	return filename || hive_partitioning || union_by_name || !schema.empty() || !bound_schema.empty();
}

Value MultiFileOptions::GetHivePartitionValue(const string &value, const string &key, ClientContext &context) const {
	auto it = hive_types_schema.find(key);
	if (it == hive_types_schema.end()) {
		return HivePartitioning::GetValue(context, key, value, LogicalType::VARCHAR);
	}
	return HivePartitioning::GetValue(context, key, value, it->second);
}

} // namespace duckdb
