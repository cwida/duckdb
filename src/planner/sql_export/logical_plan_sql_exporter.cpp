#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export/bound_expression_sql_exporter_internal.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/common/unordered_set.hpp"
#include <functional>

namespace duckdb {

static LogicalPlanVerificationIssue UnsupportedOperator(const LogicalPlanVerificationPath &path,
                                                        LogicalOperatorType type) {
	return SQLExportHelpers::MakeIssue(LogicalPlanVerificationIssueCode::UNSUPPORTED_OPERATOR,
	                                   LogicalPlanVerificationPhase::PLAN_EXPORT, path,
	                                   LogicalPlanVerificationConstructIdentity::LogicalOperator(type),
	                                   "The logical operator does not have a SQL AST representation in this exporter");
}

static optional<Value> ConstantSQLInput(const Expression &expression, LogicalOperator &input,
                                        const LogicalPlanSQLExportContext &context) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return expression.Cast<BoundConstantExpression>().GetValue();
	}
	if (auto wrapped = CMUtils::GetWrappedInput(expression)) {
		return ConstantSQLInput(*wrapped, input, context);
	}
	if (expression.IsFoldable()) {
		auto &client_context = context.GetClientContext();
		if (Settings::Get<PortablePlanSetting>(client_context) &&
		    (!expression.IsConsistent() || expression.ObservesBindingContext())) {
			// an environment observation must stay an expression in the exported SQL
			return {};
		}
		// An unfolded constant expression, e.g. a cast of a literal with the expression rewriter disabled.
		// Fold it like GetConstant does at bind time, so constant-required arguments restore to constants.
		Value value;
		if (ExpressionExecutor::TryEvaluateScalar(client_context, expression, value)) {
			return value;
		}
		return {};
	}
	if (context.FindSource(input) || expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    input.children.size() != 1) {
		return {};
	}
	auto &column = expression.Cast<BoundColumnRefExpression>();
	if (column.Depth() != 0) {
		return {};
	}
	switch (input.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = input.Cast<LogicalProjection>();
		if (column.Binding().table_index != projection.table_index) {
			return {};
		}
		return ConstantSQLInput(*projection.expressions[column.Binding().column_index], *input.children[0], context);
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggregate = input.Cast<LogicalAggregate>();
		auto index = column.Binding().column_index;
		if (column.Binding().table_index != aggregate.group_index) {
			return {};
		}
		for (auto &grouping_set : aggregate.grouping_sets) {
			if (!grouping_set.count(index)) {
				return {};
			}
		}
		return ConstantSQLInput(*aggregate.groups[index], *input.children[0], context);
	}
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return ConstantSQLInput(expression, *input.children[0], context);
	default:
		return {};
	}
}

static LogicalPlanSQLExportResult EnsureSQLProjection(LogicalPlanSQLExportResult result) {
	if (result.HasError() || !result.GetValue().fields.empty() ||
	    result.GetValue().query->type != QueryNodeType::SELECT_NODE) {
		return result;
	}
	auto &select = result.GetValue().query->Cast<SelectNode>();
	if (select.select_list.empty()) {
		// SQL needs a SELECT expression even when the logical relation only carries row counts.
		select.select_list.push_back(make_uniq<ConstantExpression>(Literal::Null()));
	}
	return result;
}

