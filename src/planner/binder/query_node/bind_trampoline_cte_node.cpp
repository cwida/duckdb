#include "duckdb/main/settings.hpp"
#include "duckdb/parser/query_node/trampoline_cte_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_trampoline.hpp"

namespace duckdb {

//! Whether grouping the type's physical values implements SQL DISTINCT equality:
//! any effective collation, explicit or from the default-collation setting, at any
//! nesting level makes SQL equality coarser than physical equality.
static bool TypeUsesPhysicalDistinctEquality(ClientContext &context, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR: {
		auto collation = StringType::GetCollation(type);
		if (collation.empty()) {
			collation = Settings::Get<DefaultCollationSetting>(context);
		}
		collation = StringUtil::Lower(collation);
		return collation.empty() || collation == "binary" || collation == "c" || collation == "posix";
	}
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::UNION: {
		for (auto &entry : StructType::GetChildTypes(type)) {
			if (!TypeUsesPhysicalDistinctEquality(context, entry.second)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return TypeUsesPhysicalDistinctEquality(context, ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return TypeUsesPhysicalDistinctEquality(context, ArrayType::GetChildType(type));
	default:
		return true;
	}
}

//! Finds frontier references to the given trampoline in a bound branch plan. A
//! reference below a nested trampoline would cross round scopes and is rejected.
static void DetectFrontierReferences(const LogicalOperator &op, TableIndex cte_index, idx_t nested_depth,
                                     bool &reads_frontier) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		if (ref.is_frontier && ref.cte_index == cte_index) {
			if (nested_depth > 0) {
				throw NotImplementedException("frontier references across nested TRAMPOLINE CTEs are not supported");
			}
			reads_frontier = true;
		}
	}
	const auto next_depth = nested_depth + (op.type == LogicalOperatorType::LOGICAL_TRAMPOLINE ? 1 : 0);
	for (auto &child : op.children) {
		DetectFrontierReferences(*child, cte_index, next_depth, reads_frontier);
	}
}

BoundStatement Binder::BindNode(TrampolineCTENode &statement) {
	D_ASSERT(statement.seed);
	D_ASSERT(!statement.branches.empty());

	auto ctename = statement.ctename;
	auto setop_index = GenerateTableIndex();

	auto seed_binder = Binder::CreateBinder(context, this);
	{
		// keep frontier.<name> in scope during the seed so it cannot resolve to an
		// outer trampoline's frontier, but reject any reference: the frontier does
		// not exist before the first round
		auto guard = make_uniq<CTEBinding>(
		    BindingAlias(Identifier("frontier"), ctename), vector<LogicalType> {LogicalType::INTEGER},
		    vector<Identifier> {Identifier("frontier_guard")}, setop_index, CTEType::CAN_BE_REFERENCED);
		guard->frontier_seed_guard = true;
		seed_binder->bind_context.AddCTEBinding(std::move(guard));
	}
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
		branch_binder->bind_context.AddCTEBinding(setop_index, BindingAlias(Identifier("frontier"), ctename),
		                                          result.names, result.types);
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
	root->branch_distinct = statement.branch_distinct;
	root->branch_distinct.resize(statement.branches.size(), false);
	root->branch_merge = statement.branch_merge;
	root->branch_merge.resize(statement.branches.size(), false);
	bool wants_admission = statement.emit_distinct;
	for (auto distinct : root->branch_distinct) {
		wants_admission = wants_admission || distinct;
	}
	if (wants_admission) {
		// admission hashes physical values; collation-aware equality is not wired up
		for (auto &type : result.types) {
			if (!TypeUsesPhysicalDistinctEquality(context, type)) {
				throw NotImplementedException(
				    "DISTINCT admission on a TRAMPOLINE CTE with collated columns is not supported");
			}
		}
	}
	root->emit_distinct = statement.emit_distinct;
	for (idx_t branch_idx = 1; branch_idx < root->children.size(); branch_idx++) {
		DetectFrontierReferences(*root->children[branch_idx], setop_index, 0, root->reads_frontier);
	}
	result.plan = std::move(root);
	return result;
}

} // namespace duckdb
