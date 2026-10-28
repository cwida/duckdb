#include "duckdb.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/ast/limit_percent_result.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

using namespace duckdb;

static bool IsRule(const ParseResult &parse_result, const char *name) {
	return StringUtil::CIEquals(parse_result.Name(), name);
}

static ParseResult &GetChoice(ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	return list.Child<ChoiceParseResult>(0).GetResult();
}

static SelectNode &WrapPipeInput(SelectStatement &statement) {
	if (!statement.node) {
		throw InternalException("Pipe input has no query node");
	}
	auto input = make_uniq<SelectStatement>();
	input->node = std::move(statement.node);
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = make_uniq<SubqueryRef>(std::move(input));
	statement.node = std::move(select_node);
	return statement.node->Cast<SelectNode>();
}

static unique_ptr<SelectStatement> TransformPipeSource(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &source = GetChoice(parse_result);
	if (!IsRule(source, "FromClause")) {
		return transformer.Transform<unique_ptr<SelectStatement>>(source);
	}

	auto result = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = transformer.Transform<unique_ptr<TableRef>>(source);
	result->node = std::move(select_node);
	return result;
}

static void ApplyWhere(PEGTransformer &transformer, ParseResult &parse_result, SelectNode &select_node) {
	auto &where_clause = parse_result.Cast<ListParseResult>().GetChild(0);
	auto expression = transformer.Transform<unique_ptr<ParsedExpression>>(where_clause);
	if (select_node.where_clause) {
		select_node.where_clause = make_uniq<ConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, std::move(select_node.where_clause), std::move(expression));
	} else {
		select_node.where_clause = std::move(expression);
	}
}

static void ApplySelect(PEGTransformer &transformer, ParseResult &parse_result, SelectNode &select_node, bool extend) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto expressions = transformer.Transform<vector<unique_ptr<ParsedExpression>>>(list.GetChild(1));
	if (!extend) {
		select_node.select_list.clear();
	}
	for (auto &expression : expressions) {
		select_node.select_list.push_back(std::move(expression));
	}
}

static void ApplyAggregate(PEGTransformer &transformer, ParseResult &parse_result, SelectNode &select_node,
                           bool group_only) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (group_only) {
		select_node.groups = transformer.Transform<GroupByNode>(list.GetChild(1));
		select_node.select_list.clear();
		for (auto &expression : select_node.groups.group_expressions) {
			select_node.select_list.push_back(expression->Copy());
		}
		return;
	}

	auto aggregate_expressions = transformer.Transform<vector<unique_ptr<ParsedExpression>>>(list.GetChild(1));
	auto &group_by = list.Child<OptionalParseResult>(2);
	if (group_by.HasResult()) {
		select_node.groups = transformer.Transform<GroupByNode>(group_by.GetResult());
	}
	select_node.select_list.clear();
	for (auto &expression : select_node.groups.group_expressions) {
		select_node.select_list.push_back(expression->Copy());
	}
	for (auto &expression : aggregate_expressions) {
		select_node.select_list.push_back(std::move(expression));
	}
}

static void ApplyOrderBy(PEGTransformer &transformer, ParseResult &parse_result, SelectNode &select_node) {
	auto &order_by = parse_result.Cast<ListParseResult>().GetChild(0);
	auto modifier = make_uniq<OrderModifier>();
	modifier->orders = transformer.Transform<vector<OrderByNode>>(order_by);
	select_node.modifiers.push_back(std::move(modifier));
}

static void ApplyLimit(PEGTransformer &transformer, ParseResult &parse_result, SelectNode &select_node) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto limit = transformer.Transform<LimitPercentResult>(list.GetChild(0));
	LimitPercentResult offset;
	auto &offset_clause = list.Child<OptionalParseResult>(1);
	if (offset_clause.HasResult()) {
		offset = transformer.Transform<LimitPercentResult>(offset_clause.GetResult());
	}
	if (offset.is_percent) {
		throw ParserException("Percentage for offsets are not supported");
	}
	auto modifier = make_uniq<LimitModifier>();
	modifier->limit_type = limit.is_percent ? LimitValueType::PERCENTAGE : LimitValueType::ROW_COUNT;
	modifier->limit = std::move(limit.expression);
	modifier->offset = std::move(offset.expression);
	select_node.modifiers.push_back(std::move(modifier));
}