LogicalPlanSQLExportResult LogicalPlanSQLExportHelpers::ApplyOutputNames(LogicalPlanSQLExportResult result,
                                                                         const vector<Identifier> &output_names) {
	if (result.HasError()) {
		return result;
	}
	if (SQLExportDMLHelpers::IsDMLNode(*result.GetValue().query)) {
		// a data-modifying statement's result shape is inherent; its names are not a projection
		return result;
	}
	if (output_names.size() != result.GetValue().fields.size()) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    LogicalPlanVerificationPath(), "output_names", "Output name count does not match the exported plan")});
	}
	if (output_names.empty()) {
		return result;
	}
	if (result.GetValue().query->type == QueryNodeType::SELECT_NODE) {
		auto &select = result.GetValue().query->Cast<SelectNode>();
		D_ASSERT(select.select_list.size() == output_names.size());
		for (idx_t i = 0; i < output_names.size(); i++) {
			select.select_list[i]->SetAlias(output_names[i]);
		}
		return result;
	}
	auto fields = result.GetValue().fields;
	LogicalPlanSQLExportedChild child {std::move(result.GetValue()), Identifier("exported_query")};
	auto select = make_uniq<SelectNode>();
	for (idx_t i = 0; i < output_names.size(); i++) {
		auto expression =
		    make_uniq<ColumnRefExpression>(LogicalPlanSQLExportHelpers::FieldIdentifier(i), child.relation_alias);
		expression->SetAlias(output_names[i]);
		select->select_list.push_back(std::move(expression));
	}
	select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child));
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields)});
}

struct LogicalPlanSQLExportContext::SourceScope {
	SourceScope(LogicalPlanSQLExportContext &context_p, const vector<LogicalPlanSQLExportSource> &sources_p)
	    : context(context_p), sources(sources_p), parent(context.source_scope) {
		context.source_scope = this;
	}
	~SourceScope() {
		context.source_scope = parent;
	}

	LogicalPlanSQLExportContext &context;
	const vector<LogicalPlanSQLExportSource> &sources;
	optional_ptr<const SourceScope> parent;
};

LogicalPlanSQLExportContext::LogicalPlanSQLExportContext(ClientContext &context_p) : context(context_p) {
}

optional_ptr<const LogicalPlanSQLExportSource>
LogicalPlanSQLExportContext::FindSource(const LogicalOperator &op) const {
	for (auto scope = source_scope; scope; scope = scope->parent) {
		for (auto &source : scope->sources) {
			if (&source.op.get() == &op) {
				return &source;
			}
		}
	}
	return nullptr;
}

LogicalPlanSQLExportResult LogicalPlanSQLExportContext::Export(LogicalOperator &op,
                                                               const LogicalPlanVerificationPath &path) {
	if (auto source = FindSource(op)) {
		auto read = CreateNamedSource(source->name, source->relation.fields);
		source_reads.insert(*read->from_table);
		return EnsureSQLProjection(LogicalPlanSQLExportResult::Success({std::move(read), source->relation.fields}));
	}
	ancestors.push_back(op);
	auto result = op.ToSQL(*this, path);
	ancestors.pop_back();
	return EnsureSQLProjection(std::move(result));
}

void LogicalPlanSQLExportContext::PushNamedRelation(TableIndex index, const Identifier &name, bool is_recurring) {
	named_relations.push_back({index, name, is_recurring, 0});
}

idx_t LogicalPlanSQLExportContext::PopNamedRelation() {
	D_ASSERT(!named_relations.empty());
	auto references = named_relations.back().references;
	named_relations.pop_back();
	return references;
}

optional<Identifier> LogicalPlanSQLExportContext::ReferenceNamedRelation(TableIndex index, bool is_recurring) {
	for (idx_t i = named_relations.size(); i > 0; i--) {
		auto &relation = named_relations[i - 1];
		if (relation.index == index && relation.is_recurring == is_recurring) {
			relation.references++;
			return relation.name;
		}
	}
	return optional<Identifier>();
}

Identifier LogicalPlanSQLExportContext::NextRelationAlias(const Identifier &preferred) {
	if (!preferred.empty() && relation_aliases.insert(preferred).second) {
		return preferred;
	}
	while (true) {
		auto name = Identifier("r" + to_string(next_relation_ordinal++));
		if (relation_aliases.insert(name).second) {
			return name;
		}
	}
}

