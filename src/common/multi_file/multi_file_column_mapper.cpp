#include "duckdb/common/multi_file/multi_file_column_mapper.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/filter/list.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

MultiFileColumnMapper::MultiFileColumnMapper(ClientContext &context, MultiFileReader &multi_file_reader,
                                             MultiFileReaderData &reader_data,
                                             const vector<MultiFileColumnDefinition> &global_columns,
                                             const vector<ColumnIndex> &global_column_ids,
                                             optional_ptr<TableFilterSet> filters, MultiFileList &multi_file_list,
                                             const virtual_column_map_t &virtual_columns)
    : context(context), multi_file_reader(multi_file_reader), multi_file_list(multi_file_list),
      reader_data(reader_data), global_columns(global_columns), global_column_ids(global_column_ids),
      global_filters(filters), virtual_columns(virtual_columns) {
}

struct MultiFileIndexMapping {
public:
	explicit MultiFileIndexMapping(MultiFileLocalIndex index) : index(index) {
	}

public:
	MultiFileLocalIndex index;
	unordered_map<MultiFileGlobalIndex, unique_ptr<MultiFileIndexMapping>> child_mapping;
};

//! The local to global conversion required for table filters on this column
enum class FilterConversionType { COPY_DIRECTLY, CAST_FILTER, CANNOT_CONVERT };

struct MultiFileColumnMap {
	MultiFileColumnMap(MultiFileLocalIndex index, const LogicalType &local_type_p, const LogicalType &global_type_p)
	    : mapping(index), local_type(local_type_p), global_type(global_type_p),
	      filter_conversion(local_type == global_type ? FilterConversionType::COPY_DIRECTLY
	                                                  : FilterConversionType::CAST_FILTER) {
	}
	MultiFileColumnMap(MultiFileIndexMapping mapping_p, const LogicalType &local_type_p,
	                   const LogicalType &global_type_p, FilterConversionType filter_conversion)
	    : mapping(std::move(mapping_p)), local_type(local_type_p), global_type(global_type_p),
	      filter_conversion(filter_conversion) {
	}

	MultiFileIndexMapping mapping;
	const LogicalType local_type;
	const LogicalType global_type;
	FilterConversionType filter_conversion;
};

struct ResultColumnMapping {
	unordered_map<MultiFileGlobalIndex, MultiFileColumnMap> global_to_local;
	string error;

public:
	bool HasError() const {
		return !error.empty();
	}
};

struct ColumnMapResult {
	//! Contains the name of the local column that corresponds to this field
	Value column_map;
	unique_ptr<Expression> default_value;
	optional_ptr<const MultiFileColumnDefinition> local_column;
	unique_ptr<ColumnIndex> column_index;
	unique_ptr<MultiFileIndexMapping> mapping;
};

struct ColumnMapper {
	virtual ~ColumnMapper() = default;
	virtual unique_ptr<ColumnMapper> Create(const vector<MultiFileColumnDefinition> &columns) const = 0;
	virtual MultiFileLocalIndex Find(const MultiFileColumnDefinition &column) const = 0;
	virtual unique_ptr<Expression> GetDefaultExpression(ClientContext &context, const MultiFileColumnDefinition &column,
	                                                    bool is_root) const = 0;
	virtual idx_t MapCount() const = 0;
};

struct FieldIdMapper : public ColumnMapper {
	explicit FieldIdMapper(const vector<MultiFileColumnDefinition> &columns) {
		for (idx_t col_idx = 0; col_idx < columns.size(); col_idx++) {
			auto &column = columns[col_idx];
			if (column.identifier.IsNull()) {
				// Extra columns at the end will not have a field_id
				break;
			}
			auto field_id = column.GetIdentifierFieldId();
			field_id_map.emplace(field_id, MultiFileLocalIndex(col_idx));
		}
	}

	unique_ptr<ColumnMapper> Create(const vector<MultiFileColumnDefinition> &columns) const override {
		return make_uniq<FieldIdMapper>(columns);
	}
	MultiFileLocalIndex Find(const MultiFileColumnDefinition &column) const override {
		D_ASSERT(!column.identifier.IsNull());
		auto entry = field_id_map.find(column.GetIdentifierFieldId());
		if (entry == field_id_map.end()) {
			return MultiFileLocalIndex();
		}
		return entry->second;
	}
	static unique_ptr<Expression> GetDefault(ClientContext &context, const MultiFileColumnDefinition &column) {
		auto &default_val = column.default_expression;
		if (!default_val) {
			throw InvalidInputException("Field %s (field id %d) is missing from the file schema and has no default "
			                            "expression",
			                            column.name, column.GetIdentifierFieldId());
		}
		auto binder = Binder::CreateBinder(context);
		binder->SetCanContainNulls(true);
		ConstantBinder constant_binder(*binder, context, "default value");
		auto expr = default_val->Copy();
		LogicalType target_type = column.type;
		auto bound = constant_binder.Bind(expr, &target_type);
		if (!bound->IsFoldable()) {
			return bound;
		}
		// a constant default is kept as a constant so filters on the missing column can be evaluated up front
		return make_uniq<BoundConstantExpression>(ExpressionExecutor::EvaluateScalar(context, *bound, true));
	}

	unique_ptr<Expression> GetDefaultExpression(ClientContext &context, const MultiFileColumnDefinition &column,
	                                            bool is_root) const override {
		return GetDefault(context, column);
	}
	idx_t MapCount() const override {
		return field_id_map.size();
	}

private:
	unordered_map<int32_t, MultiFileLocalIndex> field_id_map;
};

struct NameMapper : public ColumnMapper {
	NameMapper(MultiFileColumnMapper &mapper, const vector<MultiFileColumnDefinition> &columns) : mapper(mapper) {
		for (idx_t col_idx = 0; col_idx < columns.size(); col_idx++) {
			auto &column = columns[col_idx];
			name_map.emplace(column.name, MultiFileLocalIndex(col_idx));
		}
	}

	unique_ptr<ColumnMapper> Create(const vector<MultiFileColumnDefinition> &columns) const override {
		return make_uniq<NameMapper>(mapper, columns);
	}
	MultiFileLocalIndex Find(const MultiFileColumnDefinition &column) const override {
		auto entry = name_map.find(column.GetIdentifierName());
		if (entry == name_map.end()) {
			return MultiFileLocalIndex();
		}
		return entry->second;
	}
	unique_ptr<Expression> GetDefaultExpression(ClientContext &context, const MultiFileColumnDefinition &column,
	                                            bool is_root) const override {
		if (column.default_expression) {
			// we have an explicit default - return it
			return FieldIdMapper::GetDefault(context, column);
		}
		// no explicit default and no match
		if (is_root) {
			// no match found in a root column - throw an error
			mapper.ThrowColumnNotFoundError(column.GetIdentifierName());
		}
		// no match found in a struct field - replace with NULL
		return make_uniq<BoundConstantExpression>(Value(column.type));
	}

