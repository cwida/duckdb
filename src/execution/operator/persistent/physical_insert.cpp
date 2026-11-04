#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/common/types/constraint_conflict_info.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/persistent/collection_merger.hpp"
#include "duckdb/execution/row_id_deduplicator.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace duckdb {

PhysicalInsert::PhysicalInsert(PhysicalPlan &physical_plan, vector<LogicalType> types_p, DuckTableEntry &table,
                               vector<unique_ptr<BoundConstraint>> bound_constraints_p,
                               vector<unique_ptr<Expression>> set_expressions, vector<PhysicalIndex> set_columns,
                               vector<LogicalType> set_types, idx_t estimated_cardinality, bool return_chunk,
                               bool parallel, OnConflictAction action_type,
                               unique_ptr<Expression> on_conflict_condition_p,
                               unique_ptr<Expression> do_update_condition_p, unordered_set<column_t> conflict_target_p,
                               vector<column_t> columns_to_fetch_p, bool update_is_del_and_insert)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::INSERT, std::move(types_p), estimated_cardinality),
      insert_table(&table), insert_types(table.GetTypes()), bound_constraints(std::move(bound_constraints_p)),
      return_chunk(return_chunk), parallel(parallel), action_type(action_type),
      set_expressions(std::move(set_expressions)), set_columns(std::move(set_columns)), set_types(std::move(set_types)),
      on_conflict_condition(std::move(on_conflict_condition_p)), do_update_condition(std::move(do_update_condition_p)),
      conflict_target(std::move(conflict_target_p)), update_is_del_and_insert(update_is_del_and_insert) {
	if (action_type == OnConflictAction::THROW) {
		return;
	}

	D_ASSERT(this->set_expressions.size() == this->set_columns.size());

	// One or more columns are referenced from the existing table,
	// we use the 'insert_types' to figure out which types these columns have
	types_to_fetch = vector<LogicalType>(columns_to_fetch_p.size(), LogicalType::SQLNULL);
	for (idx_t i = 0; i < columns_to_fetch_p.size(); i++) {
		auto &id = columns_to_fetch_p[i];
		D_ASSERT(id < insert_types.size());
		types_to_fetch[i] = insert_types[id];
		columns_to_fetch.emplace_back(id);
	}
}

PhysicalInsert::PhysicalInsert(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
                               unique_ptr<BoundCreateTableInfo> info_p, idx_t estimated_cardinality, bool parallel)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::CREATE_TABLE_AS, op.types, estimated_cardinality),
      insert_table(nullptr), return_chunk(false), schema(&schema), info(std::move(info_p)), parallel(parallel),
      action_type(OnConflictAction::THROW), update_is_del_and_insert(false) {
	GetInsertInfo(*info, insert_types);
}

void PhysicalInsert::GetInsertInfo(const BoundCreateTableInfo &info, vector<LogicalType> &insert_types) {
	auto &create_info = info.base->Cast<CreateTableInfo>();
	for (auto &col : create_info.columns.Physical()) {
		insert_types.push_back(col.GetType());
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

InsertGlobalState::InsertGlobalState(ClientContext &context, const vector<LogicalType> &return_types,
                                     DuckTableEntry &table)
    : table(table), insert_count(0), return_collection(context, return_types) {
	table.GetStorage().BindIndexes(context);
}

InsertLocalState::InsertLocalState(ClientContext &context, const vector<LogicalType> &types,
                                   const vector<unique_ptr<BoundConstraint>> &bound_constraints,
                                   OnConflictAction action_type)
    : collection_index(DConstants::INVALID_INDEX), bound_constraints(bound_constraints) {
	auto &allocator = Allocator::Get(context);
	update_chunk.Initialize(allocator, types);
	append_chunk.Initialize(allocator, types);
	if (action_type == OnConflictAction::UPDATE) {
		updated_rows = make_uniq<RowIdDeduplicator>(context, vector<LogicalType> {LogicalType::ROW_TYPE});
	}
}

ConstraintState &InsertLocalState::GetConstraintState(DataTable &table, TableCatalogEntry &table_ref) {
	if (!constraint_state) {
		constraint_state = table.InitializeConstraintState(table_ref, bound_constraints);
	}
	return *constraint_state;
}

TableDeleteState &InsertLocalState::GetDeleteState(DataTable &table, TableCatalogEntry &table_ref,
                                                   ClientContext &context) {
	if (!delete_state) {
		delete_state = table.InitializeDelete(table_ref, context, bound_constraints);
	}
	return *delete_state;
}

unique_ptr<GlobalSinkState> PhysicalInsert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<DuckTableEntry> table;
	if (info) {
		// CREATE TABLE AS
		D_ASSERT(!insert_table);
		auto &catalog = schema->catalog;
		table = &catalog.CreateTable(catalog.GetCatalogTransaction(context), *schema, *info)->Cast<DuckTableEntry>();
	} else {
		D_ASSERT(insert_table);
		D_ASSERT(insert_table->IsDuckTable());
		table = insert_table;
	}
	auto result = make_uniq<InsertGlobalState>(context, GetTypes(), *table);
	return std::move(result);
}

unique_ptr<LocalSinkState> PhysicalInsert::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<InsertLocalState>(context.client, insert_types, bound_constraints, action_type);
}

