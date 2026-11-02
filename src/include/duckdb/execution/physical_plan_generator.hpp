//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/physical_plan_generator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_tokens.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {
class ClientContext;
class ColumnDataCollection;
class PipelineBroadcastExchange;
class PhysicalRecursiveCTEStateScan;

struct RecursiveCTEPlanningInfo {
	bool using_key = false;
	vector<idx_t> distinct_indices;
	vector<idx_t> payload_indices;
	vector<LogicalType> hash_key_types;
	vector<LogicalType> aggregate_types;
	vector<bool> key_requires_normalization;
	vector<reference<PhysicalRecursiveCTEStateScan>> state_scans;
};

class PhysicalPlan {
public:
	explicit PhysicalPlan(Allocator &allocator) : arena(allocator) {};

	~PhysicalPlan() {
		// Call the destructor of each physical operator.
		for (auto &op : ops) {
			auto &op_ref = op.get();
			op_ref.~PhysicalOperator();
		}
	}

public:
	template <class T, class... ARGS>
	PhysicalOperator &Make(ARGS &&... args) {
		static_assert(std::is_base_of<PhysicalOperator, T>::value, "T must be a physical operator");
		auto ptr = arena.Make<T>(*this, std::forward<ARGS>(args)...);
		ops.push_back(*ptr);
		return *ptr;
	}

	PhysicalOperator &Root() {
		D_ASSERT(root);
		return *root;
	}
	void SetRoot(PhysicalOperator &op) {
		root = op;
	}
	//! Get a reference to the arena.
	ArenaAllocator &ArenaRef() {
		return arena;
	}

private:
	//! The arena allocator storing the physical operator memory.
	ArenaAllocator arena;
	//! References to the physical operators.
	vector<reference<PhysicalOperator>> ops;
	//! The root of the physical plan.
	optional_ptr<PhysicalOperator> root;
};

//! The physical plan generator generates a physical execution plan from a logical query plan.
class PhysicalPlanGenerator {
public:
	explicit PhysicalPlanGenerator(ClientContext &context);
	~PhysicalPlanGenerator();

	LogicalDependencyList dependencies;
	//! Recursive CTEs require at least one ChunkScan, referencing the working_table.
	//! This data structure is used to establish it.
	unordered_map<TableIndex, shared_ptr<ColumnDataCollection>> recursive_cte_tables;
	//! Materialized CTEs that are executed through a streaming exchange
	unordered_map<TableIndex, shared_ptr<PipelineBroadcastExchange>> materialized_cte_exchanges;
	//! Used to reference the recurring tables
	unordered_map<TableIndex, shared_ptr<ColumnDataCollection>> recurring_cte_tables;
	//! Physical planning information for recursive CTE state references.
	unordered_map<TableIndex, RecursiveCTEPlanningInfo> recursive_cte_planning;
	//! Materialized CTE ids must be collected.
	unordered_map<TableIndex, vector<const_reference<PhysicalOperator>>> materialized_ctes;
	unordered_map<TableIndex, OrderPreservationType> materialized_cte_orders;
	//! The index for duplicate eliminated joins.
	idx_t delim_index = 0;
	//! Tracks whether we are planning the recursive member of a recursive CTE.
	idx_t planning_recursive_cte_depth = 0;

public:
	//! Creates and returns the physical plan from the logical operator.
	//! Performs a verification pass.
	unique_ptr<PhysicalPlan> Plan(unique_ptr<LogicalOperator> logical);
	PhysicalOperator &CreatePlan(LogicalOperator &op);

	//! Whether or not we can (or should) use a batch-index based operator for executing the given sink
	static bool UseBatchIndex(ClientContext &context, PhysicalOperator &plan);
	//! Whether or not we should preserve insertion order for executing the given sink
	static bool PreserveInsertionOrder(ClientContext &context, PhysicalOperator &plan);
	//! The order preservation type of the given operator decided by recursively looking at its children
	static OrderPreservationType OrderPreservationRecursive(PhysicalOperator &op);
	//! Determine whether a child has a single value partitioning for the given expressions.
	static bool HasSingleValuePartitions(ClientContext &context, const vector<unique_ptr<Expression>> &partitions,
	                                     PhysicalOperator &child, vector<column_t> &partition_columns);
	//! Make a physical operator in the physical plan.
	template <class T, class... ARGS>
	PhysicalOperator &Make(ARGS &&... args) {
		return physical_plan->Make<T>(std::forward<ARGS>(args)...);
	}