static void ApplyPipeStage(PEGTransformer &transformer, ParseResult &parse_result, SelectStatement &statement) {
	auto &stage_list = parse_result.Cast<ListParseResult>();
	auto &pipe_operator = stage_list.GetChild(1);
	auto &operation = GetChoice(pipe_operator);
	auto &select_node = WrapPipeInput(statement);

	if (IsRule(operation, "PipeWhere")) {
		ApplyWhere(transformer, operation, select_node);
	} else if (IsRule(operation, "PipeSelect")) {
		ApplySelect(transformer, operation, select_node, false);
	} else if (IsRule(operation, "PipeExtend")) {
		ApplySelect(transformer, operation, select_node, true);
	} else if (IsRule(operation, "PipeDistinct")) {
		select_node.modifiers.push_back(make_uniq<DistinctModifier>());
	} else if (IsRule(operation, "PipeOrderBy")) {
		ApplyOrderBy(transformer, operation, select_node);
	} else if (IsRule(operation, "PipeLimit")) {
		ApplyLimit(transformer, operation, select_node);
	} else if (IsRule(operation, "PipeAggregate")) {
		ApplyAggregate(transformer, operation, select_node, false);
	} else if (IsRule(operation, "PipeAggregateGroupOnly")) {
		ApplyAggregate(transformer, operation, select_node, true);
	} else {
		throw InternalException("Unknown pipe operator rule '%s'", operation.Name());
	}
}

static arena_ptr<TransformResultValue> FinalizePipeSelectAtom(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &pipe = parse_result.Cast<ListParseResult>();
	auto statement = TransformPipeSource(transformer, pipe.GetChild(0));
	auto &stages = pipe.Child<RepeatParseResult>(1);
	for (auto &stage : stages.GetChildren()) {
		ApplyPipeStage(transformer, stage.get(), *statement);
	}
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(statement));
}

static arena_ptr<TransformProcess> StartPipeSelectAtomTransform(PEGTransformer &transformer,
                                                                ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizePipeSelectAtom);
}

static arena_ptr<TransformResultValue> MakeRangeSelectResult(PEGTransformer &transformer, LimitPercentResult limit,
                                                             LimitPercentResult offset) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(ConstantExpression::Integer(10));
	auto from_table = make_uniq<TableFunctionRef>();
	from_table->function = make_uniq<FunctionExpression>("range", std::move(arguments));
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(from_table);
	auto modifier = make_uniq<LimitModifier>();
	modifier->limit_type = limit.is_percent ? LimitValueType::PERCENTAGE : LimitValueType::ROW_COUNT;
	modifier->limit = std::move(limit.expression);
	modifier->offset = std::move(offset.expression);
	select_node->modifiers.push_back(std::move(modifier));
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(statement));
}

static arena_ptr<TransformResultValue> FinalizeWrappedSelectAtom(PEGTransformer &transformer,
                                                                 ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	LimitPercentResult limit;
	LimitPercentResult offset;
	// the wrapper nodes are passed to Transform without unwrapping on purpose
	auto &clause = list.Child<ListParseResult>(1).GetChild(0);
	auto &target = IsRule(clause, "LimitClause") ? limit : offset;
	target = transformer.Transform<LimitPercentResult>(clause);
	if (list.Child<OptionalParseResult>(2).HasResult()) {
		if (offset.expression) {
			throw ParserException("WRAPPED accepts a single OFFSET clause");
		}
		offset = transformer.Transform<LimitPercentResult>(list.GetChild(2));
	}
	return MakeRangeSelectResult(transformer, std::move(limit), std::move(offset));
}

