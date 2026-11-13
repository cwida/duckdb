#include "duckdb/optimizer/lateral_row_function.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

LateralRowFunction::LateralRowFunction(Optimizer &optimizer) : optimizer(optimizer) {
}

static unique_ptr<Expression> ColumnRef(const LogicalType &type, const ColumnBinding &binding) {
	return make_uniq<BoundColumnRefExpression>(type, binding);
}

//! The binding of an uncorrelated bare column reference
static bool PlainColumn(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &column_ref = expr.Cast<BoundColumnRefExpression>();
	if (column_ref.Depth() != 0) {
		return false;
	}
	binding = column_ref.Binding();
	return true;
}

//! Points every column reference the map knows at its new binding
static void Rebind(Expression &expr, const column_binding_map_t<ColumnBinding> &map) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column_ref = expr.Cast<BoundColumnRefExpression>();
		auto entry = map.find(column_ref.Binding());
		if (entry != map.end()) {
			column_ref.BindingMutable() = entry->second;
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { Rebind(child, map); });
}

//! Collects the bindings the expression reads; false when one of them is correlated
static bool ReferencedBindings(const Expression &expr, vector<ColumnBinding> &bindings) {
	bool uncorrelated = true;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		uncorrelated = uncorrelated && ref.Depth() == 0;
		bindings.push_back(ref.Binding());
	});
	return uncorrelated;
}

static bool IsCTERef(const LogicalOperator &op, TableIndex cte_index) {
	return op.type == LogicalOperatorType::LOGICAL_CTE_REF && op.Cast<LogicalCTERef>().cte_index == cte_index;
}

static idx_t CountCTERefs(const LogicalOperator &op, TableIndex cte_index) {
	idx_t result = IsCTERef(op, cte_index) ? 1 : 0;
	for (auto &child : op.children) {
		result += CountCTERefs(*child, cte_index);
	}
	return result;
}

//! Whether the subtree has an expression or operator whose evaluation is observable: a volatile expression or DML
static bool HasVolatileOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return true;
	default:
		break;
	}
	bool is_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expr) { is_volatile = is_volatile || (*expr)->IsVolatile(); });
	if (is_volatile) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasVolatileOperator(*child)) {
			return true;
		}
	}
	return false;
}

//! Whether two values of the type that compare equal are identical. The domain keeps one value per group of equal
//! keys and the join hands it to every left row of the group, so a key of a type in which distinguishable values
//! compare equal (0.0 and -0.0, INTERVAL 1 MONTH and INTERVAL 30 DAY, collated strings) cannot be replaced by the
//! value of the left row.
static bool EqualValuesAreIdentical(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::ENUM:
		return true;
	case LogicalTypeId::VARCHAR:
		return StringType::GetCollation(type).empty();
	case LogicalTypeId::LIST:
		return EqualValuesAreIdentical(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return EqualValuesAreIdentical(ArrayType::GetChildType(type));
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			if (!EqualValuesAreIdentical(child.second)) {
				return false;
			}
		}
		return true;
	default:
		return false;
	}
}

static bool HasVolatileExpression(LogicalOperator &op) {
	bool is_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expr) { is_volatile = is_volatile || (*expr)->IsVolatile(); });
	return is_volatile;
}

//! The unnest table function over a projection, with one list (or array) argument in the first column of the
//! projection, one output column of the element type and no ordinality: what an UNNEST over the projection computes
static bool IsConvertibleUnnestFunction(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = op.Cast<LogicalGet>();
	if (!ExpressionBinder::IsUnnestFunction(get.function.GetName()) || get.ordinality_idx.IsValid() ||
	    !get.named_parameters.empty() || op.children.size() != 1 || get.table_filters.HasFilters()) {
		return false;
	}
	auto &child = *op.children[0];
	if (child.type != LogicalOperatorType::LOGICAL_PROJECTION || child.expressions.empty()) {
		return false;
	}
	auto &argument_type = child.expressions[0]->GetReturnType();
	LogicalType element_type;
	if (argument_type.id() == LogicalTypeId::LIST) {
		element_type = ListType::GetChildType(argument_type);
	} else if (argument_type.id() == LogicalTypeId::ARRAY) {
		element_type = ArrayType::GetChildType(argument_type);
	} else {
		return false;
	}
	idx_t own_columns = 0;
	for (auto &binding : op.GetColumnBindings()) {
		if (binding.table_index != get.table_index) {
			continue;
		}
		if (binding.column_index.GetIndex() != 0) {
			return false;
		}
		own_columns++;
	}
	return own_columns == 1 && get.returned_types.size() == 1 && get.returned_types[0] == element_type;
}

