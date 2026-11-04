#include "duckdb/planner/operator/logical_get.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_serialization.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {

static void ConvertLegacyTableFilters(LogicalGet &get) {
	vector<pair<ProjectionIndex, unique_ptr<TableFilter>>> converted_filters;
	for (auto &entry : get.table_filters) {
		auto filter_idx = entry.GetIndex();
		auto &filter = entry.Filter();
		if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
			continue;
		}
		if (filter_idx.GetIndex() >= get.GetColumnIds().size()) {
			throw SerializationException("LogicalGet::Deserialize - filter index %llu is out of bounds for column ids",
			                             filter_idx.GetIndex());
		}
		auto &column_index = get.GetColumnIds()[filter_idx.GetIndex()];
		auto &column_type = get.GetColumnType(column_index);
		converted_filters.emplace_back(filter_idx, ExpressionFilter::FromTableFilter(filter, column_type));
	}
	for (auto &entry : converted_filters) {
		get.table_filters.SetFilterByColumnIndex(entry.first, std::move(entry.second));
	}
}

//! Pre-v2.0.0 versions key their table filters by table column index. Map each key to the scanned column it names.
static void MapLegacyTableFilterKeys(LogicalGet &get) {
	if (get.table_filters.HasMultiColumnFilters()) {
		throw SerializationException(
		    "LogicalGet::Deserialize - unexpected multi-column filters in legacy table filters");
	}
	auto &column_ids = get.GetColumnIds();
	vector<pair<ProjectionIndex, unique_ptr<TableFilter>>> filters;
	for (auto &entry : get.table_filters) {
		auto table_column = entry.GetIndex().GetIndexUnsafe();
		optional_idx projection_index;
		for (idx_t i = 0; i < column_ids.size(); i++) {
			auto &column_index = column_ids[i];
			if (column_index.HasPrimaryIndex() && column_index.GetPrimaryIndex() == table_column) {
				projection_index = i;
				break;
			}
		}
		if (!projection_index.IsValid()) {
			throw SerializationException("LogicalGet::Deserialize - table filter on column %llu that is not scanned",
			                             table_column);
		}
		filters.emplace_back(ProjectionIndex(projection_index.GetIndex()), entry.TakeFilter());
	}
	get.table_filters.ClearFilters();
	for (auto &filter : filters) {
		get.table_filters.SetFilterByColumnIndex(filter.first, std::move(filter.second));
	}
}

LogicalGet::LogicalGet() : LogicalOperator(LogicalOperatorType::LOGICAL_GET) {
}

LogicalGet::LogicalGet(TableIndex table_index, BoundTableFunction function, unique_ptr<FunctionData> bind_data,
                       vector<LogicalType> returned_types, vector<Identifier> returned_names,
                       virtual_column_map_t virtual_columns_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_GET), table_index(table_index), function(std::move(function)),
      bind_data(std::move(bind_data)), returned_types(std::move(returned_types)), names(std::move(returned_names)),
      virtual_columns(std::move(virtual_columns_p)), extra_info() {
}

optional_ptr<TableCatalogEntry> LogicalGet::GetTable() const {
	if (!function.get_table_entry) {
		return nullptr;
	}
	return function.get_table_entry(bind_data.get());
}

