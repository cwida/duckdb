#include "duckdb/function/table_function.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>

namespace duckdb {

GlobalTableFunctionState::~GlobalTableFunctionState() {
}

LocalTableFunctionState::~LocalTableFunctionState() {
}

PartitionStatistics::PartitionStatistics() : row_start(0), count(0), count_type(CountType::COUNT_APPROXIMATE) {
}

TableFunctionInfo::~TableFunctionInfo() {
}

BaseTableFunction::BaseTableFunction(table_function_t function_, table_function_bind_t bind,
                                     table_function_init_global_t init_global, table_function_init_local_t init_local)
    : bind(bind), bind_replace(nullptr), bind_operator(nullptr), init_global(init_global), init_local(init_local),
      function(function_), in_out_function(nullptr), in_out_function_final(nullptr), statistics(nullptr),
      statistics_extended(nullptr), dependency(nullptr), cardinality(nullptr), get_metrics(nullptr),
      pushdown_complex_filter(nullptr), pushdown_expression(nullptr), to_string(nullptr), table_scan_progress(nullptr),
      get_partition_data(nullptr), get_bind_info(nullptr), get_table_entry(nullptr),
      projection_expression_pushdown(nullptr), get_multi_file_reader(nullptr), supports_pushdown_type(nullptr),
      supports_pushdown_extract(nullptr), is_repeatable(nullptr), get_partition_info(nullptr),
      get_partition_stats(nullptr), get_virtual_columns(nullptr), get_row_id_columns(nullptr), set_scan_order(nullptr),
      serialize(nullptr), deserialize(nullptr), projection_pushdown(false), filter_pushdown(false), filter_prune(false),
      sampling_pushdown(false), late_materialization(false),
      return_type(TableFunctionReturnType::TABLE_RETURNING_FUNCTION) {
}

TableFunction::TableFunction(Identifier name, const vector<LogicalType> &arguments, table_function_t function_,
                             table_function_bind_t bind, table_function_init_global_t init_global,
                             table_function_init_local_t init_local)
    : BaseTableFunction(function_, bind, init_global, init_local),
      SimpleFunction(std::move(name), arguments, LogicalType::INVALID) {
}

TableFunction::TableFunction(Identifier name, const vector<LogicalType> &arguments, std::nullptr_t,
                             table_function_bind_t bind, table_function_init_global_t init_global,
                             table_function_init_local_t init_local)
    : BaseTableFunction(nullptr, bind, init_global, init_local),
      SimpleFunction(std::move(name), arguments, LogicalType::INVALID) {
}

TableFunction::TableFunction(Identifier name, FunctionSignature signature, table_function_t function_,
                             table_function_bind_t bind, table_function_init_global_t init_global,
                             table_function_init_local_t init_local)
    : BaseTableFunction(function_, bind, init_global, init_local),
      SimpleFunction(std::move(name), std::move(signature)) {
}

TableFunction::TableFunction(FunctionSignature signature, table_function_t function_, table_function_bind_t bind,
                             table_function_init_global_t init_global, table_function_init_local_t init_local)
    : BaseTableFunction(function_, bind, init_global, init_local), SimpleFunction("", std::move(signature)) {
}

TableFunction::TableFunction(Identifier name, FunctionSignature signature, std::nullptr_t, table_function_bind_t bind,
                             table_function_init_global_t init_global, table_function_init_local_t init_local)
    : BaseTableFunction(nullptr, bind, init_global, init_local), SimpleFunction(std::move(name), std::move(signature)) {
}

BoundTableFunction::BoundTableFunction() : BaseTableFunction(nullptr, nullptr, nullptr, nullptr) {
}

BoundTableFunction::BoundTableFunction(const TableFunction &function)
    // the function does not come from a function set - copy it into a definition of its own
    : BoundTableFunction(make_shared_ptr<const TableFunction>(function)) {
}

BoundTableFunction::BoundTableFunction(shared_ptr<const TableFunction> function_p)
    : BaseTableFunction(nullptr, nullptr, nullptr, nullptr) {
	definition = std::move(function_p);
	auto &function = *definition;
	qualified_name = function.GetQualifiedName();
	extra_info = function.extra_info;

	// the behaviour is taken over one member at a time - the declaration stays behind on the definition
	bind = function.bind;
	bind_replace = function.bind_replace;
	bind_operator = function.bind_operator;
	init_global = function.init_global;
	init_local = function.init_local;
	this->function = function.function;
	in_out_function = function.in_out_function;
	in_out_function_final = function.in_out_function_final;
	statistics = function.statistics;
	statistics_extended = function.statistics_extended;
	dependency = function.dependency;
	cardinality = function.cardinality;
	get_metrics = function.get_metrics;
	pushdown_complex_filter = function.pushdown_complex_filter;
	pushdown_expression = function.pushdown_expression;
	to_string = function.to_string;
	to_sql = function.to_sql;
	table_scan_progress = function.table_scan_progress;
	get_partition_data = function.get_partition_data;
	get_bind_info = function.get_bind_info;
	get_table_entry = function.get_table_entry;
	projection_expression_pushdown = function.projection_expression_pushdown;
	get_multi_file_reader = function.get_multi_file_reader;
	supports_pushdown_type = function.supports_pushdown_type;
	supports_pushdown_extract = function.supports_pushdown_extract;
	is_repeatable = function.is_repeatable;
	get_partition_info = function.get_partition_info;
	get_partition_stats = function.get_partition_stats;
	get_virtual_columns = function.get_virtual_columns;
	get_row_id_columns = function.get_row_id_columns;
	set_scan_order = function.set_scan_order;
	set_partitions_to_scan = function.set_partitions_to_scan;
	serialize = function.serialize;
	deserialize = function.deserialize;
	verify_serialization = function.verify_serialization;
	projection_pushdown = function.projection_pushdown;
	filter_pushdown = function.filter_pushdown;
	runtime_filter_pushdown = function.runtime_filter_pushdown;
	filter_prune = function.filter_prune;
	sampling_pushdown = function.sampling_pushdown;
	late_materialization = function.late_materialization;
	function_info = function.function_info;
	BaseTableFunction::return_type = function.return_type;
	call_return_type = function.call_return_type;
	order_preservation_type = function.order_preservation_type;
	global_initialization = function.global_initialization;
	parallelism = function.parallelism;

	// the parameters a call fills by position - these are the types plan serialization records, so the named
	// options that follow them take no part
	auto &signature = function.GetSignature();
	for (idx_t i = 0; i < signature.GetPositionalParameterCount(); i++) {
		arguments.push_back(signature.GetParameter(i).GetType());
	}
	positional_arguments = arguments.size();
}

void BoundTableFunction::SetCallArguments(const vector<Value> &parameters,
                                          const named_argument_map_t &named_parameters) {
	BoundSimpleFunction::SetCallArguments(GetSignature(), parameters, named_parameters);
}

bool BoundTableFunction::operator==(const BoundTableFunction &rhs) const {
	return GetQualifiedName() == rhs.GetQualifiedName() && GetArguments() == rhs.GetArguments() &&
	       BaseTableFunction::operator==(rhs);
}

bool BoundTableFunction::operator!=(const BoundTableFunction &rhs) const {
	return !(*this == rhs);
}

TableFunction::TableFunction(const vector<LogicalType> &arguments, table_function_t function_,
                             table_function_bind_t bind, table_function_init_global_t init_global,
                             table_function_init_local_t init_local)
    : TableFunction("", arguments, function_, bind, init_global, init_local) {
}

TableFunction::TableFunction(const vector<LogicalType> &arguments, std::nullptr_t function_, table_function_bind_t bind,
                             table_function_init_global_t init_global, table_function_init_local_t init_local)
    : TableFunction("", arguments, function_, bind, init_global, init_local) {
}

TableFunction::TableFunction() : TableFunction("", {}, nullptr, nullptr, nullptr, nullptr) {
}

bool BaseTableFunction::operator==(const BaseTableFunction &rhs) const {
	return bind == rhs.bind && bind_replace == rhs.bind_replace && bind_operator == rhs.bind_operator &&
	       init_global == rhs.init_global && init_local == rhs.init_local && function == rhs.function &&
	       in_out_function == rhs.in_out_function && in_out_function_final == rhs.in_out_function_final &&
	       statistics == rhs.statistics && dependency == rhs.dependency && cardinality == rhs.cardinality &&
	       pushdown_complex_filter == rhs.pushdown_complex_filter && pushdown_expression == rhs.pushdown_expression &&
	       to_string == rhs.to_string && to_sql == rhs.to_sql && table_scan_progress == rhs.table_scan_progress &&
	       get_partition_data == rhs.get_partition_data && get_bind_info == rhs.get_bind_info &&
	       get_table_entry == rhs.get_table_entry &&
	       projection_expression_pushdown == rhs.projection_expression_pushdown &&
	       get_multi_file_reader == rhs.get_multi_file_reader && supports_pushdown_type == rhs.supports_pushdown_type &&
	       is_repeatable == rhs.is_repeatable && get_partition_info == rhs.get_partition_info &&
	       get_partition_stats == rhs.get_partition_stats && get_virtual_columns == rhs.get_virtual_columns &&
	       get_row_id_columns == rhs.get_row_id_columns && serialize == rhs.serialize &&
	       deserialize == rhs.deserialize && verify_serialization == rhs.verify_serialization &&
	       projection_pushdown == rhs.projection_pushdown && filter_pushdown == rhs.filter_pushdown &&
	       runtime_filter_pushdown == rhs.runtime_filter_pushdown && filter_prune == rhs.filter_prune &&
	       sampling_pushdown == rhs.sampling_pushdown && late_materialization == rhs.late_materialization &&
	       return_type == rhs.return_type && global_initialization == rhs.global_initialization;
}

bool TableFunction::operator==(const TableFunction &rhs) const {
	return name == rhs.name && GetSignature() == rhs.GetSignature() && BaseTableFunction::operator==(rhs);
}

bool TableFunction::operator!=(const TableFunction &rhs) const {
	return !(*this == rhs);
}

bool TableFunctionInput::HandleBlocked(AsyncResult &blocked_result) {
	D_ASSERT(blocked_result.GetResultType() == AsyncResultType::BLOCKED);
	switch (results_execution_mode) {
	case AsyncResultsExecutionMode::TASK_EXECUTOR:
		async_result = std::move(blocked_result);
		return true;
	case AsyncResultsExecutionMode::SYNCHRONOUS:
		// run the I/O synchronously, then loop again to resume
		blocked_result.ExecuteTasksSynchronously();
		if (blocked_result.GetResultType() != AsyncResultType::HAVE_MORE_OUTPUT) {
			throw InternalException("Unexpected behaviour from ExecuteTasksSynchronously");
		}
		return false;
	default:
		throw InternalException("Unexpected AsyncResultsExecutionMode in HandleBlocked");
	}
}

bool TableFunctionInput::HandleBlocked(ClientContext &context, AsyncResult &blocked_result) {
	D_ASSERT(blocked_result.GetResultType() == AsyncResultType::BLOCKED);
	switch (results_execution_mode) {
	case AsyncResultsExecutionMode::TASK_EXECUTOR:
		async_result = std::move(blocked_result);
		return true;
	case AsyncResultsExecutionMode::SYNCHRONOUS:
		// run the I/O here, side by side, then loop again to resume
		blocked_result.ExecuteTasksSynchronously(context);
		if (blocked_result.GetResultType() != AsyncResultType::HAVE_MORE_OUTPUT) {
			throw InternalException("Unexpected behaviour from ExecuteTasksSynchronously");
		}
		return false;
	default:
		throw InternalException("Unexpected AsyncResultsExecutionMode in HandleBlocked");
	}
}

} // namespace duckdb