	idx_t MapCount() const override {
		return name_map.size();
	}

private:
	MultiFileColumnMapper &mapper;
	case_insensitive_map_t<MultiFileLocalIndex> name_map;
};

void MultiFileColumnMapper::ThrowColumnNotFoundError(const string &global_column_name) const {
	auto &reader = *reader_data.reader;
	auto &local_columns = reader.GetColumns();

	string candidate_names;
	for (auto &column : local_columns) {
		if (!candidate_names.empty()) {
			candidate_names += ", ";
		}
		candidate_names += column.name;
	}
	auto &file_name = reader.GetFileName();
	throw InvalidInputException("Failed to read file \"%s\": schema mismatch in glob: column \"%s\" was read from "
	                            "the original file \"%s\", but could not be found in file \"%s\".\nCandidate names: "
	                            "%s\nIf you are trying to "
	                            "read files with different schemas, try setting union_by_name=True",
	                            file_name, global_column_name, multi_file_list.GetFirstFile().path, file_name,
	                            candidate_names);
}

//! Check if a column is trivially mappable (i.e. the column is effectively identical to the global column)
static bool IsTriviallyMappable(const MultiFileColumnDefinition &global_column,
                                const vector<MultiFileColumnDefinition> &local_columns, const ColumnMapper &mapper,
                                optional_idx expected_idx = optional_idx()) {
	auto entry = mapper.Find(global_column);
	if (!entry.IsValid()) {
		return false;
	}
	auto local_id = entry.GetIndex();
	if (expected_idx.IsValid() && local_id != expected_idx.GetIndex()) {
		return false;
	}
	auto &local_column = local_columns[local_id];
	if (local_column.type != global_column.type) {
		return false;
	}
	if (local_column.children.size() != global_column.children.size()) {
		// child count difference - cannot map trivially
		return false;
	}
	auto nested_mapper = mapper.Create(local_column.children);
	for (idx_t i = 0; i < global_column.children.size(); i++) {
		auto &global_child = global_column.children[i];
		bool trivially_mappable = IsTriviallyMappable(global_child, local_column.children, *nested_mapper, i);
		if (!trivially_mappable) {
			return false;
		}
	}
	return true;
}

static ColumnMapResult MapColumn(ClientContext &context, const MultiFileColumnDefinition &global_column,
                                 const ColumnIndex &global_index,
                                 const vector<MultiFileColumnDefinition> &local_columns, const ColumnMapper &mapper,
                                 MultiFileLocalIndex top_level_index = MultiFileLocalIndex());

static ColumnMapResult MapColumnList(ClientContext &context, const MultiFileColumnDefinition &global_column,
                                     const ColumnIndex &global_index, const MultiFileColumnDefinition &local_column,
                                     const MultiFileLocalIndex &local_id, const ColumnMapper &mapper,
                                     unique_ptr<MultiFileIndexMapping> mapping, const bool is_root) {
	const idx_t expected_list_children = 1;
	if (global_column.children.size() != expected_list_children) {
		throw InvalidInputException(
		    "Mismatch between field id children in global_column.children (%d) and list child in type",
		    global_column.children.size());
	}

	auto nested_mapper = mapper.Create(local_column.children);
	child_list_t<Value> column_mapping;
	unique_ptr<Expression> default_expression;
	unordered_map<idx_t, const_reference<ColumnIndex>> selected_children;
	if (global_index.HasChildren()) {
		//! FIXME: is this expected for lists??
		for (auto &index : global_index.GetChildIndexes()) {
			selected_children.emplace(index.GetPrimaryIndex(), index);
		}
	}

	vector<ColumnIndex> child_indexes;
	auto &global_child = global_column.children[0];

	bool is_selected = true;
	const_reference<ColumnIndex> global_child_index = global_index;
	if (!selected_children.empty()) {
		auto entry = selected_children.find(0);
		if (entry != selected_children.end()) {
			// the column is relevant - set the child index
			global_child_index = entry->second;
		} else {
			// not relevant - ignore the column
			is_selected = false;
		}
	}

	ColumnMapResult child_map;
	if (is_selected) {
		child_map = MapColumn(context, global_child, global_child_index.get(), local_column.children, *nested_mapper);
	} else {
		// column is not relevant for the query - push a NULL value
		child_map.default_value = make_uniq<BoundConstantExpression>(Value(global_child.type));
	}

	if (child_map.column_index) {
		child_indexes.push_back(std::move(*child_map.column_index));
		mapping->child_mapping.insert(make_pair(0, std::move(child_map.mapping)));
	}
	if (!child_map.column_map.IsNull()) {
		// found a column mapping for this child - emplace it
		column_mapping.emplace_back("list", std::move(child_map.column_map));
	}

	ColumnMapResult result;
	result.local_column = local_column;
	if (!column_mapping.empty()) {
		// we have column mappings at this level - construct the struct
		result.column_map = Value::STRUCT(std::move(column_mapping));
		if (!is_root) {
			// if this is nested we need to refer to the current column at this level
			vector<Value> child_list;
			child_list.push_back(Value(local_column.name));
			child_list.push_back(std::move(result.column_map));
			result.column_map = Value::TUPLE(std::move(child_list));
		}
	}
	if (is_selected && child_map.default_value) {
		// we have default values at a previous level wrap it in a "list"
		vector<unique_ptr<Expression>> default_expressions;
		child_map.default_value->SetAlias("list");
		default_expressions.push_back(std::move(child_map.default_value));

		// auto default_type = LogicalType::STRUCT(std::move(default_type_list));
		result.default_value = BindBuiltinScalarFunction(context, StructPackFun::Name, std::move(default_expressions));
	}
	result.column_index = make_uniq<ColumnIndex>(local_id.GetIndex(), std::move(child_indexes));
	if (global_index.HasType()) {
		result.column_index->SetType(local_column.type);
	}
	result.mapping = std::move(mapping);
	return result;
}