//! GET(unnest) -> PROJECTION becomes UNNEST -> PROJECTION under the table index of the function: the column of the
//! function keeps its binding and the columns of the projection, of which the function passed some through, all
//! stay visible
static void ConvertUnnestFunction(ClientContext &context, unique_ptr<LogicalOperator> &op) {
	auto &get = op->Cast<LogicalGet>();
	auto table_index = get.table_index;
	auto element_type = get.returned_types[0];
	auto child = std::move(op->children[0]);
	auto &projection = child->Cast<LogicalProjection>();
	auto argument =
	    ColumnRef(child->expressions[0]->GetReturnType(), ColumnBinding(projection.table_index, ProjectionIndex(0)));
	auto unnest_expression = make_uniq<BoundUnnestExpression>(element_type);
	unnest_expression->ChildMutable() = BoundCastExpression::AddArrayCastToList(context, std::move(argument));
	auto unnest = make_uniq<LogicalUnnest>(table_index);
	unnest->expressions.push_back(std::move(unnest_expression));
	unnest->children.push_back(std::move(child));
	op = std::move(unnest);
}

//! Follows the binding from the output of the top operator of the chain down to a group of the aggregate
static bool TraceToGroup(const vector<reference<unique_ptr<LogicalOperator>>> &chain, TableIndex group_index,
                         ColumnBinding binding, idx_t &group) {
	for (auto &entry : chain) {
		auto &op = *entry.get();
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			if (binding.table_index != op.Cast<LogicalProjection>().table_index ||
			    binding.column_index.GetIndex() >= op.expressions.size() ||
			    !PlainColumn(*op.expressions[binding.column_index.GetIndex()], binding)) {
				return false;
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER:
			break;
		case LogicalOperatorType::LOGICAL_UNNEST:
			if (binding.table_index == op.Cast<LogicalUnnest>().unnest_index) {
				return false;
			}
			break;
		case LogicalOperatorType::LOGICAL_GET:
			if (binding.table_index == op.Cast<LogicalGet>().table_index) {
				return false;
			}
			break;
		default:
			return false;
		}
	}
	if (binding.table_index != group_index) {
		return false;
	}
	group = binding.column_index.GetIndex();
	return true;
}

//! Follows the binding down the column-reference projections and filters to a column of the reference
static bool TraceToReference(const vector<reference<LogicalOperator>> &below, idx_t start, TableIndex table_index,
                             ColumnBinding binding, idx_t &column) {
	for (idx_t i = start; i < below.size(); i++) {
		auto &op = below[i].get();
		if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
			continue;
		}
		if (binding.table_index != op.Cast<LogicalProjection>().table_index ||
		    binding.column_index.GetIndex() >= op.expressions.size() ||
		    !PlainColumn(*op.expressions[binding.column_index.GetIndex()], binding)) {
			return false;
		}
	}
	if (binding.table_index != table_index) {
		return false;
	}
	column = binding.column_index.GetIndex();
	return true;
}