InsertionOrderPreservingMap<string> LogicalGet::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	string filters_info;
	bool first_item = true;
	for (auto &kv : table_filters) {
		auto filter_idx = kv.GetIndex();
		auto &filter = kv.Filter().Cast<ExpressionFilter>();
		auto &col_id_entry = column_ids[filter_idx];
		const auto col_id = col_id_entry.GetPrimaryIndex();
		if (col_id_entry.IsVirtualColumn()) {
			auto entry = virtual_columns.find(col_id);
			if (entry != virtual_columns.end()) {
				if (!first_item) {
					filters_info += "\n";
				}
				first_item = false;
				filters_info += filter.ToString(entry->second.name.GetIdentifierName());
			}
		} else if (col_id < names.size()) {
			if (!first_item) {
				filters_info += "\n";
			}
			auto column_name = col_id_entry.GetName(names[col_id].GetIdentifierName());
			first_item = false;
			filters_info += filter.ToString(column_name);
		}
	}
	result["Filters"] = filters_info;

	if (extra_info.sample_options) {
		if (extra_info.sample_options->is_percentage) {
			result["Sample Method"] = "System: " + extra_info.sample_options->sample_size.ToString() + "%";
		} else {
			result["Sample Method"] = "System: " + extra_info.sample_options->sample_size.ToString() + " rows";
		}
	}

	if (!extra_info.file_filters.empty()) {
		result["File Filters"] = extra_info.file_filters;
		if (extra_info.filtered_files.IsValid() && extra_info.total_files.IsValid()) {
			result["Scanning Files"] = StringUtil::Format("%llu/%llu", extra_info.filtered_files.GetIndex(),
			                                              extra_info.total_files.GetIndex());
		}
	}

	if (function.to_string) {
		TableFunctionToStringInput input(function, bind_data.get());
		auto to_string_result = function.to_string(input);
		for (const auto &it : to_string_result) {
			result[it.first] = it.second;
		}
	}
	SetParamsEstimatedCardinality(result);
	return result;
}

void LogicalGet::SetColumnIds(vector<ColumnIndex> &&column_ids) {
	this->column_ids = std::move(column_ids);
}

ProjectionIndex LogicalGet::AddColumnId(column_t column_id) {
	ProjectionIndex result(column_ids.size());
	column_ids.emplace_back(column_id);
	return result;
}

void LogicalGet::ClearColumnIds() {
	column_ids.clear();
}

const vector<ColumnIndex> &LogicalGet::GetColumnIds() const {
	return column_ids;
}

vector<ColumnIndex> &LogicalGet::GetMutableColumnIds() {
	return column_ids;
}

ProjectionIndex LogicalGet::TryGetProjectionIndex(idx_t col_idx) const {
	for (idx_t c = 0; c < column_ids.size(); c++) {
		if (column_ids[c].GetPrimaryIndex() == col_idx) {
			return ProjectionIndex(c);
		}
	}
	return ProjectionIndex();
}

const ColumnIndex &LogicalGet::GetColumnIndex(ColumnBinding binding) const {
	if (binding.table_index != table_index) {
		throw InternalException("LogicalGet::GetColumnIndex - table index does not match LogicalGet table index");
	}
	return column_ids[binding.column_index];
}

const ColumnIndex &LogicalGet::GetColumnIndex(ProjectionIndex proj_index) const {
	return GetColumnIndex(ColumnBinding(table_index, proj_index));
}

vector<ColumnBinding> LogicalGet::GetColumnBindings() {
	if (column_ids.empty()) {
		return {ColumnBinding(table_index, ProjectionIndex(0))};
	}
	vector<ColumnBinding> result;
	if (projection_ids.empty()) {
		for (auto col_idx : ProjectionIndex::GetIndexes(column_ids.size())) {
			result.emplace_back(table_index, ProjectionIndex(col_idx));
		}
	} else {
		for (auto proj_id : projection_ids) {
			result.emplace_back(table_index, proj_id);
		}
	}
	if (!projected_input.empty()) {
		if (children.size() != 1) {
			throw InternalException("LogicalGet::project_input can only be set for table-in-out functions");
		}
		auto child_bindings = children[0]->GetColumnBindings();
		for (auto entry : projected_input) {
			D_ASSERT(entry < child_bindings.size());
			result.emplace_back(child_bindings[entry]);
		}
	}
	return result;
}

const LogicalType &LogicalGet::GetColumnType(const ColumnIndex &index) const {
	if (index.IsVirtualColumn()) {
		auto entry = virtual_columns.find(index.GetPrimaryIndex());
		if (entry == virtual_columns.end()) {
			throw InternalException("Failed to find referenced virtual column %d", index.GetPrimaryIndex());
		}
		return entry->second.type;
	} else if (index.HasType()) {
		return index.GetScanType();
	} else {
		return returned_types[index.GetPrimaryIndex()];
	}
}