static ColumnMapResult
MapColumnMapComponent(ClientContext &context,
                      const unordered_map<idx_t, const_reference<ColumnIndex>> &selected_children,
                      const ColumnIndex &global_index, const ColumnMapper &nested_mapper, idx_t component_idx,
                      const MultiFileColumnDefinition &component, const MultiFileColumnDefinition &local_map_column) {
	bool is_selected = true;
	const_reference<ColumnIndex> child_index = global_index;
	if (!selected_children.empty()) {
		auto entry = selected_children.find(component_idx);
		if (entry != selected_children.end()) {
			// the column is relevant - set the child index
			child_index = entry->second;
		} else {
			// not relevant - ignore the column
			is_selected = false;
		}
	}

	ColumnMapResult child_map;
	if (is_selected) {
		child_map = MapColumn(context, component, child_index.get(), local_map_column.children, nested_mapper);
	} else {
		// column is not relevant for the query - push a NULL value
		child_map.default_value = make_uniq<BoundConstantExpression>(Value(component.type));
	}
	return child_map;
}

static bool IsInvalidMapKeyDefault(const ColumnMapResult &mapping) {
	if (!mapping.default_value) {
		return false;
	}
	auto &expr = *mapping.default_value;
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &constant_expr = expr.Cast<BoundConstantExpression>();
	return constant_expr.GetValue().IsNull();
}

static ColumnMapResult MapColumnMap(ClientContext &context, const MultiFileColumnDefinition &global_column,
                                    const ColumnIndex &global_index, const MultiFileColumnDefinition &local_column,
                                    const MultiFileLocalIndex &local_id, const ColumnMapper &mapper,
                                    unique_ptr<MultiFileIndexMapping> mapping, const bool is_root) {
	const idx_t expected_map_children = 2;
	if (global_column.children.size() != expected_map_children) {
		throw InvalidInputException(
		    "Mismatch between field id children in global_column.children (%d) and map children in type",
		    global_column.children.size());
	}

	D_ASSERT(local_column.children.size() == 1);
	D_ASSERT(local_column.children[0].name == "key_value");
	auto &local_key_value = local_column.children[0];

	auto nested_mapper = mapper.Create(local_key_value.children);
	child_list_t<Value> column_mapping;
	vector<unique_ptr<Expression>> default_expressions;
	unordered_map<idx_t, const_reference<ColumnIndex>> selected_children;
	if (global_index.HasChildren()) {
		//! FIXME: is this expected for maps??
		for (auto &index : global_index.GetChildIndexes()) {
			selected_children.emplace(index.GetPrimaryIndex(), index);
		}
	}

	vector<ColumnIndex> child_indexes;
	auto &global_key = global_column.children[0];
	auto &global_value = global_column.children[1];

	child_list_t<reference<const MultiFileColumnDefinition>> map_components;
	map_components.emplace_back("key", global_key);
	map_components.emplace_back("value", global_value);

	for (idx_t i = 0; i < map_components.size(); i++) {
		auto &name = map_components[i].first;
		auto &global_component = map_components[i].second;

		auto map_result = MapColumnMapComponent(context, selected_children, global_index, *nested_mapper, i,
		                                        global_component, local_key_value);
		if (name == "key" && IsInvalidMapKeyDefault(map_result)) {
			throw InvalidInputException(
			    "'key' of MAP did not map to a value and the registered DEFAULT is NULL, which is not allowed");
		}

		if (map_result.column_index) {
			child_indexes.push_back(std::move(*map_result.column_index));
			mapping->child_mapping.insert(make_pair(i, std::move(map_result.mapping)));
		}
		if (!map_result.column_map.IsNull()) {
			// found a column mapping for the component - emplace it
			column_mapping.emplace_back(name, std::move(map_result.column_map));
		}
		if (map_result.default_value) {
			map_result.default_value->SetAlias(name);
			default_expressions.push_back(std::move(map_result.default_value));
		}
	}

	ColumnMapResult result;
	result.local_column = local_column;
	if (!column_mapping.empty()) {
		// we have column mappings at this level - construct the struct
		result.column_map = Value::STRUCT(std::move(column_mapping));
		if (!is_root) {
			// if this is nested we need to refer to the current column at this level
			vector<Value> child_list;
			child_list.push_back(Value(local_column.name));
			child_list.push_back(std::move(result.column_map));
			result.column_map = Value::TUPLE(std::move(child_list));
		}
	}
	if (!default_expressions.empty()) {
		// we have default values at a previous level wrap it in a "list"
		result.default_value = BindBuiltinScalarFunction(context, StructPackFun::Name, std::move(default_expressions));
	}
	vector<ColumnIndex> map_indexes;
	map_indexes.emplace_back(0, std::move(child_indexes));

	result.column_index = make_uniq<ColumnIndex>(local_id.GetIndex(), std::move(map_indexes));
	if (global_index.HasType()) {
		result.column_index->SetType(local_column.type);
	}
	result.mapping = std::move(mapping);
	return result;
}

