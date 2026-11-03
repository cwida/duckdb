#include "duckdb/optimizer/scalar_aggregate_merger.hpp"

#include "duckdb/common/set.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Branch: [projection ->] ungrouped aggregate -> filters -> plain scan(s)
//===--------------------------------------------------------------------===//
//! One leaf of a cross-product tree that can be merged with its siblings
struct ScalarAggregateMerger::Branch {
	//! The leaf's slot in the cross-product tree
	reference<unique_ptr<LogicalOperator>> leaf;
	//! The projection over the aggregate, if any
	optional_ptr<LogicalProjection> projection;
	reference<LogicalAggregate> aggregate;
	//! The slot under the filters: the scan, or the cross product of scans of different tables (a FROM list)
	reference<unique_ptr<LogicalOperator>> relation;
	vector<reference<LogicalGet>> scans;
	//! The filters' predicates, with ANDs split into their conjuncts
	vector<reference<const Expression>> conjuncts;
};

//! One of the first branch's scans, which the merged leaf keeps, and the columns it gains for the other branches
struct ScalarAggregateMerger::MergeTarget {
	reference<LogicalGet> scan;
	//! A column the scan gains, and the binding that column lands on
	vector<std::pair<column_t, ColumnBinding>> added_columns;
};

//! The aggregates that are merged: FILTER (WHERE p) feeds exactly the rows where p holds, so the result over the rows
//! where p holds equals the result with FILTER (WHERE p) over any superset of them, for empty input too
//! (0 for the counts, NULL otherwise). min(x, n) and max(x, n) take two arguments and are not matched.
static bool IsMergeableAggregateFunction(const BoundAggregateExpression &aggregate) {
	auto &name = aggregate.Function().GetName().GetIdentifierName();
	const auto argument_count = aggregate.GetChildren().size();
	if (name == "count_star") {
		return argument_count == 0;
	}
	return argument_count == 1 && (name == "count" || name == "sum" || name == "avg" || name == "min" || name == "max");
}

//! An expression that may be evaluated on more rows than before: neither volatile nor able to throw. A foldable
//! subtree that can throw failed to fold, and a CASE or OR above it decides per row whether it is reached.
static bool IsSafeOnOtherRows(const Expression &expr) {
	return !expr.IsVolatile() && !expr.CanThrow();
}

static bool IsMergeableAggregate(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &aggregate = expr.Cast<BoundAggregateExpression>();
	// DISTINCT is fine: the FILTER applies before the deduplication. ORDER BY is not matched.
	if (aggregate.GetOrderBys() || !IsMergeableAggregateFunction(aggregate)) {
		return false;
	}
	for (auto &child : aggregate.GetChildren()) {
		if (!IsSafeOnOtherRows(*child)) {
			return false;
		}
	}
	return !aggregate.GetFilter() || IsSafeOnOtherRows(*aggregate.GetFilter());
}

static void SplitConjunction(const Expression &expr, vector<reference<const Expression>> &conjuncts) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			SplitConjunction(*child, conjuncts);
		}
		return;
	}
	conjuncts.push_back(expr);
}

//! A scan as bound: no filters, time travel, ordinality, projection list, sample, input table or process-local input,
//! and only plain columns. A scan that reads no column gets the empty placeholder once its types are resolved.
static bool IsPlainScan(const LogicalGet &get) {
	if (!get.children.empty() || get.bind_info || get.table_filters.HasFilters() || get.at_clause ||
	    get.source_ordinality != OrdinalityType::WITHOUT_ORDINALITY || !get.projection_ids.empty() ||
	    get.has_pushed_projection || get.extra_info.sample_options || get.dynamic_filters ||
	    !get.input_table_types.empty() || !get.projected_input.empty()) {
		return false;
	}
	for (auto &column_id : get.GetColumnIds()) {
		if (column_id.IsEmptyColumn()) {
			continue;
		}
		if (column_id.IsVirtualColumn() || column_id.HasChildren()) {
			return false;
		}
	}
	return true;
}

static bool SameValues(const vector<Value> &a, const vector<Value> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (idx_t i = 0; i < a.size(); i++) {
		if (a[i].type() != b[i].type() || !Value::NotDistinctFrom(a[i], b[i])) {
			return false;
		}
	}
	return true;
}