bool LateralRowFunction::MatchDomain(LogicalOperator &op, TableIndex cte_index, Domain &domain) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = op.Cast<LogicalAggregate>();
	if (!op.expressions.empty() || aggregate.grouping_sets.size() > 1 || !aggregate.grouping_functions.empty() ||
	    aggregate.groups.empty() || op.children.size() != 1) {
		return false;
	}
	// projections and filters down to the reference
	vector<reference<LogicalOperator>> below;
	reference<LogicalOperator> source = *op.children[0];
	while (source.get().children.size() == 1 &&
	       (source.get().type == LogicalOperatorType::LOGICAL_PROJECTION ||
	        (source.get().type == LogicalOperatorType::LOGICAL_FILTER && !source.get().HasProjectionMap()))) {
		below.push_back(source);
		source = *source.get().children[0];
	}
	if (!IsCTERef(source.get(), cte_index)) {
		return false;
	}
	auto &cte_ref = source.get().Cast<LogicalCTERef>();
	if (cte_ref.is_recurring || cte_ref.correlated_columns != 0) {
		return false;
	}
	domain.table_index = cte_ref.table_index;
	for (auto &group : aggregate.groups) {
		ColumnBinding binding;
		idx_t column;
		if (!PlainColumn(*group, binding) || !TraceToReference(below, 0, domain.table_index, binding, column)) {
			return false;
		}
		domain.group_columns.push_back(column);
	}
	// a filter below the aggregate must be a function of the group columns: then it holds for a left row exactly
	// when it holds for the group of the row
	for (idx_t position = 0; position < below.size(); position++) {
		auto &filter_op = below[position].get();
		if (filter_op.type != LogicalOperatorType::LOGICAL_FILTER) {
			continue;
		}
		for (auto &expr : filter_op.expressions) {
			vector<ColumnBinding> bindings;
			if (expr->IsVolatile() || !ReferencedBindings(*expr, bindings)) {
				return false;
			}
			column_binding_map_t<ColumnBinding> map;
			for (auto &binding : bindings) {
				idx_t column;
				if (!TraceToReference(below, position + 1, domain.table_index, binding, column)) {
					return false;
				}
				bool is_group = false;
				for (auto &group_column : domain.group_columns) {
					is_group = is_group || group_column == column;
				}
				if (!is_group) {
					return false;
				}
				map[binding] = ColumnBinding(domain.table_index, ProjectionIndex(column));
			}
			auto filter = expr->Copy();
			Rebind(*filter, map);
			domain.filters.push_back(std::move(filter));
		}
	}
	return true;
}

//! Whether a column of the CTE definition is a row_number() OVER (): one value per row
static bool IsRowNumberColumn(LogicalOperator &producer, idx_t column) {
	auto bindings = producer.GetColumnBindings();
	if (column >= bindings.size()) {
		return false;
	}
	auto binding = bindings[column];
	reference<LogicalOperator> op = producer;
	while (true) {
		switch (op.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (binding.table_index != op.get().Cast<LogicalProjection>().table_index ||
			    binding.column_index.GetIndex() >= op.get().expressions.size() ||
			    !PlainColumn(*op.get().expressions[binding.column_index.GetIndex()], binding)) {
				return false;
			}
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			break;
		case LogicalOperatorType::LOGICAL_WINDOW: {
			if (binding.table_index != op.get().Cast<LogicalWindow>().window_index ||
			    binding.column_index.GetIndex() >= op.get().expressions.size()) {
				return false;
			}
			auto &expr = *op.get().expressions[binding.column_index.GetIndex()];
			return expr.GetExpressionClass() == ExpressionClass::BOUND_WINDOW &&
			       expr.GetExpressionType() == ExpressionType::WINDOW_ROW_NUMBER &&
			       expr.Cast<BoundWindowExpression>().Partitions().empty();
		}
		default:
			return false;
		}
		if (op.get().children.empty()) {
			return false;
		}
		op = *op.get().children[0];
	}
}