static ColumnMapResult MapColumnStruct(ClientContext &context, const MultiFileColumnDefinition &global_column,
                                       const ColumnIndex &global_index, const MultiFileColumnDefinition &local_column,
                                       const MultiFileLocalIndex &local_id, const ColumnMapper &mapper,
                                       unique_ptr<MultiFileIndexMapping> mapping, const bool is_root) {
	auto &struct_children = StructType::GetChildTypes(global_column.type);
	if (struct_children.size() != global_column.children.size()) {
		throw InvalidInputException(
		    "Mismatch between field id children in global_column.children and struct children in type");
	}

	auto nested_mapper = mapper.Create(local_column.children);

	if (global_index.IsPushdownExtract()) {
		auto &child_index = global_index.GetChildIndexes()[0];
		auto &child_column = global_column.children[child_index.GetPrimaryIndex()];
		auto child_mapping = MapColumn(context, child_column, child_index, local_column.children, *nested_mapper);

		if (child_mapping.column_index) {
			auto &child_column_index = *child_mapping.column_index;
			if (child_mapping.local_column && child_mapping.local_column->type.id() == LogicalTypeId::STRUCT &&
			    !child_column_index.IsPushdownExtract()) {
				// the extracted field is read with the type it has in this file, and then remapped to the global type
				// (the fields of a nested struct can differ between files, e.g. with union_by_name)
				child_column_index.SetType(child_mapping.local_column->type);
				if (child_mapping.column_map.type().id() == LogicalTypeId::TUPLE) {
					// the mapping of a nested struct is (local name, mapping) - we extract the field directly
					auto map_entry = StructValue::GetChildren(child_mapping.column_map)[1];
					child_mapping.column_map = std::move(map_entry);
				}
			}
			vector<ColumnIndex> single_child;
			single_child.push_back(std::move(*child_mapping.column_index));
			child_mapping.column_index = make_uniq<ColumnIndex>(local_id.GetIndex(), single_child);
			child_mapping.column_index->SetType(local_column.type);
			child_mapping.column_index->SetPushdownExtract();

			mapping->child_mapping.emplace(MultiFileGlobalIndex(0), std::move(child_mapping.mapping));
			child_mapping.mapping = std::move(mapping);
		}
		return child_mapping;
	}

	child_list_t<Value> column_mapping;
	vector<unique_ptr<Expression>> default_expressions;
	unordered_map<idx_t, const_reference<ColumnIndex>> selected_children;
	if (global_index.HasChildren()) {
		for (auto &index : global_index.GetChildIndexes()) {
			selected_children.emplace(index.GetPrimaryIndex(), index);
		}
	}

	vector<ColumnIndex> child_indexes;
	for (idx_t i = 0; i < global_column.children.size(); i++) {
		bool is_selected = true;
		const_reference<ColumnIndex> global_child_index = global_index;
		if (!selected_children.empty()) {
			auto entry = selected_children.find(i);
			if (entry != selected_children.end()) {
				// the column is relevant - set the child index
				global_child_index = entry->second;
			} else {
				// not relevant - ignore the column
				is_selected = false;
			}
		}
		auto &global_child = global_column.children[i];
		ColumnMapResult child_map;
		if (is_selected) {
			child_map =
			    MapColumn(context, global_child, global_child_index.get(), local_column.children, *nested_mapper);
		} else {
			// column is not relevant for the query - push a NULL value
			child_map.default_value = make_uniq<BoundConstantExpression>(Value(global_child.type));
		}

		if (child_map.column_index) {
			child_indexes.push_back(std::move(*child_map.column_index));
			mapping->child_mapping.insert(make_pair(i, std::move(child_map.mapping)));
		}
		if (!child_map.column_map.IsNull()) {
			// found a column mapping for this child - emplace it
			column_mapping.emplace_back(global_child.name, std::move(child_map.column_map));
		}
		//! FIXME: the 'default_value' should only be used if the STRUCT's default value is not NULL
		if (child_map.default_value) {
			// found a default value for this child - emplace it
			child_map.default_value->SetAlias(Identifier(global_child.name));
			default_expressions.push_back(std::move(child_map.default_value));
		}
	}

	ColumnMapResult result;
	result.local_column = local_column;

	if (!column_mapping.empty()) {
		// we have column mappings at this level - construct the struct
		result.column_map = Value::STRUCT(std::move(column_mapping));
		if (!is_root) {
			// if this is nested we need to refer to the current column at this level
			vector<Value> child_list;
			child_list.push_back(Value(local_column.name));
			child_list.push_back(std::move(result.column_map));
			result.column_map = Value::TUPLE(std::move(child_list));
		}
	}

	if (!default_expressions.empty()) {
		result.default_value = BindBuiltinScalarFunction(context, StructPackFun::Name, std::move(default_expressions));
	}
	result.column_index = make_uniq<ColumnIndex>(local_id.GetIndex(), std::move(child_indexes));
	if (global_index.HasType()) {
		result.column_index->SetType(local_column.type);
	}
	result.mapping = std::move(mapping);
	return result;
}

static ColumnMapResult MapColumn(ClientContext &context, const MultiFileColumnDefinition &global_column,
                                 const ColumnIndex &global_index,
                                 const vector<MultiFileColumnDefinition> &local_columns, const ColumnMapper &mapper,
                                 MultiFileLocalIndex top_level_index) {
	bool is_root = top_level_index.IsValid();
	ColumnMapResult result;
	auto local_idx = mapper.Find(global_column);
	if (!local_idx.IsValid()) {
		// entry not present in map, use default value
		result.default_value = mapper.GetDefaultExpression(context, global_column, is_root);
		return result;
	}
	// the field exists! get the local column
	auto &local_column = local_columns[local_idx];
	if (local_column.type.id() == LogicalTypeId::SQLNULL) {
		// An explicitly NULL field can represent any result type, including a nested field.
		// Do not feed it into remap_struct, which requires matching nested source types.
		auto &type = (is_root || global_index.IsPushdownExtract()) && global_index.HasType()
		                 ? global_index.GetScanType()
		                 : global_column.type;
		result.default_value = make_uniq<BoundConstantExpression>(Value(type));
		return result;
	}
	auto mapping_idx = is_root ? top_level_index : local_idx;
	auto mapping = make_uniq<MultiFileIndexMapping>(mapping_idx);
	if (global_column.children.empty() || !local_column.type.IsNested()) {
		// Map directly when no child remapping is needed or the source requires a scalar cast.
		result.column_map = Value(local_column.name);
		result.column_index = make_uniq<ColumnIndex>(global_index.RemapRootIndex(local_idx.GetIndex()));
		result.mapping = std::move(mapping);
		result.local_column = local_column;
		return result;
	}

	// nested type - check if the field identifiers match and if we need to remap
	D_ASSERT(global_column.type.IsNested());
	if (global_column.type.id() != local_column.type.id()) {
		throw BinderException("Failed to map file-column of type '%s' to result-column of type '%s'", local_column.type,
		                      global_column.type);
	}

	switch (global_column.type.id()) {
	case LogicalTypeId::STRUCT:
		return MapColumnStruct(context, global_column, global_index, local_column, local_idx, mapper,
		                       std::move(mapping), is_root);
	case LogicalTypeId::LIST:
		return MapColumnList(context, global_column, global_index, local_column, local_idx, mapper, std::move(mapping),
		                     is_root);
	case LogicalTypeId::MAP:
		return MapColumnMap(context, global_column, global_index, local_column, local_idx, mapper, std::move(mapping),
		                    is_root);
	case LogicalTypeId::ARRAY: {
		throw NotImplementedException("Can't map an ARRAY with nested children!");
	}
	default:
		throw NotImplementedException("MapColumn for children of type %s not implemented",
		                              global_column.type.ToString());
	}
}

