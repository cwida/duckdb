#include "duckdb/common/arrow/arrow.hpp"

#include "duckdb.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/function/table/arrow/arrow_type_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

void ArrowTableFunction::PopulateArrowTableSchema(ClientContext &context, ArrowTableSchema &arrow_table,
                                                  const ArrowSchema &arrow_schema) {
	vector<string> names;
	// We first gather the column names and deduplicate them
	for (idx_t col_idx = 0; col_idx < static_cast<idx_t>(arrow_schema.n_children); col_idx++) {
		const auto &schema = *arrow_schema.children[col_idx];
		if (!schema.release) {
			throw InvalidInputException("arrow_scan: released schema passed");
		}
		auto name = string(schema.name);
		if (name.empty()) {
			name = string("v") + to_string(col_idx);
		}
		names.push_back(name);
	}
	QueryResult::DeduplicateColumns(names);

	// We do a second iteration to figure out the arrow types and already set their deduplicated names
	for (idx_t col_idx = 0; col_idx < static_cast<idx_t>(arrow_schema.n_children); col_idx++) {
		auto &schema = *arrow_schema.children[col_idx];
		if (!schema.release) {
			throw InvalidInputException("arrow_scan: released schema passed");
		}
		auto arrow_type = ArrowType::GetArrowLogicalType(context, schema);
		arrow_table.AddColumn(col_idx, std::move(arrow_type), names[col_idx]);
	}
}

unique_ptr<FunctionData> ArrowTableFunction::ArrowScanBindDumb(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<Identifier> &names) {
	auto bind_data = ArrowScanBind(context, input, return_types, names);
	auto &arrow_bind_data = bind_data->Cast<ArrowScanFunctionData>();
	arrow_bind_data.projection_pushdown_enabled = false;
	return bind_data;
}

unique_ptr<FunctionData> ArrowTableFunction::ArrowScanBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types,
                                                           vector<Identifier> &names) {
	if (!input.ref.bind_info) {
		throw BinderException("arrow_scan requires an ArrowScanFactory bind input");
	}
	DynamicCastCheck<ArrowScanFactory>(input.ref.bind_info.get());
	auto factory = shared_ptr_cast<TableFunctionInfo, ArrowScanFactory>(input.ref.bind_info);
	auto res = make_uniq<ArrowScanFunctionData>(std::move(factory));
	res->factory->GetSchema(res->schema_root.arrow_schema);
	PopulateArrowTableSchema(context, res->arrow_table, res->schema_root.arrow_schema);
	names = StringsToIdentifiers(res->arrow_table.GetNames());
	return_types = res->arrow_table.GetTypes();
	res->all_types = return_types;
	if (return_types.empty()) {
		throw InvalidInputException("Provided table/dataframe must have at least one column");
	}
	return std::move(res);
}

unique_ptr<ArrowArrayStreamWrapper> ProduceArrowScan(const ArrowScanFunctionData &function,
                                                     const vector<column_t> &column_ids, TableFilterSet *filters) {
	//! Generate Projection Pushdown Vector
	ArrowStreamParameters parameters;
	D_ASSERT(!column_ids.empty());
	auto &arrow_types = function.arrow_table.GetColumns();
	for (idx_t idx = 0; idx < column_ids.size(); idx++) {
		auto col_idx = column_ids[idx];
		if (col_idx != COLUMN_IDENTIFIER_ROW_ID) {
			auto &schema = *function.schema_root.arrow_schema.children[col_idx];
			arrow_types.at(col_idx)->ThrowIfInvalid();
			parameters.projected_columns.projection_map[idx] = schema.name;
			parameters.projected_columns.columns.emplace_back(schema.name);
			parameters.projected_columns.filter_to_col[idx] = col_idx;
		}
	}
	parameters.filters = filters;
	return function.factory->ProduceStream(parameters);
}

idx_t ArrowTableFunction::ArrowScanMaxThreads(ClientContext &context, const FunctionData *bind_data_p) {
	return context.db->NumberOfThreads();
}

