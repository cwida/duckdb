#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/enums/trigger_type.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/token_iterator.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/enums/merge_action_type.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"

namespace duckdb {

static unique_ptr<SQLStatement> ExtractAndTransformStatement(PEGTransformer &transformer,
                                                             const TokenIterator &token_iterator, ParseResult &stmt_pr,
                                                             optional_idx terminator_offset) {
	auto stmt = transformer.Transform<unique_ptr<SQLStatement>>(stmt_pr);

	if (!transformer.named_parameter_map.empty()) {
		stmt->named_param_map = transformer.named_parameter_map;
	}
	if (!transformer.pivot_entries.empty()) {
		stmt = transformer.CreatePivotStatement(std::move(stmt));
		// Unpacking discards the wrapper, so the parts keep their map; the wrapper carries it for introspection.
		stmt->named_param_map = transformer.named_parameter_map;
	}
	transformer.Clear();

	// Calculate location and length cleanly
	auto offset = stmt_pr.GetOffset();
	if (offset.IsValid()) {
		auto start = offset.GetIndex();
		idx_t end_index = terminator_offset.IsValid() ? terminator_offset.GetIndex() : token_iterator.EndOffset();
		stmt->stmt_location = QueryLocation(start, end_index - start);
	}

	return stmt;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformTopLevelStatement(TokenIterator &token_iterator,
                                                                           ParserOptions &options,
                                                                           const CompiledGrammar &grammar,
                                                                           optional_idx &failure_index) {
	if (!token_iterator.Current()) {
		return nullptr;
	}
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_result_allocator;
	idx_t max_token_index = token_iterator.Position();
	// the packrat cache outlives the match processes, so they need separate arenas
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	ArenaAllocator packrat_allocator(Allocator::DefaultAllocator());
	ParserPackratCache packrat_cache(packrat_allocator, token_iterator.Position(), token_iterator.Size());
	MatchContext match_context(suggestions, parse_result_allocator, process_allocator, max_token_index,
	                           MatchMode::BUILD_PARSE_RESULT, options.identifier_case_mode, &packrat_cache);
	MatchState state(token_iterator, match_context);
	auto match_result = grammar.TopLevelStatementMatcher().MatchParseResult(state);
	process_allocator.FreeAll();
	if (!match_result.IsSuccess()) {
		failure_index = state.GetMaxTokenIndex();
		return nullptr;
	}
	D_ASSERT(match_result.HasParseResult());

	// Advance the caller's cursor past the consumed tokens.
	token_iterator.SetPosition(state.token_iterator);

	// TopLevelStatement <- Statement? (';'+ / EndOfInput)
	//   child 0: Optional<Statement>
	//   child 1: bracket-wrapper list around Choice<';'+ | EndOfInput>
	auto &tls = match_result.GetParseResult()->Cast<ListParseResult>();
	auto &stmt_opt = tls.Child<OptionalParseResult>(0);
	if (!stmt_opt.HasResult()) {
		// separator-only or EOI-only TopLevelStatement — no statement to yield
		return nullptr;
	}
	auto &term_wrapper = tls.Child<ListParseResult>(1);
	auto &term_inner = term_wrapper.Child<ChoiceParseResult>(0).GetResult();
	optional_idx terminator_offset;
	if (term_inner.type != ParseResultType::END_OF_INPUT) {
		auto semi_children = term_inner.Cast<RepeatParseResult>().GetChildren();
		if (!semi_children.empty()) {
			terminator_offset = semi_children[0].get().GetOffset();
		}
	}

	ArenaAllocator transformer_allocator(Allocator::DefaultAllocator());
	PEGTransformer transformer(transformer_allocator, token_iterator, options, grammar);

	return ExtractAndTransformStatement(transformer, token_iterator, stmt_opt.GetResult(), terminator_offset);
}

PEGTransformerFactory::PEGTransformerFactory(ParsedGrammar &grammar_p) : grammar(grammar_p) {
	case_insensitive_set_t collapsible_rules;
	//===--------------------------------------------------------------------===//
	// START GENERATED COLLAPSIBLE RULES
	//===--------------------------------------------------------------------===//
	collapsible_rules.insert("Expression");
	collapsible_rules.insert("ColumnDefaultExpr");
	collapsible_rules.insert("LambdaArrowExpression");
	collapsible_rules.insert("LogicalOrExpression");
	collapsible_rules.insert("ColDefOrExpr");
	collapsible_rules.insert("LogicalAndExpression");
	collapsible_rules.insert("ColDefAndExpr");
	collapsible_rules.insert("LogicalNotExpression");
	collapsible_rules.insert("IsExpression");
	collapsible_rules.insert("IsDistinctFromExpression");
	collapsible_rules.insert("ComparisonExpression");
	collapsible_rules.insert("BetweenInLikeExpression");
	collapsible_rules.insert("OtherOperatorExpression");
	collapsible_rules.insert("InfixOtherOperatorExpression");
	collapsible_rules.insert("BitwiseExpression");
	collapsible_rules.insert("TildeExpression");
	collapsible_rules.insert("AdditiveExpression");
	collapsible_rules.insert("MultiplicativeExpression");
	collapsible_rules.insert("ExponentiationExpression");
	collapsible_rules.insert("CollateExpression");
	collapsible_rules.insert("AtTimeZoneExpression");
	collapsible_rules.insert("PrefixExpression");
	collapsible_rules.insert("BaseExpression");
	collapsible_rules.insert("SelectSetOpChain");
	collapsible_rules.insert("IntersectChain");
	collapsible_rules.insert("TableRef");
	//===--------------------------------------------------------------------===//
	// END GENERATED COLLAPSIBLE RULES
	//===--------------------------------------------------------------------===//

	for (auto &entry : GeneratedTransformFrameOps()) {
		auto process_info = entry.second;
		grammar.SetTransformProcess(
		    entry.first,
		    [process_info](PEGTransformer &transformer, ParseResult &parse_result) -> arena_ptr<TransformProcess> {
			    return transformer.MakeProcess<GeneratedTransformProcess>(transformer, TransformInput {parse_result},
			                                                              *process_info);
		    },
		    collapsible_rules.count(entry.first) > 0);
	}
	//===--------------------------------------------------------------------===//
	// START GENERATED EXPRESSION DEPTH RULES
	//===--------------------------------------------------------------------===//
	grammar.SetExpressionDepth("SingleExpression", ExpressionDepthKind::NESTING);
	grammar.SetExpressionDepth("Type", ExpressionDepthKind::NESTING);
	grammar.SetExpressionDepth("NestedType", ExpressionDepthKind::NESTING);
	grammar.SetExpressionDepth("SubqueryReference", ExpressionDepthKind::NESTING);
	grammar.SetExpressionDepth("CustomPrefixExpression", ExpressionDepthKind::NESTING);
	grammar.SetExpressionDepth("LambdaArrowExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("NotExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("IsExpressionContinuation", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("IsDistinctFromExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("ComparisonExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("BetweenInLikeExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("InfixOtherOperatorExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("BitwiseExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("TildeExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("AdditiveExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("MultiplicativeExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("ExponentiationExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("CollateExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("AtTimeZoneExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("PrefixExpression", ExpressionDepthKind::CHAIN);
	grammar.SetExpressionDepth("IndirectionList", ExpressionDepthKind::CHAIN);
	//===--------------------------------------------------------------------===//
	// END GENERATED EXPRESSION DEPTH RULES
	//===--------------------------------------------------------------------===//
}

void PEGTransformerFactory::RegisterDefaultTransforms(ParsedGrammar &grammar) {
	PEGTransformerFactory factory(grammar);
}

template <class FUNC>
static idx_t VisitParseResultsFromList(ParseResult &parse_result, FUNC &&func) {
	// List(D) <- D (',' D)* ','?; validate every child before returning, including when only counting.
	auto &list_pr = parse_result.Cast<ListParseResult>();
	func(list_pr.GetChild(0));
	idx_t count = 1;
	auto &opt_child = list_pr.Child<OptionalParseResult>(1);
	if (opt_child.HasResult()) {
		auto &repeat_result = opt_child.GetResult().Cast<RepeatParseResult>();
		for (auto &child : repeat_result.GetChildren()) {
			auto &list_child = child.get().Cast<ListParseResult>();
			func(list_child.GetChild(1));
			count++;
		}
	}
	return count;
}

vector<reference<ParseResult>> PEGTransformerFactory::ExtractParseResultsFromList(ParseResult &parse_result) {
	vector<reference<ParseResult>> result;
	VisitParseResultsFromList(parse_result, [&result](ParseResult &child) { result.push_back(child); });
	return result;
}

idx_t PEGTransformerFactory::CountParseResultsFromList(ParseResult &parse_result) {
	return VisitParseResultsFromList(parse_result, [](ParseResult &) {});
}

ParseResult &PEGTransformerFactory::ExtractResultFromParens(ParseResult &parse_result) {
	// Parens(D) <- '(' D ')'
	auto &list_pr = parse_result.Cast<ListParseResult>();
	return list_pr.GetChild(1);
}

bool PEGTransformerFactory::ExpressionIsEmptyStar(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::STAR) {
		return false;
	}
	auto &star = expr.Cast<StarExpression>();
	if (star.IsColumns()) {
		return false;
	}
	if (!star.ExcludeList().empty()) {
		return false;
	}
	if (!star.ReplaceList().empty()) {
		return false;
	}
	if (!star.RenameList().empty()) {
		return false;
	}
	return true;
}

QualifiedName PEGTransformerFactory::StringToQualifiedName(vector<string> input) {
	if (input.empty()) {
		throw InternalException("QualifiedName cannot be made with an empty input.");
	}
	// Store the qualification as the full path - it may be deeper than [catalog, schema, name] for nested
	// schemas. The leading component is resolved to a catalog at bind time (Binder::BindTableName).
	vector<Identifier> path;
	path.reserve(input.size());
	for (auto &part : input) {
		path.emplace_back(std::move(part));
	}
	return QualifiedName::FromPath(std::move(path));
}

QualifiedColumnName PEGTransformerFactory::StringToQualifiedColumnName(const vector<string> &input) {
	if (input.empty()) {
		throw InternalException("QualifiedColumnName cannot be made with an empty input.");
	}
	auto identifiers = StringsToIdentifiers(input);
	if (identifiers.size() == 1) {
		return QualifiedColumnName(std::move(identifiers[0]));
	} else if (identifiers.size() == 2) {
		return QualifiedColumnName(std::move(identifiers[0]), std::move(identifiers[1]));
	} else if (identifiers.size() == 3) {
		QualifiedColumnName result;
		result.schema = std::move(identifiers[0]);
		result.table = std::move(identifiers[1]);
		result.column = std::move(identifiers[2]);
		return result;
	} else if (identifiers.size() == 4) {
		QualifiedColumnName result;
		result.catalog = std::move(identifiers[0]);
		result.schema = std::move(identifiers[1]);
		result.table = std::move(identifiers[2]);
		result.column = std::move(identifiers[3]);
		return result;
	}
	throw ParserException("Expected at most 4 entries (catalog.schema.table.column), but found %zu entries (input: %s)",
	                      input.size(), StringUtil::Join(input, "."));
}

LogicalType PEGTransformerFactory::GetIntervalTargetType(DatePartSpecifier date_part) {
	switch (date_part) {
	case DatePartSpecifier::YEAR:
	case DatePartSpecifier::MONTH:
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::QUARTER:
	case DatePartSpecifier::DECADE:
	case DatePartSpecifier::CENTURY:
	case DatePartSpecifier::MILLENNIUM:
		return LogicalType::INTEGER;
	case DatePartSpecifier::HOUR:
	case DatePartSpecifier::MINUTE:
	case DatePartSpecifier::MICROSECONDS:
		return LogicalType::BIGINT;
	case DatePartSpecifier::MILLISECONDS:
	case DatePartSpecifier::SECOND:
		return LogicalType::DOUBLE;
	default:
		throw InternalException("Unsupported interval post-fix");
	}
}

LogicalType PEGTransformerFactory::ApplyColumnCollation(const optional<LogicalType> &type,
                                                        unique_ptr<ParsedExpression> collation) {
	if (!type) {
		throw ParserException("Specify the VARCHAR type to set a collation");
	}
	if (!type->IsUnbound()) {
		throw InternalException("Expected only unbound types here");
	}
	auto &expr = UnboundType::GetTypeExpression(*type);
	if (expr->GetExpressionClass() != ExpressionClass::TYPE) {
		throw InternalException("Expected a type expression");
	}
	auto &type_expr = expr->Cast<TypeExpression>();
	if (DefaultTypeGenerator::GetDefaultType(type_expr.GetTypeName()) != LogicalTypeId::VARCHAR) {
		throw ParserException("Only VARCHAR columns can have collations!");
	}
	vector<unique_ptr<ParsedExpression>> type_children;
	type_children.push_back(std::move(collation));
	return LogicalType::UNBOUND(make_uniq<TypeExpression>(Identifier("VARCHAR"), std::move(type_children)));
}

} // namespace duckdb
