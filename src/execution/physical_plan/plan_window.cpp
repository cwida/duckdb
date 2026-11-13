#include "duckdb/execution/operator/aggregate/physical_streaming_window.hpp"
#include "duckdb/execution/operator/aggregate/physical_window.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/optimizer/apply_functional_dependencies.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder/base_select_binder.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

namespace {

using ucc_set_t = vector<column_binding_set_t>;
using bindings_t = vector<ColumnBinding>;

// §4.2: Ranking functions require UCC partitions
bool IsRankingFunction(BoundWindowExpression &wexpr) {
	//	Aggregates are not ranking
	if (wexpr.AggregateFunction()) {
		return false;
	}

	return wexpr.WindowFunction()->IsRanking();
}

bool HasDegenerateFrameCase1(ClientContext &client, BoundWindowExpression &wexpr) {
	if (IsRankingFunction(wexpr)) {
		return false;
	}

	const auto start_boundary = wexpr.WindowStart();
	const auto end_boundary = wexpr.WindowEnd();
	if (start_boundary == WindowBoundary::CURRENT_ROW_ROWS && end_boundary == WindowBoundary::CURRENT_ROW_ROWS) {
		return true;
	}

	if (start_boundary != WindowBoundary::EXPR_PRECEDING_ROWS && end_boundary != WindowBoundary::EXPR_FOLLOWING_ROWS) {
		return false;
	}

	auto &start_expr = wexpr.StartExpr();
	if (!start_expr || !start_expr->IsFoldable()) {
		return false;
	}
	const auto start_val = ExpressionExecutor::EvaluateScalar(client, *start_expr);
	if (start_val.GetValue<int64_t>()) {
		return false;
	}

	auto &end_expr = wexpr.EndExpr();
	if (!end_expr || !end_expr->IsFoldable()) {
		return false;
	}
	const auto end_val = ExpressionExecutor::EvaluateScalar(client, *end_expr);
	if (end_val.GetValue<int64_t>()) {
		return false;
	}

	return true;
}

bool HasDegenerateFrameCase2(BoundWindowExpression &wexpr) {
	if (IsRankingFunction(wexpr)) {
		return false;
	}

	switch (wexpr.WindowStart()) {
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		break;
	default:
		return false;
	}
	switch (wexpr.WindowEnd()) {
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		break;
	default:
		return false;
	}
	return wexpr.WindowExclude() == WindowExcludeMode::TIES;
}

ColumnBinding GetColumnBinding(const Expression &expr, const bindings_t &bindings) {
	switch (expr.GetExpressionType()) {
	case ExpressionType::BOUND_COLUMN_REF:
		return expr.Cast<BoundColumnRefExpression>().Binding();
	case ExpressionType::BOUND_REF:
		return bindings.at(expr.Cast<BoundReferenceExpression>().Index());
	default:
		break;
	}

	//	Return invalid binding, which will not be in the UCCs
	return ColumnBinding();
}

bool HasDegenerateFrameCase3(BoundWindowExpression &wexpr, const ucc_set_t &uccs, const bindings_t &bindings) {
	if (IsRankingFunction(wexpr)) {
		return false;
	}

	switch (wexpr.WindowStart()) {
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		break;
	default:
		return false;
	}
	switch (wexpr.WindowEnd()) {
	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::CURRENT_ROW_GROUPS:
		break;
	default:
		return false;
	}

	auto &order_bys = wexpr.OrderBy();
	if (order_bys.empty()) {
		return false;
	}

	for (const auto &ucc : uccs) {
		auto fds = ucc;
		for (const auto &order : order_bys) {
			fds.erase(GetColumnBinding(*order.expression, bindings));
		}
		if (fds.empty()) {
			return true;
		}
	}

	return false;
}

bool HasDegenerateFrameCase4(BoundWindowExpression &wexpr, const ucc_set_t &uccs, const bindings_t &bindings) {
	auto &partition_bys = wexpr.Partitions();
	if (partition_bys.empty()) {
		return false;
	}

	for (const auto &ucc : uccs) {
		auto fds = ucc;
		for (const auto &expr : partition_bys) {
			fds.erase(GetColumnBinding(*expr, bindings));
		}
		if (fds.empty()) {
			return true;
		}
	}

	return false;
}

bool HasDegenerateFrame(ClientContext &client, BoundWindowExpression &wexpr, const ucc_set_t &uccs,
                        const bindings_t &bindings) {
	//	From https://www.vldb.org/pvldb/vol19/p3525-lindner.pdf §4 Frame Analysis
	const bool case_iv = HasDegenerateFrameCase4(wexpr, uccs, bindings);
	const bool case_i_iv = case_iv || HasDegenerateFrameCase1(client, wexpr) || HasDegenerateFrameCase2(wexpr) ||
	                       HasDegenerateFrameCase3(wexpr, uccs, bindings);
	if (!case_i_iv) {
		return false;
	}

	//	Aggregates are simple
	if (wexpr.AggregateFunction()) {
		return true;
	}

	//	For window functions, we need to check their bounds needs
	const BoundWindowFunction wfunc(*wexpr.WindowFunction());
	if (!wfunc.HasBoundsCallback()) {
		return false;
	}

	WindowBoundsSet bounds;
	wfunc.GetBoundsCallback()(bounds, wexpr);

	//	Partition-only functions can only use case iv
	const auto part_bounds = bounds.count(WindowBounds::PARTITION_BEGIN) + bounds.count(WindowBounds::PARTITION_END);
	if (part_bounds == bounds.size()) {
		return case_iv;
	}

	//	Everything else can use cases i-iv
	return case_i_iv;
}

}; // namespace

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalWindow &op) {
	D_ASSERT(op.children.size() == 1);

	//	Extract the UCCs for simplifications
	auto &child = *op.children[0];
	const auto uccs = ApplyFunctionalDependencies::GetUniqueColumnCombinations(child);
	const auto bindings = child.GetColumnBindings();

	op.estimated_cardinality = op.EstimateCardinality(context);
	reference<PhysicalOperator> plan = CreatePlan(child);
#ifdef DEBUG
	for (auto &expr : op.expressions) {
		D_ASSERT(expr->IsWindow());
	}
#endif

	op.estimated_cardinality = op.EstimateCardinality(context);

	// Slice types
	auto types = op.types;
	const auto input_width = types.size() - op.expressions.size();
	types.resize(input_width);

	// Identify streaming windows, partitioned windows and degenerate frames (0,1)
	using Columns = vector<column_t>;
	const bool enable_optimizer = Settings::Get<EnableOptimizerSetting>(context);
	const bool can_stream = enable_optimizer && (plan.get().GetSources().size() == 1);
	vector<idx_t> blocking_windows;
	vector<idx_t> streaming_windows;
	vector<idx_t> partitioned_windows;
	vector<idx_t> degenerate_frames;
	vector<Columns> partitioned_columns;
	for (idx_t expr_idx = 0; expr_idx < op.expressions.size(); expr_idx++) {
		auto &wexpr = op.expressions[expr_idx]->Cast<BoundWindowExpression>();
		Columns partition_columns;
		if (HasDegenerateFrame(context, wexpr, uccs, bindings)) {
			degenerate_frames.emplace_back(expr_idx);
		} else if (can_stream && PhysicalStreamingWindow::IsStreamingFunction(context, wexpr)) {
			streaming_windows.push_back(expr_idx);
		} else if (!wexpr.Partitions().empty() &&
		           HasSingleValuePartitions(context, wexpr.Partitions(), plan, partition_columns)) {
			partitioned_windows.push_back(expr_idx);
		} else {
			blocking_windows.push_back(expr_idx);
		}
		//	Index these by expr_idx so we don't have to move them...
		partitioned_columns.emplace_back(std::move(partition_columns));
	}

	// 	Streaming takes priority over partitioning
	const bool has_streaming = !streaming_windows.empty();
	if (has_streaming) {
		for (auto &expr_idx : partitioned_windows) {
			blocking_windows.emplace_back(expr_idx);
		}
		partitioned_windows.clear();
	}

	//	Find the widest partitioning supported by the input
	if (!partitioned_windows.empty()) {
		vector<idx_t> remaining;
		auto widest = *std::max_element(partitioned_windows.begin(), partitioned_windows.end(),
		                                [&](const idx_t lhs, const idx_t rhs) {
			                                return partitioned_columns[lhs].size() < partitioned_columns[rhs].size();
		                                });
		for (auto &expr_idx : partitioned_windows) {
			if (partitioned_columns[expr_idx] == partitioned_columns[widest]) {
				remaining.emplace_back(expr_idx);
			} else {
				blocking_windows.emplace_back(expr_idx);
			}
		}
		remaining.swap(partitioned_windows);
	}

	//	Restore blocking function order
	std::sort(blocking_windows.begin(), blocking_windows.end());

	// Process the window functions by sharing the partition/order definitions
	unordered_map<idx_t, idx_t> projection_map;
	vector<vector<idx_t>> window_expressions;
	idx_t special_count = 0;
	auto output_pos = input_width;
	auto &special_windows = streaming_windows.empty() ? partitioned_windows : streaming_windows;
	while (!blocking_windows.empty() || !special_windows.empty()) {
		const bool process_blocking = special_windows.empty();
		auto &remaining = process_blocking ? blocking_windows : special_windows;
		special_count += process_blocking ? 0 : 1;

		// Find all functions that share the partitioning of the first remaining expression
		auto over_idx = remaining[0];

		vector<idx_t> matching;
		vector<idx_t> unprocessed;
		for (const auto &expr_idx : remaining) {
			D_ASSERT(op.expressions[expr_idx]->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
			auto &wexpr = op.expressions[expr_idx]->Cast<BoundWindowExpression>();

			// Just record the first one (it defines the partition)
			if (over_idx == expr_idx) {
				matching.emplace_back(expr_idx);
				continue;
			}

			// If it is in a different partition, skip it
			// Equivalent partitionings may still differ in length (e.g. PARTITION BY a, a),
			// and the shared sort layout is built from a single expression.
			const auto &over_expr = op.expressions[over_idx]->Cast<BoundWindowExpression>();
			if (!over_expr.PartitionsAreEquivalent(wexpr) ||
			    over_expr.Partitions().size() != wexpr.Partitions().size()) {
				unprocessed.emplace_back(expr_idx);
				continue;
			}

			// CSE Elimination: Search for a previous match (volatile expressions must not be deduplicated)
			bool cse = false;
			if (!wexpr.IsVolatile()) {
				for (idx_t i = 0; i < matching.size(); ++i) {
					const auto match_idx = matching[i];
					auto &match_expr = op.expressions[match_idx]->Cast<BoundWindowExpression>();
					if (wexpr.Equals(match_expr)) {
						projection_map[input_width + expr_idx] = output_pos + i;
						cse = true;
						break;
					}
				}
			}
			if (cse) {
				continue;
			}

			// Is there a common sort prefix?
			const auto prefix = over_expr.GetSharedOrders(wexpr);
			if (prefix != MinValue<idx_t>(over_expr.OrderBy().size(), wexpr.OrderBy().size())) {
				unprocessed.emplace_back(expr_idx);
				continue;
			}
			matching.emplace_back(expr_idx);

			// Switch to the longer prefix
			if (prefix < wexpr.OrderBy().size()) {
				over_idx = expr_idx;
			}
		}
		remaining.swap(unprocessed);

		// Remember the projection order
		for (const auto &expr_idx : matching) {
			projection_map[input_width + expr_idx] = output_pos++;
		}

		window_expressions.emplace_back(std::move(matching));
	}

	// Build the window operators
	for (idx_t i = 0; i < window_expressions.size(); ++i) {
		// Extract the matching expressions
		const auto &matching = window_expressions[i];
		vector<unique_ptr<Expression>> select_list;
		for (const auto &expr_idx : matching) {
			select_list.emplace_back(std::move(op.expressions[expr_idx]));
			types.emplace_back(op.types[input_width + expr_idx]);
		}

		// Chain the new window operator on top of the plan
		if (i >= special_count) {
			auto &window = Make<PhysicalWindow>(types, std::move(select_list), op.estimated_cardinality);
			window.children.push_back(plan);
			plan = window;
		} else if (has_streaming) {
			auto &window = Make<PhysicalStreamingWindow>(types, std::move(select_list), op.estimated_cardinality);
			window.children.push_back(plan);
			plan = window;
		} else {
			const auto expr_idx = matching[0];
			auto &partitions = partitioned_columns[expr_idx];
			auto &window =
			    Make<PhysicalWindow>(types, std::move(select_list), op.estimated_cardinality, std::move(partitions));
			window.children.push_back(plan);
			plan = window;
		}
	}

	// Put everything back into place if it moved
	vector<unique_ptr<Expression>> select_list(op.types.size());
	// The inputs don't move
	for (idx_t i = 0; i < input_width; ++i) {
		select_list[i] = make_uniq<BoundReferenceExpression>(op.types[i], i);
	}
	// The outputs have been rearranged
	for (const auto &p : projection_map) {
		select_list[p.first] = make_uniq<BoundReferenceExpression>(op.types[p.first], p.second);
	}
	// Evaluate the degenerate functions as scalar window functions
	FunctionBinder binder(context);
	const vector<LogicalType> input_types(op.types.begin(), op.types.begin() + NumericCast<int64_t>(input_width));
	for (const auto &expr_idx : degenerate_frames) {
		auto &wexpr = op.expressions[expr_idx]->Cast<BoundWindowExpression>();
		select_list[input_width + expr_idx] = binder.BindScalarWindowFunction(wexpr, input_types);
	}

	auto &proj = Make<PhysicalProjection>(op.types, std::move(select_list), op.estimated_cardinality);
	proj.children.push_back(plan);
	plan = proj;

	return plan;
}

} // namespace duckdb
