#include "duckdb/parser/query_node/trampoline_cte_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_trampoline.hpp"

namespace duckdb {

BoundStatement Binder::BindNode(TrampolineCTENode &statement) {
	D_ASSERT(statement.seed);
	D_ASSERT(!statement.branches.empty());

	auto ctename = statement.ctename;
	auto setop_index = GenerateTableIndex();

	auto seed_binder = Binder::CreateBinder(context, this);
	auto seed = seed_binder->BindNode(*statement.seed);

	BoundStatement result;
	// the schema of the trampoline is the schema of the seed
	result.types = seed.types;
	result.names = seed.names;
	for (idx_t i = 0; i < statement.aliases.size() && i < result.names.size(); i++) {
		if (statement.aliases[i].empty()) {
			throw BinderException("column alias cannot be empty");
		}
		result.names[i] = statement.aliases[i];
	}

	// resolve the selector column against the trampoline schema
	idx_t selector_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < result.names.size(); i++) {
		if (statement.selector == result.names[i]) {
			if (selector_idx != DConstants::INVALID_INDEX) {
				throw BinderException("TRAMPOLINE selector \"%s\" is ambiguous in CTE \"%s\"",
				                      statement.selector.GetIdentifierName(), ctename.GetIdentifierName());
			}
			selector_idx = i;
		}
	}
	if (selector_idx == DConstants::INVALID_INDEX) {
		throw BinderException("TRAMPOLINE selector column \"%s\" was not found in CTE \"%s\"",
		                      statement.selector.GetIdentifierName(), ctename.GetIdentifierName());
	}
	switch (result.types[selector_idx].id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		break;
	default:
		throw BinderException("TRAMPOLINE selector column \"%s\" must have an integer type, not %s",
		                      statement.selector.GetIdentifierName(), result.types[selector_idx].ToString());
	}

	if (!statement.modifiers.empty()) {
		throw BinderException("result modifiers are not supported on a TRAMPOLINE CTE; "
		                      "apply them in the outer query instead");
	}

	// bind every branch with the trampoline relation in scope
	vector<unique_ptr<LogicalOperator>> branch_plans;
	BindingAlias cte_alias(ctename);
	for (idx_t branch_idx = 0; branch_idx < statement.branches.size(); branch_idx++) {
		auto branch_binder = Binder::CreateBinder(context, this);
		branch_binder->bind_context.AddCTEBinding(setop_index, cte_alias, result.names, result.types);
		auto branch = branch_binder->BindNode(*statement.branches[branch_idx]);
		if (!branch_binder->correlated_columns.empty()) {
			throw NotImplementedException("correlated TRAMPOLINE CTEs are not supported yet");
		}
		if (branch.types.size() != result.types.size()) {
			throw BinderException(
			    "BRANCH %llu of TRAMPOLINE CTE \"%s\" returns %llu columns, but the seed defines %llu", branch_idx + 1,
			    ctename.GetIdentifierName(), branch.types.size(), result.types.size());
		}
		branch_plans.push_back(CastLogicalOperatorToTypes(branch.types, result.types, std::move(branch.plan)));
	}
	if (!seed_binder->correlated_columns.empty()) {
		throw NotImplementedException("correlated TRAMPOLINE CTEs are not supported yet");
	}

	auto seed_plan = std::move(seed.plan);
	auto root = make_uniq<LogicalTrampoline>(ctename, setop_index, result.types.size(), selector_idx,
	                                         std::move(seed_plan), std::move(branch_plans));
	root->branch_tee = statement.branch_tee;
	root->branch_tee.resize(statement.branches.size(), false);
	result.plan = std::move(root);
	return result;
}

} // namespace duckdb