static arena_ptr<TransformResultValue> FinalizeWrappedNestedAtom(PEGTransformer &transformer,
                                                                 ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	LimitPercentResult limit;
	LimitPercentResult offset;
	// the wrapper nodes are passed to Transform without unwrapping on purpose
	auto &optional = list.Child<OptionalParseResult>(1);
	if (optional.HasResult()) {
		auto &choice = optional.GetResult().Cast<ListParseResult>().GetChild(0);
		auto &target = IsRule(choice, "LimitClause") ? limit : offset;
		target = transformer.Transform<LimitPercentResult>(list.GetChild(1));
	}
	return MakeRangeSelectResult(transformer, std::move(limit), std::move(offset));
}

static arena_ptr<TransformResultValue> FinalizeWrappedBadAtom(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	// the unconditional Transform call exercises the dead-end error paths on purpose
	auto limit = transformer.Transform<LimitPercentResult>(list.GetChild(1));
	return MakeRangeSelectResult(transformer, std::move(limit), LimitPercentResult());
}

static arena_ptr<TransformProcess> StartWrappedSelectAtomTransform(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizeWrappedSelectAtom);
}

static arena_ptr<TransformProcess> StartWrappedNestedAtomTransform(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizeWrappedNestedAtom);
}

static arena_ptr<TransformProcess> StartWrappedBadAtomTransform(PEGTransformer &transformer,
                                                                ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizeWrappedBadAtom);
}

static void InitializeWrappedChildAtom(PEGTransformer &transformer, GeneratedTransformProcess &process) {
	auto &list = process.parse_result.Cast<ListParseResult>();
	process.ReserveChildSlots(1);
	// the wrapper node is pushed without an explicit rule on purpose
	process.PushChild({list.GetChild(1)}, 0);
}

static arena_ptr<TransformResultValue> FinalizeWrappedChildAtom(PEGTransformer &transformer,
                                                                GeneratedTransformProcess &process) {
	auto &list = process.parse_result.Cast<ListParseResult>();
	LimitPercentResult limit;
	LimitPercentResult offset;
	auto &choice = list.Child<ListParseResult>(1).GetChild(0);
	auto &target = IsRule(choice, "LimitClause") ? limit : offset;
	target = process.TakeResult<LimitPercentResult>(0);
	return MakeRangeSelectResult(transformer, std::move(limit), std::move(offset));
}

static const TransformFrameOps WRAPPED_CHILD_ATOM_OPS = {"WrappedChildAtom", &InitializeWrappedChildAtom,
                                                         &FinalizeWrappedChildAtom};

static arena_ptr<TransformProcess> StartWrappedChildAtomTransform(PEGTransformer &transformer,
                                                                  ParseResult &parse_result) {
	return transformer.MakeProcess<GeneratedTransformProcess>(transformer, TransformInput(parse_result),
	                                                          WRAPPED_CHILD_ATOM_OPS);
}

