// A self-join of one table on equal keys K and on `a.x <> b.x` for one more column x, read only through K by a parent
// that ignores duplicates, is `GROUP BY K HAVING min(x) <> max(x)` over a single scan. TPC-DS q95's ws_wh is the case:
//
//     WITH ws_wh AS (SELECT ws1.ws_order_number, ws1.ws_warehouse_sk wh1, ws2.ws_warehouse_sk wh2
//                    FROM web_sales ws1, web_sales ws2
//                    WHERE ws1.ws_order_number = ws2.ws_order_number AND ws1.ws_warehouse_sk <> ws2.ws_warehouse_sk)
//     ... ws_order_number IN (SELECT ws_order_number FROM ws_wh)
//
// The join pairs every order's rows with each other only to learn which orders ship from more than one warehouse;
// the group-by reads web_sales once and keeps one row per order.
//
// Why it is exact. The join's rows are the pairs (r1, r2) of rows with the same non-NULL K (`=` drops NULL keys) and
// non-NULL, different x (`<>` drops NULL x). The set of their K values is the set of K values of the non-NULL groups
// that hold two different non-NULL x, which is `min(x) <> max(x)` over the group (min and max skip NULLs; a group
// whose x are all NULL gives NULL <> NULL, which the HAVING drops). So the two produce the same SET of K, and a parent
// that reads only K and ignores how often each row occurs cannot tell them apart. The NULL key group is filtered out
// first: in the join no NULL key pairs with anything, and an IN over a NULL would turn a FALSE marker into NULL.
//
// Which parents ignore duplicates, checked from the root down (Walk): the right side of a SEMI, ANTI or MARK join (and
// the left side of a RIGHT_SEMI or RIGHT_ANTI one), whose rows only decide whether a match exists -- a MARK join's NULL
// included, which depends on whether a NULL occurs, not how often; a DISTINCT, whose targets it reads through
// expressions, a DISTINCT ON passing its other columns through by position, which therefore all count as read; and a
// GROUP BY whose aggregates are min, max or DISTINCT ones (`min(x, n)`, the n smallest values, counts duplicates and
// is not one of them). Below such a parent the property passes through projections and filters without volatile
// expressions, cross products and INNER joins: their output, as a set, depends only on their inputs as sets. Anything
// else stops it, a UNION among them: it reads its inputs' columns by position rather than through expressions, so what
// it reads would not be seen. A materialized CTE's definition ignores duplicates when every one of its references does.
//
// What must hold of the join, in the plan as bound (the rewrite runs before filter pushdown, which would move the
// conditions into the scans; the later optimizers then push the new filter into the scan and order the joins around
// the group-by as for any other): a projection over filters over a cross product or an INNER comparison join of two
// scans of the same relation -- the same table, or the same function with equal bind data and marked repeatable, so
// that the two scans return the same rows, read the same way, without table filters, time travel, sampling or
// ordinality -- and every condition of the filters and the join a `=` or `<>` between the same column of the two
// scans. The `=` columns are K, at least one; the `<>` column is x, exactly one and not in K (with two the pairs must
// differ in both, which no per-column test says). K and x are integral or DATE columns, whose ordering agrees with
// their equality; a collated VARCHAR's need not. The projection's columns read above it (through the references of
// the CTE it defines, when it does) must each be one of K: those then come from the group-by and the others, which
// nothing reads, become typed NULLs, so a CTE keeps the columns its references expect.
#include "duckdb/optimizer/self_join_aggregate.hpp"

#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

namespace {

bool AnyVolatile(const vector<unique_ptr<Expression>> &expressions) {
	for (auto &expr : expressions) {
		if (expr->IsVolatile()) {
			return true;
		}
	}
	return false;
}

bool JoinConditionsVolatile(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		return op.Cast<LogicalAnyJoin>().condition->IsVolatile();
	}
	for (auto &condition : op.Cast<LogicalComparisonJoin>().conditions) {
		if (condition.IsComparison() ? condition.GetLHS().IsVolatile() || condition.GetRHS().IsVolatile()
		                             : condition.GetJoinExpression().IsVolatile()) {
			return true;
		}
	}
	return false;
}