static bool SameNamedParameters(const named_argument_map_t &a, const named_argument_map_t &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto &entry : a) {
		auto other = b.find(entry.first);
		if (other == b.end() || entry.second.type() != other->second.type() ||
		    !Value::NotDistinctFrom(entry.second, other->second)) {
			return false;
		}
	}
	return true;
}

//! Two plain scans that read the same rows: the same catalog table, or the same table function (read_parquet,
//! read_csv, ...) called with the same arguments. What a call without arguments reads is known to its bind data alone.
static bool IsSameRelation(const LogicalGet &a, const LogicalGet &b) {
	if (a.function != b.function || !SameValues(a.parameters, b.parameters) ||
	    !SameNamedParameters(a.named_parameters, b.named_parameters)) {
		return false;
	}
	auto table = a.GetTable();
	if (table || b.GetTable()) {
		return table.get() == b.GetTable().get();
	}
	return !a.parameters.empty() || FunctionData::Equals(a.bind_data.get(), b.bind_data.get());
}

//! The columns a scan reads, without the empty placeholder
static set<column_t> ReadColumns(const LogicalGet &get) {
	set<column_t> columns;
	for (auto &column_id : get.GetColumnIds()) {
		if (!column_id.IsEmptyColumn()) {
			columns.insert(column_id.GetPrimaryIndex());
		}
	}
	return columns;
}

//! A named argument that is set: given, not NULL and not false
static bool HasOption(const LogicalGet &get, const char *name) {
	auto entry = get.named_parameters.find(name);
	if (entry == get.named_parameters.end() || entry->second.IsNull()) {
		return false;
	}
	return entry->second.type().id() != LogicalTypeId::BOOLEAN || BooleanValue::Get(entry->second);
}

//! A scan that stores the rows it rejects writes them as a side effect of each scan
static bool RecordsRejects(const LogicalGet &get) {
	return HasOption(get, "store_rejects") || HasOption(get, "rejects_table") || HasOption(get, "rejects_scan") ||
	       HasOption(get, "rejects_limit");
}

//! Two scans that one scan reading both of their columns can replace: the same relation, and the rows a scan returns do
//! not depend on the columns it reads. A scan that drops the rows it fails to parse (ignore_errors) returns a row when
//! every column it reads parses, so such scans are only shared when they read the same columns.
static bool CanShareScan(const LogicalGet &a, const LogicalGet &b) {
	if (!IsSameRelation(a, b) || RecordsRejects(a)) {
		return false;
	}
	return !HasOption(a, "ignore_errors") || ReadColumns(a) == ReadColumns(b);
}

//! The plain scans under `op`: one scan, or a cross product of scans of different relations (a self-join's copies of
//! a table would not pair one to one with another branch's)
static bool CollectScans(LogicalOperator &op, vector<reference<LogicalGet>> &scans) {
	if (op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		for (auto &child : op.children) {
			if (!CollectScans(*child, scans)) {
				return false;
			}
		}
		return true;
	}
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &scan = op.Cast<LogicalGet>();
	if (!IsPlainScan(scan)) {
		return false;
	}
	for (auto &other : scans) {
		if (IsSameRelation(other.get(), scan)) {
			return false;
		}
	}
	scans.push_back(scan);
	return true;
}