bool AllConflictsMeetCondition(DataChunk &result) {
	result.Flatten();
	auto data = FlatVector::GetData<bool>(result.data[0]);
	for (idx_t i = 0; i < result.size(); i++) {
		if (!data[i]) {
			return false;
		}
	}
	return true;
}

void CheckOnConflictCondition(ExecutionContext &context, DataChunk &conflicts, const unique_ptr<Expression> &condition,
                              DataChunk &result) {
	ExpressionExecutor executor(context.client, *condition);
	result.Initialize(context.client, {LogicalType::BOOLEAN});
	executor.Execute(conflicts, result);
}

static void CombineExistingAndInsertTuples(DataChunk &result, DataChunk &scan_chunk, DataChunk &input_chunk,
                                           ClientContext &client, const PhysicalInsert &op) {
	auto &types_to_fetch = op.types_to_fetch;
	auto &insert_types = op.insert_types;

	if (types_to_fetch.empty()) {
		// We have not scanned the initial table, so we can just duplicate the initial chunk
		result.Initialize(client, input_chunk.GetTypes());
		result.Reference(input_chunk);
		return;
	}
	vector<LogicalType> combined_types;
	combined_types.reserve(insert_types.size() + types_to_fetch.size());
	combined_types.insert(combined_types.end(), insert_types.begin(), insert_types.end());
	combined_types.insert(combined_types.end(), types_to_fetch.begin(), types_to_fetch.end());

	result.Initialize(client, combined_types);
	result.Reset();
	// Add the VALUES list
	for (idx_t i = 0; i < insert_types.size(); i++) {
		idx_t col_idx = i;
		const auto &other_col = input_chunk.data[i];
		auto &this_col = result.data[col_idx];
		D_ASSERT(other_col.GetType() == this_col.GetType());
		this_col.Reference(other_col);
	}
	// Add the columns from the original conflicting tuples
	for (idx_t i = 0; i < types_to_fetch.size(); i++) {
		idx_t col_idx = i + insert_types.size();
		const auto &other_col = scan_chunk.data[i];
		auto &this_col = result.data[col_idx];
		D_ASSERT(other_col.GetType() == this_col.GetType());
		this_col.Reference(other_col);
	}
	// This is guaranteed by the requirement of a conflict target to have a condition or set expressions
	// Only when we have any sort of condition or SET expression that references the existing table is this possible
	// to not be true.
	// We can have a SET expression without a conflict target ONLY if there is only 1 Index on the table
	// In which case this also can't cause a discrepancy between existing tuple count and insert tuple count
	D_ASSERT(input_chunk.size() == scan_chunk.size());
}

static void CreateUpdateChunk(ExecutionContext &context, DataChunk &chunk, DuckTableEntry &table, Vector &row_ids,
                              DataChunk &update_chunk, const PhysicalInsert &op) {
	auto &do_update_condition = op.do_update_condition;
	auto &set_types = op.set_types;
	auto &set_expressions = op.set_expressions;

	// Check the optional condition for the DO UPDATE clause, to filter which rows will be updated
	if (do_update_condition) {
		DataChunk do_update_filter_result;
		do_update_filter_result.Initialize(context.client, {LogicalType::BOOLEAN});
		ExpressionExecutor where_executor(context.client, *do_update_condition);
		where_executor.Execute(chunk, do_update_filter_result);
		do_update_filter_result.Flatten();

		SelectionVector sel(chunk.size());
		idx_t count = 0;

		auto where_data = FlatVector::GetData<bool>(do_update_filter_result.data[0]);
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (where_data[i]) {
				sel.set_index(count, i);
				count++;
			}
		}
		if (count != chunk.size()) {
			// Filter any conflicts not meeting the condition.
			chunk.Slice(sel, count);
			row_ids.Slice(sel, count);
			row_ids.Flatten();
		}
	}

	if (chunk.size() == 0) {
		auto initialize = vector<bool>(set_types.size(), false);
		update_chunk.Initialize(context.client, set_types, initialize, chunk.size());
		update_chunk.SetChildCardinality(chunk.size());
		return;
	}

	// Execute the SET expressions.
	update_chunk.Initialize(context.client, set_types, chunk.size());
	ExpressionExecutor executor(context.client, set_expressions);
	executor.Execute(chunk, update_chunk);
}