static void VisitRelationReferences(QueryNode &query, const std::function<void(TableRef &)> &callback) {
	std::function<void(const ParsedExpression &)> visit_expression = [&](const ParsedExpression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			VisitRelationReferences(*expression.Cast<SubqueryExpression>().Subquery()->node, callback);
		}
		ParsedExpressionIterator::EnumerateChildren(expression, visit_expression);
	};
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    query, [&](unique_ptr<ParsedExpression> &expression) { visit_expression(*expression); }, callback);
}

void LogicalPlanSQLExportContext::ReserveRelationNames(QueryNode &query) {
	VisitRelationReferences(query, [&](TableRef &ref) {
		if (ref.type == TableReferenceType::BASE_TABLE) {
			relation_aliases.insert(ref.Cast<BaseTableRef>().Table());
		}
	});
}

optional<Identifier>
LogicalPlanSQLExportContext::FindCapturedSourceReference(QueryNode &query, const identifier_set_t &source_names) const {
	optional<Identifier> captured;
	VisitRelationReferences(query, [&](TableRef &ref) {
		if (captured || ref.type != TableReferenceType::BASE_TABLE) {
			return;
		}
		// Qualified references bind past the generated WITH scope.
		auto &name = ref.Cast<BaseTableRef>().GetQualifiedName();
		if (!name.Catalog().empty() || !name.Schema().empty()) {
			return;
		}
		if (source_names.count(name.Name()) &&
		    source_reads.find(reference<const TableRef>(ref)) == source_reads.end()) {
			captured = name.Name();
		}
	});
	return captured;
}

LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>
LogicalPlanSQLExportContext::ExportChild(LogicalOperator &child, const LogicalPlanVerificationPath &path) {
	auto exported = Export(child, path);
	if (exported.HasError()) {
		return LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>::Failure(exported);
	}
	LogicalPlanSQLExportedChild result {std::move(exported.GetValue()), NextRelationAlias()};
	return LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>::Success(std::move(result));
}

LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>
LogicalPlanSQLExportContext::ExportChild(LogicalOperator &child, const LogicalPlanVerificationPath &path,
                                         const vector<LogicalPlanSQLExportSource> &sources) {
	SourceScope scope(*this, sources);
	return ExportChild(child, path);
}

LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>
LogicalPlanSQLExportContext::ExportRoot(LogicalOperator &root, const LogicalPlanVerificationPath &path,
                                        const vector<LogicalPlanSQLExportSource> &sources,
                                        const optional<vector<Identifier>> &output_names) {
	SourceScope scope(*this, sources);
	auto returning = SQLExportDMLHelpers::MatchReturningRoot(root);
	// a replaced root or a replaced statement below it exports through the ordinary
	// relational path, which reads the supplied replacement instead of the statement
	if (!returning || SQLExportDMLHelpers::ContainsReplacedBoundary(*this, root)) {
		return ExportChild(root, path);
	}
	// consumers of an exported statement address its RETURNING columns positionally
	optional<vector<Identifier>> names = output_names;
	if (!names) {
		vector<Identifier> positional;
		for (idx_t i = 0; i < returning->expressions.size(); i++) {
			positional.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
		}
		names = std::move(positional);
	}
	auto exported = SQLExportDMLHelpers::ExportReturning(*this, *returning, names, path);
	if (exported.HasError()) {
		return LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>::Failure(exported);
	}
	return LogicalPlanVerificationResult<LogicalPlanSQLExportedChild>::Success(
	    {std::move(exported.GetValue()), NextRelationAlias()});
}

