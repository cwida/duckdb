#include "duckdb/planner/operator/logical_copy_to_file.hpp"

#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/function_serialization.hpp"

namespace duckdb {

vector<LogicalType> LogicalCopyToFile::GetTypesWithoutPartitions(const vector<LogicalType> &col_types,
                                                                 const vector<idx_t> &part_cols, bool write_part_cols) {
	if (write_part_cols || part_cols.empty()) {
		return col_types;
	}
	vector<LogicalType> types;
	set<idx_t> part_col_set(part_cols.begin(), part_cols.end());
	for (idx_t col_idx = 0; col_idx < col_types.size(); col_idx++) {
		if (part_col_set.find(col_idx) == part_col_set.end()) {
			types.push_back(col_types[col_idx]);
		}
	}
	return types;
}

vector<Identifier> LogicalCopyToFile::GetNamesWithoutPartitions(const vector<Identifier> &col_names,
                                                                const vector<column_t> &part_cols,
                                                                bool write_part_cols) {
	if (write_part_cols || part_cols.empty()) {
		return col_names;
	}
	vector<Identifier> names;
	set<idx_t> part_col_set(part_cols.begin(), part_cols.end());
	for (idx_t col_idx = 0; col_idx < col_names.size(); col_idx++) {
		if (part_col_set.find(col_idx) == part_col_set.end()) {
			names.push_back(col_names[col_idx]);
		}
	}
	return names;
}

void LogicalCopyToFile::Serialize(Serializer &serializer) const {
	LogicalOperator::Serialize(serializer);
	serializer.WriteProperty(200, "file_path", file_path);
	serializer.WriteProperty(201, "use_tmp_file", use_tmp_file);
	serializer.WriteProperty(202, "filename_pattern", filename_pattern);
	serializer.WriteProperty(203, "overwrite_or_ignore", overwrite_mode);
	serializer.WriteProperty(204, "per_thread_output", per_thread_output);
	serializer.WriteProperty(205, "partition_output", partition_output);
	serializer.WriteProperty(206, "partition_columns", partition_columns);
	serializer.WriteProperty(207, "names", names);
	serializer.WriteProperty(208, "expected_types", expected_types);
	serializer.WriteProperty(209, "copy_info", copy_info);

	// Serialize function
	serializer.WriteProperty(210, "function_name", function.name);

	bool has_serialize = function.serialize;
	serializer.WriteProperty(211, "function_has_serialize", has_serialize);
	if (has_serialize) {
		D_ASSERT(function.deserialize); // if serialize is set, deserialize should be set as well
		serializer.WriteObject(212, "function_data",
		                       [&](Serializer &obj) { function.serialize(obj, *bind_data, function); });
	}

	serializer.WriteProperty(213, "file_extension", file_extension);
	serializer.WriteProperty(214, "rotate", rotate);
	serializer.WriteProperty(215, "return_type", return_type);
	serializer.WritePropertyWithDefault(216, "write_partition_columns", write_partition_columns, true);
	serializer.WritePropertyWithDefault(217, "write_empty_file", write_empty_file, true);
	serializer.WritePropertyWithDefault(218, "preserve_order", preserve_order, PreserveOrderType::AUTOMATIC);
	serializer.WritePropertyWithDefault(219, "hive_file_pattern", hive_file_pattern, true);
	serializer.WritePropertyWithDefault(220, "file_size_bytes", file_size_bytes, optional_idx());
	serializer.WritePropertyWithDefault(221, "batch_size", batch_size, optional_idx());
	serializer.WritePropertyWithDefault(222, "batch_size_bytes", batch_size_bytes, optional_idx());
	serializer.WritePropertyWithDefault(223, "batches_per_file", batches_per_file, optional_idx());
	serializer.WritePropertyWithDefault(224, "order_columns", order_columns);
	serializer.WritePropertyWithDefault(225, "table_index", table_index, TableIndex(0));
	serializer.WritePropertyWithDefault<unique_ptr<Expression>>(226, "partition_path_expression",
	                                                            partition_path_expression);
	serializer.WritePropertyWithDefault(227, "query_names", query_names);
	serializer.WritePropertyWithDefault(228, "query_types", query_types);
}

unique_ptr<LogicalOperator> LogicalCopyToFile::Deserialize(Deserializer &deserializer) {
	auto file_path = deserializer.ReadProperty<string>(200, "file_path");
	auto use_tmp_file = deserializer.ReadProperty<bool>(201, "use_tmp_file");
	auto filename_pattern = deserializer.ReadProperty<FilenamePattern>(202, "filename_pattern");
	auto overwrite_mode = deserializer.ReadProperty<CopyOverwriteMode>(203, "overwrite_mode");
	auto per_thread_output = deserializer.ReadProperty<bool>(204, "per_thread_output");
	auto partition_output = deserializer.ReadProperty<bool>(205, "partition_output");
	auto partition_columns = deserializer.ReadProperty<vector<idx_t>>(206, "partition_columns");
	auto names = deserializer.ReadProperty<vector<Identifier>>(207, "names");
	auto expected_types = deserializer.ReadProperty<vector<LogicalType>>(208, "expected_types");
	auto copy_info =
	    unique_ptr_cast<ParseInfo, CopyInfo>(deserializer.ReadProperty<unique_ptr<ParseInfo>>(209, "copy_info"));

	// Deserialize function
	auto &context = deserializer.Get<ClientContext &>();
	auto name = deserializer.ReadProperty<string>(210, "function_name");

	auto &func_catalog_entry = Catalog::GetEntry<CopyFunctionCatalogEntry>(
	    context, QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(), Identifier(name)));
	if (func_catalog_entry.type != CatalogType::COPY_FUNCTION_ENTRY) {
		throw InternalException("DeserializeFunction - cant find catalog entry for function %s", name);
	}
	auto &function_entry = func_catalog_entry.Cast<CopyFunctionCatalogEntry>();
	auto function = function_entry.function;
	// Deserialize function data
	unique_ptr<FunctionData> bind_data;
	auto has_serialize = deserializer.ReadProperty<bool>(211, "function_has_serialize");
	if (has_serialize) {
		// Just deserialize the bind data
		deserializer.ReadObject(212, "function_data",
		                        [&](Deserializer &obj) { bind_data = function.deserialize(obj, function); });
	}