static unique_ptr<Expression> ConstructMapExpression(ClientContext &context, MultiFileLocalIndex local_idx,
                                                     ColumnMapResult &mapping, const LogicalType &global_column_type,
                                                     const LogicalType &local_column_type, bool is_trivially_mappable) {
	unique_ptr<Expression> expr = make_uniq<BoundReferenceExpression>(local_column_type, local_idx.GetIndex());
	if (mapping.column_map.type().id() == LogicalTypeId::TUPLE) {
		// A pushed-down extract already selects the source column; retain only its child mapping.
		auto child_mapping = StructValue::GetChildren(mapping.column_map)[1];
		mapping.column_map = std::move(child_mapping);
	}
	const bool can_use_remap_struct =
	    global_column_type.IsNested() &&
	    (mapping.column_map.IsNull() || mapping.column_map.type().id() == LogicalTypeId::STRUCT) &&
	    !is_trivially_mappable && local_column_type.IsNested();
	if (!can_use_remap_struct) {
		// not a struct - potentially add a cast
		if (local_column_type.id() == LogicalTypeId::VARCHAR && global_column_type.id() == LogicalTypeId::BLOB) {
			// the text in a file becomes its UTF-8 bytes, it is not a blob literal with \x escapes
			expr = BoundCastExpression::Create(std::move(expr), global_column_type,
			                                   BoundCastInfo(DefaultCasts::ReinterpretCast));
		} else if (local_column_type != global_column_type) {
			expr = BoundCastExpression::AddCastToType(context, std::move(expr), global_column_type);
		}
		return expr;
	}
	// generate the remap_struct function call
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(expr));
	children.push_back(make_uniq<BoundConstantExpression>(Value(global_column_type)));
	children.push_back(make_uniq<BoundConstantExpression>(std::move(mapping.column_map)));
	if (!mapping.default_value) {
		children.push_back(make_uniq<BoundConstantExpression>(Value()));
	} else {
		children.push_back(std::move(mapping.default_value));
	}
	return BindBuiltinScalarFunction(context, RemapStructFun::Name, std::move(children));
}

ResultColumnMapping MultiFileColumnMapper::CreateColumnMappingByMapper(const ColumnMapper &mapper) {
	auto &reader = *reader_data.reader;
	auto &local_columns = reader.GetColumns();

	ResultColumnMapping result;

	// loop through the schema definition
	auto &expressions = reader_data.expressions;
	for (idx_t i = 0; i < global_column_ids.size(); i++) {
		auto global_idx = MultiFileGlobalIndex(i);

		optional_idx constant_idx;
		for (idx_t j = 0; j < reader_data.constant_map.size(); j++) {
			auto constant_index = MultiFileConstantMapIndex(j);
			auto &entry = reader_data.constant_map[constant_index];
			if (entry.column_idx.GetIndex() == i) {
				constant_idx = j;
				break;
			}
		}
		if (constant_idx.IsValid()) {
			// this column is constant for this file
			auto constant_index = MultiFileConstantMapIndex(constant_idx.GetIndex());
			auto &constant_entry = reader_data.constant_map[constant_index];
			expressions.push_back(make_uniq<BoundConstantExpression>(constant_entry.value));
			continue;
		}

		// Handle any generate columns that are not in the schema (currently only file_row_number)
		auto &global_id = global_column_ids[i];
		auto global_column_id = global_id.GetPrimaryIndex();
		optional_ptr<MultiFileColumnDefinition> global_column_reference;

		optional_ptr<const MultiFileColumnDefinition> global_column_p;
		auto local_idx = MultiFileLocalIndex(reader.column_ids.size());
		if (IsVirtualColumn(global_column_id)) {
			// virtual column - look it up in the virtual column entry map
			auto virtual_entry = virtual_columns.find(global_column_id);
			if (virtual_entry == virtual_columns.end()) {
				throw InternalException("Virtual column id %d not found in virtual columns map", global_column_id);
			}
			auto &virtual_column_type = virtual_entry->second.type;
			// check if this column is constant for the entire file
			auto constant_expr =
			    multi_file_reader.GetConstantVirtualColumn(reader_data, global_column_id, virtual_column_type);
			if (constant_expr) {
				// the column is constant for the entire file - handle it
				expressions.push_back(std::move(constant_expr));
				continue;
			}
			// the column is not constant for the file
			// get the expression to evaluate the column OR the global column to read into
			auto virtual_column_bind_result = multi_file_reader.GetVirtualColumnExpression(
			    context, reader_data, local_columns, global_column_id, virtual_column_type, local_idx);
			auto bind_result_type = virtual_column_bind_result.GetType();
			switch (bind_result_type) {
			case MultiFileReaderVirtualColumnBinding::VirtualColumnBindingType::CONSTANT: {
				expressions.push_back(make_uniq<BoundConstantExpression>(virtual_column_bind_result.constant));
				continue;
			}
			case MultiFileReaderVirtualColumnBinding::VirtualColumnBindingType::EXPRESSION: {
				auto &expr = virtual_column_bind_result.expression;
				auto &column_ids = virtual_column_bind_result.local_virtual_column_ids;

				auto is_reference = expr->GetExpressionType() == ExpressionType::BOUND_REF;
				expressions.push_back(std::move(expr));

				// add the virtual column to the reader
				bool seen_virtual_column = false;
				for (auto &id : column_ids) {
					if (!IsVirtualColumn(id)) {
						reader.column_ids.push_back(MultiFileLocalColumnId(id));
						reader.column_indexes.push_back(global_id.RemapRootIndex(id));
						continue;
					}
					if (seen_virtual_column) {
						throw InternalException("GetVirtualColumnExpression is only allowed to return one virtual "
						                        "column id for the EXPRESSION result");
					}
					seen_virtual_column = true;
					MultiFileLocalColumnId local_id(reader.columns.size());
					auto local_index = global_id.RemapRootIndex(local_id.GetId());

					reader.columns.emplace_back(virtual_entry->second.name.GetIdentifierName(), virtual_column_type);
					reader.column_ids.push_back(local_id);
					reader.column_indexes.push_back(std::move(local_index));
					reader.AddVirtualColumn(id);
				}

				// set it as being projected in this spot
				MultiFileColumnMap index_mapping(local_idx, virtual_column_type, virtual_column_type);
				if (!is_reference) {
					index_mapping.filter_conversion = FilterConversionType::CANNOT_CONVERT;
				}
				result.global_to_local.insert(make_pair(global_idx, std::move(index_mapping)));
				continue;
			}
			case MultiFileReaderVirtualColumnBinding::VirtualColumnBindingType::COLUMN_REFERENCE: {
				global_column_p = virtual_column_bind_result.global_column_reference;
				break;
			}
			}
		} else {
			global_column_p = global_columns[global_column_id];
		}

		const auto &global_column = *global_column_p;
		if (reader.UseCastMap()) {
			// reader is responsible for converting types - perform a top-level match only
			auto entry = mapper.Find(global_column);
			if (!entry.IsValid()) {
				// the file lacks the column - it takes its default value, and is an error when it has none
				reader_data.expressions.push_back(mapper.GetDefaultExpression(context, global_column, true));
				continue;
			}
			MultiFileLocalColumnId local_id(entry.GetIndex());
			auto local_index = global_id.RemapRootIndex(local_id.GetId());
			auto &local_type = local_columns[local_id.GetId()].type;
			auto &global_type = global_column.type;
			auto expr = make_uniq<BoundReferenceExpression>(global_type, local_idx.GetIndex());
			if (global_type != local_type) {
				reader.cast_map[local_id.GetId()] = global_type;
				// if types are not equivalent we need to remove the child indexes
				local_index.GetChildIndexesMutable().clear();
			}
			reader_data.expressions.push_back(std::move(expr));

			MultiFileColumnMap index_mapping(local_idx, local_type, global_type);
			result.global_to_local.insert(make_pair(global_idx, std::move(index_mapping)));
			reader.column_ids.push_back(local_id);
			reader.column_indexes.push_back(std::move(local_index));
			continue;
		}

		auto column_map = MapColumn(context, global_column, global_id, local_columns, mapper, local_idx);
		if (!column_map.column_index) {
			// no columns were emitted
			reader_data.expressions.push_back(std::move(column_map.default_value));
			continue;
		}
		auto trivial_map = IsTriviallyMappable(global_column, local_columns, mapper);
		auto local_index = std::move(column_map.column_index);
		auto local_id = local_index->GetPrimaryIndex();
		auto &local_type = local_columns[local_id].type;
		auto global_column_type = global_id.HasType() ? global_id.GetScanType() : global_column.type;
		auto local_column_type = local_index->HasType() ? local_index->GetScanType() : local_type;
		auto expr =
		    ConstructMapExpression(context, local_idx, column_map, global_column_type, local_column_type, trivial_map);
		reader_data.expressions.push_back(std::move(expr));
		auto filter_conversion = trivial_map ? FilterConversionType::COPY_DIRECTLY : FilterConversionType::CAST_FILTER;

		MultiFileColumnMap index_mapping(std::move(*column_map.mapping), local_column_type, global_column_type,
		                                 filter_conversion);
		result.global_to_local.insert(make_pair(global_idx, std::move(index_mapping)));
		reader.column_ids.emplace_back(local_id);
		reader.column_indexes.push_back(std::move(*local_index));
	}
	D_ASSERT(global_column_ids.size() == reader_data.expressions.size());
	return result;
}