LogicalPlanVerificationResult<unique_ptr<ParsedExpression>> LogicalPlanSQLExportContext::ExportExpression(
    const LogicalOperator &op, const vector<reference<const Expression>> &expressions, idx_t expression_ordinal,
    const BoundExpressionSQLExportContext &expression_context, const LogicalPlanVerificationPath &path,
    optional_ptr<BoundExpressionSQLExportState> composition) {
	D_ASSERT(expression_ordinal < expressions.size());
	auto &expression = expressions[expression_ordinal].get();
	unique_ptr<Expression> restored;
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && op.children.size() == 1) {
		auto &arguments = expression.Cast<BoundAggregateExpression>().GetChildren();
		for (idx_t i = 0; i < arguments.size(); i++) {
			if (arguments[i]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				continue;
			}
			auto value = ConstantSQLInput(*arguments[i], *op.children[0], *this);
			if (value && value->type().EqualsIncludingCollation(arguments[i]->GetReturnType())) {
				if (!restored) {
					restored = expression.Copy();
				}
				restored->Cast<BoundAggregateExpression>().GetChildrenMutable()[i] =
				    make_uniq<BoundConstantExpression>(*value);
			}
		}
	}
	if (composition) {
		return composition->Export(restored ? *restored : expression,
		                           LogicalPlanSQLExportHelpers::PlanExpressionPath(path, expression_ordinal));
	}
	return BoundExpressionSQLExporter::ExportAtPath(
	    restored ? *restored : expression, expression_context,
	    LogicalPlanSQLExportHelpers::PlanExpressionPath(path, expression_ordinal));
}

unique_ptr<SelectNode> LogicalPlanSQLExportContext::ForwardFields(const LogicalPlanSQLExportedChild &child,
                                                                  const vector<LogicalPlanSQLExportField> &fields,
                                                                  optional_ptr<const SelectNode> plain) {
	auto select = make_uniq<SelectNode>();
	for (auto &field : fields) {
		bool found = false;
		for (idx_t i = 0; i < child.relation.fields.size(); i++) {
			if (field.source_binding != child.relation.fields[i].source_binding) {
				continue;
			}
			auto expression =
			    plain ? plain->select_list[i]->Copy() : LogicalPlanSQLExportHelpers::ChildColumn(child, i);
			expression->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(select->select_list.size()));
			select->select_list.push_back(std::move(expression));
			found = true;
			break;
		}
		D_ASSERT(found);
		(void)found;
	}
	return select;
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalOperator::ToSQL(LogicalPlanSQLExportContext &, const LogicalPlanVerificationPath &path) {
	if (type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::UnsupportedSource(
		    path, LogicalPlanSQLExportHelpers::LogicalSourceIdentity(), "delim_get")});
	}
	D_ASSERT(type != LogicalOperatorType::LOGICAL_INVALID);
	return LogicalPlanSQLExportResult::Failure({UnsupportedOperator(path, type)});
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalPlanSQLExporter::Export(ClientContext &context, LogicalOperator &root,
                               const LogicalPlanSQLExportOptions &options) {
	auto verification = LogicalPlanVerifier::VerifyAlways(root);
	if (verification.HasError()) {
		return LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>::Failure(verification);
	}
	LogicalPlanSQLExportContext state(context);
	if (auto returning = SQLExportDMLHelpers::MatchReturningRoot(root)) {
		// output names become the RETURNING aliases, so they are not reapplied below
		return SQLExportDMLHelpers::ExportReturning(state, *returning, options.output_names,
		                                            LogicalPlanVerificationPath());
	}
	auto result = state.Export(root, LogicalPlanVerificationPath());
	if (!options.output_names) {
		return result;
	}
	return LogicalPlanSQLExportHelpers::ApplyOutputNames(std::move(result), *options.output_names);
}

