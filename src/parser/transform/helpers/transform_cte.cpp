#include "duckdb/parser/transformer.hpp"
#include "duckdb/common/enums/set_operation_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"

namespace duckdb {

void Transformer::TransformCTE(duckdb_libpgquery::PGWithClause *de_with_clause, QueryNode &select) {
	// TODO: might need to update in case of future lawsuit
	D_ASSERT(de_with_clause);

	D_ASSERT(de_with_clause->ctes);
	for (auto cte_ele = de_with_clause->ctes->head; cte_ele != nullptr; cte_ele = cte_ele->next) {
		auto info = make_unique<CommonTableExpressionInfo>();

		auto cte = reinterpret_cast<duckdb_libpgquery::PGCommonTableExpr *>(cte_ele->data.ptr_value);
		if (cte->aliascolnames) {
			for (auto node = cte->aliascolnames->head; node != nullptr; node = node->next) {
				info->aliases.emplace_back(
				    reinterpret_cast<duckdb_libpgquery::PGValue *>(node->data.ptr_value)->val.str);
			}
		}
		// lets throw some errors on unsupported features early
		if (cte->ctecolnames) {
			throw NotImplementedException("Column name setting not supported in CTEs");
		}
		if (cte->ctecoltypes) {
			throw NotImplementedException("Column type setting not supported in CTEs");
		}
		if (cte->ctecoltypmods) {
			throw NotImplementedException("Column type modification not supported in CTEs");
		}
		if (cte->ctecolcollations) {
			throw NotImplementedException("CTE collations not supported");
		}
		// we need a query
		if (!cte->ctequery || cte->ctequery->type != duckdb_libpgquery::T_PGSelectStmt) {
			throw InternalException("A CTE needs a SELECT");
		}

		// CTE transformation can either result in inlining for non recursive CTEs, or in recursive CTE bindings
		// otherwise.
		if (cte->cterecursive || de_with_clause->recursive) {
			info->query = TransformRecursiveCTE(cte, *info);
		} else {
			info->query = TransformSelect(cte->ctequery);
		}
		D_ASSERT(info->query);
		auto cte_name = string(cte->ctename);

		auto it = select.cte_map.find(cte_name);
		if (it != select.cte_map.end()) {
			// can't have two CTEs with same name
			throw ParserException("Duplicate CTE name \"%s\"", cte_name);
		}
		select.cte_map[cte_name] = move(info);
	}
}

unique_ptr<SelectStatement> Transformer::TransformRecursiveCTE(duckdb_libpgquery::PGCommonTableExpr *cte,
                                                               CommonTableExpressionInfo &info) {
	auto stmt = (duckdb_libpgquery::PGSelectStmt *)cte->ctequery;

	unique_ptr<SelectStatement> select;
	switch (stmt->op) {
	case duckdb_libpgquery::PG_SETOP_UNION:
	case duckdb_libpgquery::PG_SETOP_EXCEPT:
	case duckdb_libpgquery::PG_SETOP_INTERSECT: {
		select = make_unique<SelectStatement>();
		select->node = make_unique_base<QueryNode, RecursiveCTENode>();
		auto result = (RecursiveCTENode *)select->node.get();
		result->ctename = string(cte->ctename);
		result->union_all = stmt->all;
		result->left = TransformSelectNode(stmt->larg);
		result->right = TransformSelectNode(stmt->rarg);
		result->aliases = info.aliases;

		D_ASSERT(result->left);
		D_ASSERT(result->right);

		if (stmt->op != duckdb_libpgquery::PG_SETOP_UNION) {
			throw ParserException("Unsupported setop type for recursive CTE: only UNION or UNION ALL are supported");
		}
		break;
	}
	default:
		// This CTE is not recursive. Fallback to regular query transformation.
		return TransformSelect(cte->ctequery);
	}

	if (stmt->limitCount || stmt->limitOffset) {
		throw ParserException("LIMIT or OFFSET in a recursive query is not allowed");
	}
	if (stmt->sortClause) {
		throw ParserException("ORDER BY in a recursive query is not allowed");
	}
	return select;
}

} // namespace duckdb