ResultColumnMapping MultiFileColumnMapper::CreateColumnMapping(MultiFileColumnMappingMode mapping_mode) {
	auto &reader = *reader_data.reader;
	auto &local_columns = reader.GetColumns();
	switch (mapping_mode) {
	case MultiFileColumnMappingMode::BY_NAME: {
		// we have expected types: create a map of name -> (local) column id
		NameMapper name_map(*this, local_columns);
		return CreateColumnMappingByMapper(name_map);
	}
	case MultiFileColumnMappingMode::BY_FIELD_ID: {
#ifdef DEBUG
		//! Make sure the global columns have field_ids to match on
		for (auto &column : global_columns) {
			D_ASSERT(!column.identifier.IsNull());
			D_ASSERT(column.identifier.type().id() == LogicalTypeId::INTEGER);
		}
#endif

		// we have expected types: create a map of field_id -> column index
		FieldIdMapper field_id_map(local_columns);
		return CreateColumnMappingByMapper(field_id_map);
	}
	default: {
		throw InternalException("Unsupported MultiFileColumnMappingMode type");
	}
	}
}

static unique_ptr<Expression> GetFilterExpression(const TableFilter &filter);

bool EvaluateTableFilterAgainstConstant(ClientContext &context, const TableFilter &filter, const Value &constant) {
	auto expression = GetFilterExpression(filter);
	return ExpressionFilter(std::move(expression)).EvaluateWithConstant(context, constant);
}

bool MultiFileColumnMapper::EvaluateFilterAgainstConstant(const TableFilter &filter, const Value &constant) {
	return EvaluateTableFilterAgainstConstant(context, filter, constant);
}

Value MultiFileColumnMapper::GetConstantValue(MultiFileGlobalIndex global_index) {
	auto global_column_id = global_column_ids[global_index].GetPrimaryIndex();
	auto &expr = reader_data.expressions[global_index];
	if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		return expr->Cast<BoundConstantExpression>().GetValue();
	}
	for (idx_t i = 0; i < reader_data.constant_map.size(); i++) {
		auto &constant_map_entry = reader_data.constant_map[MultiFileConstantMapIndex(i)];
		if (constant_map_entry.column_idx == global_index) {
			return constant_map_entry.value;
		}
	}
	auto &global_column = global_columns[global_column_id];
	throw InternalException("Column %s is not present in the file, but no constant_map entry exists for it!",
	                        global_column.name);
}

ReaderInitializeType
MultiFileColumnMapper::EvaluateConstantFilters(ResultColumnMapping &mapping,
                                               map<MultiFileGlobalIndex, reference<TableFilter>> &remaining_filters) {
	if (!global_filters) {
		return ReaderInitializeType::INITIALIZED;
	}
	auto &global_to_local = mapping.global_to_local;
	for (auto &it : *global_filters) {
		MultiFileGlobalIndex global_index(it.GetIndex());
		auto &global_filter = it.Filter();

		auto local_it = global_to_local.find(global_index);
		if (local_it != global_to_local.end()) {
			//! File has this column, filter needs to be evaluated later
			remaining_filters.emplace(global_index, global_filter);
			continue;
		}

		//! FIXME: this does not check for filters against struct fields that are not present in the file
		auto constant_value = GetConstantValue(global_index);
		if (!EvaluateFilterAgainstConstant(global_filter, constant_value)) {
			return ReaderInitializeType::SKIP_READING_FILE;
		}
	}
	return ReaderInitializeType::INITIALIZED;
}

static unique_ptr<Expression> CreateReferenceExpression(const LogicalType &type) {
	return make_uniq<BoundReferenceExpression>(type, 0ULL);
}

static unique_ptr<Expression> GetFilterExpression(const TableFilter &filter) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "GetFilterExpression");
	return expr_filter.expr->Copy();
}

static unique_ptr<Expression> CreateStructExtractExpression(unique_ptr<Expression> source_expr,
                                                            const LogicalType &source_type, idx_t child_idx) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(source_expr));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(static_cast<int64_t>(child_idx + 1))));

	BoundScalarFunction bound_func(GetExtractAtFunction());
	bound_func.SetReturnType(StructType::GetChildType(source_type, child_idx));

	return make_uniq<BoundFunctionExpression>(std::move(bound_func), std::move(arguments),
	                                          StructExtractAtFun::GetBindData(child_idx));
}