//! A GROUP BY whose result is the same over its input with or without repeated rows
bool AggregateIgnoresDuplicates(const LogicalAggregate &aggregate) {
	if (AnyVolatile(aggregate.groups)) {
		return false;
	}
	for (auto &expr : aggregate.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || expr->IsVolatile()) {
			return false;
		}
		auto &bound = expr->Cast<BoundAggregateExpression>();
		if (bound.IsDistinct()) {
			continue;
		}
		auto &name = bound.Function().GetName().GetIdentifierName();
		if ((name != "min" && name != "max") || bound.GetChildren().size() != 1) {
			return false;
		}
	}
	return true;
}

//! The column bindings the plan reads, visited from the root down so that a candidate projection's expressions count
//! only when the column they compute is itself read: everything above a candidate reads it through expressions, so
//! an unreferenced column of it is dead, and so is whatever only that column referenced (an inlined CTE's identity
//! projection names every column of its body, whether or not anything above reads it)
class LiveBindings : public LogicalOperatorVisitor {
public:
	//! `readers`: a candidate projection -> the table indexes through which its columns are read
	explicit LiveBindings(const reference_map_t<LogicalOperator, vector<TableIndex>> &readers) : readers(readers) {
	}

	column_binding_set_t read;

	bool IsRead(LogicalOperator &projection, idx_t column) const {
		for (auto reader : readers.at(projection)) {
			if (read.count(ColumnBinding(reader, ProjectionIndex(column))) > 0) {
				return true;
			}
		}
		return false;
	}

	void VisitOperator(LogicalOperator &op) override {
		if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			// the references, below the second child, are visited before the definition they read
			VisitOperator(*op.children[1]);
			VisitOperator(*op.children[0]);
			return;
		}
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION && readers.count(op) > 0) {
			for (idx_t k = 0; k < op.expressions.size(); k++) {
				if (IsRead(op, k)) {
					VisitExpression(&op.expressions[k]);
				}
			}
		} else {
			VisitOperatorExpressions(op);
		}
		VisitOperatorChildren(op);
	}

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		read.insert(expr.Binding());
		return nullptr;
	}

private:
	const reference_map_t<LogicalOperator, vector<TableIndex>> &readers;
};

//! A scan that reads its whole relation, so that one of two equal scans can stand in for both
bool PlainScan(const LogicalGet &get) {
	return get.children.empty() && !get.table_filters.HasFilters() && !get.at_clause && !get.dynamic_filters &&
	       !get.extra_info.sample_options && get.source_ordinality == OrdinalityType::WITHOUT_ORDINALITY;
}

bool SameParameters(const LogicalGet &a, const LogicalGet &b) {
	if (a.parameters.size() != b.parameters.size() || a.named_parameters.size() != b.named_parameters.size()) {
		return false;
	}
	for (idx_t i = 0; i < a.parameters.size(); i++) {
		if (!Value::NotDistinctFrom(a.parameters[i], b.parameters[i])) {
			return false;
		}
	}
	for (auto &entry : a.named_parameters) {
		auto other = b.named_parameters.find(entry.first);
		if (other == b.named_parameters.end() || !Value::NotDistinctFrom(entry.second, other->second)) {
			return false;
		}
	}
	return true;
}

//! Two scans that read the same rows: the same catalog table, or the same repeatable function with equal bind data
bool SameRelation(LogicalGet &a, LogicalGet &b) {
	if (!PlainScan(a) || !PlainScan(b) || a.function != b.function || !SameParameters(a, b)) {
		return false;
	}
	auto table = a.GetTable();
	if (table) {
		return table.get() == b.GetTable().get();
	}
	if (b.GetTable()) {
		return false;
	}
	// a function not marked repeatable may return different rows each time it runs
	if (ClassifyLogicalOperatorRepeatability(a) != LogicalOperatorRepeatability::REPEATABLE ||
	    ClassifyLogicalOperatorRepeatability(b) != LogicalOperatorRepeatability::REPEATABLE) {
		return false;
	}
	return a.bind_data && b.bind_data && a.bind_data->Equals(*b.bind_data);
}

//! The table column a scan emits at `binding`, when it is a plain one
bool ScanColumn(const LogicalGet &get, ProjectionIndex binding, column_t &column) {
	idx_t position = binding.GetIndex();
	if (!get.projection_ids.empty()) {
		if (position >= get.projection_ids.size()) {
			return false;
		}
		position = get.projection_ids[position].GetIndex();
	}
	auto &column_ids = get.GetColumnIds();
	if (position >= column_ids.size()) {
		return false;
	}
	auto &id = column_ids[position];
	if (id.IsVirtualColumn() || id.HasChildren()) {
		return false;
	}
	column = id.GetPrimaryIndex();
	return true;
}