const Identifier &LogicalGet::GetColumnName(const ColumnIndex &index) const {
	if (index.IsVirtualColumn()) {
		auto entry = virtual_columns.find(index.GetPrimaryIndex());
		if (entry == virtual_columns.end()) {
			throw InternalException("Failed to find referenced virtual column %d", index.GetPrimaryIndex());
		}
		return entry->second.name;
	}
	return names[index.GetPrimaryIndex()];
}

column_t LogicalGet::GetAnyColumn() const {
	auto entry = virtual_columns.find(COLUMN_IDENTIFIER_EMPTY);
	if (entry != virtual_columns.end()) {
		// return the empty column if the projection supports it
		return COLUMN_IDENTIFIER_EMPTY;
	}
	entry = virtual_columns.find(COLUMN_IDENTIFIER_ROW_ID);
	if (entry != virtual_columns.end()) {
		// return the rowid column if the projection supports it
		return COLUMN_IDENTIFIER_ROW_ID;
	}
	// otherwise return the first column
	return 0;
}

void LogicalGet::ResolveTypes() {
	if (column_ids.empty()) {
		// no projection - we need to push a column
		column_ids.emplace_back(GetAnyColumn());
	}
	types.clear();
	if (projection_ids.empty()) {
		for (auto &index : column_ids) {
			types.push_back(GetColumnType(index));
		}
	} else {
		for (auto &proj_index : projection_ids) {
			auto &index = column_ids[proj_index];
			types.push_back(GetColumnType(index));
		}
	}
	if (!projected_input.empty()) {
		if (children.size() != 1) {
			throw InternalException("LogicalGet::project_input can only be set for table-in-out functions");
		}
		for (auto entry : projected_input) {
			D_ASSERT(entry < children[0]->types.size());
			types.push_back(children[0]->types[entry]);
		}
	}
}

bool LogicalGet::TryGetStorageIndex(const ColumnIndex &column_index, StorageIndex &out_index) const {
	if (column_index.IsRowIdColumn()) {
		return false;
	}
	if (column_index.IsVirtualColumn()) {
		return false;
	}

	auto table = GetTable();
	if (!table) {
		//! If there's no table we assume there's no mismatch between
		//! logical/storage index
		out_index = StorageIndex::FromColumnIndex(column_index);
		return true;
	}

	auto &column = table->GetColumn(LogicalIndex(column_index.GetPrimaryIndex()));
	if (column.Generated()) {
		//! This is a generated column, can't use the row group pruner
		return false;
	}
	out_index = table->GetStorageIndex(column_index);
	return true;
}

idx_t LogicalGet::EstimateCardinality(ClientContext &context) {
	// join order optimizer does better cardinality estimation.
	if (has_estimated_cardinality) {
		return estimated_cardinality;
	}
	return EstimateSourceCardinality(context);
}

idx_t LogicalGet::EstimateSourceCardinality(ClientContext &context) {
	// planning a LogicalGet consumes its bind data - the callbacks that require it must not be called,
	// while callbacks that accept a null bind data (e.g. the range functions) return no estimate anyway
	if (function.cardinality && bind_data) {
		auto node_stats = function.cardinality(context, bind_data.get());
		if (node_stats && node_stats->has_estimated_cardinality) {
			return node_stats->estimated_cardinality;
		}
	}
	if (!children.empty()) {
		return children[0]->EstimateCardinality(context);
	}
	return 1;
}

void LogicalGet::SetScanOrder(unique_ptr<RowGroupOrderOptions> options) {
	if (!function.set_scan_order) {
		throw InternalException("LogicalGet::SetScanOrder called but function does not have scan order defined");
	}
	row_group_order_options = make_uniq<RowGroupOrderOptions>(*options);
	function.set_scan_order(std::move(options), bind_data.get());
}

void LogicalGet::SetPartitionsToScan(vector<idx_t> partition_indices) {
	if (!function.set_partitions_to_scan) {
		throw InternalException("LogicalGet::SetPartitionsToScan called but function is not defined");
	}
	scan_partition_indices = partition_indices;
	function.set_partitions_to_scan(std::move(partition_indices), bind_data.get());
}

