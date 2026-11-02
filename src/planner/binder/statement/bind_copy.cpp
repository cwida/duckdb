#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/common/filename_pattern.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/column_data_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/common/algorithm.hpp"

#include "duckdb/main/extension_entries.hpp"

namespace duckdb {

BoundStatement Binder::BindNode(CopyQueryNode &node) {
	CopyStatement statement;
	statement.info = std::move(node.info);
	return Bind(statement, CopyToType::COPY_TO_FILE);
}

static bool GetBooleanArg(ClientContext &context, const vector<Value> &arg) {
	return arg.empty() || arg[0].CastAs(context, LogicalType::BOOLEAN).GetValue<bool>();
}

void IsFormatExtensionKnown(const string &format) {
	for (auto &file_postfixes : EXTENSION_FILE_POSTFIXES) {
		if (format == file_postfixes.name + 1) {
			// It's a match, we must throw
			throw CatalogException(
			    "Copy Function with name \"%s\" is not in the catalog, but it exists in the %s extension.", format,
			    file_postfixes.extension);
		}
	}
}

identifier_map_t<CopyOption> Binder::GetFullCopyOptionsList(const CopyFunction &function, CopyOptionMode mode) {
	identifier_map_t<CopyOption> copy_options;
	CopyOptionsInput input(copy_options);
	function.copy_options(context, input);

	// first erase all options that don't match this type
	if (mode != CopyOptionMode::READ_WRITE) {
		vector<Identifier> erased_options;
		for (auto &[option_name, copy_option] : copy_options) {
			if (copy_option.mode == CopyOptionMode::READ_WRITE) {
				// used for both
				continue;
			}
			if (copy_option.mode != mode) {
				erased_options.push_back(option_name);
			}
		}
		for (auto &erased : erased_options) {
			copy_options.erase(erased);
		}
	}

	// now we have a list of all options for this copy type
	// add generic options
	if (mode != CopyOptionMode::READ_ONLY) {
		copy_options["use_tmp_file"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["overwrite_or_ignore"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["overwrite"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["append"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["filename_pattern"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
		copy_options["file_extension"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
		copy_options["per_thread_output"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["row_group_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
		copy_options["row_group_size_bytes"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
		copy_options["row_groups_per_file"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
		copy_options["file_size_bytes"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
		copy_options["partition_by"] = CopyOption(LogicalType::LIST(LogicalType::VARCHAR), CopyOptionMode::WRITE_ONLY);
		copy_options["order_by"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
		copy_options["return_files"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["preserve_order"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["return_stats"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["return_column_types"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["write_partition_columns"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["write_empty_file"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
		copy_options["hive_file_pattern"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	}
	return copy_options;
}

static idx_t ParseBytesArg(const Identifier &name, Value &arg) {
	if (arg.IsNull()) {
		throw BinderException("NULL is not supported as a valid option for COPY option \"%s\"",
		                      StringUtil::Upper(name.GetIdentifierName()));
	}
	optional_idx result;
	if (arg.type().id() == LogicalTypeId::VARCHAR) {
		result = DBConfig::ParseMemoryLimit(arg.ToString());
	} else {
		auto cast_arg = arg.DefaultTryCastAs(LogicalType::UBIGINT);
		if (!cast_arg) {
			throw BinderException("Unable to parse bytes from \"%s\" for copy option \"%s\" ", arg.ToString(),
			                      StringUtil::Upper(name.GetIdentifierName()));
		}
		auto bytes = cast_arg->GetValue<idx_t>();
		if (bytes != DConstants::INVALID_INDEX) {
			result = bytes;
		}
	}
	if (!result.IsValid()) {
		// e.g. '-1' or 'none', which ParseMemoryLimit parses as unlimited
		throw BinderException("Copy option \"%s\" must be a valid size, not \"%s\"",
		                      StringUtil::Upper(name.GetIdentifierName()), arg.ToString());
	}
	return result.GetIndex();
}

struct CopyToParsedOptions {
	optional<bool> use_tmp_file;
	optional<CopyOverwriteMode> overwrite_mode;
	optional<FilenamePattern> filename_pattern;
	optional<bool> per_thread_output;
	optional_idx batch_size;
	optional_idx batch_size_bytes;
	optional_idx file_size_bytes;
	optional_idx batches_per_file;
	vector<idx_t> partition_cols;
	vector<BoundOrderByNode> order_columns;
	optional<bool> write_partition_columns;
	optional<bool> write_empty_file;
	optional<bool> hive_file_pattern;
	optional<PreserveOrderType> preserve_order;
	optional<CopyFunctionReturnType> return_type;
	optional<bool> return_column_types;
	unique_ptr<ParsedExpression> partition_path;

	bool UserSetUseTmpFile() const {
		return use_tmp_file.has_value();
	}

	bool PerThreadOutput() const {
		return per_thread_output.value_or(false);
	}

	bool WritePartitionColumns() const {
		return write_partition_columns.value_or(false);
	}

	bool WriteEmptyFile() const {
		return write_empty_file.value_or(true);
	}

	bool HiveFilePattern() const {
		return hive_file_pattern.value_or(true);
	}

	PreserveOrderType PreserveOrder() const {
		return preserve_order.value_or(PreserveOrderType::AUTOMATIC);
	}

	CopyOverwriteMode OverwriteMode() const {
		return overwrite_mode.value_or(CopyOverwriteMode::COPY_ERROR_ON_CONFLICT);
	}

	FilenamePattern GetFilenamePattern() const {
		return filename_pattern.value_or(FilenamePattern());
	}

	CopyFunctionReturnType ReturnType() const {
		auto result = return_type.value_or(CopyFunctionReturnType::CHANGED_ROWS);
		return return_column_types.value_or(false) ? GetCopyFunctionReturnTypeWithColumnTypes(result) : result;
	}

	void SetFilenamePattern(const string &pattern) {
		FilenamePattern result;
		result.SetFilenamePattern(pattern);
		filename_pattern = std::move(result);
	}

	void SetReturnType(CopyFunctionReturnType return_type_p) {
		if (return_type.has_value() && *return_type != return_type_p) {
			throw BinderException("Can only set one of RETURN_FILES or RETURN_STATS for COPY");
		}
		return_type = return_type_p;
	}

	bool Rotate() const {
		return file_size_bytes.IsValid() || batches_per_file.IsValid();
	}

	bool Partitioned() const {
		return !partition_cols.empty();
	}

	bool PartitionedOrOrdered() const {
		return Partitioned() || !order_columns.empty();
	}
};

struct CopyToResolvedOptions {
	bool use_tmp_file = true;
	CopyOverwriteMode overwrite_mode = CopyOverwriteMode::COPY_ERROR_ON_CONFLICT;
	FilenamePattern filename_pattern;
	bool per_thread_output = false;
	optional_idx batch_size;
	optional_idx batch_size_bytes;
	optional_idx file_size_bytes;
	optional_idx batches_per_file;
	vector<idx_t> partition_cols;
	vector<BoundOrderByNode> order_columns;
	bool write_partition_columns = false;
	bool write_empty_file = true;
	bool hive_file_pattern = true;
	PreserveOrderType preserve_order = PreserveOrderType::AUTOMATIC;
	CopyFunctionReturnType return_type = CopyFunctionReturnType::CHANGED_ROWS;
	//! Unbound, as it is bound against the partition columns
	unique_ptr<ParsedExpression> partition_path;

	bool Rotate() const {
		return file_size_bytes.IsValid() || batches_per_file.IsValid();
	}

	bool Partitioned() const {
		return !partition_cols.empty();
	}

	bool PartitionedOrOrdered() const {
		return Partitioned() || !order_columns.empty();
	}
};

static bool ResolveUseTmpFile(ClientContext &context, const string &file_path, const CopyToParsedOptions &options) {
	if (FileSystem::IsRemoteFile(file_path)) {
		return false;
	}
	if (options.use_tmp_file.has_value()) {
		return *options.use_tmp_file;
	}

	auto &fs = FileSystem::GetFileSystem(context);
	const bool is_file_and_exists = fs.FileExists(file_path);
	const bool is_stdout = file_path == "/dev/stdout";

	// Disable temp file if multiple files could be involved.
	if (options.Rotate() || options.PerThreadOutput() || options.PartitionedOrOrdered()) {
		return false;
	}
	// Disable temp file if emit to stdout.
	if (is_stdout) {
		return false;
	}

	return is_file_and_exists;
}

static CopyToResolvedOptions ResolveCopyToOptions(ClientContext &context, const string &file_path,
                                                  CopyToParsedOptions options) {
	CopyToResolvedOptions result;
	result.use_tmp_file = ResolveUseTmpFile(context, file_path, options);
	result.overwrite_mode = options.OverwriteMode();
	result.filename_pattern = options.GetFilenamePattern();
	result.per_thread_output = options.PerThreadOutput();
	result.batch_size = options.batch_size;
	result.batch_size_bytes = options.batch_size_bytes;
	result.file_size_bytes = options.file_size_bytes;
	result.batches_per_file = options.batches_per_file;
	result.partition_cols = std::move(options.partition_cols);
	result.order_columns = std::move(options.order_columns);
	result.write_partition_columns = options.WritePartitionColumns();
	result.write_empty_file = options.WriteEmptyFile();
	result.hive_file_pattern = options.HiveFilePattern();
	result.preserve_order = options.PreserveOrder();
	result.return_type = options.ReturnType();
	result.partition_path = std::move(options.partition_path);
	return result;
}

static void ValidateCopyToOptionCombinations(const CopyToParsedOptions &options, const CopyFunction &function,
                                             const string &format) {
	if (options.OverwriteMode() == CopyOverwriteMode::COPY_APPEND && !options.GetFilenamePattern().HasUUID()) {
		throw BinderException("APPEND mode requires a {uuid} label in filename_pattern");
	}
	if (options.UserSetUseTmpFile() && options.PerThreadOutput()) {
		throw NotImplementedException("Can't combine USE_TMP_FILE and PER_THREAD_OUTPUT for COPY");
	}
	if (options.UserSetUseTmpFile() && options.Rotate()) {
		throw NotImplementedException("Can't combine USE_TMP_FILE and FILE_SIZE_BYTES/BATCHES_PER_FILE for COPY");
	}
	if (options.UserSetUseTmpFile() && options.Partitioned()) {
		throw NotImplementedException("Can't combine USE_TMP_FILE and PARTITIONED BY for COPY");
	}
	if (options.UserSetUseTmpFile() && !options.order_columns.empty()) {
		throw NotImplementedException("Can't combine USE_TMP_FILE and ORDER BY for COPY");
	}
	if (options.PerThreadOutput() && options.Partitioned()) {
		throw NotImplementedException("Can't combine PER_THREAD_OUTPUT and PARTITIONED BY for COPY");
	}
	if (options.PerThreadOutput() && !options.order_columns.empty()) {
		throw NotImplementedException("Can't combine PER_THREAD_OUTPUT and ORDER BY for COPY");
	}
	if (options.Rotate() && (!function.prepare_batch || !function.flush_batch)) {
		throw NotImplementedException("Can't use file rotation (e.g., ROW_GROUPS_PER_FILE) with FORMAT %s",
		                              function.name);
	}
	if (!options.WriteEmptyFile()) {
		if (options.PerThreadOutput()) {
			throw NotImplementedException("Can't combine WRITE_EMPTY_FILE false with PER_THREAD_OUTPUT");
		}
		if (options.Partitioned()) {
			throw NotImplementedException("Can't combine WRITE_EMPTY_FILE false with PARTITIONED BY");
		}
		if (!options.order_columns.empty()) {
			throw NotImplementedException("Can't combine WRITE_EMPTY_FILE false with ORDER BY");
		}
	}
	if (options.partition_path) {
		if (!options.Partitioned()) {
			throw BinderException("PARTITION_PATH requires PARTITION_BY");
		}
		if (options.hive_file_pattern.has_value()) {
			throw BinderException("Can't combine HIVE_FILE_PATTERN and PARTITION_PATH for COPY");
		}
	}
	if (GetCopyFunctionBaseReturnType(options.ReturnType()) == CopyFunctionReturnType::WRITTEN_FILE_STATISTICS &&
	    !function.copy_to_get_written_statistics) {
		throw NotImplementedException("RETURN_STATS is not supported for the \"%s\" copy format", format);
	}
}

static void ValidateCopyToOutputColumns(const CopyToResolvedOptions &options, idx_t column_count) {
	if (!options.write_partition_columns && options.partition_cols.size() == column_count) {
		throw NotImplementedException("No column to write as all columns are specified as partition columns. "
		                              "WRITE_PARTITION_COLUMNS option can be used to write partition columns.");
	}
}

//! Binds the PARTITION_PATH expression, which may only reference partition columns. Partition column i (in PARTITION_BY
//! order) is bound as reference i.
class PartitionPathBinder : public ExpressionBinder {
public:
	PartitionPathBinder(Binder &binder, ClientContext &context, const vector<Identifier> &names,
	                    const vector<LogicalType> &types, const vector<idx_t> &partition_columns)
	    : ExpressionBinder(binder, context), names(names), types(types), partition_columns(partition_columns) {
		target_type = LogicalType::VARCHAR;
	}

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) override {
		auto &expr = *expr_ptr;
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::WINDOW:
			return BindResult(BinderException::Unsupported(expr, "window functions are not allowed in PARTITION_PATH"));
		case ExpressionClass::SUBQUERY:
			return BindResult(BinderException::Unsupported(expr, "cannot use subquery in PARTITION_PATH"));
		case ExpressionClass::COLUMN_REF:
			return BindColumnReference(expr.Cast<ColumnRefExpression>());
		default:
			return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
		}
	}

	BindResult BindLambdaFunction(FunctionExpression &expr, ScalarFunctionCatalogEntry &function,
	                              idx_t depth) override {
		return BindResult(BinderException::Unsupported(expr, "lambda functions are not allowed in PARTITION_PATH"));
	}

	string UnsupportedAggregateMessage() override {
		return "aggregate functions are not allowed in PARTITION_PATH";
	}

private:
	BindResult BindColumnReference(ColumnRefExpression &col_ref) {
		if (col_ref.ColumnNames().size() == 1) {
			auto &column_name = col_ref.GetColumnName();
			for (idx_t i = 0; i < partition_columns.size(); i++) {
				auto column_idx = partition_columns[i];
				if (names[column_idx] == column_name) {
					return BindResult(make_uniq<BoundReferenceExpression>(types[column_idx], i));
				}
			}
		}
		throw BinderException(col_ref, "Column \"%s\" referenced in PARTITION_PATH is not a PARTITION_BY column",
		                      col_ref.ToString());
	}

private:
	const vector<Identifier> &names;
	const vector<LogicalType> &types;
	const vector<idx_t> &partition_columns;
};

BoundStatement Binder::BindCopyTo(CopyStatement &stmt, const CopyFunction &function, CopyToType copy_to_type) {
	if (function.plan) {
		// plan rewrite COPY TO
		auto result = function.plan(*this, stmt);
		// a rewrite that re-binds the COPY consumes PARTITION_PATH
		if (stmt.info && stmt.info->parsed_options.find("partition_path") != stmt.info->parsed_options.end()) {
			throw NotImplementedException("PARTITION_PATH is not supported for FORMAT \"%s\"", stmt.info->format);
		}
		return result;
	}

	auto &copy_info = *stmt.info;
	// bind the select statement
	// preserve SQLNULL types from table functions (e.g. JSON reader null columns)
	// so file writers that support it can emit the correct null type (e.g. parquet UNKNOWN/NullType)
	auto node_copy = copy_info.select_statement->Copy();
	auto select_node = Bind(*node_copy);

	if (!function.copy_to_bind) {
		throw NotImplementedException("COPY TO is not supported for FORMAT \"%s\"", stmt.info->format);
	}

	CopyToParsedOptions parsed_options;
	auto partition_path_entry = stmt.info->parsed_options.find("partition_path");
	if (partition_path_entry != stmt.info->parsed_options.end()) {
		parsed_options.partition_path = std::move(partition_path_entry->second);
		stmt.info->parsed_options.erase(partition_path_entry);
	}
	CopyFunctionBindInput bind_input(*stmt.info, function.function_info);

	bind_input.file_extension = function.extension;

	auto original_options = stmt.info->options;
	stmt.info->options.clear();
	for (auto &[option_name, option_values] : original_options) {
		if (option_name == "use_tmp_file") {
			parsed_options.use_tmp_file = GetBooleanArg(context, option_values);
		} else if (option_name == "overwrite_or_ignore" || option_name == "overwrite" || option_name == "append") {
			if (parsed_options.overwrite_mode.has_value()) {
				throw BinderException("Can only set one of OVERWRITE_OR_IGNORE, OVERWRITE or APPEND");
			}

			auto boolean = GetBooleanArg(context, option_values);
			if (boolean) {
				if (option_name == "overwrite_or_ignore") {
					parsed_options.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE;
				} else if (option_name == "overwrite") {
					parsed_options.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE;
				} else if (option_name == "append") {
					if (!parsed_options.filename_pattern.has_value()) {
						parsed_options.SetFilenamePattern("{uuid}");
					}
					parsed_options.overwrite_mode = CopyOverwriteMode::COPY_APPEND;
				}
			} else {
				parsed_options.overwrite_mode = CopyOverwriteMode::COPY_ERROR_ON_CONFLICT;
			}
		} else if (option_name == "filename_pattern") {
			if (option_values.empty()) {
				throw IOException("FILENAME_PATTERN cannot be empty");
			}
			parsed_options.SetFilenamePattern(
			    option_values[0].CastAs(context, LogicalType::VARCHAR).GetValue<string>());
		} else if (option_name == "file_extension") {
			if (option_values.empty()) {
				throw IOException("FILE_EXTENSION cannot be empty");
			}
			bind_input.file_extension = option_values[0].CastAs(context, LogicalType::VARCHAR).GetValue<string>();
		} else if (option_name == "per_thread_output") {
			parsed_options.per_thread_output = GetBooleanArg(context, option_values);
		} else if (option_name == "batch_size" || option_name == "row_group_size") {
			if (option_values.empty()) {
				throw BinderException("BATCH_SIZE/ROW_GROUP_SIZE cannot be empty");
			}
			auto batch_size = option_values[0].GetValue<uint64_t>();
			if (batch_size == DConstants::INVALID_INDEX) {
				throw BinderException("BATCH_SIZE/ROW_GROUP_SIZE value %llu is too large", batch_size);
			}
			parsed_options.batch_size = batch_size;
		} else if (option_name == "batch_size_bytes" || option_name == "row_group_size_bytes") {
			if (option_values.empty()) {
				throw BinderException("BATCH_SIZE_BYTES/ROW_GROUP_SIZE_BYTES cannot be empty");
			}
			parsed_options.batch_size_bytes = ParseBytesArg(option_name, option_values[0]);
		} else if (option_name == "file_size_bytes") {
			if (option_values.empty()) {
				throw BinderException("FILE_SIZE_BYTES cannot be empty");
			}
			if (!function.file_size_bytes) {
				throw BinderException("FILE_SIZE_BYTES not implemented for %s", function.name);
			}
			parsed_options.file_size_bytes = ParseBytesArg(option_name, option_values[0]);
		} else if (option_name == "batches_per_file" || option_name == "row_groups_per_file") {
			if (option_values.empty()) {
				throw BinderException("BATCHES_PER_FILE/ROW_GROUPS_PER_FILE cannot be empty");
			}
			auto batches_per_file = option_values[0].GetValue<uint64_t>();
			if (batches_per_file == 0 || batches_per_file == DConstants::INVALID_INDEX) {
				throw BinderException("BATCHES_PER_FILE/ROW_GROUPS_PER_FILE must be a positive number");
			}
			parsed_options.batches_per_file = batches_per_file;
		} else if (option_name == "partition_by") {
			auto converted = ConvertVectorToValue(std::move(option_values));
			parsed_options.partition_cols = ParseColumnsOrdered(converted, select_node.names, option_name);
		} else if (option_name == "order_by") {
			parsed_options.order_columns = ParseOrderByColumns(*this, option_values, select_node, option_name);
		} else if (option_name == "return_files") {
			if (GetBooleanArg(context, option_values)) {
				parsed_options.SetReturnType(CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST);
			}
		} else if (option_name == "preserve_order") {
			if (GetBooleanArg(context, option_values)) {
				parsed_options.preserve_order = PreserveOrderType::PRESERVE_ORDER;
			} else {
				parsed_options.preserve_order = PreserveOrderType::DONT_PRESERVE_ORDER;
			}
		} else if (option_name == "return_stats") {
			if (GetBooleanArg(context, option_values)) {
				parsed_options.SetReturnType(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS);
			}
		} else if (option_name == "return_column_types") {
			parsed_options.return_column_types = GetBooleanArg(context, option_values);
		} else if (option_name == "write_partition_columns") {
			parsed_options.write_partition_columns = GetBooleanArg(context, option_values);
		} else if (option_name == "write_empty_file") {
			parsed_options.write_empty_file = GetBooleanArg(context, option_values);
		} else if (option_name == "hive_file_pattern") {
			parsed_options.hive_file_pattern = GetBooleanArg(context, option_values);
		} else {
			stmt.info->options[option_name] = option_values;
		}
	}

	ValidateCopyToOptionCombinations(parsed_options, function, stmt.info->format);
	auto resolved_options = ResolveCopyToOptions(context, stmt.info->file_path, std::move(parsed_options));

	// RETURN_COLUMN_TYPES describes the query before the copy function can change its types (e.g., CSV casts every
	// column to VARCHAR), with the names the copy writes
	vector<Identifier> query_names;
	vector<LogicalType> query_types;
	if (CopyFunctionReturnsColumnTypes(resolved_options.return_type)) {
		query_names = select_node.names;
		QueryResult::DeduplicateColumns(query_names);
		query_types = select_node.types;
	}
	// Allow the copy function to intercept the select list and types and push a new projection on top of the plan
	if (function.copy_to_select) {
		auto bindings = select_node.plan->GetColumnBindings();

		CopyToSelectInput input = {context, stmt.info->options, {}, copy_to_type};
		input.select_list.reserve(bindings.size());

		// Create column references for the select list
		for (idx_t i = 0; i < bindings.size(); i++) {
			auto &binding = bindings[i];
			auto &name = select_node.names[i];
			auto &type = select_node.types[i];
			input.select_list.push_back(make_uniq<BoundColumnRefExpression>(name, type, binding));
		}

		auto new_select_list = function.copy_to_select(input);
		if (!new_select_list.empty()) {
			// We have a new select list, create a projection on top of the current plan
			auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(new_select_list));
			projection->children.push_back(std::move(select_node.plan));
			projection->ResolveOperatorTypes();

			// Update the names and types of the select node
			select_node.names.clear();
			select_node.types.clear();
			for (auto &expr : projection->expressions) {
				select_node.names.push_back(expr->GetName());
				select_node.types.push_back(expr->GetReturnType());
			}
			select_node.plan = std::move(projection);
		}
	}

	auto unique_column_names = select_node.names;
	QueryResult::DeduplicateColumns(unique_column_names);
	auto file_path = stmt.info->file_path;
	if (!file_path.empty() && !FileSystem::IsRemoteFile(file_path) &&
	    DatabaseManager::Get(context).IsAttachedDatabaseFile(context, file_path)) {
		// writing to the file of an attached database would corrupt it
		throw BinderException("Cannot COPY to \"%s\": the file belongs to an attached database", file_path);
	}

	ValidateCopyToOutputColumns(resolved_options, select_node.names.size());

	auto names_to_write = LogicalCopyToFile::GetNamesWithoutPartitions(
	    unique_column_names, resolved_options.partition_cols, resolved_options.write_partition_columns);
	auto types_to_write = LogicalCopyToFile::GetTypesWithoutPartitions(
	    select_node.types, resolved_options.partition_cols, resolved_options.write_partition_columns);
	auto function_data = function.copy_to_bind(context, bind_input, names_to_write, types_to_write);

	unique_ptr<Expression> partition_path_expression;
	if (resolved_options.partition_path) {
		PartitionPathBinder partition_path_binder(*this, context, select_node.names, select_node.types,
		                                          resolved_options.partition_cols);
		partition_path_expression = partition_path_binder.Bind(resolved_options.partition_path);
		if (partition_path_expression->HasParameter()) {
			throw ParameterNotResolvedException();
		}
	}

	// now create the copy information
	auto copy =
	    make_uniq<LogicalCopyToFile>(function, std::move(function_data), std::move(stmt.info), GenerateTableIndex());
	copy->file_path = file_path;
	copy->use_tmp_file = resolved_options.use_tmp_file;
	copy->overwrite_mode = resolved_options.overwrite_mode;
	copy->filename_pattern = resolved_options.filename_pattern;
	copy->file_extension = bind_input.file_extension;
	copy->per_thread_output = resolved_options.per_thread_output;
	copy->batch_size = resolved_options.batch_size;
	copy->batch_size_bytes = resolved_options.batch_size_bytes;
	copy->batches_per_file = resolved_options.batches_per_file;
	copy->file_size_bytes = resolved_options.file_size_bytes;
	copy->rotate = resolved_options.Rotate();
	copy->partition_output = resolved_options.PartitionedOrOrdered();
	copy->write_partition_columns = resolved_options.write_partition_columns;
	copy->partition_columns = std::move(resolved_options.partition_cols);
	copy->write_empty_file = resolved_options.write_empty_file;
	copy->return_type = resolved_options.return_type;
	copy->query_names = std::move(query_names);
	copy->query_types = std::move(query_types);
	copy->preserve_order = resolved_options.preserve_order;
	copy->hive_file_pattern = resolved_options.hive_file_pattern;
	copy->order_columns = std::move(resolved_options.order_columns);
	copy->partition_path_expression = std::move(partition_path_expression);

	copy->names = unique_column_names;
	copy->expected_types = select_node.types;

	copy->AddChild(std::move(select_node.plan));

	auto &properties = GetStatementProperties();
	switch (copy->return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS:
		properties.return_type = StatementReturnType::CHANGED_ROWS;
		break;
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST:
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS:
	case CopyFunctionReturnType::CHANGED_ROWS_AND_COLUMN_TYPES:
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST_AND_COLUMN_TYPES:
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS_AND_COLUMN_TYPES:
		properties.return_type = StatementReturnType::QUERY_RESULT;
		break;
	default:
		throw NotImplementedException("Unknown CopyFunctionReturnType");
	}

	// This must be set
	if (!copy->batch_size.IsValid() && copy->function.desired_batch_size) {
		copy->batch_size = copy->function.desired_batch_size(context, *copy->bind_data);
	}

	BoundStatement result;
	result.names = GetCopyFunctionReturnNames(copy->return_type);
	result.types = GetCopyFunctionReturnLogicalTypes(copy->return_type);
	result.plan = std::move(copy);

	return result;
}

static optional_ptr<unique_ptr<LogicalOperator>> FindCopySource(unique_ptr<LogicalOperator> &op,
                                                                ColumnDataCollection &copy_source) {
	if (op->type == LogicalOperatorType::LOGICAL_CHUNK_GET &&
	    op->Cast<LogicalColumnDataGet>().collection.get() == &copy_source) {
		return op;
	}
	for (auto &child : op->children) {
		auto source = FindCopySource(child, copy_source);
		if (source) {
			return source;
		}
	}
	return nullptr;
}

BoundStatement Binder::BindCopyFrom(CopyStatement &stmt, const CopyFunction &function) {
	BoundStatement result;
	result.types = {LogicalType::BIGINT};
	result.names = {"Count"};

	if (stmt.info->Table().empty()) {
		throw ParserException("COPY FROM requires a table name to be specified");
	}
	if (!function.copy_from_bind) {
		throw NotImplementedException("COPY FROM is not supported for FORMAT \"%s\"", stmt.info->format);
	}

	// lookup the table to copy into
	stmt.info->SetQualifiedName(BindTableName(stmt.info->GetQualifiedName()));
	auto &table = Catalog::GetEntry<TableCatalogEntry>(context, stmt.info->GetQualifiedName());
	IndexVector<idx_t, PhysicalIndex> column_index_map;
	vector<LogicalIndex> named_column_map;
	vector<LogicalType> expected_types;
	vector<Identifier> expected_names;
	BindInsertColumnList(table, stmt.info->select_list, false, named_column_map, expected_types, column_index_map);
	expected_names.reserve(named_column_map.size());
	for (auto &column_index : named_column_map) {
		expected_names.push_back(table.GetColumn(column_index).Name());
	}

	// COPY FROM a file
	// generate an insert statement for the to-be-inserted table
	InsertStatement insert;
	auto &insert_node = *insert.node;
	insert_node.qualified_name = stmt.info->GetQualifiedName();
	insert_node.columns = expected_names;
	ColumnDataCollection empty_collection(Allocator::DefaultAllocator(), expected_types);
	auto empty_select = make_uniq<SelectStatement>();
	auto empty_select_node = make_uniq<SelectNode>();
	empty_select_node->select_list.push_back(make_uniq<StarExpression>());
	auto empty_table = make_uniq<ColumnDataRef>(empty_collection, expected_names);
	empty_table->alias = "__copy_from";
	empty_select_node->from_table = std::move(empty_table);
	empty_select->node = std::move(empty_select_node);
	insert_node.select_statement = std::move(empty_select);

	// bind the insert statement to the base table
	auto insert_statement = Bind(insert);
	auto copy_source = FindCopySource(insert_statement.plan, empty_collection);
	if (!copy_source) {
		throw InternalException("Failed to find the source operator for COPY FROM");
	}
	D_ASSERT((*copy_source)->Cast<LogicalColumnDataGet>().chunk_types == expected_types);
	auto source_bindings = (*copy_source)->GetColumnBindings();
	D_ASSERT(!source_bindings.empty());

	auto copy_from_function = function.copy_from_function;
	CopyFromFunctionBindInput input(*stmt.info, copy_from_function);
	auto function_data = function.copy_from_bind(context, input, expected_names, expected_types);
	auto get = make_uniq<LogicalGet>(source_bindings[0].table_index, BoundTableFunction(copy_from_function),
	                                 std::move(function_data), expected_types, expected_names);
	for (idx_t i = 0; i < expected_types.size(); i++) {
		get->AddColumnId(i);
	}
	*copy_source = std::move(get);
	result.plan = std::move(insert_statement.plan);
	return result;
}

vector<Value> BindCopyOption(ClientContext &context, Binder &binder, const Identifier &name,
                             unique_ptr<ParsedExpression> &expr,
                             TableFunctionBinder::IdentifierConversionPolicy conversion_policy) {
	vector<Value> result;
	if (!expr) {
		return result;
	}
	if (expr->GetExpressionType() == ExpressionType::STAR) {
		auto &star = expr->Cast<StarExpression>();
		// for compatibility with previous copy implementation - turn a raw * into a * string literal
		if (star.RelationName().empty() && star.ExcludeList().empty() && star.ReplaceList().empty() &&
		    star.RenameList().empty() && !star.Expression() && !star.IsColumns()) {
			result.push_back("*");
			return result;
		}
	}
	TableFunctionBinder option_binder(binder, context, "Copy", "Copy options", conversion_policy);
	auto bound_expr = option_binder.Bind(expr);
	if (bound_expr->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	auto val = ExpressionExecutor::EvaluateScalar(context, *bound_expr, true);
	if (val.IsNull()) {
		throw BinderException("NULL is not supported as a valid option for COPY option \"" + name + "\"");
	}
	if (val.type().id() == LogicalTypeId::TUPLE) {
		// unpack unnamed structs (tuples) into a list of options
		return StructValue::GetChildren(val);
	}
	result.push_back(std::move(val));
	return result;
}

string ExtractFormat(const string &file_path) {
	auto format = StringUtil::Lower(file_path);
	// We first remove extension suffixes
	if (StringUtil::EndsWith(format, CompressionExtensionFromType(FileCompressionType::GZIP))) {
		format = format.substr(0, format.size() - 3);
	} else if (StringUtil::EndsWith(format, CompressionExtensionFromType(FileCompressionType::ZSTD))) {
		format = format.substr(0, format.size() - 4);
	}
	// Now lets check for the last .
	size_t dot_pos = format.rfind('.');
	if (dot_pos == std::string::npos || dot_pos == format.length() - 1) {
		// No format found
		return "";
	}
	// We found something
	return format.substr(dot_pos + 1);
}

void Binder::BindCopyFormat(CopyInfo &info) {
	const auto conversion_policy = TableFunctionBinder::IdentifierConversionPolicy::FOLLOW_SETTING;
	if (info.file_path_expression) {
		auto inputs = BindCopyOption(context, *this, "filename", info.file_path_expression, conversion_policy);
		if (inputs.size() != 1 || inputs[0].type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("Unsupported parameter type for filename: expected e.g. TARGET 'file.parquet'");
		}
		if (!info.file_path.empty()) {
			throw InternalException("Both a file path and a file path expression were provided for COPY - only one of "
			                        "the two can be provided");
		}
		info.file_path = inputs[0].ToString();
		info.file_path_expression.reset();
	}
	auto format_entry = info.parsed_options.find("format");
	if (format_entry != info.parsed_options.end()) {
		auto inputs = BindCopyOption(context, *this, "format", format_entry->second, conversion_policy);
		if (inputs.size() != 1 || inputs[0].type().id() != LogicalTypeId::VARCHAR) {
			throw ParserException("Unsupported parameter type for FORMAT: expected e.g. FORMAT 'csv', 'parquet'");
		}
		info.format = StringUtil::Lower(inputs[0].ToString());
		info.is_format_auto_detected = false;
		info.parsed_options.erase(format_entry);
	}
	if (info.is_format_auto_detected && info.format.empty()) {
		info.format = ExtractFormat(info.file_path);
	}
}

void Binder::BindCopyOptions(CopyInfo &info, const CopyFunction &function) {
	identifier_map_t<CopyOption> copy_options;
	if (function.copy_options) {
		copy_options = GetFullCopyOptionsList(function, CopyOptionMode::READ_WRITE);
	}
	unique_ptr<ParsedExpression> partition_path;
	for (auto &[option_name, option_expr] : info.parsed_options) {
		if (option_name == "partition_path") {
			// bound later against the partition columns
			if (!option_expr) {
				throw BinderException("PARTITION_PATH expects an expression");
			}
			partition_path = std::move(option_expr);
			continue;
		}
		// column list options accept bare column names, also ones that spell a SQL value function such as user
		auto copy_option = copy_options.find(option_name);
		const bool is_column_list =
		    copy_option != copy_options.end() && copy_option->second.type == LogicalType::LIST(LogicalType::VARCHAR);
		auto conversion_policy = is_column_list ? TableFunctionBinder::IdentifierConversionPolicy::ALLOW
		                                        : TableFunctionBinder::IdentifierConversionPolicy::FOLLOW_SETTING;
		info.options[option_name] = BindCopyOption(context, *this, option_name, option_expr, conversion_policy);
	}
	info.parsed_options.clear();
	if (partition_path) {
		info.parsed_options["partition_path"] = std::move(partition_path);
	}
}

BoundStatement Binder::Bind(CopyStatement &stmt, CopyToType copy_to_type) {
	BindCopyFormat(*stmt.info);
	if (stmt.info->is_from && stmt.info->parsed_options.find("partition_path") != stmt.info->parsed_options.end()) {
		throw InvalidInputException("Option partition_path is not supported for reading - only for writing");
	}

	if (!stmt.info->is_from && !stmt.info->select_statement) {
		// copy table into file without a query
		// generate SELECT * FROM table;
		auto ref = make_uniq<BaseTableRef>();
		ref->SetQualifiedName(stmt.info->GetQualifiedName());

		auto statement = make_uniq<SelectNode>();
		statement->from_table = std::move(ref);
		if (!stmt.info->select_list.empty()) {
			for (auto &name : stmt.info->select_list) {
				statement->select_list.push_back(make_uniq<ColumnRefExpression>(name));
			}
		} else {
			statement->select_list.push_back(make_uniq<StarExpression>());
		}
		stmt.info->select_statement = std::move(statement);
	}

	// Let's first bind our format
	// lookup the format in the catalog
	auto on_entry_do =
	    stmt.info->is_format_auto_detected ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	CatalogEntryRetriever entry_retriever {context};
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto entry = catalog.GetEntry(
	    entry_retriever,
	    EntryLookupInfo(CatalogType::COPY_FUNCTION_ENTRY,
	                    QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), Identifier(stmt.info->format))),
	    on_entry_do);

	if (!entry) {
		IsFormatExtensionKnown(stmt.info->format);
		// If we did not find an entry, we default to a CSV
		entry = catalog.GetEntry(
		    entry_retriever,
		    EntryLookupInfo(CatalogType::COPY_FUNCTION_ENTRY,
		                    QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), Identifier("csv"))),
		    OnEntryNotFound::THROW_EXCEPTION);
	}
	auto &copy_function = entry->Cast<CopyFunctionCatalogEntry>();
	auto &function = copy_function.function;

	// bind the copy options
	BindCopyOptions(*stmt.info, function);
	if (function.copy_options) {
		// list all copy options - then bind them and offer alternatives
		auto copy_mode = stmt.info->is_from ? CopyOptionMode::READ_ONLY : CopyOptionMode::WRITE_ONLY;
		auto copy_options = GetFullCopyOptionsList(function, CopyOptionMode::READ_WRITE);
		for (auto &[provided_option, provided_values] : stmt.info->options) {
			auto option_entry = copy_options.find(provided_option);
			if (option_entry == copy_options.end()) {
				// option not found - offer an alternative suggestion
				vector<string> candidates;
				for (auto &[candidate_option, candidate] : copy_options) {
					candidates.push_back(candidate_option.GetIdentifierName());
				}
				string candidate_str = StringUtil::CandidatesMessage(
				    StringUtil::TopNJaroWinkler(candidates, provided_option), "Candidate options");

				throw NotImplementedException("Unrecognized option %s for %s\n%s", provided_option, stmt.info->format,
				                              candidate_str);
			}
			auto &copy_option = option_entry->second;
			// check if this matches the mode
			if (copy_option.mode != CopyOptionMode::READ_WRITE && copy_option.mode != copy_mode) {
				throw InvalidInputException("Option %s is not supported for %s - only for %s", provided_option,
				                            stmt.info->is_from ? "reading" : "writing",
				                            stmt.info->is_from ? "writing" : "reading");
			}
			if (copy_option.type.id() != LogicalTypeId::ANY && copy_option.type.id() != LogicalTypeId::LIST) {
				if (provided_values.empty()) {
					if (copy_option.type.id() == LogicalTypeId::BOOLEAN) {
						// boolean can be empty (e.g. "HEADER")
						continue;
					}
					throw InvalidInputException("Copy option %s requires an argument of type %s", provided_option,
					                            copy_option.type.ToString());
				}
				if (provided_values.size() > 1) {
					throw InvalidInputException("Copy option %s did not expect a list as argument", provided_option);
				}
				auto &original_value = provided_values[0];
				if (original_value.IsNull()) {
					throw BinderException("NULL is not supported as a valid option for COPY option %s",
					                      provided_option);
				}
				if (copy_option.type == original_value.type()) {
					// types match
					continue;
				}
				bool can_cast =
				    CastFunctionSet::ImplicitCastCost(context, original_value.type(), copy_option.type) >= 0;
				if (!can_cast) {
					// for backwards compatibility - we are more lax on casting rules for copy options
					if (copy_option.type.IsNumeric()) {
						can_cast = original_value.type().IsNumeric();
					} else if (copy_option.type.id() == LogicalTypeId::BOOLEAN) {
						can_cast = original_value.type().IsIntegral();
					}
					if (original_value.type().id() == LogicalTypeId::VARCHAR) {
						can_cast = true;
					}
				}

				auto new_value = can_cast ? original_value.TryCastAs(context, copy_option.type) : nullopt;
				if (!new_value) {
					throw InvalidInputException("Copy option %s expected an argument of type %s - the argument "
					                            "\"%s\" of type %s could not be cast as this type",
					                            provided_option, copy_option.type, original_value.ToString(),
					                            original_value.type());
				}
				original_value = std::move(*new_value);
			}
		}
	}

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::CHANGED_ROWS;
	if (stmt.info->is_from) {
		return BindCopyFrom(stmt, function);
	} else {
		return BindCopyTo(stmt, function, copy_to_type);
	}
}

} // namespace duckdb
