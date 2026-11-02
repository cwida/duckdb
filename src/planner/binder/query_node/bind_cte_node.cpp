#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/parser/query_node/list.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

struct BoundCTEData {
	Identifier ctename;
	CTEMaterialize materialized;
	TableIndex setop_index;
	shared_ptr<Binder> child_binder;
	shared_ptr<CTEBindState> cte_bind_state;
};

static bool IsDMLQueryNode(QueryNodeType t) {
	return t == QueryNodeType::INSERT_QUERY_NODE || t == QueryNodeType::UPDATE_QUERY_NODE ||
	       t == QueryNodeType::DELETE_QUERY_NODE || t == QueryNodeType::MERGE_QUERY_NODE;
}

static bool IsSideEffectingQueryNode(QueryNodeType type) {
	return IsDMLQueryNode(type) || type == QueryNodeType::COPY_QUERY_NODE;
}

BoundStatement Binder::BindNode(QueryNode &node) {
	reference<Binder> current_binder(*this);
	vector<BoundCTEData> bound_ctes;
	idx_t dml_cte_count = 0;
	for (auto &cte : node.cte_map.map) {
		auto cte_type = cte.second->query_node->type;
		if (IsSideEffectingQueryNode(cte_type)) {
			if (parent && !cte.second->is_trigger_generated) {
				if (cte_type == QueryNodeType::COPY_QUERY_NODE) {
					throw BinderException("WITH clause containing a COPY statement must be at the top level");
				}
				throw BinderException("WITH clause containing a data-modifying statement must be at the top level");
			}
		}
		if (IsDMLQueryNode(cte_type)) {
			++dml_cte_count;
		}
		bound_ctes.push_back(current_binder.get().PrepareCTE(cte.first, *cte.second));
		if (current_binder.get().IsBindingTrySubquery()) {
			// the child binder binds the same query node, so TRY lowering follows it
			bound_ctes.back().child_binder->SetBindingTrySubquery();
		}
		current_binder = *bound_ctes.back().child_binder;
	}
	BoundStatement result;
	// now we bind the node
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		result = current_binder.get().BindNode(node.Cast<SelectNode>());
		break;
	case QueryNodeType::RECURSIVE_CTE_NODE:
		result = current_binder.get().BindNode(node.Cast<RecursiveCTENode>());
		break;
	case QueryNodeType::SET_OPERATION_NODE:
		result = current_binder.get().BindNode(node.Cast<SetOperationNode>());
		break;
	case QueryNodeType::STATEMENT_NODE:
		result = current_binder.get().BindNode(node.Cast<StatementNode>());
		break;
	case QueryNodeType::UPDATE_QUERY_NODE:
		result = current_binder.get().BindNode(node.Cast<UpdateQueryNode>());
		break;
	case QueryNodeType::DELETE_QUERY_NODE:
		result = current_binder.get().BindNode(node.Cast<DeleteQueryNode>());
		break;
	case QueryNodeType::INSERT_QUERY_NODE:
		result = current_binder.get().BindNode(node.Cast<InsertQueryNode>());
		break;
	case QueryNodeType::MERGE_QUERY_NODE:
		result = current_binder.get().BindNode(node.Cast<MergeQueryNode>());
		break;
	case QueryNodeType::COPY_QUERY_NODE:
		result = current_binder.get().BindNode(node.Cast<CopyQueryNode>());
		break;
	default:
		throw InternalException("Unsupported query node type");
	}
	if (!bound_ctes.empty() && IsBindingTrySubquery()) {
		// carry the plan indexes of the TRY select node back across the CTE binder handoff
		GetTrySubqueryPlanIndexesMutable() = current_binder.get().GetTrySubqueryPlanIndexes();
	}
	for (idx_t i = bound_ctes.size(); i > 0; i--) {
		auto &finish_binder = i == 1 ? *this : *bound_ctes[i - 2].child_binder;
		result = finish_binder.FinishCTE(bound_ctes[i - 1], std::move(result));
	}
	if (dml_cte_count > 1) {
		auto &properties = GetStatementProperties();
		auto &manager = DatabaseManager::Get(context);
		for (auto &entry : properties.modified_databases) {
			auto db = manager.GetDatabase(context, entry.first);
			if (db && !db->GetCatalog().SupportsMultipleDMLCTEs()) {
				throw BinderException("Multiple DML statements in a single WITH clause are not supported for "
				                      "database %s",
				                      entry.first);
			}
		}
	}
	return result;
}

CTEBindState::CTEBindState(Binder &parent_binder_p, QueryNode &cte_def_p, const vector<Identifier> &aliases_p)
    : parent_binder(parent_binder_p), cte_def(cte_def_p), aliases(aliases_p),
      active_binder_count(parent_binder.GetEnclosingScopes().size()) {
}