template <bool GLOBAL>
static idx_t PerformOnConflictAction(InsertLocalState &lstate, InsertGlobalState &gstate, ExecutionContext &context,
                                     DataChunk &chunk, DuckTableEntry &table, Vector &row_ids,
                                     const PhysicalInsert &op) {
	// Early-out, if we do nothing on conflicting rows.
	if (op.action_type == OnConflictAction::NOTHING) {
		return 0;
	}

	auto &set_columns = op.set_columns;
	DataChunk update_chunk;
	CreateUpdateChunk(context, chunk, table, row_ids, update_chunk, op);
	auto &data_table = table.GetStorage();

	if (update_chunk.size() == 0) {
		// Nothing to do
		return update_chunk.size();
	}

	// Arrange the columns in the standard table order.
	DataChunk &append_chunk = lstate.append_chunk;
	for (idx_t i = 0; i < append_chunk.ColumnCount(); i++) {
		append_chunk.data[i].Reference(chunk.data[i]);
	}
	for (idx_t i = 0; i < set_columns.size(); i++) {
		append_chunk.data[set_columns[i].index].Reference(update_chunk.data[i]);
	}

	// Perform the UPDATE on the (global) storage.
	if (!op.update_is_del_and_insert) {
		if (!op.parallel && op.return_chunk) {
			gstate.return_collection.Append(append_chunk);
		}

		if (GLOBAL) {
			auto update_state = data_table.InitializeUpdate(table, context.client, op.bound_constraints);
			data_table.Update(*update_state, context.client, table, row_ids, set_columns, update_chunk);
			return update_chunk.size();
		}
		auto &local_storage = LocalStorage::Get(context.client, data_table.db);
		local_storage.Update(data_table, table, row_ids, set_columns, update_chunk);
		return update_chunk.size();
	}

	if (GLOBAL) {
		auto &delete_state = lstate.GetDeleteState(data_table, table, context.client);
		data_table.Delete(delete_state, context.client, table, row_ids, update_chunk.size());
	} else {
		auto &local_storage = LocalStorage::Get(context.client, data_table.db);
		local_storage.Delete(data_table, table, row_ids, update_chunk.size());
	}

	if (!op.parallel && op.return_chunk) {
		gstate.return_collection.Append(append_chunk);
	}
	data_table.LocalAppend(table, context.client, append_chunk, op.bound_constraints, row_ids, append_chunk);
	return update_chunk.size();
}

static void RegisterUpdatedRows(InsertLocalState &lstate, const Vector &row_ids, idx_t count) {
	// Register all row-ids; a distinct count below `count` means a row-id repeated, which we reject.
	D_ASSERT(lstate.updated_rows);
	auto distinct = lstate.updated_rows->Register(row_ids, count);
	if (distinct != count) {
		// This is following postgres behavior:
		throw InvalidInputException(
		    "ON CONFLICT DO UPDATE can not update the same row twice in the same command. Ensure that no rows "
		    "proposed for insertion within the same command have duplicate constrained values");
	}
}

static void CheckDistinctnessInternal(ValidityMask &valid, vector<reference<Vector>> &sort_keys, idx_t count,
                                      map<idx_t, vector<idx_t>> &result) {
	for (idx_t i = 0; i < count; i++) {
		bool has_conflicts = false;
		for (idx_t j = i + 1; j < count; j++) {
			if (!valid.RowIsValid(j)) {
				// Already a conflict
				continue;
			}
			bool matches = true;
			for (auto &sort_key : sort_keys) {
				auto &this_row = FlatVector::GetData<string_t>(sort_key.get())[i];
				auto &other_row = FlatVector::GetData<string_t>(sort_key.get())[j];
				if (this_row != other_row) {
					matches = false;
					break;
				}
			}
			if (matches) {
				auto &row_ids = result[i];
				has_conflicts = true;
				row_ids.push_back(j);
				valid.SetInvalid(j);
			}
		}
		if (has_conflicts) {
			valid.SetInvalid(i);
		}
	}
}