//! Types whose ordering agrees with their equality, so that min <> max says exactly "two different values"
bool OrderMatchesEquality(const LogicalType &type) {
	return type.IsIntegral() || type.id() == LogicalTypeId::DATE;
}

void SplitConjunctions(const Expression &expr, vector<reference<const Expression>> &out) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			SplitConjunctions(*child, out);
		}
		return;
	}
	out.push_back(expr);
}

//! One column of the scanned relation, as each of the two scans emits it
struct PairedColumn {
	column_t column;
	ProjectionIndex left;
	ProjectionIndex right;
	LogicalType type;
};

struct Candidate {
	//! The projection's slot in its parent
	reference<unique_ptr<LogicalOperator>> slot;
	//! The CTE the projection defines, or null for one read through its own bindings
	optional_ptr<LogicalMaterializedCTE> cte;
};

class SelfJoinAggregateRewriter {
public:
	SelfJoinAggregateRewriter(ClientContext &context, Binder &binder) : context(context), binder(binder) {
	}

	void Run(unique_ptr<LogicalOperator> &plan) {
		Walk(plan, false);
		if (candidates.empty()) {
			return;
		}
		reference_map_t<LogicalOperator, vector<TableIndex>> readers;
		for (auto &candidate : candidates) {
			auto &projection = *candidate.slot.get();
			if (candidate.cte) {
				readers[projection] = cte_references[candidate.cte->table_index];
			} else {
				readers[projection] = {projection.Cast<LogicalProjection>().table_index};
			}
		}
		LiveBindings live(readers);
		live.VisitOperator(*plan);
		// deepest first: a rewrite replaces what is below its projection, never what is above it
		for (idx_t i = candidates.size(); i-- > 0;) {
			auto &projection = *candidates[i].slot.get();
			vector<bool> read(projection.expressions.size(), false);
			for (idx_t k = 0; k < read.size(); k++) {
				read[k] = live.IsRead(projection, k);
			}
			Rewrite(projection.Cast<LogicalProjection>(), read);
		}
	}

private:
	//! `ignores`: whether the parent of `slot` gives the same result over its rows with or without repeats
	void Walk(unique_ptr<LogicalOperator> &slot, bool ignores) {
		auto &op = *slot;
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (ignores) {
				candidates.push_back(Candidate {slot, nullptr});
			}
			Walk(op.children[0], ignores && !AnyVolatile(op.expressions));
			return;
		case LogicalOperatorType::LOGICAL_FILTER:
			Walk(op.children[0], ignores && !AnyVolatile(op.expressions));
			return;
		case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
			for (auto &child : op.children) {
				Walk(child, ignores);
			}
			return;
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		case LogicalOperatorType::LOGICAL_ANY_JOIN: {
			const bool deterministic = !JoinConditionsVolatile(op);
			bool left = false;
			bool right = false;
			switch (op.Cast<LogicalJoin>().join_type) {
			case JoinType::INNER:
				left = right = ignores;
				break;
			case JoinType::SEMI:
			case JoinType::ANTI:
			case JoinType::MARK:
				left = ignores;
				right = true;
				break;
			case JoinType::RIGHT_SEMI:
			case JoinType::RIGHT_ANTI:
				left = true;
				right = ignores;
				break;
			default:
				break;
			}
			Walk(op.children[0], left && deterministic);
			Walk(op.children[1], right && deterministic);
			return;
		}
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
			Walk(op.children[0], AggregateIgnoresDuplicates(op.Cast<LogicalAggregate>()));
			return;
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
			// its references are all below its second child, so they are all seen before its definition is
			auto &cte = op.Cast<LogicalMaterializedCTE>();
			Walk(op.children[1], ignores);
			auto entry = cte_ignores_duplicates.find(cte.table_index);
			const bool definition_ignores = entry != cte_ignores_duplicates.end() && entry->second;
			auto &definition = op.children[0];
			if (definition->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				// read through the references' bindings, not its own: a candidate of its own kind
				if (definition_ignores) {
					candidates.push_back(Candidate {definition, &cte});
				}
				Walk(definition->children[0], definition_ignores && !AnyVolatile(definition->expressions));
			} else {
				Walk(definition, definition_ignores);
			}
			return;
		}
		case LogicalOperatorType::LOGICAL_CTE_REF: {
			auto &reference = op.Cast<LogicalCTERef>();
			auto entry = cte_ignores_duplicates.emplace(reference.cte_index, true);
			entry.first->second = entry.first->second && ignores;
			cte_references[reference.cte_index].push_back(reference.table_index);
			return;
		}
		default:
			for (auto &child : op.children) {
				Walk(child, false);
			}
			return;
		}
	}

	//! Rewrites the self-join below `projection` when it is one; `read[k]`: whether column k is read above
	bool Rewrite(LogicalProjection &projection, const vector<bool> &read) {
		vector<reference<const Expression>> predicates;
		reference<LogicalOperator> op = *projection.children[0];
		while (op.get().type == LogicalOperatorType::LOGICAL_FILTER) {
			auto &filter = op.get().Cast<LogicalFilter>();
			if (filter.HasProjectionMap()) {
				return false;
			}
			for (auto &expr : filter.expressions) {
				SplitConjunctions(*expr, predicates);
			}
			op = *op.get().children[0];
		}
		vector<reference<const JoinCondition>> conditions;
		if (op.get().type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			auto &comparison_join = op.get().Cast<LogicalComparisonJoin>();
			if (comparison_join.join_type != JoinType::INNER || comparison_join.HasProjectionMap()) {
				return false;
			}
			for (auto &condition : comparison_join.conditions) {
				if (!condition.IsComparison()) {
					return false;
				}
				conditions.push_back(condition);
			}
		} else if (op.get().type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			return false;
		}
		auto &join = op.get();
		if (join.children.size() != 2 || join.children[0]->type != LogicalOperatorType::LOGICAL_GET ||
		    join.children[1]->type != LogicalOperatorType::LOGICAL_GET) {
			return false;
		}
		auto &left = join.children[0]->Cast<LogicalGet>();
		auto &right = join.children[1]->Cast<LogicalGet>();
		if (!SameRelation(left, right)) {
			return false;
		}

		// every condition a `=` (a key) or `<>` (the inequality) between the same column of the two scans
		vector<PairedColumn> keys;
		vector<PairedColumn> inequalities;
		auto classify = [&](ExpressionType comparison, const Expression &lhs, const Expression &rhs) {
			if (comparison != ExpressionType::COMPARE_EQUAL && comparison != ExpressionType::COMPARE_NOTEQUAL) {
				return false;
			}
			if (lhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
			    rhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				return false;
			}
			auto lhs_binding = lhs.Cast<BoundColumnRefExpression>().Binding();
			auto rhs_binding = rhs.Cast<BoundColumnRefExpression>().Binding();
			if (lhs_binding.table_index == right.table_index && rhs_binding.table_index == left.table_index) {
				std::swap(lhs_binding, rhs_binding);
			}
			if (lhs_binding.table_index != left.table_index || rhs_binding.table_index != right.table_index) {
				return false;
			}
			column_t left_column;
			column_t right_column;
			if (!ScanColumn(left, lhs_binding.column_index, left_column) ||
			    !ScanColumn(right, rhs_binding.column_index, right_column) || left_column != right_column ||
			    lhs.GetReturnType() != rhs.GetReturnType() || !OrderMatchesEquality(lhs.GetReturnType())) {
				return false;
			}
			auto &into = comparison == ExpressionType::COMPARE_EQUAL ? keys : inequalities;
			for (auto &known : into) {
				if (known.column == left_column) {
					return known.left == lhs_binding.column_index && known.right == rhs_binding.column_index;
				}
			}
			into.push_back(
			    PairedColumn {left_column, lhs_binding.column_index, rhs_binding.column_index, lhs.GetReturnType()});
			return true;
		};
		for (auto &predicate : predicates) {
			auto &expr = predicate.get();
			if (!BoundComparisonExpression::IsComparison(expr)) {
				return false;
			}
			auto &comparison = expr.Cast<BoundFunctionExpression>();
			if (!classify(expr.GetExpressionType(), BoundComparisonExpression::Left(comparison),
			              BoundComparisonExpression::Right(comparison))) {
				return false;
			}
		}
		for (auto &condition : conditions) {
			if (!classify(condition.get().GetComparisonType(), condition.get().GetLHS(), condition.get().GetRHS())) {
				return false;
			}
		}
		if (keys.empty() || inequalities.size() != 1) {
			return false;
		}
		auto &inequality = inequalities[0];
		for (auto &key : keys) {
			if (key.column == inequality.column) {
				return false;
			}
		}

		// each column read above is a key, of either scan: the group-by emits it
		vector<idx_t> key_of(projection.expressions.size(), DConstants::INVALID_INDEX);
		for (idx_t k = 0; k < projection.expressions.size(); k++) {
			if (!read[k]) {
				continue;
			}
			auto &expr = *projection.expressions[k];
			if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				return false;
			}
			auto binding = expr.Cast<BoundColumnRefExpression>().Binding();
			for (idx_t i = 0; i < keys.size(); i++) {
				if ((binding.table_index == left.table_index && binding.column_index == keys[i].left) ||
				    (binding.table_index == right.table_index && binding.column_index == keys[i].right)) {
					key_of[k] = i;
				}
			}
			if (key_of[k] == DConstants::INVALID_INDEX) {
				return false;
			}
		}

		// scan WHERE key IS NOT NULL ... GROUP BY keys [min(x), max(x)] HAVING min(x) <> max(x)
		auto scan_index = left.table_index;
		auto scan = std::move(join.children[0]);
		auto not_null = make_uniq<LogicalFilter>();
		for (auto &key : keys) {
			auto check = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			check->GetChildrenMutable().push_back(
			    make_uniq<BoundColumnRefExpression>(key.type, ColumnBinding(scan_index, key.left)));
			not_null->expressions.push_back(std::move(check));
		}
		not_null->AddChild(std::move(scan));

		vector<unique_ptr<Expression>> bounds;
		FunctionBinder function_binder(context);
		for (auto name : {"min", "max"}) {
			vector<unique_ptr<Expression>> arguments;
			arguments.push_back(
			    make_uniq<BoundColumnRefExpression>(inequality.type, ColumnBinding(scan_index, inequality.left)));
			auto function = GetBuiltinAggregateFunction(context, Identifier(name), {inequality.type});
			bounds.push_back(function_binder.BindAggregateFunction(function, std::move(arguments)));
		}
		auto group_index = binder.GenerateTableIndex();
		auto aggregate_index = binder.GenerateTableIndex();
		auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(bounds));
		aggregate->groupings_index = binder.GenerateTableIndex();
		GroupingSet every_key;
		for (idx_t i = 0; i < keys.size(); i++) {
			aggregate->groups.push_back(
			    make_uniq<BoundColumnRefExpression>(keys[i].type, ColumnBinding(scan_index, keys[i].left)));
			every_key.insert(ProjectionIndex(i));
		}
		aggregate->grouping_sets.push_back(std::move(every_key));
		aggregate->AddChild(std::move(not_null));

		auto having = make_uniq<LogicalFilter>(BoundComparisonExpression::Create(
		    ExpressionType::COMPARE_NOTEQUAL,
		    make_uniq<BoundColumnRefExpression>(inequality.type, ColumnBinding(aggregate_index, ProjectionIndex(0))),
		    make_uniq<BoundColumnRefExpression>(inequality.type, ColumnBinding(aggregate_index, ProjectionIndex(1)))));
		having->AddChild(std::move(aggregate));

		// the projection keeps its table index and its columns' types: a read key from the groups, the rest a NULL
		for (idx_t k = 0; k < projection.expressions.size(); k++) {
			auto &expr = projection.expressions[k];
			unique_ptr<Expression> replacement;
			if (key_of[k] != DConstants::INVALID_INDEX) {
				replacement = make_uniq<BoundColumnRefExpression>(
				    expr->GetReturnType(), ColumnBinding(group_index, ProjectionIndex(key_of[k])));
			} else {
				replacement = make_uniq<BoundConstantExpression>(Value(expr->GetReturnType()));
			}
			replacement->SetAlias(expr->GetAlias());
			expr = std::move(replacement);
		}
		// drops the filters, the join and the other scan
		projection.children[0] = std::move(having);
		projection.ResolveOperatorTypes();
		return true;
	}

private:
	ClientContext &context;
	Binder &binder;
	vector<Candidate> candidates;
	//! A CTE's index -> whether each of its references seen so far ignores duplicates
	unordered_map<TableIndex, bool> cte_ignores_duplicates;
	//! A CTE's index -> the table indexes of its references
	unordered_map<TableIndex, vector<TableIndex>> cte_references;
};

} // namespace

SelfJoinAggregate::SelfJoinAggregate(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> SelfJoinAggregate::Optimize(unique_ptr<LogicalOperator> op) {
	SelfJoinAggregateRewriter rewriter(optimizer.GetContext(), optimizer.binder);
	rewriter.Run(op);
	return op;
}

} // namespace duckdb
