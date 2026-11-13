#include "duckdb/execution/physical_plan_generator.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/execution/operator/helper/physical_verify_vector.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

PhysicalPlanGenerator::PhysicalPlanGenerator(ClientContext &context) : context(context) {
}

PhysicalPlanGenerator::~PhysicalPlanGenerator() {
}

unique_ptr<PhysicalPlan> PhysicalPlanGenerator::Plan(unique_ptr<LogicalOperator> op) {
	auto &plan = ResolveAndPlan(std::move(op));
	plan.Verify();
	return std::move(physical_plan);
}

PhysicalOperator &PhysicalPlanGenerator::ResolveAndPlan(unique_ptr<LogicalOperator> op) {
	auto &profiler = QueryProfiler::Get(context);

	// Resolve the types of each operator.
	{
		auto timer = profiler.StartTimer<MetricPhysicalPlannerResolveTypes>();
		op->ResolveOperatorTypes();
	}

	// Resolve the column references.
	{
		auto timer = profiler.StartTimer<MetricPhysicalPlannerColumnBinding>();
		ColumnBindingResolver resolver;
		resolver.VisitOperator(*op);
	}

	// Create the main physical plan.
	{
		auto timer = profiler.StartTimer<MetricPhysicalPlannerCreatePlan>();
		physical_plan = PlanInternal(*op);
	}

	// Return a reference to the root of this plan.
	return physical_plan->Root();
}