static void PrepareSortKeys(DataChunk &input, unordered_map<column_t, unique_ptr<Vector>> &sort_keys,
                            const unordered_set<column_t> &column_ids) {
	OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	for (auto &it : column_ids) {
		auto &sort_key = sort_keys[it];
		if (sort_key != nullptr) {
			continue;
		}
		const auto &column = input.data[it];
		sort_key = make_uniq<Vector>(LogicalType::BLOB);
		CreateSortKeyHelpers::CreateSortKey(column, order_modifiers, *sort_key);
	}
}

static map<idx_t, vector<idx_t>> CheckDistinctness(DataChunk &input, ConflictInfo &info,
                                                   const vector<unordered_set<column_t>> &matched_index_columns) {
	map<idx_t, vector<idx_t>> conflicts;
	unordered_map<idx_t, unique_ptr<Vector>> sort_keys;
	//! Register which rows have already caused a conflict
	ValidityMask valid(input.size());

	auto &column_ids = info.column_ids;
	if (column_ids.empty()) {
		for (const auto &index_column_ids : matched_index_columns) {
			PrepareSortKeys(input, sort_keys, index_column_ids);
			vector<reference<Vector>> columns;
			for (auto &idx : index_column_ids) {
				columns.push_back(*sort_keys[idx]);
			}
			CheckDistinctnessInternal(valid, columns, input.size(), conflicts);
		}
	} else {
		PrepareSortKeys(input, sort_keys, column_ids);
		vector<reference<Vector>> columns;
		for (auto &idx : column_ids) {
			columns.push_back(*sort_keys[idx]);
		}
		CheckDistinctnessInternal(valid, columns, input.size(), conflicts);
	}
	return conflicts;
}

template <bool GLOBAL>
static void VerifyOnConflictCondition(ExecutionContext &context, DataChunk &combined_chunk,
                                      const unique_ptr<Expression> &on_conflict_condition,
                                      ConstraintState &constraint_state, DataChunk &tuples, DataTable &data_table,
                                      LocalStorage &local_storage) {
	if (!on_conflict_condition) {
		return;
	}
	DataChunk conflict_condition_result;
	CheckOnConflictCondition(context, combined_chunk, on_conflict_condition, conflict_condition_result);
	bool conditions_met = AllConflictsMeetCondition(conflict_condition_result);
	if (conditions_met) {
		return;
	}

	// We need to throw.
	// Filter any passing tuples and verify again with those violating the constraint.
	SelectionVector sel(combined_chunk.size());
	idx_t count = 0;
	auto data = FlatVector::GetData<bool>(conflict_condition_result.data[0]);
	for (idx_t i = 0; i < combined_chunk.size(); i++) {
		if (!data[i]) {
			// The tuple does not meet the condition.
			sel.set_index(count, i);
			count++;
		}
	}
	combined_chunk.Slice(sel, count);

	// Verify and throw.
	if (GLOBAL) {
		data_table.VerifyAppendConstraints(constraint_state, context.client, combined_chunk, nullptr, nullptr);
		throw InternalException("VerifyAppendConstraints was expected to throw but didn't");
	}

	auto &indexes = local_storage.GetIndexes(context.client, data_table);
	auto storage = local_storage.GetStorage(data_table);
	indexes.VerifyUniqueIndexes(storage ? &storage->delete_indexes : nullptr, tuples, nullptr);
	throw InternalException("VerifyUniqueIndexes was expected to throw but didn't");
}

