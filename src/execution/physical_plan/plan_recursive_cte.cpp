#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/perfect_aggregate_hashtable.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalRecursiveCTE &op) {
	D_ASSERT(op.children.size() == 2);

	// Create the working_table that the PhysicalRecursiveCTE will use for evaluation.
	const auto &working_types = !op.key_targets.empty() && !op.union_all ? op.types : op.internal_types;
	auto working_table = make_shared_ptr<ColumnDataCollection>(context, working_types);

	// Add the ColumnDataCollection to the context of this PhysicalPlanGenerator
	recursive_cte_tables[op.table_index] = working_table;

	auto &left = CreatePlan(*op.children[0]);

	// If the logical operator has no key targets, then we create a normal recursive CTE operator.
	if (op.key_targets.empty()) {
		auto recurring_table = make_shared_ptr<ColumnDataCollection>(context, op.types);
		recurring_cte_tables[op.table_index] = recurring_table;
		planning_recursive_cte_depth++;
		auto &right = CreatePlan(*op.children[1]);
		planning_recursive_cte_depth--;
		auto &cte = Make<PhysicalRecursiveCTE>(op.ctename, op.table_index, op.types,
		                                       op.union_all ? RecursiveCTEMode::UNION_ALL : RecursiveCTEMode::UNION,
		                                       left, right, op.estimated_cardinality);
		auto &cast_cte = cte.Cast<PhysicalRecursiveCTE>();
		cast_cte.ref_recurring = op.ref_recurring;
		cast_cte.recurring_table = recurring_table;
		cast_cte.distinct_types = op.types;
		cast_cte.working_table = working_table;
		cast_cte.non_repeatable_operators = non_repeatable_operators;
		return cte;
	}

	vector<LogicalType> distinct_types, payload_types;
	vector<idx_t> distinct_idx, payload_idx;
	vector<unique_ptr<Expression>> payload_aggregates;
	vector<unique_ptr<Expression>> payload_comparisons;

	// create a group for each target, these are the columns that should be grouped
	unordered_map<idx_t, idx_t> group_by_references;
	for (idx_t i = 0; i < op.key_targets.size(); i++) {
		auto &target = op.key_targets[i];
		D_ASSERT(target->GetExpressionType() == ExpressionType::BOUND_REF);
		auto &bound_ref = target->Cast<BoundReferenceExpression>();
		distinct_idx.emplace_back(bound_ref.Index());
		distinct_types.push_back(bound_ref.GetReturnType());
		group_by_references[bound_ref.Index()] = i;
	}

	/*
	 * iterate over all types
	 * 		Differentiate the occurrence of the column in the key clause.
	 */
	idx_t pay_idx = 0;
	for (idx_t i = 0; i < left.GetTypes().size(); ++i) {
		// Check if we can directly refer to a group, or if we need to push an aggregate
		auto entry = group_by_references.find(i);
		if (entry == group_by_references.end()) {
			D_ASSERT(op.payload_aggregates[pay_idx]->GetExpressionType() == ExpressionType::BOUND_AGGREGATE);
			auto &bound_aggr = op.payload_aggregates[pay_idx]->Cast<BoundAggregateExpression>();

			// add the logical type of the aggregate to the payload types
			payload_types.push_back(bound_aggr.GetReturnType());
			payload_aggregates.push_back(std::move(op.payload_aggregates[pay_idx++]));
			payload_idx.emplace_back(i);
		}
	}

	vector<LogicalType> hash_key_types;
	vector<LogicalType> aggregate_types = payload_types;
	vector<unique_ptr<Expression>> key_normalizers;
	vector<bool> key_requires_normalization;
	vector<idx_t> key_representative_indices(distinct_types.size(), DConstants::INVALID_INDEX);
	bool has_key_normalizers = false;
	FunctionBinder function_binder(context);
	for (idx_t key_idx = 0; key_idx < distinct_types.size(); key_idx++) {
		unique_ptr<Expression> normalizer = make_uniq<BoundReferenceExpression>(distinct_types[key_idx], key_idx);
		const auto requires_normalization =
		    ExpressionBinder::PushCollation(context, normalizer, distinct_types[key_idx]);
		has_key_normalizers = has_key_normalizers || requires_normalization;
		key_requires_normalization.push_back(requires_normalization);
		hash_key_types.push_back(normalizer->GetReturnType());
		key_normalizers.push_back(std::move(normalizer));
		if (!requires_normalization) {
			continue;
		}

		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq<BoundReferenceExpression>(distinct_types[key_idx], distinct_idx[key_idx]));
		auto representative = function_binder.BindAggregateFunction(
		    GetBuiltinAggregateFunction(context, FirstFun::Name, {distinct_types[key_idx]}), std::move(children));
		key_representative_indices[key_idx] = payload_aggregates.size();
		aggregate_types.push_back(representative->GetReturnType());
		payload_aggregates.push_back(std::move(representative));
	}
	if (!has_key_normalizers) {
		key_normalizers.clear();
	}
	if (!op.union_all) {
		for (idx_t i = 0; i < payload_types.size(); i++) {
			unique_ptr<Expression> previous = make_uniq<BoundReferenceExpression>(payload_types[i], i);
			unique_ptr<Expression> current =
			    make_uniq<BoundReferenceExpression>(payload_types[i], payload_types.size() + i);
			const auto normalize_previous = ExpressionBinder::PushCollation(context, previous, payload_types[i]);
#ifdef D_ASSERT_IS_ENABLED
			const auto normalize_current = ExpressionBinder::PushCollation(context, current, payload_types[i]);
			D_ASSERT(normalize_previous == normalize_current);
#else
			ExpressionBinder::PushCollation(context, current, payload_types[i]);
#endif
			if (normalize_previous) {
				payload_comparisons.push_back(BoundComparisonExpression::Create(
				    ExpressionType::COMPARE_DISTINCT_FROM, std::move(previous), std::move(current)));
			} else {
				payload_comparisons.push_back(nullptr);
			}
		}
	}

	// If the key variant has been used, a recurring table will be created.
	auto recurring_table = make_shared_ptr<ColumnDataCollection>(context, op.types);
	recurring_cte_tables[op.table_index] = recurring_table;
	{
		auto &planning_info = recursive_cte_planning[op.table_index];
		planning_info.using_key = true;
		planning_info.distinct_indices = distinct_idx;
		planning_info.payload_indices = payload_idx;
		planning_info.hash_key_types = hash_key_types;
		planning_info.aggregate_types = aggregate_types;
		planning_info.key_requires_normalization = key_requires_normalization;
	}

	planning_recursive_cte_depth++;
	auto &right = CreatePlan(*op.children[1]);
	planning_recursive_cte_depth--;
	auto &cte = Make<PhysicalRecursiveCTE>(op.ctename, op.table_index, op.types,
	                                       op.union_all ? RecursiveCTEMode::KEY_UNION_ALL : RecursiveCTEMode::KEY_UNION,
	                                       left, right, op.estimated_cardinality);
	auto &cast_cte = cte.Cast<PhysicalRecursiveCTE>();
	cast_cte.payload_aggregates = std::move(payload_aggregates);
	cast_cte.payload_comparisons = std::move(payload_comparisons);
	cast_cte.key_normalizers = std::move(key_normalizers);
	cast_cte.key_representative_indices = std::move(key_representative_indices);
	cast_cte.distinct_idx = distinct_idx;
	cast_cte.distinct_types = distinct_types;
	cast_cte.hash_key_types = std::move(hash_key_types);
	cast_cte.payload_idx = payload_idx;
	cast_cte.payload_types = payload_types;
	cast_cte.aggregate_types = std::move(aggregate_types);
	cast_cte.internal_types = op.internal_types;
	cast_cte.keyed = RecursiveCTEKeyedProperties::Derive(cast_cte);
	cast_cte.ref_recurring = op.ref_recurring;
	cast_cte.working_table = working_table;
	cast_cte.recurring_table = recurring_table;
	cast_cte.non_repeatable_operators = non_repeatable_operators;
	auto planning_entry = recursive_cte_planning.find(op.table_index);
	D_ASSERT(planning_entry != recursive_cte_planning.end());
	if (!planning_entry->second.state_scans.empty()) {
		for (auto &scan_ref : planning_entry->second.state_scans) {
			auto &scan = scan_ref.get();
			scan.recursive_cte = cast_cte;
			scan.distinct_idx = distinct_idx;
			scan.payload_idx = payload_idx;
			for (auto &spec : scan.partial_key_index_specs) {
				bool found = false;
				for (auto &existing : cast_cte.partial_key_index_specs) {
					found = found || existing == spec;
				}
				if (!found) {
					cast_cte.partial_key_index_specs.push_back(spec);
				}
			}
		}
	}
	return cte;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalCTERef &op) {
	D_ASSERT(op.children.empty());

	// Check if this LogicalCTERef is supposed to scan a materialized CTE.
	// Lookup if there is a materialized CTE for the cte_index.
	auto materialized_cte = materialized_ctes.find(op.cte_index);

	// If this check fails, this is a reference to a materialized recursive CTE.
	if (materialized_cte != materialized_ctes.end()) {
		auto &chunk_scan = Make<PhysicalColumnDataScan>(op.chunk_types, PhysicalOperatorType::CTE_SCAN,
		                                                op.estimated_cardinality, op.cte_index);

		auto cte = recursive_cte_tables.find(op.cte_index);
		if (cte == recursive_cte_tables.end()) {
			throw InvalidInputException("Referenced materialized CTE does not exist.");
		}

		auto &cast_chunk_scan = chunk_scan.Cast<PhysicalColumnDataScan>();
		cast_chunk_scan.collection = cte->second.get();
		auto cte_order = materialized_cte_orders.find(op.cte_index);
		if (cte_order != materialized_cte_orders.end()) {
			cast_chunk_scan.source_order = cte_order->second;
		}

		auto exchange = materialized_cte_exchanges.find(op.cte_index);
		if (exchange != materialized_cte_exchanges.end()) {
			// Exchange consumers can still be converted to materialized scans during pipeline construction.
			auto consumer_idx = exchange->second->RegisterConsumer();
			auto &source = Make<PhysicalCTEConsumerSource>(op.chunk_types, op.estimated_cardinality, op.cte_index,
			                                               exchange->second, consumer_idx);
			cast_chunk_scan.cte_source = source;
		}
		materialized_cte->second.push_back(cast_chunk_scan);
		return chunk_scan;
	}

	// CreatePlan of a LogicalRecursiveCTE must have happened before.
	auto cte = recursive_cte_tables.find(op.cte_index);
	if (cte == recursive_cte_tables.end()) {
		throw InvalidInputException("Referenced recursive CTE does not exist.");
	}
	auto planning_entry = recursive_cte_planning.find(op.cte_index);
	if (op.is_recurring && planning_entry != recursive_cte_planning.end() && planning_entry->second.using_key) {
		auto &state_scan = Make<PhysicalRecursiveCTEStateScan>(op.chunk_types, op.estimated_cardinality, op.cte_index)
		                       .Cast<PhysicalRecursiveCTEStateScan>();
		state_scan.distinct_idx = planning_entry->second.distinct_indices;
		state_scan.payload_idx = planning_entry->second.payload_indices;
		state_scan.hash_key_types = planning_entry->second.hash_key_types;
		state_scan.aggregate_types = planning_entry->second.aggregate_types;
		state_scan.key_requires_normalization = planning_entry->second.key_requires_normalization;
		planning_entry->second.state_scans.push_back(state_scan);
		return state_scan;
	}

	// If we found a recursive CTE and we want to scan the recurring table, we search for it,
	if (op.is_recurring) {
		cte = recurring_cte_tables.find(op.cte_index);
		if (cte == recurring_cte_tables.end()) {
			throw InvalidInputException("RECURRING can only be used with USING KEY in recursive CTE.");
		}
	}

	auto &types = cte->second.get()->Types();
	auto op_type =
	    op.is_recurring ? PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN : PhysicalOperatorType::RECURSIVE_CTE_SCAN;
	auto &chunk_scan = Make<PhysicalColumnDataScan>(types, op_type, op.estimated_cardinality, op.cte_index);
	auto &cast_chunk_scan = chunk_scan.Cast<PhysicalColumnDataScan>();
	cast_chunk_scan.collection = cte->second.get();
	return chunk_scan;
}

} // namespace duckdb
