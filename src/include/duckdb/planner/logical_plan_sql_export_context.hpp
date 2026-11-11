#pragma once

#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/common/reference_map.hpp"

namespace duckdb {
class Expression;
class BoundExpressionSQLExportState;
class LogicalOperator;
class LogicalComparisonJoin;
struct LogicalExtensionOperator;
class ClientContext;

struct LogicalPlanSQLExportedChild {
	LogicalPlanSQLExportRelation relation;
	Identifier relation_alias;
};

struct LogicalPlanSQLExportSource {
	reference<LogicalOperator> op;
	Identifier name;
	LogicalPlanSQLExportRelation relation;
	CTEMaterialize materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
};

class LogicalPlanSQLExportContext {
public:
	explicit LogicalPlanSQLExportContext(ClientContext &context_p);
	LogicalPlanSQLExportResult Export(LogicalOperator &op, const LogicalPlanVerificationPath &path);

public:
	ClientContext &GetClientContext() const {
		return context;
	}

	Identifier NextRelationAlias(const Identifier &preferred = Identifier());
	//! Reserve parsed relation names before introducing generated CTEs around a query.
	void ReserveRelationNames(QueryNode &query);
	//! An unqualified relation reference a generated source name would rebind; empty when there is none.
	optional<Identifier> FindCapturedSourceReference(QueryNode &query, const identifier_set_t &source_names) const;
	//! The effective source boundary, including enclosing export scopes.
	optional_ptr<const LogicalPlanSQLExportSource> FindSource(const LogicalOperator &op) const;
	LogicalPlanVerificationResult<LogicalPlanSQLExportedChild> ExportChild(LogicalOperator &child,
	                                                                       const LogicalPlanVerificationPath &path);
	LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>
	ExportChild(LogicalOperator &child, const LogicalPlanVerificationPath &path,
	            const vector<LogicalPlanSQLExportSource> &sources);
	//! Export a plan root inside the given source scope. A RETURNING root exports as its statement,
	//! aliasing the RETURNING items with the given output names, or positionally without them.
	LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>
	ExportRoot(LogicalOperator &root, const LogicalPlanVerificationPath &path,
	           const vector<LogicalPlanSQLExportSource> &sources, const optional<vector<Identifier>> &output_names);
	LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
	ExportExpression(const LogicalOperator &op, const vector<reference<const Expression>> &expressions,
	                 idx_t expression_ordinal, const BoundExpressionSQLExportContext &expression_context,
	                 const LogicalPlanVerificationPath &path,
	                 optional_ptr<BoundExpressionSQLExportState> composition = nullptr);
	unique_ptr<SelectNode> CreateNamedSource(const Identifier &name, const vector<LogicalPlanSQLExportField> &fields,
	                                         bool recurring = false);

	LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>
	ExportNamedProducer(LogicalOperator &op, const LogicalPlanVerificationPath &path, const Identifier &name);

	LogicalPlanSQLExportResult ExportRow(unique_ptr<SelectNode> select, unique_ptr<ParsedExpression> value,
	                                     vector<LogicalPlanSQLExportField> fields);
	unique_ptr<SelectNode> ForwardFields(const LogicalPlanSQLExportedChild &child,
	                                     const vector<LogicalPlanSQLExportField> &fields,
	                                     optional_ptr<const SelectNode> plain = nullptr);

	//! Operators from the root down to the operator currently being exported
	const vector<reference<LogicalOperator>> &Ancestors() const {
		return ancestors;
	}
	//! Enters an operator exported outside the standard child recursion (e.g. a RETURNING root)
	struct AncestorScope {
		AncestorScope(LogicalPlanSQLExportContext &context_p, LogicalOperator &op) : context(context_p) {
			context.ancestors.push_back(op);
		}
		~AncestorScope() {
			context.ancestors.pop_back();
		}
		LogicalPlanSQLExportContext &context;
	};
	//! Make a CTE relation referenceable while its consumers are exported
	void PushNamedRelation(TableIndex index, const Identifier &name, bool is_recurring);
	//! Remove the innermost named relation and return how often it was referenced
	idx_t PopNamedRelation();
	//! Reference the innermost named relation with this index; empty when it is out of scope
	optional<Identifier> ReferenceNamedRelation(TableIndex index, bool is_recurring);

private:
	struct SourceScope;
	optional_ptr<const SourceScope> source_scope;
	struct NamedRelation {
		TableIndex index;
		Identifier name;
		bool is_recurring;
		idx_t references;
	};
	ClientContext &context;
	idx_t next_relation_ordinal = 0;
	identifier_set_t relation_aliases;
	//! The named-source reads this export created; any other reference to a source name is a capture.
	reference_set_t<const TableRef> source_reads;
	vector<reference<LogicalOperator>> ancestors;
	vector<NamedRelation> named_relations;
};

} // namespace duckdb