template <bool GLOBAL>
static idx_t HandleInsertConflicts(DuckTableEntry &table, ExecutionContext &context, InsertLocalState &lstate,
                                   InsertGlobalState &gstate, DataChunk &tuples, const PhysicalInsert &op) {
	auto &types_to_fetch = op.types_to_fetch;
	auto &on_conflict_condition = op.on_conflict_condition;
	auto &conflict_target = op.conflict_target;
	auto &columns_to_fetch = op.columns_to_fetch;
	auto &data_table = table.GetStorage();
	auto &local_storage = LocalStorage::Get(context.client, data_table.db);

	ConflictInfo conflict_info(conflict_target);
	ConflictManager conflict_manager(VerifyExistenceType::APPEND, tuples.size(), &conflict_info);
	auto storage = local_storage.GetStorage(data_table);
	if (GLOBAL) {
		auto &constraint_state = lstate.GetConstraintState(data_table, table);
		data_table.VerifyAppendConstraints(constraint_state, context.client, tuples, storage, &conflict_manager);
	} else {
		auto &indexes = local_storage.GetIndexes(context.client, data_table);
		indexes.VerifyUniqueIndexes(storage ? &storage->delete_indexes : nullptr, tuples, &conflict_manager);
	}

	if (!conflict_manager.HasConflicts()) {
		// No conflicts, i.e., no updates.
		return 0;
	}

	if (GLOBAL) {
		auto &transaction = DuckTransaction::Get(context.client, table.catalog);
		conflict_manager.FinalizeGlobal(transaction, data_table);
	} else {
		conflict_manager.FinalizeLocal(data_table, local_storage);
	}
	auto &row_ids = conflict_manager.GetRowIds();
	auto conflict_count = conflict_manager.ConflictCount();

	// Contains the original values causing the conflicts.
	DataChunk scan_chunk;
	// ColumnFetchState pins the fetched rows.
	unique_ptr<ColumnFetchState> fetch_state;

	if (!types_to_fetch.empty()) {
		D_ASSERT(scan_chunk.size() == 0);
		// We scan the existing table for the conflicting tuples, if we
		// need them for the conditions, or SET expressions.
		scan_chunk.Initialize(context.client, types_to_fetch);
		fetch_state = make_uniq<ColumnFetchState>();

		if (GLOBAL) {
			auto &transaction = DuckTransaction::Get(context.client, table.catalog);
			data_table.Fetch(transaction, scan_chunk, columns_to_fetch, row_ids, conflict_count, *fetch_state);
		} else {
			local_storage.FetchChunk(data_table, row_ids, conflict_count, columns_to_fetch, scan_chunk, *fetch_state);
		}
	}

	// Only contains the conflicting values.
	DataChunk conflict_chunk;
	conflict_chunk.InitializeEmpty(tuples.GetTypes());
	conflict_chunk.Reference(tuples);
	conflict_chunk.Slice(conflict_manager.GetInvertedSel(), conflict_count);

	// Contains the conflict chunk and the scanned chunk (wide).
	DataChunk combined_chunk;

	// Splice the Input chunk and the fetched chunk together
	CombineExistingAndInsertTuples(combined_chunk, scan_chunk, conflict_chunk, context.client, op);

	auto &constraint_state = lstate.GetConstraintState(data_table, table);
	VerifyOnConflictCondition<GLOBAL>(context, combined_chunk, on_conflict_condition, constraint_state, tuples,
	                                  data_table, local_storage);

	if (RefersToSameObject(tuples, lstate.update_chunk)) {
		D_ASSERT(op.action_type == OnConflictAction::UPDATE);
		// Reject updating the same target row more than once.
		RegisterUpdatedRows(lstate, row_ids, conflict_count);
	}
	auto affected_tuples = PerformOnConflictAction<GLOBAL>(lstate, gstate, context, combined_chunk, table, row_ids, op);

	// Remove the conflicting tuples from the insert chunk
	// We can use only the primary data because the secondary data has the same indexes in the chunk.
	SelectionVector sel_vec(tuples.size());
	auto &inverted_sel = conflict_manager.GetInvertedSel();
	auto new_size = SelectionVector::Inverted(inverted_sel, sel_vec, conflict_count, tuples.size());
	tuples.Slice(sel_vec, new_size);

	return affected_tuples;
}