	auto default_extension = function.extension;

	auto file_extension =
	    deserializer.ReadPropertyWithExplicitDefault<string>(213, "file_extension", std::move(default_extension));

	auto rotate = deserializer.ReadPropertyWithExplicitDefault(214, "rotate", false);
	auto return_type =
	    deserializer.ReadPropertyWithExplicitDefault(215, "return_type", CopyFunctionReturnType::CHANGED_ROWS);
	auto write_partition_columns = deserializer.ReadPropertyWithExplicitDefault(216, "write_partition_columns", true);
	auto write_empty_file = deserializer.ReadPropertyWithExplicitDefault(217, "write_empty_file", true);
	auto preserve_order =
	    deserializer.ReadPropertyWithExplicitDefault(218, "preserve_order", PreserveOrderType::AUTOMATIC);
	auto hive_file_pattern = deserializer.ReadPropertyWithExplicitDefault(219, "hive_file_pattern", true);
	auto file_size_bytes = deserializer.ReadPropertyWithExplicitDefault(220, "file_size_bytes", optional_idx());
	auto batch_size = deserializer.ReadPropertyWithExplicitDefault(221, "batch_size", optional_idx());
	auto batch_size_bytes = deserializer.ReadPropertyWithExplicitDefault(222, "batch_size_bytes", optional_idx());
	auto batches_per_file = deserializer.ReadPropertyWithExplicitDefault(223, "batches_per_file", optional_idx());
	auto order_columns = deserializer.ReadPropertyWithExplicitDefault(224, "order_columns", vector<BoundOrderByNode>());
	auto table_index = deserializer.ReadPropertyWithExplicitDefault(225, "table_index", TableIndex(0));
	unique_ptr<Expression> partition_path_expression;
	deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(226, "partition_path_expression",
	                                                             partition_path_expression);
	auto query_names = deserializer.ReadPropertyWithDefault<vector<Identifier>>(227, "query_names");
	auto query_types = deserializer.ReadPropertyWithDefault<vector<LogicalType>>(228, "query_types");

	if (!has_serialize) {
		// If not serialized, re-bind with the copy info
		if (!function.copy_to_bind) {
			throw InternalException("Copy function %s has neither bind nor (de)serialize", function.name);
		}

		CopyFunctionBindInput function_bind_input(*copy_info, function.function_info);
		auto names_to_write = GetNamesWithoutPartitions(names, partition_columns, write_partition_columns);
		auto types_to_write = GetTypesWithoutPartitions(expected_types, partition_columns, write_partition_columns);
		bind_data = function.copy_to_bind(context, function_bind_input, names_to_write, types_to_write);
	}

	auto result = make_uniq<LogicalCopyToFile>(function, std::move(bind_data), std::move(copy_info), table_index);
	result->file_path = file_path;
	result->use_tmp_file = use_tmp_file;
	result->filename_pattern = filename_pattern;
	result->file_extension = file_extension;
	result->overwrite_mode = overwrite_mode;
	result->per_thread_output = per_thread_output;
	result->partition_output = partition_output;
	result->partition_columns = partition_columns;
	result->names = names;
	result->expected_types = expected_types;
	result->rotate = rotate;
	result->return_type = return_type;
	result->write_partition_columns = write_partition_columns;
	result->write_empty_file = write_empty_file;
	result->preserve_order = preserve_order;
	result->hive_file_pattern = hive_file_pattern;
	result->file_size_bytes = file_size_bytes;
	result->batch_size = batch_size;
	result->batch_size_bytes = batch_size_bytes;
	result->batches_per_file = batches_per_file;
	result->order_columns = std::move(order_columns);
	result->partition_path_expression = std::move(partition_path_expression);
	result->query_names = std::move(query_names);
	result->query_types = std::move(query_types);

	return std::move(result);
}

vector<ColumnBinding> LogicalCopyToFile::GetColumnBindings() {
	idx_t return_column_count = GetCopyFunctionReturnLogicalTypes(return_type).size();
	vector<ColumnBinding> result;
	for (auto return_col_idx : ProjectionIndex::GetIndexes(return_column_count)) {
		result.emplace_back(table_index, return_col_idx);
	}
	return result;
}

vector<TableIndex> LogicalCopyToFile::GetTableIndex() const {
	return {table_index};
}

idx_t LogicalCopyToFile::EstimateCardinality(ClientContext &context) {
	return 1;
}

} // namespace duckdb