//! A cast matches STRUCT fields by name, so nested fields need the same names and positions and invertible casts
static bool MappedCastIsInvertible(const MultiFileIndexMapping &mapping, const LogicalType &source_type,
                                   const LogicalType &target_type) {
	if (!BoundCastExpression::CastIsInvertible(source_type, target_type)) {
		return false;
	}
	if (!source_type.IsNested()) {
		return true;
	}
	// the reader only maps the children of STRUCT, LIST and MAP one by one
	auto type_id = source_type.id();
	if (type_id != target_type.id() ||
	    (type_id != LogicalTypeId::STRUCT && type_id != LogicalTypeId::LIST && type_id != LogicalTypeId::MAP)) {
		return false;
	}
	auto source_children = LogicalType::GetNamedChildTypes(source_type);
	auto target_children = LogicalType::GetNamedChildTypes(target_type);
	if (source_children.size() != target_children.size()) {
		return false;
	}
	for (idx_t i = 0; i < source_children.size(); i++) {
		auto entry = mapping.child_mapping.find(MultiFileGlobalIndex(i));
		if (entry == mapping.child_mapping.end() || entry->second->index.GetIndex() != i ||
		    !(source_children[i].first == target_children[i].first) ||
		    !MappedCastIsInvertible(*entry->second, source_children[i].second, target_children[i].second)) {
			return false;
		}
	}
	return true;
}

static bool TryCastConstant(Value &constant, const MultiFileIndexMapping &mapping, const LogicalType &target_type) {
	// this filter replaces the global one, so both casts must preserve which rows match
	if (!BoundCastExpression::CastIsInvertible(constant.type(), target_type) ||
	    !MappedCastIsInvertible(mapping, target_type, constant.type())) {
		return false;
	}
	auto cast = constant.DefaultTryCastAs(target_type);
	if (!cast) {
		return false;
	}
	auto roundtrip = cast->DefaultTryCastAs(constant.type());
	if (!roundtrip || *roundtrip != constant) {
		return false;
	}
	constant = std::move(*cast);
	return true;
}

struct RewrittenMappedExpression {
	unique_ptr<Expression> expr;
	const MultiFileIndexMapping *mapping = nullptr;
	const LogicalType *type = nullptr;
};

static RewrittenMappedExpression RewriteMappedValueExpression(const Expression &expr,
                                                              const MultiFileIndexMapping &mapping,
                                                              const LogicalType &target_type) {
	RewrittenMappedExpression result;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
		result.expr = CreateReferenceExpression(target_type);
		result.mapping = &mapping;
		result.type = &target_type;
		return result;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		idx_t child_idx;
		if (!TryGetStructExtractChildIndex(func, child_idx)) {
			return result;
		}
		auto child_result = RewriteMappedValueExpression(*func.GetChildren()[0], mapping, target_type);
		if (!child_result.expr || !child_result.mapping || !child_result.type ||
		    child_result.type->id() != LogicalTypeId::STRUCT) {
			return result;
		}
		auto entry = child_result.mapping->child_mapping.find(MultiFileGlobalIndex(child_idx));
		if (entry == child_result.mapping->child_mapping.end()) {
			return result;
		}
		auto &local_mapping = *entry->second;
		auto local_child_idx = local_mapping.index.GetIndex();
		result.expr = CreateStructExtractExpression(std::move(child_result.expr), *child_result.type, local_child_idx);
		result.mapping = &local_mapping;
		result.type = &StructType::GetChildType(*child_result.type, local_child_idx);
		return result;
	}
	default:
		return result;
	}
}

static unique_ptr<Expression> RewriteDynamicFilterExpression(const shared_ptr<DynamicFilterData> &filter_data,
                                                             const MultiFileIndexMapping &mapping,
                                                             const LogicalType &target_type) {
	if (!filter_data || !filter_data->initialized) {
		return nullptr;
	}
	annotated_shared_lock<annotated_shared_mutex> lock(filter_data->lock);
	auto new_constant = filter_data->constant;
	if (!TryCastConstant(new_constant, mapping, target_type)) {
		return nullptr;
	}
	return BoundComparisonExpression::Create(filter_data->comparison_type, CreateReferenceExpression(target_type),
	                                         make_uniq<BoundConstantExpression>(std::move(new_constant)));
}

static unique_ptr<Expression> TryCastFilterExpression(const Expression &expr, const MultiFileIndexMapping &mapping,
                                                      const LogicalType &target_type) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (ExpressionFilter::IsRootOptionalExpression(expr)) {
			auto optional_child = ExpressionFilter::GetOptionalFilterChild(expr);
			auto child_expr = optional_child ? TryCastFilterExpression(*optional_child, mapping, target_type) : nullptr;
			if (optional_child && !child_expr) {
				return nullptr;
			}
			if (func.Function().GetName() == OptionalFilterScalarFun::NAME) {
				return CreateOptionalFilterExpression(std::move(child_expr), target_type);
			}
			if (!func.BindInfo()) {
				return CreateSelectivityOptionalFilterExpression(std::move(child_expr), target_type, 0.5f, idx_t(6));
			}
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			return CreateSelectivityOptionalFilterExpression(std::move(child_expr), target_type,
			                                                 data.selectivity_threshold, data.n_vectors_to_check);
		}
		if (func.Function().GetName() == DynamicFilterScalarFun::NAME) {
			if (!func.BindInfo()) {
				return nullptr;
			}
			auto &data = func.BindInfo()->Cast<DynamicFilterFunctionData>();
			return RewriteDynamicFilterExpression(data.filter_data, mapping, target_type);
		}
		if (!BoundComparisonExpression::IsComparison(expr)) {
			return nullptr;
		}
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		if (right.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			return nullptr;
		}
		auto lhs = RewriteMappedValueExpression(left, mapping, target_type);
		if (!lhs.expr || !lhs.type) {
			return nullptr;
		}
		auto constant = right.Cast<BoundConstantExpression>().GetValue();
		if (!TryCastConstant(constant, *lhs.mapping, *lhs.type)) {
			return nullptr;
		}
		return BoundComparisonExpression::Create(comparison.GetExpressionType(), std::move(lhs.expr),
		                                         make_uniq<BoundConstantExpression>(std::move(constant)));
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		auto result = make_uniq<BoundConjunctionExpression>(conjunction.GetExpressionType());
		for (auto &child : conjunction.GetChildren()) {
			auto rewritten_child = TryCastFilterExpression(*child, mapping, target_type);
			if (!rewritten_child) {
				return nullptr;
			}
			result->GetChildrenMutable().push_back(std::move(rewritten_child));
		}
		return std::move(result);
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL: {
			if (op.GetChildren().size() != 1) {
				return nullptr;
			}
			auto child = RewriteMappedValueExpression(*op.GetChildren()[0], mapping, target_type);
			if (!child.expr) {
				return nullptr;
			}
			auto result = make_uniq<BoundOperatorExpression>(op.GetExpressionType(), op.GetReturnType());
			result->GetChildrenMutable().push_back(std::move(child.expr));
			return std::move(result);
		}
		case ExpressionType::COMPARE_IN: {
			if (op.GetChildren().empty()) {
				return nullptr;
			}
			auto lhs = RewriteMappedValueExpression(*op.GetChildren()[0], mapping, target_type);
			if (!lhs.expr || !lhs.type) {
				return nullptr;
			}
			auto result = make_uniq<BoundOperatorExpression>(op.GetExpressionType(), op.GetReturnType());
			result->GetChildrenMutable().push_back(std::move(lhs.expr));
			for (idx_t i = 1; i < op.GetChildren().size(); i++) {
				if (op.GetChildren()[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
					return nullptr;
				}
				auto constant = op.GetChildren()[i]->Cast<BoundConstantExpression>().GetValue();
				if (!TryCastConstant(constant, *lhs.mapping, *lhs.type)) {
					return nullptr;
				}
				result->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(std::move(constant)));
			}
			return std::move(result);
		}
		default:
			return nullptr;
		}
	}
	case ExpressionClass::BOUND_CONSTANT:
		return expr.Copy();
	default:
		return nullptr;
	}
}