CTEBindState::~CTEBindState() {
}

bool CTEBindState::IsBound() const {
	return query_binder.get() != nullptr;
}

void CTEBindState::Bind(CTEBinding &binding) {
	// we are lazily binding the CTE
	// we need to bind it as if we were binding it during PrepareCTE
	query_binder = Binder::CreateBinder(parent_binder.context, parent_binder);
	query_binder->SetCanContainNulls(true);

	// we clear any expression binders that were added in the mean-time, to ensure we are not binding to any newly added
	// correlated columns
	auto stored_binders = parent_binder.SaveScopesAfter(active_binder_count);

	// add this CTE to the query binder on the RHS with "CANNOT_BE_REFERENCED" to detect recursive references to
	// ourselves
	query_binder->bind_context.AddCTEBinding(binding.GetIndex(), binding.GetBindingAlias(), vector<Identifier>(),
	                                         vector<LogicalType>(), CTEType::CANNOT_BE_REFERENCED);

	// bind the actual CTE
	query = query_binder->Bind(cte_def);

	// after binding - we add the active binders we removed back so we can leave the binder in its original state
	parent_binder.RestoreScopes(stored_binders);

	// the result types of the CTE are the types of the LHS
	types = query.types;
	// names are picked from the LHS, unless aliases are explicitly specified
	names = query.names;
	for (idx_t i = 0; i < aliases.size() && i < names.size(); i++) {
		if (aliases[i].empty()) {
			throw BinderException("column alias cannot be empty");
		}
		names[i] = aliases[i];
	}

	// Rename columns if duplicate names are detected
	QueryResult::DeduplicateColumns(names);
}

BoundCTEData Binder::PrepareCTE(const Identifier &ctename, CommonTableExpressionInfo &statement) {
	BoundCTEData result;

	// first recursively visit the materialized CTE operations
	// the left side is visited first and is added to the BindContext of the right side
	D_ASSERT(statement.query_node);

	result.ctename = ctename;
	result.materialized = statement.materialized;
	result.setop_index = GenerateTableIndex();

	// instead of eagerly binding the CTE here we add the CTE bind state to the list of CTE bindings
	// the CTE is bound lazily - when referenced for the first time we perform the binding
	result.cte_bind_state = make_shared_ptr<CTEBindState>(*this, *statement.query_node, statement.aliases);

	result.child_binder = Binder::CreateBinder(context, this);

	// Add bindings of left side to temporary CTE bindings context
	// as we are binding a CTE currently, we take precedence over the existing binding.
	// This implements the CTE shadowing behavior.
	auto cte_binding = make_uniq<CTEBinding>(BindingAlias(ctename), result.cte_bind_state, result.setop_index);
	result.child_binder->bind_context.AddCTEBinding(std::move(cte_binding));
	return result;
}

BoundStatement Binder::FinishCTE(BoundCTEData &bound_cte, BoundStatement child) {
	if (!bound_cte.cte_bind_state->IsBound()) {
		auto node_type = bound_cte.cte_bind_state->cte_def.type;
		bool has_side_effects = IsSideEffectingQueryNode(node_type);
		if (has_side_effects) {
			// Side-effecting CTEs always execute even if not referenced - force bind now
			auto dummy_binding =
			    make_uniq<CTEBinding>(BindingAlias(bound_cte.ctename), bound_cte.cte_bind_state, bound_cte.setop_index);
			bound_cte.cte_bind_state->Bind(*dummy_binding);
		} else {
			// CTE without side effects was not referenced - just ignore it
			MoveCorrelatedExpressions(*bound_cte.child_binder);
			return child;
		}
	}
	auto &bind_state = *bound_cte.cte_bind_state;
	for (auto &c : bind_state.query_binder->correlated_columns) {
		bound_cte.child_binder->AddCorrelatedColumn(c);
	}

	BoundStatement result;
	// the result types of the CTE are the types of the LHS
	result.types = child.types;
	result.names = child.names;

	MoveCorrelatedExpressions(*bound_cte.child_binder);
	MoveCorrelatedExpressions(*bind_state.query_binder);

	auto cte_query = std::move(bind_state.query.plan);
	auto cte_child = std::move(child.plan);

	auto root = make_uniq<LogicalMaterializedCTE>(bound_cte.ctename, bound_cte.setop_index, result.types.size(),
	                                              std::move(cte_query), std::move(cte_child), bound_cte.materialized);

	result.plan = std::move(root);
	return result;
}

} // namespace duckdb
