//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/filter_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class LogicalGet;
class Optimizer;

class FilterPushdown {
public:
	enum class ProjectionMode : uint8_t { ALLOW_COMPUTED_EXPRESSIONS, PRESERVE_COMPUTED_EXPRESSIONS };

	explicit FilterPushdown(Optimizer &optimizer, bool convert_mark_joins = true,
	                        ProjectionMode projection_mode = ProjectionMode::ALLOW_COMPUTED_EXPRESSIONS);

	//! Perform filter pushdown
	unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);
	//! Return a reference to the client context (from the optimizer)
	ClientContext &GetContext();

	void CheckMarkToSemi(LogicalOperator &op, const unordered_set<TableIndex> &table_bindings);

	//! Whether a filter carrying a barrier may be pushed through this operator - i.e. whether the operator is
	//! guaranteed not to remove any rows. A barred expression must never be evaluated on rows that an operator below
	//! it filters out.
	static bool BarrierCanPassThrough(LogicalOperatorType type);

	struct Filter {
		unordered_set<TableIndex> bindings;
		unique_ptr<Expression> filter;
		//! Whether the filter contains a barrier - see ExpressionBarrier
		bool has_barrier = false;

		Filter() {
		}
		explicit Filter(unique_ptr<Expression> filter) : filter(std::move(filter)) {
		}

		void ExtractBindings();
		//! Recompute has_barrier after the filter expression has been created or rewritten
		void ExtractBarrier();
	};

private:
	Optimizer &optimizer;
	FilterCombiner combiner;
	bool convert_mark_joins;
	ProjectionMode projection_mode;

	vector<unique_ptr<Filter>> filters;
	//! Push down a LogicalAggregate op
	unique_ptr<LogicalOperator> PushdownAggregate(unique_ptr<LogicalOperator> op);
	//! Push down a distinct operator
	unique_ptr<LogicalOperator> PushdownDistinct(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalFilter op
	unique_ptr<LogicalOperator> PushdownFilter(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalCrossProduct op
	unique_ptr<LogicalOperator> PushdownCrossProduct(unique_ptr<LogicalOperator> op);
	//! Push down a join operator
	unique_ptr<LogicalOperator> PushdownJoin(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalProjection op
	unique_ptr<LogicalOperator> PushdownProjection(unique_ptr<LogicalOperator> op);
	//! Split a projection so filters can reuse computed outputs without forcing all expressions to be evaluated early
	unique_ptr<LogicalOperator> SplitProjection(unique_ptr<LogicalOperator> op,
	                                            vector<unique_ptr<Expression>> split_expressions);
	//! Push down a LogicalProjection op
	unique_ptr<LogicalOperator> PushdownUnnest(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalSetOperation op
	unique_ptr<LogicalOperator> PushdownSetOperation(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalGet op
	unique_ptr<LogicalOperator> PushdownGet(unique_ptr<LogicalOperator> op);
	//! Push the barrier filters into a LogicalGet, if all the other filters were pushed into the scan as well
	void PushdownBarrierFilters(LogicalGet &get, vector<unique_ptr<Filter>> &barrier_filters);
	//! Push down a LogicalLimit op
	unique_ptr<LogicalOperator> PushdownLimit(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalWindow op
	unique_ptr<LogicalOperator> PushdownWindow(unique_ptr<LogicalOperator> op);
	//! Push down a LogicalSecureView op
	unique_ptr<LogicalOperator> PushdownSecureView(unique_ptr<LogicalOperator> op);
	//! Remove the filters carrying a barrier from the current filter set and return them
	vector<unique_ptr<Expression>> ExtractBarrierFilters();
	// Pushdown an inner join
	unique_ptr<LogicalOperator> PushdownInnerJoin(unique_ptr<LogicalOperator> op,
	                                              unordered_set<TableIndex> &left_bindings,
	                                              unordered_set<TableIndex> &right_bindings);
	// Pushdown a left join
	unique_ptr<LogicalOperator> PushdownLeftJoin(unique_ptr<LogicalOperator> op,
	                                             unordered_set<TableIndex> &left_bindings,
	                                             unordered_set<TableIndex> &right_bindings);

	// Pushdown an outer join
	unique_ptr<LogicalOperator> PushdownOuterJoin(unique_ptr<LogicalOperator> op,
	                                              unordered_set<TableIndex> &left_bindings,
	                                              unordered_set<TableIndex> &right_bindings);
	unique_ptr<LogicalOperator> PushdownSemiAntiJoin(unique_ptr<LogicalOperator> op);
	// Pushdown a mark join
	unique_ptr<LogicalOperator> PushdownMarkJoin(unique_ptr<LogicalOperator> op,
	                                             unordered_set<TableIndex> &left_bindings,
	                                             unordered_set<TableIndex> &right_bindings);
	// Pushdown a single join
	unique_ptr<LogicalOperator> PushdownSingleJoin(unique_ptr<LogicalOperator> op,
	                                               unordered_set<TableIndex> &left_bindings,
	                                               unordered_set<TableIndex> &right_bindings);

	// AddLogicalFilter used to add an extra LogicalFilter at this level,
	// because in some cases, some expressions can not be pushed down.
	unique_ptr<LogicalOperator> AddLogicalFilter(unique_ptr<LogicalOperator> op,
	                                             vector<unique_ptr<Expression>> expressions);
	//! Push any remaining filters into a LogicalFilter at this level
	unique_ptr<LogicalOperator> PushFinalFilters(unique_ptr<LogicalOperator> op);
	// Finish pushing down at this operator, creating a LogicalFilter to store any of the stored filters and recursively
	// pushing down into its children (if any)
	unique_ptr<LogicalOperator> FinishPushdown(unique_ptr<LogicalOperator> op);
	//! Adds a filter to the set of filters. Returns FilterResult::UNSATISFIABLE if the subtree should be stripped, or
	//! FilterResult::SUCCESS otherwise

	unique_ptr<LogicalOperator> PushFiltersIntoDelimJoin(unique_ptr<LogicalOperator> op);
	FilterResult AddFilter(unique_ptr<Expression> expr);
	//! Extract filter bindings to compare them with expressions in an operator and determine if the filter
	//! can be pushed down
	void ExtractFilterBindings(const Expression &expr, vector<ColumnBinding> &bindings);
	//! Generate filters from the current set of filters stored in the FilterCombiner
	void GenerateFilters();
	//! if there are filters in this FilterPushdown node, push them into the combiner. Returns
	//! FilterResult::UNSATISFIABLE if the subtree should be stripped, or FilterResult::SUCCESS otherwise
	FilterResult PushFilters();
};

} // namespace duckdb