bool ArrowTableFunction::ArrowScanParallelStateNext(ClientContext &context, const FunctionData *bind_data_p,
                                                    ArrowScanLocalState &state, ArrowScanGlobalState &parallel_state) {
	lock_guard<mutex> parallel_lock(parallel_state.main_mutex);
	if (parallel_state.done) {
		return false;
	}
	state.Reset();
	state.batch_index = ++parallel_state.batch_index;

	auto current_chunk = parallel_state.stream->GetNextChunk();
	while (current_chunk->arrow_array.length == 0 && current_chunk->arrow_array.release) {
		current_chunk = parallel_state.stream->GetNextChunk();
	}
	state.chunk = std::move(current_chunk);
	//! have we run out of chunks? we are done
	if (!state.chunk->arrow_array.release) {
		parallel_state.done = true;
		return false;
	}
	return true;
}

unique_ptr<GlobalTableFunctionState> ArrowTableFunction::ArrowScanInitGlobal(ClientContext &context,
                                                                             TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ArrowScanFunctionData>();
	auto result = make_uniq<ArrowScanGlobalState>();
	result->stream = ProduceArrowScan(bind_data, input.column_ids, input.filters.get());
	result->max_threads = ArrowScanMaxThreads(context, input.bind_data.get());
	if (!input.projection_ids.empty()) {
		result->projection_ids = input.projection_ids;
		for (const auto &col_idx : input.column_ids) {
			if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				result->scanned_types.emplace_back(LogicalType::ROW_TYPE);
			} else {
				result->scanned_types.push_back(bind_data.all_types[col_idx]);
			}
		}
	}
	return std::move(result);
}

unique_ptr<LocalTableFunctionState>
ArrowTableFunction::ArrowScanInitLocalInternal(ClientContext &context, TableFunctionInitInput &input,
                                               GlobalTableFunctionState *global_state_p) {
	auto &global_state = global_state_p->Cast<ArrowScanGlobalState>();
	auto current_chunk = make_uniq<ArrowArrayWrapper>();
	auto result = make_uniq<ArrowScanLocalState>(std::move(current_chunk), context);
	result->column_ids = input.column_ids;
	result->filters = input.filters.get();
	auto &bind_data = input.bind_data->Cast<ArrowScanFunctionData>();
	if (!bind_data.projection_pushdown_enabled) {
		result->column_ids.clear();
	} else if (!input.projection_ids.empty()) {
		auto &asgs = global_state_p->Cast<ArrowScanGlobalState>();
		result->all_columns.Initialize(context, asgs.scanned_types);
	}
	if (!ArrowScanParallelStateNext(context, input.bind_data.get(), *result, global_state)) {
		return nullptr;
	}
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> ArrowTableFunction::ArrowScanInitLocal(ExecutionContext &context,
                                                                           TableFunctionInitInput &input,
                                                                           GlobalTableFunctionState *global_state_p) {
	return ArrowScanInitLocalInternal(context.client, input, global_state_p);
}

void ArrowTableFunction::ArrowScanFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	if (!data_p.local_state) {
		return;
	}
	auto &data = data_p.bind_data->CastNoConst<ArrowScanFunctionData>(); // FIXME
	auto &state = data_p.local_state->Cast<ArrowScanLocalState>();
	auto &global_state = data_p.global_state->Cast<ArrowScanGlobalState>();

	//! Out of tuples in this chunk
	if (state.chunk_offset >= static_cast<idx_t>(state.chunk->arrow_array.length)) {
		if (!ArrowScanParallelStateNext(context, data_p.bind_data.get(), state, global_state)) {
			return;
		}
	}
	auto output_size =
	    MinValue<idx_t>(STANDARD_VECTOR_SIZE, NumericCast<idx_t>(state.chunk->arrow_array.length) - state.chunk_offset);
	data.lines_read += output_size;
	if (global_state.CanRemoveFilterColumns()) {
		state.all_columns.Reset();
		state.all_columns.SetChildCardinality(output_size);
		ArrowToDuckDB(state, data.arrow_table.GetColumns(), state.all_columns);
		output.ReferenceColumns(state.all_columns, global_state.projection_ids);
	} else {
		output.SetChildCardinality(output_size);
		ArrowToDuckDB(state, data.arrow_table.GetColumns(), output);
	}

	output.Verify(context);
	state.chunk_offset += output.size();
}

unique_ptr<NodeStatistics> ArrowTableFunction::ArrowScanCardinality(ClientContext &context, const FunctionData *data) {
	return make_uniq<NodeStatistics>();
}

OperatorPartitionData ArrowTableFunction::ArrowGetPartitionData(ClientContext &context,
                                                                TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("ArrowTableFunction::GetPartitionData: partition columns not supported");
	}
	auto &state = input.local_state->Cast<ArrowScanLocalState>();
	return OperatorPartitionData(state.batch_index);
}

