#include "duckdb/common/operator/add.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_limited_distinct.hpp"
#include "duckdb/execution/operator/aggregate/physical_perfecthash_aggregate.hpp"
#include "duckdb/execution/operator/helper/physical_limit.hpp"
#include "duckdb/execution/operator/helper/physical_streaming_limit.hpp"
#include "duckdb/execution/operator/helper/physical_limit_percent.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"

namespace duckdb {

bool UseBatchLimit(PhysicalOperator &child_node, BoundLimitNode &limit_val, BoundLimitNode &offset_val) {
#ifdef DUCKDB_ALTERNATIVE_VERIFY
	return true;
#else
	// we only want to use the batch limit when we are executing a complex query (e.g. involving a filter or join)
	// if we are doing a limit over a table scan we are otherwise scanning a lot of rows just to throw them away
	reference<PhysicalOperator> current_ref(child_node);
	bool finished = false;
	while (!finished) {
		auto &current_op = current_ref.get();
		switch (current_op.type) {
		case PhysicalOperatorType::TABLE_SCAN: {
			auto &table_scan = current_op.Cast<PhysicalTableScan>();
			if (table_scan.table_filters && table_scan.table_filters->HasFilters()) {
				finished = true;
				break;
			}
			// limit on a table scan without filters - never use batch limit
			return false;
		}
		case PhysicalOperatorType::PROJECTION:
			current_ref = current_op.children[0];
			break;
		default:
			finished = true;
			break;
		}
	}
	// we only use batch limit when we are computing a small amount of values
	// as the batch limit materializes this many rows PER thread
	static constexpr const idx_t BATCH_LIMIT_THRESHOLD = 10000;

	if (limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
		return false;
	}
	if (offset_val.Type() == LimitNodeType::EXPRESSION_VALUE) {
		return false;
	}
	idx_t total_offset = limit_val.GetConstantValue();
	if (offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		total_offset += offset_val.GetConstantValue();
	}
	return total_offset <= BATCH_LIMIT_THRESHOLD;
#endif
}

static optional_idx GetLimit(BoundLimitNode &limit_val, BoundLimitNode &offset_val) {
	if (limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
		return optional_idx();
	}
	if (offset_val.Type() != LimitNodeType::CONSTANT_VALUE && offset_val.Type() != LimitNodeType::UNSET) {
		return optional_idx();
	}

	idx_t total = limit_val.GetConstantValue();
	if (offset_val.Type() == LimitNodeType::CONSTANT_VALUE &&
	    !TryAddOperator::Operation(total, offset_val.GetConstantValue(), total)) {
		return optional_idx();
	}
	return optional_idx(total);
}

static bool CanUseLimitedDistinct(const PhysicalHashAggregate &hash_aggregate) {
	if (hash_aggregate.grouped_aggregate_data.groups.empty()) {
		return false;
	}
	if (hash_aggregate.grouping_sets.size() != 1) {
		return false;
	}
	if (!hash_aggregate.grouped_aggregate_data.GetGroupingFunctions().empty()) {
		return false;
	}
	if (!hash_aggregate.grouped_aggregate_data.aggregates.empty()) {
		return false;
	}
	return true;
}

static PhysicalOperator &CreateLimitedDistinct(PhysicalPlanGenerator &generator, PhysicalHashAggregate &hash_aggregate,
                                               idx_t limit) {
	D_ASSERT(hash_aggregate.children.size() == 1);
	auto &limited = generator.Make<PhysicalLimitedDistinct>(
	    hash_aggregate.types, std::move(hash_aggregate.grouped_aggregate_data.groups),
	    std::move(hash_aggregate.grouped_aggregate_data.aggregates), limit, hash_aggregate.estimated_cardinality);
	limited.children.push_back(hash_aggregate.children[0]);
	return limited;
}

static bool CanUseLimitedDistinct(const PhysicalPerfectHashAggregate &perfect_aggregate, idx_t limit) {
	if (perfect_aggregate.groups.empty() || !perfect_aggregate.aggregates.empty()) {
		return false;
	}
	// the group bounds cap the number of groups: when the limit cannot be reached a scan that stops early gains
	// nothing over the perfect hash table
	idx_t total_bits = 0;
	for (const auto &bits : perfect_aggregate.required_bits) {
		total_bits += bits;
	}
	if (total_bits < sizeof(idx_t) * 8 && (idx_t(1) << total_bits) <= limit) {
		return false;
	}
	return true;
}

static PhysicalOperator &CreateLimitedDistinct(PhysicalPlanGenerator &generator,
                                               PhysicalPerfectHashAggregate &perfect_aggregate, idx_t limit) {
	D_ASSERT(perfect_aggregate.children.size() == 1);
	auto &limited = generator.Make<PhysicalLimitedDistinct>(
	    perfect_aggregate.types, std::move(perfect_aggregate.groups), std::move(perfect_aggregate.aggregates), limit,
	    perfect_aggregate.estimated_cardinality);
	limited.children.push_back(perfect_aggregate.children[0]);
	return limited;
}

static PhysicalOperator *TryRewriteAggregate(PhysicalPlanGenerator &generator, PhysicalOperator &plan, idx_t limit) {
	if (plan.type == PhysicalOperatorType::HASH_GROUP_BY) {
		auto &hash_aggregate = plan.Cast<PhysicalHashAggregate>();
		if (!CanUseLimitedDistinct(hash_aggregate)) {
			return nullptr;
		}
		return &CreateLimitedDistinct(generator, hash_aggregate, limit);
	}
	if (plan.type == PhysicalOperatorType::PERFECT_HASH_GROUP_BY) {
		auto &perfect_aggregate = plan.Cast<PhysicalPerfectHashAggregate>();
		if (!CanUseLimitedDistinct(perfect_aggregate, limit)) {
			return nullptr;
		}
		return &CreateLimitedDistinct(generator, perfect_aggregate, limit);
	}
	return nullptr;
}

static PhysicalOperator *TryCreateLimitedDistinct(PhysicalPlanGenerator &generator, PhysicalOperator &plan,
                                                  idx_t limit) {
	if (auto *rewritten = TryRewriteAggregate(generator, plan, limit)) {
		return rewritten;
	}
	if (plan.type != PhysicalOperatorType::PROJECTION) {
		return nullptr;
	}

	auto &projection = plan.Cast<PhysicalProjection>();
	if (projection.children.size() != 1) {
		return nullptr;
	}
	auto *limited = TryRewriteAggregate(generator, projection.children[0].get(), limit);
	if (!limited) {
		return nullptr;
	}

	auto &new_projection = generator.Make<PhysicalProjection>(projection.types, std::move(projection.select_list),
	                                                          projection.estimated_cardinality);
	new_projection.children.push_back(*limited);
	return &new_projection;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalLimit &op) {
	D_ASSERT(op.children.size() == 1);
	auto &plan = CreatePlan(*op.children[0]);
	auto *limit_child = &plan;
	auto total_limit = GetLimit(op.limit_val, op.offset_val);
	if (op.offset_val.Type() != LimitNodeType::UNSET && !PreserveInsertionOrder(plan)) {
		DUCKDB_LOG_WARNING(context,
		                   "OFFSET is applied to a query whose row order is not guaranteed - the result may "
		                   "differ between runs. This happens when the query contains order-breaking operators (e.g. "
		                   "hash joins or aggregations), reads from a source that does not preserve order, or when "
		                   "preserve_insertion_order=false. Add an ORDER BY to make the result deterministic.");
	}
	// Unpartitioned hash tables become expensive to Combine at large sizes
	static constexpr idx_t LIMITED_DISTINCT_THRESHOLD = 100000;
	if (total_limit.IsValid() && total_limit.GetIndex() <= LIMITED_DISTINCT_THRESHOLD) {
		// Create the child plan first, then rewrite safe DISTINCT/GROUP BY hash aggregates into LIMITED_DISTINCT.
		// The LIMIT node still stays on top to enforce the actual LIMIT/OFFSET semantics.
		if (auto *limited_plan = TryCreateLimitedDistinct(*this, plan, total_limit.GetIndex())) {
			limit_child = limited_plan;
		}
	}

	switch (op.limit_val.Type()) {
	case LimitNodeType::EXPRESSION_PERCENTAGE:
	case LimitNodeType::CONSTANT_PERCENTAGE: {
		auto &limit = Make<PhysicalLimitPercent>(op.types, std::move(op.limit_val), std::move(op.offset_val),
		                                         op.estimated_cardinality);
		limit.children.push_back(plan);
		return limit;
	}
	default: {
		if (!PreserveInsertionOrder(*limit_child)) {
			// use parallel streaming limit if insertion order is not important
			auto &limit = Make<PhysicalStreamingLimit>(op.types, std::move(op.limit_val), std::move(op.offset_val),
			                                           op.estimated_cardinality, true);
			limit.children.push_back(*limit_child);
			return limit;
		}

		// maintaining insertion order is important
		if (UseBatchIndex(*limit_child) && UseBatchLimit(*limit_child, op.limit_val, op.offset_val)) {
			// source supports batch index: use parallel batch limit
			auto &limit = Make<PhysicalLimit>(op.types, std::move(op.limit_val), std::move(op.offset_val),
			                                  op.estimated_cardinality);
			limit.children.push_back(*limit_child);
			return limit;
		}

		// source does not support batch index: use a non-parallel streaming limit
		auto &limit = Make<PhysicalStreamingLimit>(op.types, std::move(op.limit_val), std::move(op.offset_val),
		                                           op.estimated_cardinality, false);
		limit.children.push_back(*limit_child);
		return limit;
	}
	}
}

} // namespace duckdb