void LogicalGet::Serialize(Serializer &serializer) const {
	if (bind_info) {
		throw NotImplementedException("Cannot serialize a table function with process-local bind input");
	}
	LogicalOperator::Serialize(serializer);
	serializer.WriteProperty(200, "table_index", table_index);
	serializer.WriteProperty(201, "returned_types", returned_types);
	serializer.WriteProperty(202, "names", names);
	/* [Deleted] (vector<column_t>) "column_ids" */
	serializer.WriteProperty(204, "projection_ids", projection_ids);
	serializer.WriteProperty(205, "table_filters", table_filters);
	FunctionSerializer::Serialize(serializer, function, bind_data.get());
	if (!function.HasSerializationCallbacks() || serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WriteProperty(206, "parameters", parameters);
		serializer.WriteProperty(207, "named_parameters", named_parameters);
		serializer.WriteProperty(208, "input_table_types", input_table_types);
		serializer.WriteProperty(209, "input_table_names", input_table_names);
	}
	serializer.WriteProperty(210, "projected_input", projected_input);
	serializer.WritePropertyWithDefault(211, "column_indexes", column_ids);
	serializer.WritePropertyWithDefault(212, "extra_info", extra_info, ExtraOperatorInfo {});
	serializer.WritePropertyWithDefault<optional_idx>(213, "ordinality_idx", ordinality_idx);
	serializer.WritePropertyWithDefault<unique_ptr<RowGroupOrderOptions>>(214, "row_group_order_options",
	                                                                      row_group_order_options);
	serializer.WritePropertyWithDefault(215, "scan_partition_indices", scan_partition_indices, vector<idx_t>());
	serializer.WritePropertyWithDefault(216, "source_ordinality", source_ordinality,
	                                    OrdinalityType::WITHOUT_ORDINALITY);
	serializer.WriteProperty(217, "table_filters_by_projection", true);
	serializer.WritePropertyWithDefault(218, "has_pushed_projection", has_pushed_projection, false);
	serializer.WritePropertyWithDefault(219, "at_clause", at_clause);
}

