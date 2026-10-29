//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/projection_index.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/enums/order_preservation_type.hpp"
#include "duckdb/common/enums/statement_type.hpp"

namespace duckdb {
enum class TablePartitionInfo : uint8_t;
struct PartitionStatistics;
struct TableFunctionFileBindInput;
struct TableFunctionFileInitInput;

//! Controls how a table function manages parallelism.
enum class TableFunctionParallelism : uint8_t {
	SELF_MANAGED_PARALLELISM = 0, //! Source handles its own parallelism (default)
	SEQUENTIAL = 1,               //! Sequential source, benefits from external parallelization
	FORCE_SINGLE_THREADED = 2     //! Sequential source, prefers single-threaded execution
};

class BaseStatistics;
class LogicalDependencyList;
class LogicalGet;
class TableFunction;
class BoundTableFunction;
class TableFilterSet;
class TableFunctionRef;
class TableCatalogEntry;
class SampleOptions;
struct MultiFileReader;
struct OperatorPartitionData;
struct OperatorPartitionInfo;
struct OperatorMetrics;
enum class OrderByColumnType : uint8_t;
enum class OrderType : uint8_t;
enum class OrderByStatistics : uint8_t;
struct RowGroupOrderOptions;
class LogicalOperator;
class Binder;
class QueryNode;

struct TableFunctionInfo {
	DUCKDB_API virtual ~TableFunctionInfo();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct GlobalTableFunctionState {
public:
	// value returned from MaxThreads when as many threads as possible should be used
	constexpr static const int64_t MAX_THREADS = 999999999;

public:
	DUCKDB_API virtual ~GlobalTableFunctionState();

	virtual idx_t MaxThreads() const {
		return 1;
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct LocalTableFunctionState {
	DUCKDB_API virtual ~LocalTableFunctionState();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct TableFunctionBindInput {
	TableFunctionBindInput(vector<Value> &inputs, named_argument_map_t &named_parameters,
	                       vector<LogicalType> &input_table_types, vector<Identifier> &input_table_names,
	                       optional_ptr<TableFunctionInfo> info, optional_ptr<Binder> binder,
	                       BoundTableFunction &table_function, const TableFunctionRef &ref,
	                       optional_ptr<unique_ptr<LogicalOperator>> input_plan = nullptr)
	    : inputs(inputs), named_parameters(named_parameters), input_table_types(input_table_types),
	      input_table_names(input_table_names), info(info), binder(binder), table_function(table_function), ref(ref),
	      input_plan(input_plan) {
	}

	vector<Value> &inputs;
	named_argument_map_t &named_parameters;
	vector<LogicalType> &input_table_types;
	vector<Identifier> &input_table_names;
	optional_ptr<TableFunctionInfo> info;
	optional_ptr<Binder> binder;
	BoundTableFunction &table_function;
	const TableFunctionRef &ref;
	optional_ptr<unique_ptr<LogicalOperator>> input_plan;
	//! (Optional) Set when a multi-file scan binds this function to read one of its files
	optional_ptr<const TableFunctionFileBindInput> multi_file_input;
};

struct TableFunctionInitInput {
	TableFunctionInitInput(optional_ptr<const FunctionData> bind_data_p, vector<column_t> column_ids_p,
	                       const vector<idx_t> &projection_ids_p, optional_ptr<TableFilterSet> filters_p,
	                       optional_ptr<SampleOptions> sample_options_p = nullptr,
	                       optional_ptr<const PhysicalOperator> op_p = nullptr)
	    : bind_data(bind_data_p), column_ids(std::move(column_ids_p)), projection_ids(projection_ids_p),
	      filters(filters_p), sample_options(sample_options_p), op(op_p) {
		for (auto &col_id : column_ids) {
			column_indexes.emplace_back(col_id);
		}
	}

	TableFunctionInitInput(optional_ptr<const FunctionData> bind_data_p, vector<ColumnIndex> column_indexes_p,
	                       const vector<idx_t> &projection_ids_p, optional_ptr<TableFilterSet> filters_p,
	                       optional_ptr<SampleOptions> sample_options_p = nullptr,
	                       optional_ptr<const PhysicalOperator> op_p = nullptr)
	    : bind_data(bind_data_p), column_indexes(std::move(column_indexes_p)), projection_ids(projection_ids_p),
	      filters(filters_p), sample_options(sample_options_p), op(op_p) {
		for (auto &col_id : column_indexes) {
			column_ids.emplace_back(col_id.GetPrimaryIndex());
		}
	}

	optional_ptr<const FunctionData> bind_data;
	vector<column_t> column_ids;
	vector<ColumnIndex> column_indexes;
	const vector<idx_t> projection_ids;
	optional_ptr<TableFilterSet> filters;
	optional_ptr<SampleOptions> sample_options;
	optional_ptr<const PhysicalOperator> op;
	//! (Optional) Set when a multi-file scan initializes this function to read one of its files
	optional_ptr<const TableFunctionFileInitInput> multi_file_input;

	bool CanRemoveFilterColumns() const {
		if (projection_ids.empty()) {
			// No filter columns to remove.
			return false;
		}
		if (projection_ids.size() == column_ids.size()) {
			// Filter column is used in remainder of plan, so we cannot remove it.
			return false;
		}
		// Fewer columns need to be projected out than that we scan.
		return true;
	}
};

struct TableFunctionInput {
public:
	TableFunctionInput(optional_ptr<const FunctionData> bind_data_p,
	                   optional_ptr<LocalTableFunctionState> local_state_p,
	                   optional_ptr<GlobalTableFunctionState> global_state_p)
	    : bind_data(bind_data_p), local_state(local_state_p), global_state(global_state_p), async_result() {
	}

public:
	//! Handles a BLOCKED result per the execution mode, returns true when the function must return to yield
	DUCKDB_API bool HandleBlocked(AsyncResult &blocked_result);
	//! Like HandleBlocked, but a synchronous execution runs the tasks side by side instead of one after the other
	DUCKDB_API bool HandleBlocked(ClientContext &context, AsyncResult &blocked_result);

public:
	optional_ptr<const FunctionData> bind_data;
	optional_ptr<LocalTableFunctionState> local_state;
	optional_ptr<GlobalTableFunctionState> global_state;
	AsyncResult async_result {};
	AsyncResultsExecutionMode results_execution_mode {AsyncResultsExecutionMode::SYNCHRONOUS};
	//! Interrupt state of the calling task, so the function might park and wake-up by returning a taskless Blocked res
	optional_ptr<const InterruptState> interrupt_state;
};

struct TableFunctionPartitionInput {
	TableFunctionPartitionInput(optional_ptr<const FunctionData> bind_data_p, const vector<column_t> &partition_ids)
	    : bind_data(bind_data_p), partition_ids(partition_ids) {
	}

	optional_ptr<const FunctionData> bind_data;
	const vector<column_t> &partition_ids;
};

struct TableFunctionProjectionExpressionInput {
	const LogicalGet &get;
	const Expression &expr;
	//! Position of pushed down column within get, usage: get.GetColumnIds()[column_index]
	ProjectionIndex column_index;
};

struct TableFunctionToStringInput {
	TableFunctionToStringInput(const BoundTableFunction &table_function_p, optional_ptr<const FunctionData> bind_data_p)
	    : table_function(table_function_p), bind_data(bind_data_p) {
	}
	const BoundTableFunction &table_function;
	optional_ptr<const FunctionData> bind_data;
};

struct TableFunctionGetPartitionInput {
public:
	TableFunctionGetPartitionInput(optional_ptr<const FunctionData> bind_data_p,
	                               optional_ptr<LocalTableFunctionState> local_state_p,
	                               optional_ptr<GlobalTableFunctionState> global_state_p,
	                               const OperatorPartitionInfo &partition_info_p)
	    : bind_data(bind_data_p), local_state(local_state_p), global_state(global_state_p),
	      partition_info(partition_info_p) {
	}

public:
	optional_ptr<const FunctionData> bind_data;
	optional_ptr<LocalTableFunctionState> local_state;
	optional_ptr<GlobalTableFunctionState> global_state;
	const OperatorPartitionInfo &partition_info;
};

struct TableFunctionGetStatisticsInput {
public:
	TableFunctionGetStatisticsInput(const FunctionData *bind_data, const ColumnIndex &column_index)
	    : bind_data(bind_data), column_index(column_index) {
	}

public:
	optional_ptr<const FunctionData> bind_data;
	ColumnIndex column_index;
};

struct GetPartitionStatsInput {
	GetPartitionStatsInput(const BoundTableFunction &table_function_p, optional_ptr<const FunctionData> bind_data_p)
	    : table_function(table_function_p), bind_data(bind_data_p) {
	}

	const BoundTableFunction &table_function;
	optional_ptr<const FunctionData> bind_data;
};

struct TableFunctionGetBindInfoInput {
public:
	TableFunctionGetBindInfoInput(ClientContext &context, optional_ptr<FunctionData> bind_data_p)
	    : context(context), bind_data(bind_data_p) {
	}

public:
	ClientContext &context;
	optional_ptr<FunctionData> bind_data;
};

struct TableFunctionGetMetricsInput {
public:
	TableFunctionGetMetricsInput(ClientContext &context, optional_ptr<const FunctionData> bind_data_p,
	                             optional_ptr<LocalTableFunctionState> local_state_p,
	                             optional_ptr<GlobalTableFunctionState> global_state_p, OperatorMetrics &metrics_p)
	    : context(context), bind_data(bind_data_p), local_state(local_state_p), global_state(global_state_p),
	      operator_metrics(metrics_p) {
	}

public:
	ClientContext &context;
	optional_ptr<const FunctionData> bind_data;
	optional_ptr<LocalTableFunctionState> local_state;
	optional_ptr<GlobalTableFunctionState> global_state;
	OperatorMetrics &operator_metrics;
};

enum class ScanType : uint8_t { TABLE, PARQUET, EXTERNAL };

struct BindInfo {
public:
	explicit BindInfo(ScanType type_p) : type(type_p) {};

	unordered_map<string, Value> options;
	ScanType type;

	//! An identifier attached to a result column of the bound call, or to a field nested inside one (addressed as in
	//! MultiFileColumnDefinition::ResolveChildPath) - e.g. a field id, by which a multi-file function wrapping the
	//! function maps the columns of its files onto one another
	struct ColumnIdentifier {
		idx_t column_index;
		vector<idx_t> child_path;
		Value identifier;
	};
	vector<ColumnIdentifier> column_identifiers;

	void InsertOption(const string &name, Value value) { // NOLINT: work-around bug in clang-tidy
		if (options.find(name) != options.end()) {
			throw InternalException("This option already exists");
		}
		options.emplace(name, std::move(value));
	}
	template <class T>
	T GetOption(const string &name) {
		if (options.find(name) == options.end()) {
			throw InternalException("This option does not exist");
		}
		return options[name].GetValue<T>();
	}
	template <class T>
	vector<T> GetOptionList(const string &name) {
		if (options.find(name) == options.end()) {
			throw InternalException("This option does not exist");
		}
		auto option = options[name];
		if (option.type().id() != LogicalTypeId::LIST) {
			throw InternalException("This option is not a list");
		}
		vector<T> result;
		auto list_children = ListValue::GetChildren(option);
		for (auto &child : list_children) {
			result.emplace_back(child.GetValue<T>());
		}
		return result;
	}
};

typedef unique_ptr<FunctionData> (*table_function_bind_t)(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types, vector<Identifier> &names);
typedef unique_ptr<TableRef> (*table_function_bind_replace_t)(ClientContext &context, TableFunctionBindInput &input);
typedef unique_ptr<LogicalOperator> (*table_function_bind_operator_t)(ClientContext &context,
                                                                      TableFunctionBindInput &input,
                                                                      TableIndex bind_index,
                                                                      vector<Identifier> &return_names);
typedef unique_ptr<GlobalTableFunctionState> (*table_function_init_global_t)(ClientContext &context,
                                                                             TableFunctionInitInput &input);
typedef unique_ptr<LocalTableFunctionState> (*table_function_init_local_t)(ExecutionContext &context,
                                                                           TableFunctionInitInput &input,
                                                                           GlobalTableFunctionState *global_state);
typedef unique_ptr<BaseStatistics> (*table_statistics_t)(ClientContext &context, const FunctionData *bind_data,
                                                         column_t column_index);
typedef unique_ptr<BaseStatistics> (*table_statistics_extended_t)(ClientContext &context,
                                                                  TableFunctionGetStatisticsInput &input);
typedef void (*table_function_t)(ClientContext &context, TableFunctionInput &data, DataChunk &output);
typedef OperatorResultType (*table_in_out_function_t)(ExecutionContext &context, TableFunctionInput &data,
                                                      DataChunk &input, DataChunk &output);
typedef OperatorFinalizeResultType (*table_in_out_function_final_t)(ExecutionContext &context, TableFunctionInput &data,
                                                                    DataChunk &output);
typedef OperatorPartitionData (*table_function_get_partition_data_t)(ClientContext &context,
                                                                     TableFunctionGetPartitionInput &input);

typedef BindInfo (*table_function_get_bind_info_t)(TableFunctionGetBindInfoInput &input);
//! The table a bound call scans, if it scans one - see LogicalGet::GetTable
typedef optional_ptr<TableCatalogEntry> (*table_function_get_table_entry_t)(optional_ptr<const FunctionData> bind_data);

typedef unique_ptr<MultiFileReader> (*table_function_get_multi_file_reader_t)(const BoundTableFunction &);

typedef bool (*table_function_supports_pushdown_type_t)(const FunctionData &bind_data, idx_t col_idx);

typedef bool (*table_function_supports_pushdown_extract_t)(const FunctionData &bind_data, const LogicalIndex &col_idx);

//! Whether repeated executions with the same bound data are stable within one query.
typedef bool (*table_function_is_repeatable_t)(optional_ptr<const FunctionData> bind_data);

typedef double (*table_function_progress_t)(ClientContext &context, const FunctionData *bind_data,
                                            const GlobalTableFunctionState *global_state);
typedef void (*table_function_dependency_t)(LogicalDependencyList &dependencies, const FunctionData *bind_data);
typedef unique_ptr<NodeStatistics> (*table_function_cardinality_t)(ClientContext &context,
                                                                   const FunctionData *bind_data);
typedef void (*table_function_get_metrics_t)(TableFunctionGetMetricsInput &input);
typedef void (*table_function_pushdown_complex_filter_t)(ClientContext &context, LogicalGet &get,
                                                         FunctionData *bind_data,
                                                         vector<unique_ptr<Expression>> &filters);
typedef bool (*table_function_pushdown_expression_t)(ClientContext &context, const LogicalGet &get, Expression &expr);
typedef InsertionOrderPreservingMap<string> (*table_function_to_string_t)(TableFunctionToStringInput &input);

struct TableFunctionToSQLResult {
	unique_ptr<TableRef> source;
	string unsupported_reason;
};

//! Reconstruct the unprojected source; the exporter applies scan projections, predicates and ordinality.
//! Return an owned table reference without executing the source, or a reason why reconstruction is unsupported.
typedef TableFunctionToSQLResult (*table_function_to_sql_t)(ClientContext &context, const LogicalGet &get);

typedef void (*table_function_serialize_t)(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                           const BoundTableFunction &function);
typedef unique_ptr<FunctionData> (*table_function_deserialize_t)(Deserializer &deserializer,
                                                                 BoundTableFunction &function);

typedef bool (*table_function_projection_expression_pushdown_t)(ClientContext &context,
                                                                const TableFunctionProjectionExpressionInput &input);
typedef TablePartitionInfo (*table_function_get_partition_info_t)(ClientContext &context,
                                                                  TableFunctionPartitionInput &input);

typedef vector<PartitionStatistics> (*table_function_get_partition_stats_t)(ClientContext &context,
                                                                            GetPartitionStatsInput &input);

typedef virtual_column_map_t (*table_function_get_virtual_columns_t)(ClientContext &context,
                                                                     optional_ptr<FunctionData> bind_data);

typedef vector<column_t> (*table_function_get_row_id_columns)(ClientContext &context,
                                                              optional_ptr<FunctionData> bind_data);

typedef void (*table_function_set_scan_order)(unique_ptr<RowGroupOrderOptions> order_options,
                                              optional_ptr<FunctionData> bind_data);

typedef void (*table_function_set_partitions_to_scan_t)(vector<idx_t> partition_indices,
                                                        optional_ptr<FunctionData> bind_data);

//! When to call init_global to initialize the table function
enum class TableFunctionInitialization { INITIALIZE_ON_EXECUTE, INITIALIZE_ON_SCHEDULE };

enum class TableFunctionReturnType { TABLE_RETURNING_FUNCTION, SET_RETURNING_FUNCTION };

//! The callbacks and flags that make up a table function's behaviour, shared by the unbound TableFunction and by the
//! BoundTableFunction that a bound call produces. It deliberately does not derive from Function, so that each of the
//! two can pair it with its own name and argument representation - the split BaseScalarFunction makes for scalars
class BaseTableFunction {
public:
	DUCKDB_API BaseTableFunction(table_function_t function, table_function_bind_t bind,
	                             table_function_init_global_t init_global, table_function_init_local_t init_local);

	bool HasBindCallback() const {
		return bind != nullptr;
	}
	table_function_bind_t GetBindCallback() const {
		return bind;
	}
	bool HasSerializationCallbacks() const {
		return serialize != nullptr && deserialize != nullptr;
	}
	void SetSerializeCallback(table_function_serialize_t callback) {
		serialize = callback;
	}
	void SetDeserializeCallback(table_function_deserialize_t callback) {
		deserialize = callback;
	}
	table_function_serialize_t GetSerializeCallback() const {
		return serialize;
	}
	table_function_deserialize_t GetDeserializeCallback() const {
		return deserialize;
	}

	//! Bind function
	//! This function is used for determining the return type of a table producing function and returning bind data
	//! The returned FunctionData object should be constant and should not be changed during execution.
	table_function_bind_t bind;
	//! (Optional) Bind replace function
	//! This function is called before the regular bind function. It allows returning a TableRef that will be used to
	//! to generate a logical plan that replaces the LogicalGet of a regularly bound TableFunction. The BindReplace can
	//! also return a nullptr to indicate a regular bind needs to be performed instead.
	table_function_bind_replace_t bind_replace;
	//! (Optional) Bind operator function
	//! This function is called before the regular bind function - similar to bind_replace - but allows returning a
	//! custom LogicalOperator instead.
	table_function_bind_operator_t bind_operator;
	//! (Optional) global init function
	//! Initialize the global operator state of the function.
	//! The global operator state is used to keep track of the progress in the table function and is shared between
	//! all threads working on the table function.
	table_function_init_global_t init_global;
	//! (Optional) local init function
	//! Initialize the local operator state of the function.
	//! The local operator state is used to keep track of the progress in the table function and is thread-local.
	table_function_init_local_t init_local;
	//! The main function
	table_function_t function;
	//! The table in-out function (if this is an in-out function)
	table_in_out_function_t in_out_function;
	//! The table in-out final function (if this is an in-out function)
	table_in_out_function_final_t in_out_function_final;
	//! (Optional) statistics function
	//! Returns the statistics of a specified column
	table_statistics_t statistics;
	//! (Optional) statistics function fed a ColumnIndex instead of a column_t
	//! Returns the statistics of a specified column
	table_statistics_extended_t statistics_extended;
	//! (Optional) dependency function
	//! Sets up which catalog entries this table function depend on
	table_function_dependency_t dependency;
	//! (Optional) cardinality function
	//! Returns the expected cardinality of this scan
	table_function_cardinality_t cardinality;
	//! (Optional) returns profiling metrics for this table scan operator
	table_function_get_metrics_t get_metrics;
	//! (Optional) pushdown a set of arbitrary filter expressions, rather than only simple comparisons with a constant
	//! Any functions remaining in the expression list will be pushed as a regular filter after the scan
	table_function_pushdown_complex_filter_t pushdown_complex_filter;
	//! (Optional) whether or not this table function supports pushing down an expression into a TableFilter
	table_function_pushdown_expression_t pushdown_expression;
	//! (Optional) function for rendering the operator to a string in explain/profiling output (invoked pre-execution)
	table_function_to_string_t to_string;
	//! (Optional) reconstruct the source's SQL-visible state without retaining native objects.
	//! Must not execute the source or perform effects during export.
	table_function_to_sql_t to_sql = nullptr;
	//! (Optional) return how much of the table we have scanned up to this point (% of the data)
	table_function_progress_t table_scan_progress;
	//! (Optional) returns the partition info of the current scan operator
	table_function_get_partition_data_t get_partition_data;
	//! (Optional) returns extra bind info
	table_function_get_bind_info_t get_bind_info;
	//! (Optional) the table a bound call scans, e.g. for a scan of a catalog table
	table_function_get_table_entry_t get_table_entry;
	//! (Optional) pushes down projection expressions like len() in "SELECT len(str)" or
	//! casts like "col as UINTEGER" in "SELECT col::UINTEGER" to scanner.
	//! Returns true if pushdown was successful
	table_function_projection_expression_pushdown_t projection_expression_pushdown;
	//! (Optional) allows injecting a custom MultiFileReader implementation
	table_function_get_multi_file_reader_t get_multi_file_reader;
	//! (Optional) If this scanner supports filter pushdown, but not to all data types
	table_function_supports_pushdown_type_t supports_pushdown_type;
	//! (Optional) If this scanner supports projection pushdown of struct extracts
	table_function_supports_pushdown_extract_t supports_pushdown_extract;
	//! Optional repeatability capability. An absent callback is treated conservatively as unknown.
	table_function_is_repeatable_t is_repeatable;
	//! Get partition info of the table
	table_function_get_partition_info_t get_partition_info;
	//! (Optional) get a list of all the partition stats of the table
	table_function_get_partition_stats_t get_partition_stats;
	//! (Optional) returns a list of virtual columns emitted by the table function
	table_function_get_virtual_columns_t get_virtual_columns;
	//! (Optional) returns a list of row id columns
	table_function_get_row_id_columns get_row_id_columns;
	//! (Optional) sets the order to scan the row groups in
	table_function_set_scan_order set_scan_order;
	//! (Optional) restricts the scan to a specific subset of partitions (by index in get_partition_stats order)
	table_function_set_partitions_to_scan_t set_partitions_to_scan = nullptr;

	table_function_serialize_t serialize;
	table_function_deserialize_t deserialize;
	bool verify_serialization = true;

	//! Whether or not the table function supports projection pushdown. If not supported a projection will be added
	//! that filters out unused columns.
	bool projection_pushdown;
	//! Whether or not the table function supports filter pushdown. If not supported a filter will be added
	//! that applies the table filter directly.
	bool filter_pushdown;
	//! Whether join filter pushdown can push Bloom and prefix-range join filters into the table function. These
	//! filters reference engine memory; table functions that hand filters to another system set this to false and
	//! receive IN-list and min/max filters instead (possibly wrapped in an optional_filter).
	bool runtime_filter_pushdown = true;
	//! Whether or not the table function can immediately prune out filter columns that are unused in the remainder of
	//! the query plan, e.g., "SELECT i FROM tbl WHERE j = 42;" - j does not need to leave the table function at all
	bool filter_prune;
	//! Whether or not the table function supports sampling pushdown. If not supported a sample will be taken after the
	//! table function.
	bool sampling_pushdown;
	//! Whether or not the table function supports late materialization
	bool late_materialization;
	TableFunctionReturnType return_type;
	//! The return type used when this function is invoked through a CALL statement
	//! By default a CALL returns a query result - functions that only have side effects can use NOTHING instead
	StatementReturnType call_return_type = StatementReturnType::QUERY_RESULT;
	//! Additional function info, passed to the bind
	shared_ptr<TableFunctionInfo> function_info;
	//! The order preservation type of the table function
	OrderPreservationType order_preservation_type = OrderPreservationType::INSERTION_ORDER;

	//! When to call init_global
	//! By default init_global is called when the pipeline is ready for execution
	//! If this is set to `INITIALIZE_ON_SCHEDULE` the table function is initialized when the query is scheduled
	TableFunctionInitialization global_initialization = TableFunctionInitialization::INITIALIZE_ON_EXECUTE;

	//! How this table function manages parallelism
	TableFunctionParallelism parallelism = TableFunctionParallelism::SELF_MANAGED_PARALLELISM;

	DUCKDB_API bool operator==(const BaseTableFunction &rhs) const;
};

//! A table function as it is registered in the catalog: its behaviour, plus the signature declaring what it accepts
class TableFunction : public BaseTableFunction, public SimpleFunction { // NOLINT: work-around bug in clang-tidy
public:
	static constexpr const FunctionKind KIND = FunctionKind::TABLE;
	FunctionKind GetKind() const override {
		return KIND;
	}
	DUCKDB_API TableFunction();
	// Overloads taking table_function_t
	DUCKDB_API
	TableFunction(Identifier name, const vector<LogicalType> &arguments, table_function_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr);
	DUCKDB_API
	TableFunction(const vector<LogicalType> &arguments, table_function_t function, table_function_bind_t bind = nullptr,
	              table_function_init_global_t init_global = nullptr, table_function_init_local_t init_local = nullptr);
	// Overloads taking std::nullptr
	DUCKDB_API
	TableFunction(Identifier name, const vector<LogicalType> &arguments, std::nullptr_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr);
	DUCKDB_API
	TableFunction(const vector<LogicalType> &arguments, std::nullptr_t function, table_function_bind_t bind = nullptr,
	              table_function_init_global_t init_global = nullptr, table_function_init_local_t init_local = nullptr);
	// Overloads taking a braced list, so that "{}" does not also match FunctionSignature
	TableFunction(Identifier name, std::initializer_list<LogicalType> arguments, table_function_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr)
	    : TableFunction(std::move(name), vector<LogicalType>(arguments), function, bind, init_global, init_local) {
	}
	TableFunction(Identifier name, std::initializer_list<LogicalType> arguments, std::nullptr_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr)
	    : TableFunction(std::move(name), vector<LogicalType>(arguments), function, bind, init_global, init_local) {
	}
	TableFunction(std::initializer_list<LogicalType> arguments, table_function_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr)
	    : TableFunction(vector<LogicalType>(arguments), function, bind, init_global, init_local) {
	}
	TableFunction(std::initializer_list<LogicalType> arguments, std::nullptr_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr)
	    : TableFunction(vector<LogicalType>(arguments), function, bind, init_global, init_local) {
	}
	// Overloads taking proper signatures
	DUCKDB_API
	TableFunction(Identifier name, FunctionSignature signature, table_function_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr);
	DUCKDB_API
	TableFunction(Identifier name, FunctionSignature signature, std::nullptr_t function,
	              table_function_bind_t bind = nullptr, table_function_init_global_t init_global = nullptr,
	              table_function_init_local_t init_local = nullptr);

	DUCKDB_API
	TableFunction(FunctionSignature signature, table_function_t function, table_function_bind_t bind = nullptr,
	              table_function_init_global_t init_global = nullptr, table_function_init_local_t init_local = nullptr);

public:
	DUCKDB_API bool operator==(const TableFunction &rhs) const;
	DUCKDB_API bool operator!=(const TableFunction &rhs) const;
};

//! A table function as a bound call refers to it: its behaviour, plus the argument types the call was bound with.
//! Those are the types plan serialization records, so they must survive independently of the catalog's declaration
class BoundTableFunction : public BaseTableFunction, public BoundSimpleFunction {
public:
	DUCKDB_API BoundTableFunction();
	DUCKDB_API explicit BoundTableFunction(const TableFunction &function);
	DUCKDB_API explicit BoundTableFunction(shared_ptr<const TableFunction> function);

	//! The function this was bound from. For a function bound from a TableFunctionSet this is the set's own
	//! overload, so it compares equal by pointer across binds. Functions bound outside of a set are copied into a
	//! definition of their own. Only null in a moved-from bound function
	const shared_ptr<const TableFunction> &GetDefinition() const {
		return definition;
	}
	//! The signature this call was bound against
	const FunctionSignature &GetSignature() const {
		return definition->GetSignature();
	}
	//! The number of arguments that were received by the standard and positional-only parameters, they come first
	idx_t GetStandardArgumentCount() const {
		return BoundSimpleFunction::GetStandardArgumentCount(definition->GetSignature());
	}
	//! The number of arguments that were received by "*args", they directly follow the standard parameters
	idx_t GetVarArgsCount() const {
		return BoundSimpleFunction::GetVarArgsCount(definition->GetSignature());
	}
	//! The number of arguments that were received by the keyword-only parameters, they follow "*args"
	idx_t GetKeywordOnlyArgumentCount() const {
		return BoundSimpleFunction::GetKeywordOnlyArgumentCount(definition->GetSignature());
	}
	//! The number of arguments that were received by "**kwargs", they are the last arguments
	idx_t GetKwargsCount() const {
		return BoundSimpleFunction::GetKwargsCount(definition->GetSignature());
	}
	//! The kind of the parameter that received the argument at the given index
	FunctionParameterKind GetArgumentParameterKind(idx_t argument_index) const {
		return BoundSimpleFunction::GetArgumentParameterKind(definition->GetSignature(), argument_index);
	}
	//! Records the arguments of the call this was bound to, laid out as [standard | *args | keyword-only | **kwargs].
	//! Plan serialization writes them, so that deserialization selects the same overload the call did
	DUCKDB_API void SetCallArguments(const vector<Value> &parameters, const named_argument_map_t &named_parameters);

private:
	shared_ptr<const TableFunction> definition;

public:
	DUCKDB_API bool operator==(const BoundTableFunction &rhs) const;
	DUCKDB_API bool operator!=(const BoundTableFunction &rhs) const;
};

} // namespace duckdb
