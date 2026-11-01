#include "duckdb/common/enums/optimizer_type.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/optimizer.hpp"

namespace duckdb {

struct DefaultOptimizerType {
	const char *name;
	OptimizerType type;
};

static const DefaultOptimizerType internal_optimizer_types[] = {
    {"expression_rewriter", OptimizerType::EXPRESSION_REWRITER},
    {"filter_pullup", OptimizerType::FILTER_PULLUP},
    {"filter_pushdown", OptimizerType::FILTER_PUSHDOWN},
    {"empty_result_pullup", OptimizerType::EMPTY_RESULT_PULLUP},
    {"cte_filter_pusher", OptimizerType::CTE_FILTER_PUSHER},
    {"regex_range", OptimizerType::REGEX_RANGE},
    {"in_clause", OptimizerType::IN_CLAUSE},
    {"join_order", OptimizerType::JOIN_ORDER},
    {"deliminator", OptimizerType::DELIMINATOR},
    {"unnest_rewriter", OptimizerType::UNNEST_REWRITER},
    {"unused_columns", OptimizerType::UNUSED_COLUMNS},
    {"statistics_propagation", OptimizerType::STATISTICS_PROPAGATION},
    {"common_subexpressions", OptimizerType::COMMON_SUBEXPRESSIONS},
    {"common_aggregate", OptimizerType::COMMON_AGGREGATE},
    {"column_lifetime", OptimizerType::COLUMN_LIFETIME},
    {"limit_pushdown", OptimizerType::LIMIT_PUSHDOWN},
    {"row_group_pruner", OptimizerType::ROW_GROUP_PRUNER},
    {"top_n", OptimizerType::TOP_N},
    {"top_n_window_elimination", OptimizerType::TOP_N_WINDOW_ELIMINATION},
    {"build_side_probe_side", OptimizerType::BUILD_SIDE_PROBE_SIDE},
    {"compressed_materialization", OptimizerType::COMPRESSED_MATERIALIZATION},
    {"duplicate_groups", OptimizerType::DUPLICATE_GROUPS},
    {"reorder_filter", OptimizerType::REORDER_FILTER},
    {"sampling_pushdown", OptimizerType::SAMPLING_PUSHDOWN},
    {"join_filter_pushdown", OptimizerType::JOIN_FILTER_PUSHDOWN},
    {"extension", OptimizerType::EXTENSION},
    {"materialized_cte", OptimizerType::MATERIALIZED_CTE},
    {"aggregate_function_rewriter", OptimizerType::AGGREGATE_FUNCTION_REWRITER},
    {"late_materialization", OptimizerType::LATE_MATERIALIZATION},
    {"cte_inlining", OptimizerType::CTE_INLINING},
    {"common_subplan", OptimizerType::COMMON_SUBPLAN},
    {"join_elimination", OptimizerType::JOIN_ELIMINATION},
    {"window_self_join", OptimizerType::WINDOW_SELF_JOIN},
    {"window_join_pushdown", OptimizerType::WINDOW_JOIN_PUSHDOWN},
    {"projection_pullup", OptimizerType::PROJECTION_PULLUP},
    {"outer_join_simplification", OptimizerType::OUTER_JOIN_SIMPLIFICATION},
    {"window_rewriter", OptimizerType::ROW_NUMBER_REWRITER},
    {"partitioned_execution", OptimizerType::PARTITIONED_EXECUTION},
    {"partial_aggregate_pushdown", OptimizerType::PARTIAL_AGGREGATE_PUSHDOWN},
    {"remote_pushdown", OptimizerType::REMOTE_PUSHDOWN},
    {"grouping_sets", OptimizerType::GROUPING_SETS},
    {"type_pushdown", OptimizerType::TYPE_PUSHDOWN},
    {"scalar_fn_pushdown", OptimizerType::SCALAR_FN_PUSHDOWN},
    {"distinct_aggregate_rewrite", OptimizerType::DISTINCT_AGGREGATE_REWRITE},
    {"aggregate_reuse", OptimizerType::AGGREGATE_REUSE},
    {"projection_placement", OptimizerType::PROJECTION_PLACEMENT},
    {"redundant_order_keys", OptimizerType::REDUNDANT_ORDER_KEYS},
    {"disjunctive_join_rewriter", OptimizerType::DISJUNCTIVE_JOIN_REWRITER},
    {"redundant_order", OptimizerType::REDUNDANT_ORDER},
    {nullptr, OptimizerType::INVALID}};

string OptimizerTypeToString(OptimizerType type) {
	for (idx_t i = 0; internal_optimizer_types[i].name; i++) {
		if (internal_optimizer_types[i].type == type) {
			return internal_optimizer_types[i].name;
		}
	}
	throw InternalException("Invalid optimizer type");
}

vector<string> ListAllOptimizers() {
	vector<string> result;
	for (idx_t i = 0; internal_optimizer_types[i].name; i++) {
		result.push_back(internal_optimizer_types[i].name);
	}
	return result;
}

OptimizerType OptimizerTypeFromString(const string &str) {
	for (idx_t i = 0; internal_optimizer_types[i].name; i++) {
		if (internal_optimizer_types[i].name == str) {
			return internal_optimizer_types[i].type;
		}
	}
	// optimizer not found, construct candidate list
	vector<string> optimizer_names;
	for (idx_t i = 0; internal_optimizer_types[i].name; i++) {
		optimizer_names.emplace_back(internal_optimizer_types[i].name);
	}
	throw ParserException("Optimizer type \"%s\" not recognized\n%s", str,
	                      StringUtil::CandidatesErrorMessage(optimizer_names, str, "Candidate optimizers"));
}

} // namespace duckdb