unique_ptr<LogicalOperator> LogicalGet::Deserialize(Deserializer &deserializer) {
	vector<column_t> legacy_column_ids;

	auto result = unique_ptr<LogicalGet>(new LogicalGet());
	deserializer.ReadProperty(200, "table_index", result->table_index);
	deserializer.ReadProperty(201, "returned_types", result->returned_types);
	deserializer.ReadProperty(202, "names", result->names);
	deserializer.ReadPropertyWithDefault(203, "column_ids", legacy_column_ids);
	deserializer.ReadProperty(204, "projection_ids", result->projection_ids);
	deserializer.ReadProperty(205, "table_filters", result->table_filters);
	auto entry = FunctionSerializer::DeserializeTableFunction(deserializer);
	result->function = entry.first;
	auto &function = result->function;
	auto has_serialize = entry.second;
	unique_ptr<FunctionData> bind_data;
	if (has_serialize) {
		bind_data = FunctionSerializer::FunctionDeserialize(deserializer, function);
	}
	deserializer.ReadPropertyWithDefault(206, "parameters", result->parameters);
	deserializer.ReadPropertyWithDefault(207, "named_parameters", result->named_parameters);
	// a plan written by an older version holds only the arguments the call passed
	function.GetSignature().FillNamedDefaults(deserializer.Get<ClientContext &>(), result->named_parameters);
	deserializer.ReadPropertyWithDefault(208, "input_table_types", result->input_table_types);
	deserializer.ReadPropertyWithDefault(209, "input_table_names", result->input_table_names);
	deserializer.ReadProperty(210, "projected_input", result->projected_input);
	deserializer.ReadPropertyWithDefault(211, "column_indexes", result->column_ids);
	result->extra_info = deserializer.ReadPropertyWithExplicitDefault<ExtraOperatorInfo>(212, "extra_info", {});
	deserializer.ReadPropertyWithDefault<optional_idx>(213, "ordinality_idx", result->ordinality_idx);
	auto row_group_order_options =
	    deserializer.ReadPropertyWithDefault<unique_ptr<RowGroupOrderOptions>>(214, "row_group_order_options");
	auto scan_partition_indices =
	    deserializer.ReadPropertyWithExplicitDefault<vector<idx_t>>(215, "scan_partition_indices", vector<idx_t>());
	result->source_ordinality = deserializer.ReadPropertyWithExplicitDefault<OrdinalityType>(
	    216, "source_ordinality", OrdinalityType::WITHOUT_ORDINALITY);
	auto table_filters_by_projection =
	    deserializer.ReadPropertyWithExplicitDefault<bool>(217, "table_filters_by_projection", false);
	result->has_pushed_projection =
	    deserializer.ReadPropertyWithExplicitDefault<bool>(218, "has_pushed_projection", false);
	result->at_clause = deserializer.ReadPropertyWithDefault<unique_ptr<BoundAtClause>>(219, "at_clause");
	if (!legacy_column_ids.empty()) {
		if (!result->column_ids.empty()) {
			throw SerializationException(
			    "LogicalGet::Deserialize - either column_ids or column_indexes should be set - not both");
		}
		for (auto &col_id : legacy_column_ids) {
			result->column_ids.emplace_back(col_id);
		}
	}
	if (!table_filters_by_projection) {
		MapLegacyTableFilterKeys(*result);
	}
	auto &context = deserializer.Get<ClientContext &>();
	virtual_column_map_t virtual_columns;
	if (!has_serialize) {
		TableFunctionRef empty_ref;
		TableFunctionBindInput input(result->parameters, result->named_parameters, result->input_table_types,
		                             result->input_table_names, function.function_info.get(), nullptr, result->function,
		                             empty_ref, nullptr);

		vector<LogicalType> bind_return_types;
		vector<Identifier> bind_names;
		if (!function.bind) {
			throw InternalException("Table function %s has neither bind nor (de)serialize", function.GetName());
		}
		bind_data = function.bind(context, input, bind_return_types, bind_names);
		if (result->ordinality_idx.IsValid()) {
			auto ordinality_pos = bind_return_types.begin() + NumericCast<int64_t>(result->ordinality_idx.GetIndex());
			bind_return_types.emplace(ordinality_pos, LogicalType::BIGINT);
		}
		if (function.get_virtual_columns) {
			virtual_columns = function.get_virtual_columns(context, bind_data.get());
		}
		for (auto &col_id : result->column_ids) {
			if (col_id.IsVirtualColumn()) {
				auto idx = col_id.GetPrimaryIndex();
				auto ventry = virtual_columns.find(idx);
				if (ventry == virtual_columns.end()) {
					throw SerializationException(
					    "Table function deserialization failure - could not find virtual column with id %d", idx);
				}
			} else {
				auto idx = col_id.GetPrimaryIndex();
				auto &ret_type = result->returned_types[idx];
				auto &col_name = result->names[idx];
				if (bind_return_types[idx] != ret_type) {
					throw SerializationException("Table function deserialization failure in function %s - column with "
					                             "name %s was serialized with type %s, but now has type %s",
					                             function.GetName(), col_name, ret_type, bind_return_types[idx]);
				}
			}
		}
		result->returned_types = std::move(bind_return_types);
	} else if (function.get_virtual_columns) {
		virtual_columns = function.get_virtual_columns(context, bind_data.get());
	}
	result->virtual_columns = std::move(virtual_columns);
	result->bind_data = std::move(bind_data);
	ConvertLegacyTableFilters(*result);
	if (row_group_order_options) {
		result->SetScanOrder(std::move(row_group_order_options));
	}
	if (!scan_partition_indices.empty()) {
		result->SetPartitionsToScan(std::move(scan_partition_indices));
	}
	return std::move(result);
}

vector<TableIndex> LogicalGet::GetTableIndex() const {
	return vector<TableIndex> {table_index};
}

string LogicalGet::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return StringUtil::Upper(function.GetName().GetIdentifierName()) +
		       StringUtil::Format(" #%llu", table_index.index);
	}
#endif
	return StringUtil::Upper(function.GetName().GetIdentifierName());
}

} // namespace duckdb