idx_t PhysicalInsert::OnConflictHandling(DuckTableEntry &table, ExecutionContext &context, InsertGlobalState &gstate,
                                         InsertLocalState &lstate, DataChunk &insert_chunk) const {
	auto &data_table = table.GetStorage();
	auto &local_storage = LocalStorage::Get(context.client, data_table.db);

	if (action_type == OnConflictAction::THROW) {
		auto &constraint_state = lstate.GetConstraintState(data_table, table);
		auto storage = local_storage.GetStorage(data_table);
		data_table.VerifyAppendConstraints(constraint_state, context.client, insert_chunk, storage, nullptr);
		return 0;
	}

	ConflictInfo conflict_info(conflict_target);
	vector<unordered_set<column_t>> matching_index_columns;

	if (conflict_info.column_ids.empty()) {
		const auto &global_indexes = data_table.GetDataTableInfo()->GetIndexes();
		// We care about every index that applies to the table if no ON CONFLICT (...) target is given
		matching_index_columns = global_indexes.GetConflictTargetColumns(conflict_info);

		const auto &local_indexes = local_storage.GetIndexes(context.client, data_table);
		auto local_index_columns = local_indexes.GetConflictTargetColumns(conflict_info);
		for (auto &column_set : local_index_columns) {
			matching_index_columns.push_back(std::move(column_set));
		}
	}

	auto inner_conflicts = CheckDistinctness(insert_chunk, conflict_info, matching_index_columns);
	idx_t count = insert_chunk.size();
	if (!inner_conflicts.empty()) {
		// We have at least one inner conflict to filter out.
		SelectionVector sel(count);
		idx_t sel_count = 0;

		ValidityMask not_a_conflict(count);
		set<idx_t> last_occurrences_of_conflict;
		for (idx_t i = 0; i < count; i++) {
			auto it = inner_conflicts.find(i);
			if (it != inner_conflicts.end()) {
				auto &conflicts = it->second;
				auto conflict_it = conflicts.begin();
				for (; conflict_it != conflicts.end();) {
					auto &idx = *conflict_it;
					not_a_conflict.SetInvalid(idx);
					conflict_it++;
					if (conflict_it == conflicts.end()) {
						last_occurrences_of_conflict.insert(idx);
					}
				}
			}
			if (not_a_conflict.RowIsValid(i)) {
				sel.set_index(sel_count, i);
				sel_count++;
			}
		}
		if (action_type == OnConflictAction::UPDATE) {
			if (do_update_condition) {
				//! See https://github.com/duckdblabs/duckdb-internal/issues/4090 for context
				throw NotImplementedException("Inner conflicts detected with a conditional DO UPDATE on-conflict "
				                              "action, not fully implemented yet");
			}

			SelectionVector last_occurrences(last_occurrences_of_conflict.size());
			idx_t last_occurrences_count = 0;
			for (auto &idx : last_occurrences_of_conflict) {
				last_occurrences.set_index(last_occurrences_count, idx);
				last_occurrences_count++;
			}

			lstate.update_chunk.Reference(insert_chunk);
			lstate.update_chunk.Slice(last_occurrences, last_occurrences_count);
		}

		insert_chunk.Slice(sel, sel_count);
	}

	// Check whether any conflicts arise, and if they all meet the conflict_target + condition
	// If that's not the case - We throw the first error
	idx_t updated_tuples = 0;
	updated_tuples += HandleInsertConflicts<true>(table, context, lstate, gstate, insert_chunk, *this);
	// Also check the transaction-local storage+ART so we can detect conflicts within this transaction
	updated_tuples += HandleInsertConflicts<false>(table, context, lstate, gstate, insert_chunk, *this);

	return updated_tuples;
}