static unique_ptr<TableFilter> TryCastTableFilter(const TableFilter &global_filter, MultiFileIndexMapping &mapping,
                                                  const LogicalType &target_type) {
	auto filter_expression = GetFilterExpression(global_filter);
	auto rewritten_expr = TryCastFilterExpression(*filter_expression, mapping, target_type);
	if (!rewritten_expr) {
		return nullptr;
	}
	return make_uniq<ExpressionFilter>(std::move(rewritten_expr));
}

static idx_t StartIndexAtZero(unique_ptr<Expression> &root_expr) {
	map<idx_t, vector<reference<BoundReferenceExpression>>> reference_map;

	//! First gather all references, sorted by their index
	ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
	    root_expr, [&reference_map](BoundReferenceExpression &ref, unique_ptr<Expression> &expr) {
		    reference_map[ref.Index()].push_back(ref);
	    });
	idx_t new_index = 0;
	//! Then assign all the references a new index, starting at 0
	for (auto &[_, references] : reference_map) {
		auto index = new_index++;
		for (auto &ref : references) {
			ref.get().IndexMutable() = index;
		}
	}
	return new_index;
}

unique_ptr<TableFilterSet>
MultiFileColumnMapper::CreateFilters(map<MultiFileGlobalIndex, reference<TableFilter>> &filters,
                                     ResultColumnMapping &mapping) {
	if (filters.empty()) {
		return nullptr;
	}
	auto &reader = *reader_data.reader;
	auto &global_to_local = mapping.global_to_local;
	auto result = make_uniq<TableFilterSet>();
	map<idx_t, MultiFileGlobalIndex> local_to_global;

	for (auto &it : filters) {
		auto &global_index = it.first;
		auto &global_filter = it.second.get().Cast<ExpressionFilter>();

		auto local_it = global_to_local.find(global_index);
		if (local_it == global_to_local.end()) {
			throw InternalException(
			    "Error in 'EvaluateConstantFilters', this filter should not end up in CreateFilters!");
		}
		auto &map_entry = local_it->second;
		auto local_id = MultiFileLocalIndex(map_entry.mapping.index.GetIndex());
		auto &local_type = map_entry.local_type;

		unique_ptr<TableFilter> local_filter;
		switch (map_entry.filter_conversion) {
		case FilterConversionType::COPY_DIRECTLY:
			// no conversion required - just copy the filter
			local_filter = global_filter.Copy();
			break;
		case FilterConversionType::CAST_FILTER:
			// types are different - try to convert
			local_filter = TryCastTableFilter(global_filter, map_entry.mapping, local_type);
			break;
		default:
			// we need to execute the filter globally
			break;
		}
		if (local_filter) {
			// succeeded in casting - push the local filter
			result->SetFilterByColumnIndex(local_id, std::move(local_filter));
		} else {
			// failed to cast - copy the global filter and evaluate the conversion expression in the reader
			result->SetFilterByColumnIndex(local_id, global_filter.Copy());

			// add the expression to the expression map - we are now evaluating this inside the reader directly
			// we need to set the index of the references inside the expression to 0
			auto &expr = reader_data.expressions[global_index.GetIndex()];
			auto unique_ref_count = StartIndexAtZero(expr);
			auto expression_type = expr->GetReturnType();

			vector<ColumnIndex> expression_column_indexes;
			for (idx_t i = 0; i < unique_ref_count; i++) {
				auto index = local_id + i;
				expression_column_indexes.push_back(reader.column_indexes[index]);
			}
			reader.expression_map.emplace(ProjectionIndex(local_id.GetIndex()),
			                              BaseFileReaderExpression(std::move(expr), expression_column_indexes));

			// reset the expression - since we are evaluating it in the reader we can just reference it
			expr = make_uniq<BoundReferenceExpression>(std::move(expression_type), local_id);
		}
		local_to_global.emplace(local_id, global_index);
	}
	reader.filter_global_indices.clear();
	reader.filter_global_indices.reserve(local_to_global.size());
	for (auto &p : local_to_global) {
		reader.filter_global_indices.push_back(p.second);
	}
	return result;
}

ReaderInitializeType MultiFileColumnMapper::CreateMapping(MultiFileColumnMappingMode mapping_mode) {
	// copy global columns and inject any different defaults
	auto result = CreateColumnMapping(mapping_mode);
	//! Evaluate the filters against the column(s) that are constant for this file (not present in the local schema)
	//! If any of these fail, the file can be skipped entirely
	map<MultiFileGlobalIndex, reference<TableFilter>> remaining_filters;
	auto evaluate_result = EvaluateConstantFilters(result, remaining_filters);
	if (evaluate_result == ReaderInitializeType::SKIP_READING_FILE) {
		return ReaderInitializeType::SKIP_READING_FILE;
	}

	reader_data.reader->filters = CreateFilters(remaining_filters, result);

	// for any remaining casts - push them as expressions
	return ReaderInitializeType::INITIALIZED;
}

} // namespace duckdb