//! Two branches read the same tables when their scans pair up one to one as scans that can be shared
static bool ReadSameTables(const ScalarAggregateMerger::Branch &a, const ScalarAggregateMerger::Branch &b) {
	if (a.scans.size() != b.scans.size()) {
		return false;
	}
	for (auto &scan : a.scans) {
		bool found = false;
		for (auto &other : b.scans) {
			found = found || CanShareScan(scan.get(), other.get());
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

//! Equal conjuncts, or the same comparison with its sides swapped (a = b and b = a)
static bool IsSameConjunct(const Expression &a, const Expression &b) {
	if (a.Equals(b)) {
		return true;
	}
	if (!BoundComparisonExpression::IsComparison(a) || !BoundComparisonExpression::IsComparison(b)) {
		return false;
	}
	auto &left = a.Cast<BoundFunctionExpression>();
	auto &right = b.Cast<BoundFunctionExpression>();
	return left.GetExpressionType() == FlipComparisonExpression(right.GetExpressionType()) &&
	       BoundComparisonExpression::Left(left).Equals(BoundComparisonExpression::Right(right)) &&
	       BoundComparisonExpression::Right(left).Equals(BoundComparisonExpression::Left(right));
}

static bool ContainsConjunct(const vector<unique_ptr<Expression>> &conjuncts, const Expression &expr) {
	for (auto &conjunct : conjuncts) {
		if (IsSameConjunct(*conjunct, expr)) {
			return true;
		}
	}
	return false;
}

static void CollectBindings(const Expression &expr, column_binding_set_t &bindings) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expr, [&](const BoundColumnRefExpression &column_ref) { bindings.insert(column_ref.Binding()); });
}

//! Points every column reference of `expr` that `remap` names at its new binding
static void RemapBindings(Expression &expr, const column_binding_map_t<ColumnBinding> &remap) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &binding = expr.Cast<BoundColumnRefExpression>().BindingMutable();
		auto entry = remap.find(binding);
		if (entry != remap.end()) {
			binding = entry->second;
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { RemapBindings(child, remap); });
}

static unique_ptr<Expression> MakeConjunction(ExpressionType type, vector<unique_ptr<Expression>> terms) {
	if (terms.size() == 1) {
		return std::move(terms[0]);
	}
	auto result = make_uniq<BoundConjunctionExpression>(type);
	for (auto &term : terms) {
		result->GetChildrenMutable().push_back(std::move(term));
	}
	return std::move(result);
}

static bool TreeHasProjectionMap(const LogicalOperator &op) {
	if (op.HasProjectionMap()) {
		return true;
	}
	for (auto &child : op.children) {
		if (TreeHasProjectionMap(*child)) {
			return true;
		}
	}
	return false;
}

//! Where `target` reads `column`: the binding it has, or the one it gains once its group merges
static ColumnBinding TargetBinding(ScalarAggregateMerger::MergeTarget &target, column_t column) {
	auto &scan = target.scan.get();
	auto &column_ids = scan.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].GetPrimaryIndex() == column) {
			return ColumnBinding(scan.table_index, ProjectionIndex(i));
		}
	}
	for (auto &entry : target.added_columns) {
		if (entry.first == column) {
			return entry.second;
		}
	}
	ColumnBinding binding(scan.table_index, ProjectionIndex(column_ids.size() + target.added_columns.size()));
	target.added_columns.emplace_back(column, binding);
	return binding;
}

//===--------------------------------------------------------------------===//
// ScalarAggregateMerger
//===--------------------------------------------------------------------===//
ScalarAggregateMerger::ScalarAggregateMerger(Optimizer &optimizer) : optimizer(optimizer) {
}

void ScalarAggregateMerger::Optimize(unique_ptr<LogicalOperator> &plan) {
	// rebuilding a cross-product tree would move the positions a projection map names: only plans without them
	if (TreeHasProjectionMap(*plan)) {
		return;
	}
	VisitOperator(plan);
	if (replacements.empty()) {
		return;
	}
	ColumnBindingReplacer replacer;
	replacer.replacement_bindings = std::move(replacements);
	replacer.VisitOperator(*plan);
	plan->ResolveOperatorTypes();
}

void ScalarAggregateMerger::VisitOperator(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		MergeCrossProduct(op);
		return;
	}
	for (auto &child : op->children) {
		VisitOperator(child);
	}
}

static void CollectLeaves(unique_ptr<LogicalOperator> &op, vector<reference<unique_ptr<LogicalOperator>>> &leaves) {
	if (op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		for (auto &child : op->children) {
			CollectLeaves(child, leaves);
		}
		return;
	}
	leaves.push_back(op);
}

static idx_t LeafPosition(const vector<reference<unique_ptr<LogicalOperator>>> &leaves,
                          const unique_ptr<LogicalOperator> &leaf) {
	for (idx_t i = 0; i < leaves.size(); i++) {
		if (&leaves[i].get() == &leaf) {
			return i;
		}
	}
	throw InternalException("ScalarAggregateMerger: a branch's leaf is not among its tree's leaves");
}

