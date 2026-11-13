//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/logical_plan_verifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/planner/logical_plan_verification_result.hpp"

namespace duckdb {

struct LogicalPlanVerificationState;

class LogicalPlanVerifier : private ColumnBindingResolver {
public:
	static void Verify(ClientContext &context, LogicalOperator &op);
	DUCKDB_API static LogicalPlanVerificationResult<LogicalPlanVerificationSuccess> VerifyAlways(LogicalOperator &op);
	//! Check bindings against an already resolved plan without resolving or changing its output metadata.
	//! The caller supplies the resolved types; this does not infer or repair stale operator types.
	DUCKDB_API static LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>
	VerifyResolvedBindings(LogicalOperator &op);

private:
	enum class TypeResolution { RESOLVE, EXISTING };
	explicit LogicalPlanVerifier(LogicalPlanVerificationState &verification_state);

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;
	static bool ResolveOperatorTypes(LogicalOperator &op, LogicalPlanVerificationState &verification_state,
	                                 TypeResolution resolution = TypeResolution::RESOLVE);
	static void VerifyColumnBindings(LogicalOperator &op, LogicalPlanVerificationState &verification_state);
	static LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>
	VerifyAlwaysInternal(LogicalOperator &op, optional_ptr<string> first_error);

private:
	LogicalPlanVerificationState &verification_state;
};

} // namespace duckdb