static bool CanPushdown(const ArrowType &type) {
	if (type.RunEndEncoded()) {
		return false;
	}
	if (type.HasDictionary()) {
		return CanPushdown(type.GetDictionary());
	}
	auto duck_type = type.GetDuckType();
	switch (duck_type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return true;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		// PyArrow doesn't support binary and string view filters yet
		return type.GetTypeInfo<ArrowStringInfo>().GetSizeType() != ArrowVariableSizeType::VIEW;
	case LogicalTypeId::DECIMAL: {
		switch (duck_type.InternalType()) {
		case PhysicalType::INT16:
		case PhysicalType::INT32:
		case PhysicalType::INT64:
			return false;
		case PhysicalType::INT128:
			return true;
		default:
			return false;
		}
	}
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		const auto &struct_info = type.GetTypeInfo<ArrowStructInfo>();
		for (idx_t i = 0; i < struct_info.ChildCount(); i++) {
			if (!CanPushdown(struct_info.GetChild(i))) {
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

// PyArrow's array_filter kernel has no implementation for string view, binary view and run-end encoded arrays.
// It filters a dictionary by its indices, so a dictionary of view values is fine here.
static bool HasUnfilterableType(const ArrowType &type) {
	if (type.RunEndEncoded()) {
		return true;
	}
	auto duck_type = type.GetDuckType();
	switch (duck_type.id()) {
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		return type.GetTypeInfo<ArrowStringInfo>().GetSizeType() == ArrowVariableSizeType::VIEW;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::UNION: {
		const auto &struct_info = type.GetTypeInfo<ArrowStructInfo>();
		for (idx_t i = 0; i < struct_info.ChildCount(); i++) {
			if (HasUnfilterableType(struct_info.GetChild(i))) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP: {
		const auto &list_info = type.GetTypeInfo<ArrowListInfo>();
		return HasUnfilterableType(list_info.GetChild());
	}
	case LogicalTypeId::ARRAY: {
		const auto &array_info = type.GetTypeInfo<ArrowArrayInfo>();
		return HasUnfilterableType(array_info.GetChild());
	}
	default:
		return false;
	}
}

static bool TableHasUnfilterableTypes(const arrow_column_map_t &column_info) {
	for (const auto &col : column_info) {
		if (HasUnfilterableType(*col.second)) {
			return true;
		}
	}
	return false;
}

bool ArrowTableFunction::ArrowPushdownType(const FunctionData &bind_data, idx_t col_idx) {
	auto &arrow_bind_data = bind_data.Cast<ArrowScanFunctionData>();
	const auto &column_info = arrow_bind_data.arrow_table.GetColumns();
	// The producer applies a pushed filter to every column of a record batch, so one column that cannot be
	// filtered turns pushdown off for the entire table.
	if (TableHasUnfilterableTypes(column_info)) {
		return false;
	}
	const auto column_type = column_info.at(col_idx);
	return CanPushdown(*column_type);
}

void ArrowTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction arrow("arrow_scan", {}, ArrowScanFunction, ArrowScanBind, ArrowScanInitGlobal, ArrowScanInitLocal);
	arrow.cardinality = ArrowScanCardinality;
	arrow.get_partition_data = ArrowGetPartitionData;
	arrow.projection_pushdown = true;
	arrow.filter_pushdown = true;
	arrow.filter_prune = true;
	arrow.supports_pushdown_type = ArrowPushdownType;
	arrow.parallelism = TableFunctionParallelism::SEQUENTIAL;
	set.AddFunction(arrow);

	TableFunction arrow_dumb("arrow_scan_dumb", {}, ArrowScanFunction, ArrowScanBindDumb, ArrowScanInitGlobal,
	                         ArrowScanInitLocal);
	arrow_dumb.cardinality = ArrowScanCardinality;
	arrow_dumb.get_partition_data = ArrowGetPartitionData;
	arrow_dumb.projection_pushdown = false;
	arrow_dumb.filter_pushdown = false;
	arrow_dumb.filter_prune = false;
	arrow_dumb.parallelism = TableFunctionParallelism::SEQUENTIAL;
	set.AddFunction(arrow_dumb);
}

void BuiltinFunctions::RegisterArrowFunctions() {
	ArrowTableFunction::RegisterFunction(*this);
}
} // namespace duckdb