bool ScalarAggregateMerger::IsWorthMerging(const Branch &branch) {
	auto threshold = Settings::Get<ScalarAggregateMergeThresholdSetting>(optimizer.context);
	for (auto &scan : branch.scans) {
		if (!scan.get().GetTable()) {
			return true;
		}
		if (threshold != 0 && scan.get().EstimateCardinality(optimizer.context) >= threshold) {
			return true;
		}
	}
	return false;
}

void ScalarAggregateMerger::MergeCrossProduct(unique_ptr<LogicalOperator> &op) {
	vector<reference<unique_ptr<LogicalOperator>>> leaves;
	CollectLeaves(op, leaves);
	for (auto &leaf : leaves) {
		VisitOperator(leaf.get());
	}
	vector<Branch> branches;
	for (auto &leaf : leaves) {
		TryMatchBranch(leaf, branches);
	}
	// groups of branches that read the same tables, in the order their first branch appears
	vector<vector<idx_t>> groups;
	for (idx_t i = 0; i < branches.size(); i++) {
		bool placed = false;
		for (auto &group : groups) {
			if (ReadSameTables(branches[group[0]], branches[i])) {
				group.push_back(i);
				placed = true;
				break;
			}
		}
		if (!placed) {
			groups.push_back({i});
		}
	}
	// the merged leaf takes the place of a group's first branch; the other branches' leaves go
	vector<unique_ptr<LogicalOperator>> merged_leaves(leaves.size());
	vector<bool> removed(leaves.size(), false);
	bool changed = false;
	for (auto &group : groups) {
		if (group.size() < 2 || !IsWorthMerging(branches[group[0]])) {
			continue;
		}
		auto merged_leaf = MergeBranches(branches, group);
		if (!merged_leaf) {
			continue;
		}
		merged_leaves[LeafPosition(leaves, branches[group[0]].leaf.get())] = std::move(merged_leaf);
		for (idx_t k = 1; k < group.size(); k++) {
			removed[LeafPosition(leaves, branches[group[k]].leaf.get())] = true;
		}
		changed = true;
	}
	if (!changed) {
		return;
	}
	// rebuild the tree over the leaves that remain: a cross product's bindings are its children's, so their order
	// changes nothing above it
	vector<unique_ptr<LogicalOperator>> kept;
	for (idx_t i = 0; i < leaves.size(); i++) {
		if (removed[i]) {
			continue;
		}
		kept.push_back(merged_leaves[i] ? std::move(merged_leaves[i]) : std::move(leaves[i].get()));
	}
	auto tree = std::move(kept[0]);
	for (idx_t i = 1; i < kept.size(); i++) {
		tree = LogicalCrossProduct::Create(std::move(tree), std::move(kept[i]));
	}
	op = std::move(tree);
}

void ScalarAggregateMerger::TryMatchBranch(unique_ptr<LogicalOperator> &leaf, vector<Branch> &branches) {
	reference<LogicalOperator> op = *leaf;
	optional_ptr<LogicalProjection> projection;
	if (op.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projection = &op.get().Cast<LogicalProjection>();
		op = *op.get().children[0];
	}
	if (op.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return;
	}
	auto &aggregate = op.get().Cast<LogicalAggregate>();
	// an aggregate without groups or grouping sets returns exactly one row
	if (!aggregate.groups.empty() || !aggregate.grouping_functions.empty() || aggregate.grouping_sets.size() > 1 ||
	    (aggregate.grouping_sets.size() == 1 && !aggregate.grouping_sets[0].empty()) || aggregate.expressions.empty()) {
		return;
	}
	for (auto &expr : aggregate.expressions) {
		if (!IsMergeableAggregate(*expr)) {
			return;
		}
	}
	vector<reference<const Expression>> conjuncts;
	reference<unique_ptr<LogicalOperator>> relation = aggregate.children[0];
	while (relation.get()->type == LogicalOperatorType::LOGICAL_FILTER) {
		for (auto &expr : relation.get()->Cast<LogicalFilter>().expressions) {
			SplitConjunction(*expr, conjuncts);
		}
		relation = relation.get()->children[0];
	}
	for (auto &conjunct : conjuncts) {
		if (!IsSafeOnOtherRows(conjunct.get())) {
			return;
		}
	}
	vector<reference<LogicalGet>> scans;
	if (!CollectScans(*relation.get(), scans)) {
		return;
	}
	branches.push_back(Branch {leaf, projection, aggregate, relation, std::move(scans), std::move(conjuncts)});
}