LogicalPlanSQLExportResult
LogicalPlanSQLExporter::ExportWithSources(ClientContext &context, LogicalOperator &root,
                                          const vector<LogicalPlanSQLExportReplacement> &replacements,
                                          const LogicalPlanSQLExportOptions &options) {
	auto verification = LogicalPlanVerifier::VerifyResolvedBindings(root);
	if (verification.HasError()) {
		return LogicalPlanSQLExportResult::Failure(verification);
	}
	auto invalid = [](const string &message) {
		auto issue = SQLExportHelpers::MakeIssue(
		    LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT, LogicalPlanVerificationPhase::PLAN_EXPORT,
		    LogicalPlanVerificationPath(), LogicalPlanVerificationConstructIdentity::ExportFeature("source_boundary"),
		    message);
		return LogicalPlanSQLExportResult::Failure({std::move(issue)});
	};
	unordered_set<LogicalOperator *> boundaries;
	for (auto &replacement : replacements) {
		auto &op = replacement.op.get();
		if (!boundaries.insert(&op).second || !replacement.relation.query) {
			return invalid("SQL source replacement requires a unique subtree and an owned relation");
		}
		auto bindings = op.GetColumnBindings();
		if (replacement.relation.fields.size() != bindings.size() || op.types.size() != bindings.size()) {
			return invalid("SQL source replacement width differs from its subtree");
		}
		for (idx_t i = 0; i < bindings.size(); i++) {
			auto &field = replacement.relation.fields[i];
			auto &type = field.optimizer_type ? *field.optimizer_type : field.type;
			if (field.source_binding != bindings[i] || !field.type.IsComplete() ||
			    !type.EqualsIncludingCollation(op.types[i])) {
				return invalid("SQL source replacement bindings or types differ from its subtree");
			}
		}
	}
	unordered_set<LogicalOperator *> reached;
	bool overlap = false;
	std::function<void(LogicalOperator &, bool)> visit = [&](LogicalOperator &op, bool replaced) {
		if (boundaries.count(&op)) {
			overlap |= replaced;
			reached.insert(&op);
			replaced = true;
		}
		for (auto &child : op.children) {
			visit(*child, replaced);
		}
	};
	visit(root, false);
	if (overlap || reached.size() != boundaries.size()) {
		return invalid("SQL source replacements must be disjoint subtrees of this plan");
	}
	LogicalPlanSQLExportContext state(context);
	vector<LogicalPlanSQLExportSource> sources;
	// All definitions share a CTE scope. Reserve references from every replacement
	// before allocating any name, so a generated source cannot capture another query's input.
	for (auto &replacement : replacements) {
		state.ReserveRelationNames(*replacement.relation.query);
	}
	for (auto &replacement : replacements) {
		sources.push_back({replacement.op,
		                   state.NextRelationAlias(Identifier("__export_source")),
		                   {replacement.relation.query->Copy(), replacement.relation.fields},
		                   replacement.materialized});
	}
	auto exported = state.ExportRoot(root, LogicalPlanVerificationPath(), sources, options.output_names);
	if (exported.HasError()) {
		return LogicalPlanSQLExportResult::Failure(exported);
	}
	auto result = std::move(exported.GetValue().relation);
	// an exported statement that cannot carry a WITH clause holds its definitions in its source query
	auto &definitions = SQLExportStatementHelpers::SourceDefinitionNode(*result.query);
	auto consumers = std::move(definitions.cte_map);
	definitions.cte_map = CommonTableExpressionMap();
	for (auto &source : sources) {
		auto info = make_uniq<CommonTableExpressionInfo>();
		for (idx_t i = 0; i < source.relation.fields.size(); i++) {
			info->aliases.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
		}
		info->query_node = std::move(source.relation.query);
		info->materialized = source.materialized;
		definitions.cte_map.map.insert(source.name, std::move(info));
	}
	for (auto &consumer : consumers.map) {
		definitions.cte_map.map.insert(consumer.first, std::move(consumer.second));
	}
	// A source callback may emit an unqualified reference the plan verifier never sees;
	// refuse a result whose generated names would rebind such a reference.
	identifier_set_t source_names;
	for (auto &source : sources) {
		source_names.insert(source.name);
	}
	if (auto captured = state.FindCapturedSourceReference(definitions, source_names)) {
		return invalid("A generated SQL source name would capture a relation reference of the exported plan: " +
		               captured->GetIdentifierName());
	}
	auto success = LogicalPlanSQLExportResult::Success(std::move(result));
	return options.output_names
	           ? LogicalPlanSQLExportHelpers::ApplyOutputNames(std::move(success), *options.output_names)
	           : std::move(success);
}

} // namespace duckdb