class PipeSQLGrammarChange final : public GrammarExtension {
public:
	PipeSQLGrammarChange()
	    : GrammarExtension("pipe_query_syntax",
	                       "Add Pipe query syntax, inspired by "
	                       "https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/pipe-syntax") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddChoice("ReservedKeyword", "'EXTEND'"));
		// AGGREGATE is an unreserved keyword in the host grammar: an unreserved keyword stays a valid identifier
		auto is_aggregate = [](const PEGExpression &expr) {
			return expr.type == PEGExpression::Type::LITERAL &&
			       StringUtil::CIEquals(expr.text.GetString(), "AGGREGATE");
		};
		changes.push_back(GrammarChange::RemoveChoice("UnreservedKeyword", is_aggregate));
		changes.push_back(GrammarChange::RemoveChoice("BareLabelKeyword", is_aggregate));
		changes.push_back(GrammarChange::AddChoice("ReservedKeyword", "'AGGREGATE'"));
		changes.push_back(
		    GrammarChange::AddRule("PipeSelectAtom <- PipeSource PipeStage+", StartPipeSelectAtomTransform));
		changes.push_back(GrammarChange::AddRule("PipeSource <- FromClause / SelectStatementType / SelectParens"));
		changes.push_back(GrammarChange::AddRule("PipeStage <- '|>' PipeOperator"));
		changes.push_back(GrammarChange::AddRule(
		    "PipeOperator <- PipeAggregate / PipeAggregateGroupOnly / PipeWhere / PipeSelect / PipeExtend / "
		    "PipeDistinct / PipeOrderBy / PipeLimit"));
		changes.push_back(GrammarChange::AddRule("PipeWhere <- WhereClause"));
		changes.push_back(GrammarChange::AddRule("PipeSelect <- 'SELECT' TargetList"));
		changes.push_back(GrammarChange::AddRule("PipeExtend <- 'EXTEND' TargetList"));
		changes.push_back(GrammarChange::AddRule("PipeDistinct <- 'DISTINCT'"));
		changes.push_back(GrammarChange::AddRule("PipeOrderBy <- OrderByClause"));
		changes.push_back(GrammarChange::AddRule("PipeLimit <- LimitClause OffsetClause?"));
		changes.push_back(GrammarChange::AddRule("PipeAggregate <- 'AGGREGATE' TargetList GroupByClause?"));
		changes.push_back(GrammarChange::AddRule("PipeAggregateGroupOnly <- 'AGGREGATE' GroupByClause"));
		changes.push_back(GrammarChange::PrependChoice("SelectAtom", "PipeSelectAtom"));
		return changes;
	}
};

//! A statement whose transform hands choice and optional wrapper nodes to Transform directly
class WrappedTransformNodesChange final : public GrammarExtension {
public:
	WrappedTransformNodesChange()
	    : GrammarExtension("wrapped_transform_nodes", "Transform limit clauses through wrapper parse nodes") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddChoice("UnreservedKeyword", "'WRAPPED'"));
		changes.push_back(GrammarChange::AddChoice("UnreservedKeyword", "'WRAPPED_NESTED'"));
		changes.push_back(GrammarChange::AddChoice("UnreservedKeyword", "'WRAPPED_CHILD'"));
		changes.push_back(GrammarChange::AddChoice("UnreservedKeyword", "'WRAPPED_BAD'"));
		changes.push_back(
		    GrammarChange::AddRule("WrappedSelectAtom <- 'WRAPPED' (LimitClause / OffsetClause) OffsetClause?",
		                           StartWrappedSelectAtomTransform));
		changes.push_back(GrammarChange::AddRule("WrappedNestedAtom <- 'WRAPPED_NESTED' (LimitClause / OffsetClause)?",
		                                         StartWrappedNestedAtomTransform));
		changes.push_back(GrammarChange::AddRule("WrappedChildAtom <- 'WRAPPED_CHILD' (LimitClause / OffsetClause)",
		                                         StartWrappedChildAtomTransform));
		changes.push_back(GrammarChange::AddRule(
		    "WrappedBadAtom <- 'WRAPPED_BAD' (LimitClause OffsetClause / LimitClause)?", StartWrappedBadAtomTransform));
		changes.push_back(GrammarChange::PrependChoice("SelectAtom", "WrappedSelectAtom"));
		changes.push_back(GrammarChange::PrependChoice("SelectAtom", "WrappedNestedAtom"));
		changes.push_back(GrammarChange::PrependChoice("SelectAtom", "WrappedChildAtom"));
		changes.push_back(GrammarChange::PrependChoice("SelectAtom", "WrappedBadAtom"));
		return changes;
	}
};

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(loadable_grammar_extension_demo, loader) {
	GrammarExtension::Register(loader.GetDatabaseInstance(), make_shared_ptr<PipeSQLGrammarChange>());
	GrammarExtension::Register(loader.GetDatabaseInstance(), make_shared_ptr<WrappedTransformNodesChange>());
}
}
