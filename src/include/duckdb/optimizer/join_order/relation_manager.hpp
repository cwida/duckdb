//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/relation_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

class JoinOrderOptimizer;
class FilterInfo;

struct JoinOrderExtraction {
	vector<unique_ptr<FilterInfo>> filters;
	vector<unique_ptr<JoinOrderOperator>> join_operators;
};

//! Represents a single relation and any metadata accompanying that relation
struct SingleJoinRelation {
public:
	SingleJoinRelation(LogicalOperator &op, optional_ptr<LogicalOperator> parent);
	SingleJoinRelation(LogicalOperator &op, optional_ptr<LogicalOperator> parent, RelationStats stats);

public:
	LogicalOperator &op;
	optional_ptr<LogicalOperator> parent;
	RelationStats stats;
};

class RelationManager {
public:
	explicit RelationManager(ClientContext &context);

public:
	idx_t NumRelations();

	bool ExtractJoinRelations(JoinOrderOptimizer &optimizer, LogicalOperator &input_op,
	                          vector<reference<LogicalOperator>> &filter_operators,
	                          optional_ptr<LogicalOperator> parent = nullptr);

	//! for each join filter in the logical plan op, extract the relations that are referred to on
	//! both sides of the join filter, along with the tables & indexes.
	JoinOrderExtraction ExtractEdges(LogicalOperator &op, vector<reference<LogicalOperator>> &filter_operators,
	                                 JoinRelationSetManager &set_manager);

	//! Extract the set of relations referred to inside an expression
	bool ExtractBindings(const Expression &expression, unordered_set<RelationIndex> &bindings);
	//! Like ExtractBindings, but returns whether the filter can be moved off its operator
	bool ExtractFilterBindings(const Expression &expression, unordered_set<RelationIndex> &bindings);
	bool TryNormalizeBinding(ColumnBinding binding, ColumnBinding &normalized) const;
	bool HasCompleteStats() const;
	bool AddRelation(LogicalOperator &op, optional_ptr<LogicalOperator> parent, const RelationStats &stats);
	//! Add an unnest relation which can come from a logical unnest or a logical get which has an unnest function
	bool AddRelationWithChildren(JoinOrderOptimizer &optimizer, LogicalOperator &op, LogicalOperator &input_op,
	                             optional_ptr<LogicalOperator> parent, RelationStats &child_stats);
	vector<unique_ptr<SingleJoinRelation>> GetRelations();

	const vector<RelationStats> GetRelationStats();
	//! A mapping of base table index -> index into relations array (relation number)
	unordered_map<TableIndex, RelationIndex> relation_mapping;

	void PrintRelationStats();

private:
	optional_ptr<JoinRelationSet> GetJoinRelations(column_binding_set_t &column_bindings,
	                                               JoinRelationSetManager &set_manager);
	void GetColumnBindingsFromExpression(const Expression &expression, column_binding_set_t &column_bindings);
	void GetColumnBindingsFromOperator(LogicalOperator &op, column_binding_set_t &column_bindings);
	void RegisterRelationBindings(LogicalOperator &op, RelationIndex relation_id, const RelationStats &stats);
	optional<RelationStats> AlignStatsWithRelationRoot(LogicalOperator &op, const RelationStats &stats);

private:
	ClientContext &context;
	//! Set of all relations considered in the join optimizer
	vector<unique_ptr<SingleJoinRelation>> relations;
	reference_map_t<LogicalOperator, RelationIndex> operator_relations;
	column_binding_map_t<ColumnBinding> normalized_bindings;
	bool stats_complete = true;
};

} // namespace duckdb