SinkResultType PhysicalInsert::Sink(ExecutionContext &context, DataChunk &insert_chunk,
                                    OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<InsertGlobalState>();
	auto &lstate = input.local_state.Cast<InsertLocalState>();

	auto &table = gstate.table;
	auto &storage = table.GetStorage();
	insert_chunk.Flatten();

	if (!parallel) {
		idx_t updated_tuples = OnConflictHandling(table, context, gstate, lstate, insert_chunk);

		gstate.insert_count += insert_chunk.size();
		gstate.insert_count += updated_tuples;
		if (return_chunk) {
			gstate.return_collection.Append(insert_chunk);
		}
		// When action_type is throw, we already verify constraints in `OnConflictHandling`
		storage.LocalAppend(table, context.client, insert_chunk, bound_constraints,
		                    action_type == OnConflictAction::THROW);
		if (action_type == OnConflictAction::UPDATE && lstate.update_chunk.size() != 0) {
			(void)HandleInsertConflicts<true>(table, context, lstate, gstate, lstate.update_chunk, *this);
			(void)HandleInsertConflicts<false>(table, context, lstate, gstate, lstate.update_chunk, *this);
			// All of the tuples should have been turned into an update, leaving the chunk empty afterwards
			D_ASSERT(lstate.update_chunk.size() == 0);
		}
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Parallel append.
	D_ASSERT(!return_chunk);
	auto &data_table = gstate.table.GetStorage();
	if (!lstate.collection_index.IsValid()) {
		lock_guard<mutex> l(gstate.lock);
		lstate.optimistic_writer = make_uniq<OptimisticDataWriter>(context.client, data_table);
		// Create the local row group collection.
		auto optimistic_collection = lstate.optimistic_writer->CreateCollection(storage, insert_types);
		auto &collection = *optimistic_collection->collection;
		collection.InitializeEmpty();
		collection.InitializeAppend(lstate.local_append_state);

		lstate.collection_index =
		    data_table.CreateOptimisticCollection(context.client, std::move(optimistic_collection));
	}

	OnConflictHandling(table, context, gstate, lstate, insert_chunk);
	D_ASSERT(action_type != OnConflictAction::UPDATE);

	auto &optimistic_collection = data_table.GetOptimisticCollection(context.client, lstate.collection_index);
	auto &collection = *optimistic_collection.collection;
	auto flushed_row_group_idx = collection.Append(insert_chunk, lstate.local_append_state);
	if (flushed_row_group_idx.IsValid()) {
		lstate.optimistic_writer->WriteNewRowGroup(optimistic_collection, flushed_row_group_idx.GetIndex());
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalInsert::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<InsertGlobalState>();
	auto &lstate = input.local_state.Cast<InsertLocalState>();
	auto &client_profiler = QueryProfiler::Get(context.client);
	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);

	if (!parallel || !lstate.collection_index.IsValid()) {
		return SinkCombineResultType::FINISHED;
	}

	auto &table = gstate.table;
	auto &storage = table.GetStorage();
	const idx_t row_group_size = storage.GetRowGroupSize();

	// parallel append: finalize the append
	// the transaction id keeps the rows invisible to other transactions until the commit
	TransactionData tdata(DuckTransaction::Get(context.client, table.catalog));
	auto &data_table = gstate.table.GetStorage();
	auto &optimistic_collection = data_table.GetOptimisticCollection(context.client, lstate.collection_index);
	auto &collection = *optimistic_collection.collection;
	collection.FinalizeAppend(tdata, lstate.local_append_state);

	const auto append_count = collection.GetTotalRows();

	if (append_count >= row_group_size) {
		// flush thread-local optimistic data
		lstate.optimistic_writer->WriteUnflushedRowGroups(optimistic_collection);
		lstate.optimistic_writer->FinalFlush();
	}

	lock_guard<mutex> lock(gstate.lock);
	gstate.insert_count += append_count;
	if (append_count < row_group_size) {
		// we have few rows - defer merging to Finalize, where the leftovers of all threads are compacted together
		gstate.unmerged_collections.push_back(lstate.collection_index);
	} else {
		// we have written rows to disk optimistically - merge directly into the transaction-local storage
		gstate.table.GetStorage().LocalMerge(context.client, gstate.table, optimistic_collection);
		auto &optimistic_writer = gstate.table.GetStorage().GetOptimisticWriter(context.client);
		optimistic_writer.Merge(*lstate.optimistic_writer);
	}

	return SinkCombineResultType::FINISHED;
}

class MergeCollectionsTask : public ExecutorTask {
public:
	MergeCollectionsTask(ClientContext &context, shared_ptr<Event> event_p, CollectionMerger &merger,
	                     OptimisticDataWriter &writer, PhysicalIndex &result, const PhysicalOperator &op)
	    : ExecutorTask(context, std::move(event_p), op), merger(merger), writer(writer), result(result) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		result = merger.Flush(writer);
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "MergeCollectionsTask";
	}

private:
	CollectionMerger &merger;
	OptimisticDataWriter &writer;
	PhysicalIndex &result;
};

class MergeCollectionsEvent : public BasePipelineEvent {
public:
	MergeCollectionsEvent(Pipeline &pipeline_p, ClientContext &context, const PhysicalInsert &op, DuckTableEntry &table,
	                      vector<unique_ptr<CollectionMerger>> mergers_p)
	    : BasePipelineEvent(pipeline_p), context(context), op(op), table(table), mergers(std::move(mergers_p)),
	      merged_collections(mergers.size(), PhysicalIndex(DConstants::INVALID_INDEX)) {
	}

public:
	void Schedule() override {
		auto &data_table = table.GetStorage();
		vector<shared_ptr<Task>> tasks;
		for (idx_t i = 0; i < mergers.size(); i++) {
			writers.push_back(make_uniq<OptimisticDataWriter>(context, data_table));
			tasks.push_back(make_uniq<MergeCollectionsTask>(context, shared_from_this(), *mergers[i], *writers[i],
			                                                merged_collections[i], op));
		}
		SetTasks(std::move(tasks));
	}