unique_ptr<PhysicalPlan> PhysicalPlanGenerator::PlanInternal(LogicalOperator &op) {
	if (!physical_plan) {
		physical_plan = make_uniq<PhysicalPlan>(Allocator::Get(context));
	}
	op.estimated_cardinality = op.EstimateCardinality(context);
	physical_plan->SetRoot(CreatePlan(op));
	physical_plan->Root().estimated_cardinality = op.estimated_cardinality;

	auto debug_verify_vector = Settings::Get<DebugVerifyVectorSetting>(context);
	if (debug_verify_vector != DebugVectorVerification::NONE) {
		if (debug_verify_vector != DebugVectorVerification::DICTIONARY_EXPRESSION &&
		    debug_verify_vector != DebugVectorVerification::VARIANT_VECTOR &&
		    debug_verify_vector != DebugVectorVerification::SHREDDED_VECTOR) {
			physical_plan->SetRoot(Make<PhysicalVerifyVector>(physical_plan->Root(), debug_verify_vector));
		}
	}
	return std::move(physical_plan);
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalOperator &op) {
	const auto repeatable = ClassifyLogicalOperatorRepeatability(op) == LogicalOperatorRepeatability::REPEATABLE;
	auto &result = CreatePlanInternal(op);
	if (!repeatable) {
		non_repeatable_operators.insert(result);
	}
	return result;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlanInternal(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		return CreatePlan(op.Cast<LogicalGet>());
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return CreatePlan(op.Cast<LogicalProjection>());
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return CreatePlan(op.Cast<LogicalEmptyResult>());
	case LogicalOperatorType::LOGICAL_FILTER:
		return CreatePlan(op.Cast<LogicalFilter>());
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		return CreatePlan(op.Cast<LogicalAggregate>());
	case LogicalOperatorType::LOGICAL_WINDOW:
		return CreatePlan(op.Cast<LogicalWindow>());
	case LogicalOperatorType::LOGICAL_UNNEST:
		return CreatePlan(op.Cast<LogicalUnnest>());
	case LogicalOperatorType::LOGICAL_LIMIT:
		return CreatePlan(op.Cast<LogicalLimit>());
	case LogicalOperatorType::LOGICAL_SAMPLE:
		return CreatePlan(op.Cast<LogicalSample>());
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		return CreatePlan(op.Cast<LogicalOrder>());
	case LogicalOperatorType::LOGICAL_TOP_N:
		return CreatePlan(op.Cast<LogicalTopN>());
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		return CreatePlan(op.Cast<LogicalCopyToFile>());
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		return CreatePlan(op.Cast<LogicalDummyScan>());
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		return CreatePlan(op.Cast<LogicalAnyJoin>());
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		return CreatePlan(op.Cast<LogicalComparisonJoin>());
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return CreatePlan(op.Cast<LogicalCrossProduct>());
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
		return CreatePlan(op.Cast<LogicalPositionalJoin>());
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
		return CreatePlan(op.Cast<LogicalSetOperation>());
	case LogicalOperatorType::LOGICAL_INSERT:
		return CreatePlan(op.Cast<LogicalInsert>());
	case LogicalOperatorType::LOGICAL_DELETE:
		return CreatePlan(op.Cast<LogicalDelete>());
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
		return CreatePlan(op.Cast<LogicalColumnDataGet>());
	case LogicalOperatorType::LOGICAL_DELIM_GET:
		return CreatePlan(op.Cast<LogicalDelimGet>());
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		return CreatePlan(op.Cast<LogicalExpressionGet>());
	case LogicalOperatorType::LOGICAL_UPDATE:
		return CreatePlan(op.Cast<LogicalUpdate>());
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return CreatePlan(op.Cast<LogicalMergeInto>());
	case LogicalOperatorType::LOGICAL_TRIGGER:
		throw InternalException("LogicalTrigger must be rewritten before physical planning");
	case LogicalOperatorType::LOGICAL_CREATE_TABLE:
		return CreatePlan(op.Cast<LogicalCreateTable>());
	case LogicalOperatorType::LOGICAL_CREATE_INDEX:
		return CreatePlan(op.Cast<LogicalCreateIndex>());
	case LogicalOperatorType::LOGICAL_CREATE_SECRET:
		return CreatePlan(op.Cast<LogicalCreateSecret>());
	case LogicalOperatorType::LOGICAL_EXPLAIN:
		return CreatePlan(op.Cast<LogicalExplain>());
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return CreatePlan(op.Cast<LogicalDistinct>());
	case LogicalOperatorType::LOGICAL_PREPARE:
		return CreatePlan(op.Cast<LogicalPrepare>());
	case LogicalOperatorType::LOGICAL_CREATE_VIEW:
	case LogicalOperatorType::LOGICAL_CREATE_SEQUENCE:
	case LogicalOperatorType::LOGICAL_CREATE_SCHEMA:
	case LogicalOperatorType::LOGICAL_CREATE_MACRO:
	case LogicalOperatorType::LOGICAL_CREATE_TYPE:
	case LogicalOperatorType::LOGICAL_CREATE_TRIGGER:
		return CreatePlan(op.Cast<LogicalCreate>());
	case LogicalOperatorType::LOGICAL_PRAGMA:
		return CreatePlan(op.Cast<LogicalPragma>());
	case LogicalOperatorType::LOGICAL_VACUUM:
		return CreatePlan(op.Cast<LogicalVacuum>());
	case LogicalOperatorType::LOGICAL_TRANSACTION:
		return CreatePlan(op.Cast<LogicalTransaction>());
	case LogicalOperatorType::LOGICAL_ALTER:
		return CreatePlan(op.Cast<LogicalAlter>());
	case LogicalOperatorType::LOGICAL_DROP:
		return CreatePlan(op.Cast<LogicalDrop>());
	case LogicalOperatorType::LOGICAL_LOAD:
		return CreatePlan(op.Cast<LogicalLoad>());
	case LogicalOperatorType::LOGICAL_ATTACH:
		return CreatePlan(op.Cast<LogicalAttach>());
	case LogicalOperatorType::LOGICAL_DETACH:
		return CreatePlan(op.Cast<LogicalDetach>());
	case LogicalOperatorType::LOGICAL_CONNECT:
		return CreatePlan(op.Cast<LogicalConnect>());
	case LogicalOperatorType::LOGICAL_DISCONNECT:
		return CreatePlan(op.Cast<LogicalDisconnect>());
	case LogicalOperatorType::LOGICAL_EXTERNAL_RESOURCE:
		return CreatePlan(op.Cast<LogicalExternalResource>());
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
		return CreatePlan(op.Cast<LogicalRecursiveCTE>());
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		return CreatePlan(op.Cast<LogicalMaterializedCTE>());
	case LogicalOperatorType::LOGICAL_CTE_REF:
		return CreatePlan(op.Cast<LogicalCTERef>());
	case LogicalOperatorType::LOGICAL_EXPORT:
		return CreatePlan(op.Cast<LogicalExport>());
	case LogicalOperatorType::LOGICAL_SET:
		return CreatePlan(op.Cast<LogicalSet>());
	case LogicalOperatorType::LOGICAL_RESET:
		return CreatePlan(op.Cast<LogicalReset>());
	case LogicalOperatorType::LOGICAL_PIVOT:
		return CreatePlan(op.Cast<LogicalPivot>());
	case LogicalOperatorType::LOGICAL_COPY_DATABASE:
		return CreatePlan(op.Cast<LogicalCopyDatabase>());
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		return CreatePlan(op.Cast<LogicalSecureView>());
	case LogicalOperatorType::LOGICAL_UPDATE_EXTENSIONS:
		return CreatePlan(op.Cast<LogicalUpdateExtensions>());
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
		auto &extension_op = op.Cast<LogicalExtensionOperator>();
		return extension_op.CreatePlan(context, *this);
	}
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
	case LogicalOperatorType::LOGICAL_INVALID: {
		throw NotImplementedException("Unimplemented logical operator type!");
	}
	}
	throw InternalException("Physical plan generator - no plan generated");
}

ArenaAllocator &PhysicalPlanGenerator::ArenaRef() {
	if (!physical_plan) {
		physical_plan = make_uniq<PhysicalPlan>(Allocator::Get(context));
	}
	return physical_plan->ArenaRef();
}

} // namespace duckdb