//! The INNER join below the operator one of whose children is a reference to the CTE. The search does not enter a
//! recursive CTE: the definition is computed once, a join inside the recursion would compute it in every iteration.
static optional_ptr<unique_ptr<LogicalOperator>> FindRowsJoin(unique_ptr<LogicalOperator> &op, TableIndex cte_index,
                                                              idx_t &rows_side) {
	if (op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return nullptr;
	}
	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op->children.size() == 2 &&
	    op->Cast<LogicalComparisonJoin>().join_type == JoinType::INNER) {
		for (idx_t side = 0; side < 2; side++) {
			if (IsCTERef(*op->children[side], cte_index)) {
				rows_side = side;
				return &op;
			}
		}
	}
	for (auto &child : op->children) {
		auto result = FindRowsJoin(child, cte_index, rows_side);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

bool LateralRowFunction::FindCandidate(unique_ptr<LogicalOperator> &cte_op, Candidate &candidate) {
	if (cte_op->type != LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		return false;
	}
	auto &cte = cte_op->Cast<LogicalMaterializedCTE>();
	if (cte.materialize != CTEMaterialize::CTE_MATERIALIZE_DEFAULT || cte_op->children.size() != 2) {
		return false;
	}
	auto cte_index = cte.table_index;
	if (CountCTERefs(*cte_op->children[1], cte_index) != 2 || CountCTERefs(*cte_op->children[0], cte_index) != 0) {
		return false;
	}
	// the definition moves to the place of the join: it must not matter where and when it is computed
	if (HasVolatileOperator(*cte_op->children[0])) {
		return false;
	}
	candidate.join = FindRowsJoin(cte_op->children[1], cte_index, candidate.rows_side);
	if (!candidate.join) {
		return false;
	}
	auto &join_op = **candidate.join;
	auto &join = join_op.Cast<LogicalComparisonJoin>();
	auto &rows_ref = join_op.children[candidate.rows_side]->Cast<LogicalCTERef>();
	if (join.filter_pushdown || rows_ref.is_recurring || rows_ref.correlated_columns != 0) {
		return false;
	}

	// the chain down to the aggregate
	reference<unique_ptr<LogicalOperator>> current = join_op.children[1 - candidate.rows_side];
	while (!MatchDomain(*current.get(), cte_index, candidate.domain)) {
		candidate.domain = Domain();
		auto &op = *current.get();
		bool supported;
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_UNNEST:
			supported = op.children.size() == 1;
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			supported = op.children.size() == 1 && !op.HasProjectionMap();
			break;
		case LogicalOperatorType::LOGICAL_GET:
			supported = IsConvertibleUnnestFunction(op);
			break;
		default:
			supported = false;
			break;
		}
		if (!supported || HasVolatileExpression(op)) {
			return false;
		}
		candidate.chain.push_back(current);
		current = op.children[0];
	}
	candidate.aggregate = &current.get();
	auto &aggregate = current.get()->Cast<LogicalAggregate>();

	// equalities between a left column and the group that reads it, as the chain passes the group up
	auto &domain = candidate.domain;
	vector<bool> covered(domain.group_columns.size(), false);
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison()) {
			return false;
		}
		auto comparison = condition.GetComparisonType();
		if (comparison != ExpressionType::COMPARE_EQUAL && comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto &rows_expr = candidate.rows_side == 0 ? condition.GetLHS() : condition.GetRHS();
		auto &chain_expr = candidate.rows_side == 0 ? condition.GetRHS() : condition.GetLHS();
		ColumnBinding rows_binding;
		ColumnBinding chain_binding;
		idx_t group;
		if (!PlainColumn(rows_expr, rows_binding) || rows_binding.table_index != rows_ref.table_index ||
		    !PlainColumn(chain_expr, chain_binding) ||
		    !TraceToGroup(candidate.chain, aggregate.group_index, chain_binding, group)) {
			return false;
		}
		auto column = rows_binding.column_index.GetIndex();
		if (group >= domain.group_columns.size() || domain.group_columns[group] != column) {
			return false;
		}
		covered[group] = true;
		if (comparison == ExpressionType::COMPARE_EQUAL) {
			bool present = false;
			for (auto &not_null_column : candidate.not_null_columns) {
				present = present || not_null_column == column;
			}
			if (!present) {
				candidate.not_null_columns.push_back(column);
			}
		}
	}
	// a joined row_number() identifies the left row by itself: every group is one left row
	for (idx_t group = 0; group < covered.size(); group++) {
		if (covered[group] && IsRowNumberColumn(*cte_op->children[0], domain.group_columns[group])) {
			return true;
		}
	}
	// otherwise every group is joined, and the chain and the operators above read the key of the left row where they
	// read the key of its group: the two must be identical, not only equal
	for (idx_t group = 0; group < covered.size(); group++) {
		if (!covered[group] || !EqualValuesAreIdentical(aggregate.groups[group]->GetReturnType())) {
			return false;
		}
	}
	return true;
}