	void FinishEvent() override {
		// merge the compacted collections into the transaction-local storage
		auto &data_table = table.GetStorage();
		auto &optimistic_writer = data_table.GetOptimisticWriter(context);
		for (idx_t i = 0; i < mergers.size(); i++) {
			auto &collection = data_table.GetOptimisticCollection(context, merged_collections[i]);
			data_table.LocalMerge(context, table, collection);
			data_table.ResetOptimisticCollection(context, merged_collections[i]);
			optimistic_writer.Merge(*writers[i]);
		}
		optimistic_writer.FinalFlush();
	}

private:
	ClientContext &context;
	const PhysicalInsert &op;
	DuckTableEntry &table;
	vector<unique_ptr<CollectionMerger>> mergers;
	vector<unique_ptr<OptimisticDataWriter>> writers;
	vector<PhysicalIndex> merged_collections;
};

SinkFinalizeType PhysicalInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<InsertGlobalState>();
	if (gstate.unmerged_collections.empty()) {
		return SinkFinalizeType::READY;
	}
	auto &table = gstate.table;
	auto &data_table = table.GetStorage();
	const idx_t row_group_size = data_table.GetRowGroupSize();

	if (gstate.insert_count < row_group_size) {
		// we are inserting a small amount of data - append directly to the transaction-local storage
		LocalAppendState append_state;
		data_table.InitializeLocalAppend(append_state, table, context, bound_constraints);
		auto &transaction = DuckTransaction::Get(context, table.catalog);
		for (auto &collection_index : gstate.unmerged_collections) {
			auto &optimistic_collection = data_table.GetOptimisticCollection(context, collection_index);
			for (auto &insert_chunk : optimistic_collection.collection->Chunks(transaction)) {
				data_table.LocalAppend(append_state, table, context, insert_chunk, false);
			}
			data_table.ResetOptimisticCollection(context, collection_index);
		}
		data_table.FinalizeLocalAppend(append_state);
		gstate.unmerged_collections.clear();
		return SinkFinalizeType::READY;
	}

	// group the leftover thread-local collections into row-group-sized merge sets
	vector<unique_ptr<CollectionMerger>> mergers;
	unique_ptr<CollectionMerger> current_merger;
	idx_t current_rows = 0;
	for (auto &collection_index : gstate.unmerged_collections) {
		if (!current_merger) {
			current_merger = make_uniq<CollectionMerger>(context, data_table);
		}
		current_merger->AddCollection(collection_index, RowGroupBatchType::NOT_FLUSHED);
		current_rows += data_table.GetOptimisticCollection(context, collection_index).collection->GetTotalRows();
		if (current_rows >= row_group_size) {
			mergers.push_back(std::move(current_merger));
			current_merger.reset();
			current_rows = 0;
		}
	}
	if (current_merger) {
		mergers.push_back(std::move(current_merger));
	}
	gstate.unmerged_collections.clear();

	// compact the merge sets in parallel through a new pipeline event
	auto merge_event = make_shared_ptr<MergeCollectionsEvent>(pipeline, context, *this, table, std::move(mergers));
	event.InsertEvent(std::move(merge_event));
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class InsertSourceState : public GlobalSourceState {
public:
	explicit InsertSourceState(const PhysicalInsert &op) : total_rows(1), rows_scanned(0) {
		if (op.return_chunk) {
			D_ASSERT(op.sink_state);
			auto &g = op.sink_state->Cast<InsertGlobalState>();
			g.return_collection.InitializeScan(scan_state);
			total_rows = g.return_collection.Count();
		}
	}

	ColumnDataScanState scan_state;
	idx_t total_rows;
	atomic<idx_t> rows_scanned;
};

unique_ptr<GlobalSourceState> PhysicalInsert::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<InsertSourceState>(*this);
}

ProgressData PhysicalInsert::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<InsertSourceState>();
	ProgressData progress;
	progress.total = double(MaxValue<idx_t>(state.total_rows, 1));
	progress.done = state.total_rows == 0 ? 1.0 : double(state.rows_scanned.load(std::memory_order_relaxed));
	return progress;
}

SourceResultType PhysicalInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<InsertSourceState>();
	auto &insert_gstate = sink_state->Cast<InsertGlobalState>();
	if (!return_chunk) {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(insert_gstate.insert_count)));
		state.rows_scanned.store(1, std::memory_order_relaxed);
		return SourceResultType::FINISHED;
	}

	insert_gstate.return_collection.Scan(state.scan_state, chunk);
	state.rows_scanned.fetch_add(chunk.size(), std::memory_order_relaxed);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