unique_ptr<LogicalOperator> ScalarAggregateMerger::MergeBranches(vector<Branch> &branches, const vector<idx_t> &group) {
	auto &binder = optimizer.binder;
	auto &first = branches[group[0]];
	// every branch's scan bindings land on the first branch's scan of the same table, which gains the columns it
	// lacks once the group is known to merge
	vector<MergeTarget> targets;
	for (auto &scan : first.scans) {
		targets.push_back(MergeTarget {scan, {}});
	}
	vector<column_binding_map_t<ColumnBinding>> remaps(group.size());
	for (idx_t k = 1; k < group.size(); k++) {
		for (auto &branch_scan : branches[group[k]].scans) {
			auto &scan = branch_scan.get();
			optional_ptr<MergeTarget> target;
			for (auto &candidate : targets) {
				if (CanShareScan(candidate.scan.get(), scan)) {
					target = &candidate;
				}
			}
			D_ASSERT(target);
			auto &column_ids = scan.GetColumnIds();
			for (idx_t i = 0; i < column_ids.size(); i++) {
				// nothing refers to the empty placeholder
				if (column_ids[i].IsEmptyColumn()) {
					continue;
				}
				remaps[k][ColumnBinding(scan.table_index, ProjectionIndex(i))] =
				    TargetBinding(*target, column_ids[i].GetPrimaryIndex());
			}
		}
	}
	// the conjuncts on the target bindings, then split into the ones every branch has and what each has beyond them
	vector<vector<unique_ptr<Expression>>> conjuncts(group.size());
	for (idx_t k = 0; k < group.size(); k++) {
		for (auto &conjunct : branches[group[k]].conjuncts) {
			auto copy = conjunct.get().Copy();
			RemapBindings(*copy, remaps[k]);
			conjuncts[k].push_back(std::move(copy));
		}
	}
	vector<unique_ptr<Expression>> shared;
	for (auto &conjunct : conjuncts[0]) {
		bool everywhere = !ContainsConjunct(shared, *conjunct);
		for (idx_t k = 1; everywhere && k < group.size(); k++) {
			everywhere = ContainsConjunct(conjuncts[k], *conjunct);
		}
		if (everywhere) {
			shared.push_back(conjunct->Copy());
		}
	}
	vector<vector<unique_ptr<Expression>>> own(group.size());
	for (idx_t k = 0; k < group.size(); k++) {
		for (auto &conjunct : conjuncts[k]) {
			if (!ContainsConjunct(shared, *conjunct)) {
				own[k].push_back(std::move(conjunct));
			}
		}
	}
	// over several tables, every own conjunct must read one and the same table: the tables are then joined by the
	// shared conjuncts alone, and the OR filters that table. A join condition only some branches have would turn the
	// merged join into a filter over the tables' product.
	bool own_table_known = false;
	TableIndex own_table;
	for (auto &branch_own : own) {
		for (auto &conjunct : branch_own) {
			column_binding_set_t bindings;
			CollectBindings(*conjunct, bindings);
			for (auto &binding : bindings) {
				if (!own_table_known) {
					own_table_known = true;
					own_table = binding.table_index;
				} else if (binding.table_index != own_table) {
					return nullptr;
				}
			}
		}
	}

	// the group merges: the target scans read the added columns
	for (auto &target : targets) {
		for (auto &entry : target.added_columns) {
			auto position = target.scan.get().AddColumnId(entry.first);
			D_ASSERT(position == entry.second.column_index);
			(void)position;
		}
	}
	// each branch's own predicate (none: every row the shared conjuncts keep)
	vector<unique_ptr<Expression>> predicates(group.size());
	bool every_branch_restricted = true;
	for (idx_t k = 0; k < group.size(); k++) {
		if (own[k].empty()) {
			every_branch_restricted = false;
			continue;
		}
		predicates[k] = MakeConjunction(ExpressionType::CONJUNCTION_AND, std::move(own[k]));
	}
	// the merged aggregate: every branch's aggregates, each FILTERed to its branch's rows
	auto aggregate_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> aggregates;
	vector<idx_t> offsets(group.size());
	for (idx_t k = 0; k < group.size(); k++) {
		offsets[k] = aggregates.size();
		for (auto &expr : branches[group[k]].aggregate.get().expressions) {
			auto copy = expr->Copy();
			RemapBindings(*copy, remaps[k]);
			auto &aggregate = copy->Cast<BoundAggregateExpression>();
			if (predicates[k]) {
				auto filter = predicates[k]->Copy();
				if (aggregate.GetFilter()) {
					vector<unique_ptr<Expression>> both;
					both.push_back(std::move(filter));
					both.push_back(std::move(aggregate.GetFilterMutable()));
					filter = MakeConjunction(ExpressionType::CONJUNCTION_AND, std::move(both));
				}
				aggregate.GetFilterMutable() = std::move(filter);
			}
			aggregates.push_back(std::move(copy));
		}
	}
	// the merged projection: every branch's columns, which the plan above reads in their place
	auto projection_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> columns;
	for (idx_t k = 0; k < group.size(); k++) {
		auto &branch = branches[group[k]];
		auto &aggregate = branch.aggregate.get();
		column_binding_map_t<ColumnBinding> to_merged;
		for (idx_t j = 0; j < aggregate.expressions.size(); j++) {
			to_merged[ColumnBinding(aggregate.aggregate_index, ProjectionIndex(j))] =
			    ColumnBinding(aggregate_index, ProjectionIndex(offsets[k] + j));
		}
		if (branch.projection) {
			auto &expressions = branch.projection->expressions;
			for (idx_t i = 0; i < expressions.size(); i++) {
				auto copy = expressions[i]->Copy();
				RemapBindings(*copy, to_merged);
				replacements.emplace_back(ColumnBinding(branch.projection->table_index, ProjectionIndex(i)),
				                          ColumnBinding(projection_index, ProjectionIndex(columns.size())));
				columns.push_back(std::move(copy));
			}
		} else {
			for (idx_t j = 0; j < aggregate.expressions.size(); j++) {
				replacements.emplace_back(ColumnBinding(aggregate.aggregate_index, ProjectionIndex(j)),
				                          ColumnBinding(projection_index, ProjectionIndex(columns.size())));
				columns.push_back(make_uniq<BoundColumnRefExpression>(
				    aggregate.expressions[j]->GetReturnType(),
				    ColumnBinding(aggregate_index, ProjectionIndex(offsets[k] + j))));
			}
		}
	}

	// projection -> aggregate -> filter [shared AND (OR of the branches' own predicates)] -> the first branch's scan(s)
	unique_ptr<LogicalOperator> relation = std::move(first.relation.get());
	vector<unique_ptr<Expression>> filters = std::move(shared);
	if (every_branch_restricted) {
		// branches with the same predicate (q9 has three per range) contribute one alternative
		vector<unique_ptr<Expression>> alternatives;
		for (auto &predicate : predicates) {
			if (!ContainsConjunct(alternatives, *predicate)) {
				alternatives.push_back(predicate->Copy());
			}
		}
		// the OR is a new expression: the rewrite rules that ran before the merge (predicate factoring) derive from it
		// the single-column filters that the pushdown places on the scan and that the cardinality estimates see
		LogicalFilter rewritten;
		rewritten.expressions.push_back(MakeConjunction(ExpressionType::CONJUNCTION_OR, std::move(alternatives)));
		optimizer.rewriter.VisitOperator(rewritten);
		for (auto &expr : rewritten.expressions) {
			filters.push_back(std::move(expr));
		}
	}
	if (!filters.empty()) {
		auto filter = make_uniq<LogicalFilter>();
		filter->expressions = std::move(filters);
		filter->AddChild(std::move(relation));
		relation = std::move(filter);
	}
	auto aggregate = make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), aggregate_index, std::move(aggregates));
	aggregate->groupings_index = binder.GenerateTableIndex();
	aggregate->grouping_sets = first.aggregate.get().grouping_sets;
	aggregate->AddChild(std::move(relation));
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(columns));
	projection->AddChild(std::move(aggregate));
	projection->ResolveOperatorTypes();
	return std::move(projection);
}

} // namespace duckdb
