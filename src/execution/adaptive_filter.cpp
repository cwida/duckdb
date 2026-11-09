#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/execution/adaptive_filter.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"

namespace duckdb {

AdaptiveFilter::AdaptiveFilter(const Expression &expr) : observe_interval(10), execute_interval(20), warmup(true) {
	auto &conj_expr = expr.Cast<BoundConjunctionExpression>();
	D_ASSERT(conj_expr.GetChildren().size() > 1);
	for (idx_t idx = 0; idx < conj_expr.GetChildren().size(); idx++) {
		permutation.push_back(idx);
		if (conj_expr.GetChildren()[idx]->CanThrow() || ExpressionBarrier::Contains(*conj_expr.GetChildren()[idx])) {
			disable_permutations = true;
		}
		if (idx != conj_expr.GetChildren().size() - 1) {
			swap_likeliness.push_back(100);
		}
	}
	right_random_border = 100 * (conj_expr.GetChildren().size() - 1);
}

AdaptiveFilter::AdaptiveFilter(const TableFilterSet &table_filters, vector<idx_t> filter_global_pos_p)
    : observe_interval(10), execute_interval(20), warmup(true) {
	permutation = ExpressionHeuristics::GetInitialOrder(table_filters);
	for (auto &entry : table_filters) {
		auto &expr_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "AdaptiveFilter");
		if (expr_filter.expr->CanThrow() || ExpressionBarrier::Contains(*expr_filter.expr)) {
			// a filter that can throw must keep the order the optimizer put it in
			disable_permutations = true;
		}
	}
	for (idx_t idx = 1; idx < table_filters.FilterCount(); idx++) {
		swap_likeliness.push_back(100);
	}
	filter_global_pos = std::move(filter_global_pos_p);
	right_random_border = 100 * (table_filters.FilterCount() - 1);
}

bool AdaptiveFilter::Remap(const TableFilterSet &new_filters, vector<idx_t> new_ids) {
	if (new_ids.size() != filter_global_pos.size() || new_ids.size() != new_filters.FilterCount()) {
		// missing filter cant remap
		return false;
	}
	unordered_map<idx_t, idx_t> new_position_by_identity;
	for (idx_t i = 0; i < new_ids.size(); i++) {
		new_position_by_identity.emplace(new_ids[i], i);
	}
	vector<idx_t> old_to_new(filter_global_pos.size());
	for (idx_t i = 0; i < filter_global_pos.size(); i++) {
		auto it = new_position_by_identity.find(filter_global_pos[i]);
		if (it == new_position_by_identity.end()) {
			return false;
		}
		old_to_new[i] = it->second;
	}
	for (auto &p : permutation) {
		p = old_to_new[p];
	}
	filter_global_pos = std::move(new_ids);
	right_random_border = 100 * (new_filters.FilterCount() - 1);
	return true;
}

void AdaptiveFilter::SetLogger(shared_ptr<Logger> logger_p, string file_path, AdaptiveFilterSource source,
                               const vector<idx_t> &filter_identities) {
	logger = std::move(logger_p);
	log_file_path = std::move(file_path);
	if (!logger) {
		return;
	}
	DUCKDB_LOG(logger, AdaptiveFilterLogType, "INIT", log_file_path, permutation,
	           BuildInitInfo(source, filter_identities));
}

vector<pair<string, string>> AdaptiveFilter::BuildInitInfo(AdaptiveFilterSource source,
                                                           const vector<idx_t> &filter_identities) const {
	vector<pair<string, string>> info;
	info.emplace_back("source", EnumUtil::ToChars(source));
	if (!filter_identities.empty() && filter_identities.size() == permutation.size()) {
		auto columns_str = "[" +
		                   StringUtil::Join(permutation, permutation.size(), ", ",
		                                    [&](idx_t p) { return to_string(filter_identities[p]); }) +
		                   "]";
		info.emplace_back("columns", std::move(columns_str));
	}
	return info;
}

AdaptiveFilterState AdaptiveFilter::BeginFilter() const {
	if (permutation.size() <= 1 || disable_permutations) {
		return AdaptiveFilterState();
	}
	AdaptiveFilterState state;
	state.monotonic_start = TimePoint::Tick();
	return state;
}

void AdaptiveFilter::EndFilter(AdaptiveFilterState state) {
	if (permutation.size() <= 1 || disable_permutations) {
		// nothing to permute
		return;
	}
	AdaptRuntimeStatistics(state.monotonic_start.ElapsedSeconds());
}

void AdaptiveFilter::AdaptRuntimeStatistics(double duration) {
	iteration_count++;
	runtime_sum += duration;

	D_ASSERT(!disable_permutations);
	if (!warmup) {
		// the last swap was observed
		if (observe && iteration_count == observe_interval) {
			// keep swap if runtime decreased, else reverse swap
			auto trial_mean = runtime_sum / static_cast<double>(iteration_count);
			const char *action;
			if (prev_mean - trial_mean <= 0) {
				// reverse swap because runtime didn't decrease
				std::swap(permutation[swap_idx], permutation[swap_idx + 1]);

				// decrease swap likeliness, but make sure there is always a small likeliness left
				if (swap_likeliness[swap_idx] > 1) {
					swap_likeliness[swap_idx] /= 2;
				}
				action = "reverted";
			} else {
				// keep swap because runtime decreased, reset likeliness
				swap_likeliness[swap_idx] = 100;
				action = "kept";
			}
			if (logger) {
				DUCKDB_LOG(
				    logger, AdaptiveFilterLogType, "REORDER", log_file_path, permutation,
				    (vector<pair<string, string>> {{"action", action},
				                                   {"swap_idx", to_string(swap_idx)},
				                                   {"prev_mean_us", StringUtil::Format("%.3f", prev_mean * 1e6)},
				                                   {"trial_mean_us", StringUtil::Format("%.3f", trial_mean * 1e6)}}));
			}
			observe = false;

			// reset values
			iteration_count = 0;
			runtime_sum = 0.0;
		} else if (!observe && iteration_count == execute_interval) {
			// save old mean to evaluate swap
			prev_mean = runtime_sum / static_cast<double>(iteration_count);

			// get swap index and swap likeliness
			// a <= i <= b
			if (!generator) {
				generator = make_uniq<RandomEngine>();
			}
			auto random_number = generator->NextRandomInteger(1, NumericCast<uint32_t>(right_random_border));

			swap_idx = random_number / 100;                    // index to be swapped
			idx_t likeliness = random_number - 100 * swap_idx; // random number between [0, 100)

			// check if swap is going to happen
			if (swap_likeliness[swap_idx] > likeliness) { // always true for the first swap of an index
				// swap
				std::swap(permutation[swap_idx], permutation[swap_idx + 1]);

				// observe whether swap will be applied
				observe = true;
			}

			// reset values
			iteration_count = 0;
			runtime_sum = 0.0;
		}
	} else {
		if (iteration_count == 5) {
			// initially set all values
			iteration_count = 0;
			runtime_sum = 0.0;
			observe = false;
			warmup = false;
		}
	}
}

} // namespace duckdb