void LateralRowFunction::Rewrite(unique_ptr<LogicalOperator> &cte_op, Candidate &candidate) {
	// the output layout of the join, before anything moves
	auto &join_ptr = *candidate.join;
	join_ptr->ResolveOperatorTypes();
	auto old_bindings = join_ptr->GetColumnBindings();
	auto old_types = join_ptr->types;
	auto rows_table_index = join_ptr->children[candidate.rows_side]->Cast<LogicalCTERef>().table_index;
	auto group_index = (*candidate.aggregate)->Cast<LogicalAggregate>().group_index;
	auto &domain = candidate.domain;

	// the left rows: the definition of the CTE, without the rows that never join (a NULL under =, a key that the
	// filters of the domain drop)
	auto producer = std::move(cte_op->children[0]);
	producer->ResolveOperatorTypes();
	auto producer_bindings = producer->GetColumnBindings();
	auto producer_types = producer->types;
	vector<unique_ptr<Expression>> conjuncts;
	for (auto &column : candidate.not_null_columns) {
		conjuncts.push_back(ExpressionFilter::CreateNullCheckExpression(
		    ColumnRef(producer_types[column], producer_bindings[column]), ExpressionType::OPERATOR_IS_NOT_NULL));
	}
	column_binding_map_t<ColumnBinding> domain_map;
	for (idx_t column = 0; column < producer_bindings.size(); column++) {
		domain_map[ColumnBinding(domain.table_index, ProjectionIndex(column))] = producer_bindings[column];
	}
	for (auto &filter : domain.filters) {
		Rebind(*filter, domain_map);
		conjuncts.push_back(std::move(filter));
	}
	if (!conjuncts.empty()) {
		auto filter = make_uniq<LogicalFilter>();
		filter->expressions = std::move(conjuncts);
		filter->children.push_back(std::move(producer));
		// filter pushdown has run already: push the new filter towards the scan (the output bindings stay)
		FilterPushdown filter_pushdown(optimizer, false);
		producer = filter_pushdown.Rewrite(std::move(filter));
	}

	// the chain reads the left rows instead of the deduplicated domain
	*candidate.aggregate = std::move(producer);
	column_binding_map_t<ColumnBinding> group_map;
	for (idx_t group = 0; group < domain.group_columns.size(); group++) {
		group_map[ColumnBinding(group_index, ProjectionIndex(group))] = producer_bindings[domain.group_columns[group]];
	}
	// bottom-up: the table function becomes an UNNEST, every projection passes the left columns through
	auto carried = producer_bindings;
	for (idx_t i = candidate.chain.size(); i > 0; i--) {
		auto &op = candidate.chain[i - 1].get();
		LogicalOperatorVisitor::EnumerateExpressions(*op,
		                                             [&](unique_ptr<Expression> *expr) { Rebind(**expr, group_map); });
		if (op->type == LogicalOperatorType::LOGICAL_GET) {
			ConvertUnnestFunction(optimizer.context, op);
		}
		if (op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto table_index = op->Cast<LogicalProjection>().table_index;
			for (idx_t column = 0; column < carried.size(); column++) {
				auto position =
				    ColumnBinding::PushExpression(op->expressions, ColumnRef(producer_types[column], carried[column]));
				carried[column] = ColumnBinding(table_index, position);
			}
		}
	}

	// a projection with the output layout of the join takes the place of the join
	auto table_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		auto &binding = old_bindings[i];
		auto column = binding.column_index.GetIndex();
		if (binding.table_index == rows_table_index) {
			select_list.push_back(ColumnRef(old_types[i], carried[column]));
		} else if (binding.table_index == group_index) {
			// a group that the chain passed up as it is (no projection in between): the left column it read
			select_list.push_back(ColumnRef(old_types[i], carried[domain.group_columns[column]]));
		} else {
			select_list.push_back(ColumnRef(old_types[i], binding));
		}
	}
	auto projection = make_uniq<LogicalProjection>(table_index, std::move(select_list));
	projection->children.push_back(std::move(join_ptr->children[1 - candidate.rows_side]));
	join_ptr = std::move(projection);

	// everything above reads the projection, and the CTE is replaced by its continuation
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		replacer.replacement_bindings.emplace_back(old_bindings[i], ColumnBinding(table_index, ProjectionIndex(i)),
		                                           old_types[i]);
	}
	replacer.stop_operator = join_ptr.get();
	replacer.VisitOperator(*cte_op->children[1]);
	auto continuation = std::move(cte_op->children[1]);
	cte_op = std::move(continuation);
	cte_op->ResolveOperatorTypes();
}

void LateralRowFunction::VisitOperator(unique_ptr<LogicalOperator> &op) {
	for (auto &child : op->children) {
		VisitOperator(child);
	}
	// the continuation of a rewritten CTE takes its place and can be a candidate itself
	while (true) {
		Candidate candidate;
		if (!FindCandidate(op, candidate)) {
			break;
		}
		Rewrite(op, candidate);
	}
}

void LateralRowFunction::Optimize(unique_ptr<LogicalOperator> &plan) {
	VisitOperator(plan);
}

} // namespace duckdb