	//! Get a reference to the ArenaAllocator of the underlying physical plan.
	//! Creates a new (empty) physical plan if none exists yet.
	ArenaAllocator &ArenaRef();

public:
	PhysicalOperator &ResolveDefaultsProjection(LogicalInsert &op, PhysicalOperator &child);
	//! Plans the copy without expanding op.file_path
	PhysicalOperator &CreatePlan(LogicalCopyToFile &op, PhysicalOperator &plan);

protected:
	PhysicalOperator &CreatePlan(LogicalAggregate &op);
	PhysicalOperator &CreatePlan(LogicalAnyJoin &op);
	PhysicalOperator &CreatePlan(LogicalColumnDataGet &op);
	PhysicalOperator &CreatePlan(LogicalComparisonJoin &op);
	PhysicalOperator &CreatePlan(LogicalCopyDatabase &op);
	PhysicalOperator &CreatePlan(LogicalCreate &op);
	PhysicalOperator &CreatePlan(LogicalCreateTable &op);
	PhysicalOperator &CreatePlan(LogicalCreateIndex &op);
	PhysicalOperator &CreatePlan(LogicalCreateSecret &op);
	PhysicalOperator &CreatePlan(LogicalCrossProduct &op);
	PhysicalOperator &CreatePlan(LogicalDelete &op);
	PhysicalOperator &CreatePlan(LogicalDelimGet &op);
	PhysicalOperator &CreatePlan(LogicalDistinct &op);
	PhysicalOperator &CreatePlan(LogicalDummyScan &expr);
	PhysicalOperator &CreatePlan(LogicalEmptyResult &op);
	PhysicalOperator &CreatePlan(LogicalExpressionGet &op);
	PhysicalOperator &CreatePlan(LogicalExport &op);
	PhysicalOperator &CreatePlan(LogicalFilter &op);
	PhysicalOperator &CreatePlan(LogicalGet &op);
	PhysicalOperator &CreatePlan(LogicalLimit &op);
	PhysicalOperator &CreatePlan(LogicalMergeInto &op);
	PhysicalOperator &CreatePlan(LogicalOrder &op);
	PhysicalOperator &CreatePlan(LogicalTopN &op);
	PhysicalOperator &CreatePlan(LogicalPositionalJoin &op);
	PhysicalOperator &CreatePlan(LogicalProjection &op);
	PhysicalOperator &CreatePlan(LogicalInsert &op);
	PhysicalOperator &CreatePlan(LogicalCopyToFile &op);
	PhysicalOperator &CreatePlan(LogicalExplain &op);
	PhysicalOperator &CreatePlan(LogicalSetOperation &op);
	PhysicalOperator &CreatePlan(LogicalUpdate &op);
	PhysicalOperator &CreatePlan(LogicalPrepare &expr);
	PhysicalOperator &CreatePlan(LogicalWindow &expr);
	PhysicalOperator &CreatePlan(LogicalExecute &op);
	PhysicalOperator &CreatePlan(LogicalPragma &op);
	PhysicalOperator &CreatePlan(LogicalSample &op);
	PhysicalOperator &CreatePlan(LogicalSecureView &op);
	PhysicalOperator &CreatePlan(LogicalSet &op);
	PhysicalOperator &CreatePlan(LogicalReset &op);
	PhysicalOperator &CreatePlan(LogicalAlter &op);
	PhysicalOperator &CreatePlan(LogicalAttach &op);
	PhysicalOperator &CreatePlan(LogicalConnect &op);
	PhysicalOperator &CreatePlan(LogicalExternalResource &op);
	PhysicalOperator &CreatePlan(LogicalDetach &op);
	PhysicalOperator &CreatePlan(LogicalDisconnect &op);
	PhysicalOperator &CreatePlan(LogicalDrop &op);
	PhysicalOperator &CreatePlan(LogicalLoad &op);
	PhysicalOperator &CreatePlan(LogicalTransaction &op);
	PhysicalOperator &CreatePlan(LogicalUpdateExtensions &op);
	PhysicalOperator &CreatePlan(LogicalVacuum &op);
	PhysicalOperator &CreatePlan(LogicalUnnest &op);
	PhysicalOperator &CreatePlan(LogicalRecursiveCTE &op);
	PhysicalOperator &CreatePlan(LogicalMaterializedCTE &op);
	PhysicalOperator &CreatePlan(LogicalCTERef &op);
	PhysicalOperator &CreatePlan(LogicalPivot &op);

	PhysicalOperator &PlanAsOfJoin(LogicalComparisonJoin &op);
	PhysicalOperator &PlanComparisonJoin(LogicalComparisonJoin &op);
	PhysicalOperator &PlanDelimJoin(LogicalComparisonJoin &op);
	PhysicalOperator &ExtractAggregateExpressions(PhysicalOperator &child, vector<unique_ptr<Expression>> &expressions,
	                                              vector<unique_ptr<Expression>> &groups,
	                                              optional_ptr<vector<GroupingSet>> grouping_sets);

private:
	ClientContext &context;
	unique_ptr<PhysicalPlan> physical_plan;
	reference_set_t<const PhysicalOperator> non_repeatable_operators;

private:
	PhysicalOperator &CreatePlanInternal(LogicalOperator &op);
	PhysicalOperator &ResolveAndPlan(unique_ptr<LogicalOperator> logical);
	unique_ptr<PhysicalPlan> PlanInternal(LogicalOperator &logical);
	bool PreserveInsertionOrder(PhysicalOperator &plan);
	bool UseBatchIndex(PhysicalOperator &plan);
	optional_ptr<PhysicalOperator> PlanAsOfLoopJoin(LogicalComparisonJoin &op, PhysicalOperator &probe,
	                                                PhysicalOperator &build, const idx_t join_cardinality);
	optional_ptr<PhysicalOperator> PlanAsOfInequalityJoin(LogicalComparisonJoin &op, PhysicalOperator &probe,
	                                                      PhysicalOperator &build, const idx_t lhs_cardinality,
	                                                      const idx_t rhs_cardinality);
};
} // namespace duckdb
