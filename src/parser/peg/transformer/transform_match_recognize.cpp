#include "duckdb/function/match_recognize.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/pattern_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// MATCH_RECOGNIZE clause
//===--------------------------------------------------------------------===//
namespace {

MatchRecognizeClause MakeClause(MatchRecognizeClauseKind kind) {
	MatchRecognizeClause result;
	result.kind = kind;
	return result;
}

const char *ClauseName(MatchRecognizeClauseKind kind) {
	switch (kind) {
	case MatchRecognizeClauseKind::PARTITION:
		return "PARTITION BY";
	case MatchRecognizeClauseKind::ORDER_BY:
		return "ORDER BY";
	case MatchRecognizeClauseKind::MEASURES:
		return "MEASURES";
	case MatchRecognizeClauseKind::ROWS:
		return "ROWS PER MATCH";
	case MatchRecognizeClauseKind::SKIP:
		return "AFTER MATCH SKIP";
	case MatchRecognizeClauseKind::PATTERN:
		return "PATTERN";
	case MatchRecognizeClauseKind::SUBSET:
		return "SUBSET";
	default:
		return "DEFINE";
	}
}

} // namespace

MatchRecognizeClause
PEGTransformerFactory::TransformMRPartition(PEGTransformer &transformer,
                                            vector<unique_ptr<ParsedExpression>> window_partition) {
	auto result = MakeClause(MatchRecognizeClauseKind::PARTITION);
	result.expressions = std::move(window_partition);
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMROrderBy(PEGTransformer &transformer,
                                                               vector<OrderByNode> order_by_clause) {
	auto result = MakeClause(MatchRecognizeClauseKind::ORDER_BY);
	result.order_by = std::move(order_by_clause);
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRMeasures(PEGTransformer &transformer,
                                                                vector<unique_ptr<ParsedExpression>> measures_clause) {
	auto result = MakeClause(MatchRecognizeClauseKind::MEASURES);
	result.expressions = std::move(measures_clause);
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRRows(PEGTransformer &transformer,
                                                            const MatchRecognizeRows &rows_per_match) {
	auto result = MakeClause(MatchRecognizeClauseKind::ROWS);
	result.rows = rows_per_match;
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRSkip(PEGTransformer &transformer,
                                                            MatchRecognizeAfterMatchClause after_match_skip) {
	auto result = MakeClause(MatchRecognizeClauseKind::SKIP);
	result.skip = std::move(after_match_skip);
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRPattern(PEGTransformer &transformer,
                                                               unique_ptr<ParsedExpression> pattern_clause) {
	auto result = MakeClause(MatchRecognizeClauseKind::PATTERN);
	result.pattern = std::move(pattern_clause);
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRSubset(PEGTransformer &transformer,
                                                              vector<MatchRecognizeSubset> subset_clause) {
	auto result = MakeClause(MatchRecognizeClauseKind::SUBSET);
	result.subsets = std::move(subset_clause);
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRDefineAuto(PEGTransformer &transformer) {
	auto result = MakeClause(MatchRecognizeClauseKind::DEFINE);
	result.define_auto = true;
	return result;
}

MatchRecognizeClause PEGTransformerFactory::TransformMRDefine(PEGTransformer &transformer,
                                                              vector<unique_ptr<ParsedExpression>> define_clause) {
	auto result = MakeClause(MatchRecognizeClauseKind::DEFINE);
	result.expressions = std::move(define_clause);
	return result;
}

//! The clauses may arrive in any order, so which is which is settled here rather than by the grammar
unique_ptr<TableRef>
PEGTransformerFactory::TransformMatchRecognizeBody(PEGTransformer &transformer,
                                                   vector<MatchRecognizeClause> match_recognize_clause) {
	auto config = make_uniq<MatchRecognizeConfig>();
	config->rows_per_match = MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_DEFAULT;
	config->after_match = MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_DEFAULT;

	// one flag per clause kind, indexed by the kind itself
	static_assert(static_cast<idx_t>(MatchRecognizeClauseKind::DEFINE) + 1 == 8,
	              "MATCH_RECOGNIZE gained a clause kind that this array has no room for");
	bool seen[8] = {false, false, false, false, false, false, false, false};
	for (auto &clause : match_recognize_clause) {
		auto index = static_cast<idx_t>(clause.kind);
		if (seen[index]) {
			throw ParserException("MATCH_RECOGNIZE has more than one %s clause", ClauseName(clause.kind));
		}
		seen[index] = true;
		switch (clause.kind) {
		case MatchRecognizeClauseKind::PARTITION:
			config->partition_expressions = std::move(clause.expressions);
			break;
		case MatchRecognizeClauseKind::ORDER_BY:
			config->order_by_expressions = std::move(clause.order_by);
			break;
		case MatchRecognizeClauseKind::MEASURES:
			config->measures_expression_list = std::move(clause.expressions);
			break;
		case MatchRecognizeClauseKind::ROWS:
			config->rows_per_match = clause.rows;
			break;
		case MatchRecognizeClauseKind::SKIP:
			config->after_match = clause.skip.after_match;
			config->after_match_variable = clause.skip.variable;
			break;
		case MatchRecognizeClauseKind::PATTERN:
			config->pattern = std::move(clause.pattern);
			break;
		case MatchRecognizeClauseKind::SUBSET:
			config->subsets = std::move(clause.subsets);
			break;
		default:
			config->defines_expression_list = std::move(clause.expressions);
			config->define_auto = clause.define_auto;
			break;
		}
	}
	if (!seen[static_cast<idx_t>(MatchRecognizeClauseKind::PATTERN)]) {
		throw ParserException("MATCH_RECOGNIZE requires a PATTERN clause");
	}
	// "DEFINE is a mandatory clause" (ISO/IEC 19075-5 4.16), even though a variable within it needs no
	// condition of its own: one without is a predicate that is always true, so a pattern that only
	// counts rows still writes the clause and leaves its other variables out of it.
	string first_symbol;
	if (config->pattern) {
		ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
		    *config->pattern, [&](const ColumnRefExpression &colref) {
			    if (first_symbol.empty()) {
				    first_symbol = colref.GetColumnName().GetIdentifierName();
			    }
		    });
	}
	// a pattern that declares no variable at all - the empty pattern, or an anchor on its own - has
	// nothing to define, and an empty DEFINE is not something the syntax can say
	if (!first_symbol.empty() && !seen[static_cast<idx_t>(MatchRecognizeClauseKind::DEFINE)]) {
		throw ParserException("MATCH_RECOGNIZE requires a DEFINE clause. A variable left out of it matches any "
		                      "row, but the clause itself has to be there: a pattern that constrains nothing "
		                      "writes DEFINE %s AS true",
		                      first_symbol);
	}

	// the input table is attached by TransformTableRef
	return make_uniq<MatchRecognizeRef>(nullptr, std::move(config));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableMatchRecognizeClause(
    PEGTransformer &transformer, unique_ptr<TableRef> match_recognize_body, const optional<TableAlias> &table_alias) {
	if (table_alias) {
		match_recognize_body->alias = table_alias->name;
		match_recognize_body->column_name_alias = table_alias->column_name_alias;
	}
	return match_recognize_body;
}

bool PEGTransformerFactory::TransformRunningSemantics(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformFinalSemantics(PEGTransformer &transformer) {
	return true;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformMeasuresElement(PEGTransformer &transformer,
                                                                             unique_ptr<ParsedExpression> expression,
                                                                             const Identifier &col_label_or_string) {
	expression->SetAlias(col_label_or_string);
	return expression;
}

//! RUNNING and FINAL are carried to the binder as a wrapper, which knows the frame they turn into
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformMeasureSemanticsExpression(PEGTransformer &transformer, const bool &measure_semantics,
                                                           unique_ptr<ParsedExpression> function_expression) {
	vector<unique_ptr<ParsedExpression>> wrapped;
	wrapped.push_back(std::move(function_expression));
	return make_uniq<FunctionExpression>(
	    measure_semantics ? MATCH_RECOGNIZE_FINAL_MARKER : MATCH_RECOGNIZE_RUNNING_MARKER, std::move(wrapped));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefineElement(PEGTransformer &transformer,
                                                                           const Identifier &col_label_or_string,
                                                                           unique_ptr<ParsedExpression> expression) {
	expression->SetAlias(col_label_or_string);
	return expression;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformMeasuresClause(PEGTransformer &transformer,
                                               vector<unique_ptr<ParsedExpression>> measures_element) {
	return measures_element;
}

MatchRecognizeSubset PEGTransformerFactory::TransformSubsetElement(PEGTransformer &transformer,
                                                                   const Identifier &col_label_or_string,
                                                                   const vector<Identifier> &col_label_or_string_1) {
	MatchRecognizeSubset result;
	result.name = col_label_or_string.GetIdentifierName();
	for (auto &member : col_label_or_string_1) {
		result.members.push_back(member.GetIdentifierName());
	}
	return result;
}

vector<MatchRecognizeSubset> PEGTransformerFactory::TransformSubsetClause(PEGTransformer &transformer,
                                                                          vector<MatchRecognizeSubset> subset_element) {
	return subset_element;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformDefineClause(PEGTransformer &transformer,
                                             vector<unique_ptr<ParsedExpression>> define_element) {
	return define_element;
}

//===--------------------------------------------------------------------===//
// ROWS PER MATCH
//===--------------------------------------------------------------------===//
MatchRecognizeRows PEGTransformerFactory::TransformOneRowPerMatch(PEGTransformer &transformer) {
	return MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ONE;
}

MatchRecognizeRows
PEGTransformerFactory::TransformAllRowsPerMatch(PEGTransformer &transformer,
                                                const optional<MatchRecognizeRows> &all_rows_empty_matches) {
	// SHOW EMPTY MATCHES is what ALL ROWS PER MATCH means when it says nothing
	return all_rows_empty_matches ? *all_rows_empty_matches : MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL;
}

MatchRecognizeRows PEGTransformerFactory::TransformShowEmptyMatches(PEGTransformer &transformer) {
	return MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL;
}

MatchRecognizeRows PEGTransformerFactory::TransformOmitEmptyMatches(PEGTransformer &transformer) {
	return MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_OMIT_EMPTY;
}

MatchRecognizeRows PEGTransformerFactory::TransformWithUnmatchedRows(PEGTransformer &transformer) {
	return MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_UNMATCHED;
}

//===--------------------------------------------------------------------===//
// AFTER MATCH SKIP
//===--------------------------------------------------------------------===//
MatchRecognizeAfterMatchClause PEGTransformerFactory::TransformSkipToNextRow(PEGTransformer &transformer) {
	return MatchRecognizeAfterMatchClause {MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_NEXT_ROW, ""};
}

MatchRecognizeAfterMatchClause PEGTransformerFactory::TransformSkipPastLastRow(PEGTransformer &transformer) {
	return MatchRecognizeAfterMatchClause {MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_ROW, ""};
}

MatchRecognizeAfterMatchClause PEGTransformerFactory::TransformSkipToFirstVar(PEGTransformer &transformer,
                                                                              const Identifier &col_label_or_string) {
	return MatchRecognizeAfterMatchClause {MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_FIRST_VAR,
	                                       col_label_or_string.GetIdentifierName()};
}

MatchRecognizeAfterMatchClause PEGTransformerFactory::TransformSkipToLastVar(PEGTransformer &transformer,
                                                                             const Identifier &col_label_or_string) {
	return MatchRecognizeAfterMatchClause {MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_VAR,
	                                       col_label_or_string.GetIdentifierName()};
}

MatchRecognizeAfterMatchClause PEGTransformerFactory::TransformSkipToVar(PEGTransformer &transformer,
                                                                         const Identifier &col_label_or_string) {
	// naming a variable without saying which of its rows resumes at its last one
	return MatchRecognizeAfterMatchClause {MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_VAR,
	                                       col_label_or_string.GetIdentifierName()};
}

//===--------------------------------------------------------------------===//
// PATTERN
//===--------------------------------------------------------------------===//
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRowPattern(PEGTransformer &transformer, unique_ptr<ParsedExpression> row_pattern_term,
                                           optional<vector<unique_ptr<ParsedExpression>>> row_pattern_alternative) {
	auto result = std::move(row_pattern_term);
	if (!row_pattern_alternative) {
		return result;
	}
	// alternation is left-associative: A | B | C becomes ((A | B) | C)
	for (auto &alternative : *row_pattern_alternative) {
		result = make_uniq_base<ParsedExpression, AlternationExpression>(std::move(result), std::move(alternative));
	}
	return result;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRowPatternTerm(PEGTransformer &transformer,
                                               vector<unique_ptr<ParsedExpression>> row_pattern_factor) {
	D_ASSERT(!row_pattern_factor.empty());
	if (row_pattern_factor.size() == 1) {
		return std::move(row_pattern_factor[0]);
	}
	return make_uniq_base<ParsedExpression, ConcatenationExpression>(std::move(row_pattern_factor));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRowPatternFactor(PEGTransformer &transformer,
                                                 unique_ptr<ParsedExpression> row_pattern_primary,
                                                 const optional<MatchRecognizeQuantifier> &row_pattern_quantifier) {
	MatchRecognizeQuantifier quantifier;
	if (row_pattern_quantifier) {
		quantifier = *row_pattern_quantifier;
	} else {
		// an unquantified symbol or group matches exactly once
		quantifier.min_count = 1;
		quantifier.max_count = 1;
	}
	return make_uniq_base<ParsedExpression, QuantifiedExpression>(std::move(row_pattern_primary), quantifier.min_count,
	                                                              quantifier.max_count, false, quantifier.reluctant);
}

//! PERMUTE(A, B, C) is the alternation of every arrangement of its parts, in lexicographic order of the
//! list as written. Expanding it here keeps it out of the matcher.
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRowPatternPermute(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> row_pattern) {
	// every arrangement is spelled out, so the program grows with the factorial of the list
	static constexpr idx_t MAX_PERMUTE_PARTS = 6;
	if (row_pattern.size() > MAX_PERMUTE_PARTS) {
		throw ParserException("PERMUTE takes at most %llu parts, because it stands for every order of them",
		                      MAX_PERMUTE_PARTS);
	}
	vector<idx_t> order;
	for (idx_t i = 0; i < row_pattern.size(); i++) {
		order.push_back(i);
	}

	unique_ptr<ParsedExpression> result;
	do {
		vector<unique_ptr<ParsedExpression>> parts;
		parts.reserve(order.size());
		for (auto index : order) {
			parts.push_back(row_pattern[index]->Copy());
		}
		auto arrangement = parts.size() == 1
		                       ? std::move(parts[0])
		                       : make_uniq_base<ParsedExpression, ConcatenationExpression>(std::move(parts));
		// alternation is left-associative here, as it is when it is written out
		result =
		    result ? make_uniq_base<ParsedExpression, AlternationExpression>(std::move(result), std::move(arrangement))
		           : std::move(arrangement);
	} while (std::next_permutation(order.begin(), order.end()));
	return result;
}

//! {- P -} matches P and takes part in the match, but its rows are left out of the output
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRowPatternExclusion(PEGTransformer &transformer,
                                                    unique_ptr<ParsedExpression> row_pattern) {
	auto result = make_uniq<QuantifiedExpression>(std::move(row_pattern), 1, 1);
	result->excluded = true;
	return std::move(result);
}

//! () matches where it stands and takes no row, which is a concatenation of nothing
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformRowPatternEmpty(PEGTransformer &transformer) {
	return make_uniq_base<ParsedExpression, ConcatenationExpression>(vector<unique_ptr<ParsedExpression>>());
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformPatternStart(PEGTransformer &transformer) {
	return make_uniq_base<ParsedExpression, AnchorExpression>(false);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformPatternEnd(PEGTransformer &transformer) {
	return make_uniq_base<ParsedExpression, AnchorExpression>(true);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformRowPatternLabel(PEGTransformer &transformer,
                                                                             const Identifier &col_label_or_string) {
	return make_uniq_base<ParsedExpression, ColumnRefExpression>(col_label_or_string);
}

//===--------------------------------------------------------------------===//
// PATTERN quantifiers
//===--------------------------------------------------------------------===//
static idx_t QuantifierCount(const unique_ptr<ParsedExpression> &number_literal) {
	if (number_literal->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("Pattern quantifier bounds must be integer constants");
	}
	auto value = number_literal->Cast<ConstantExpression>().GetLiteral().ToValue();
	if (!value.DefaultTryCastAs(LogicalType::UBIGINT)) {
		throw ParserException("Pattern quantifier bounds must be non-negative integers");
	}
	return NumericCast<idx_t>(value.GetValue<uint64_t>());
}

//! A quantifier that allows at most zero repetitions matches nothing at all, which the pattern
//! algebra has the empty pattern () for: {n} takes n > 0, {n,m} takes 0 < m, {,m} takes m > 0
static void CheckQuantifierUpperBound(optional_idx max_count) {
	if (max_count.IsValid() && max_count.GetIndex() == 0) {
		throw ParserException("A pattern quantifier's upper bound must be at least 1");
	}
}

MatchRecognizeQuantifier PEGTransformerFactory::TransformQuantifierStar(PEGTransformer &transformer) {
	MatchRecognizeQuantifier result;
	result.min_count = 0;
	return result;
}

MatchRecognizeQuantifier PEGTransformerFactory::TransformQuantifierPlus(PEGTransformer &transformer) {
	MatchRecognizeQuantifier result;
	result.min_count = 1;
	return result;
}

//! A trailing ? on any of the forms below makes it reluctant
MatchRecognizeQuantifier
PEGTransformerFactory::TransformRowPatternQuantifier(PEGTransformer &transformer,
                                                     const MatchRecognizeQuantifier &row_pattern_quantifier_kind,
                                                     const optional<bool> &quantifier_reluctant) {
	auto result = row_pattern_quantifier_kind;
	result.reluctant = quantifier_reluctant.has_value();
	return result;
}

bool PEGTransformerFactory::TransformQuantifierReluctant(PEGTransformer &transformer) {
	return true;
}

MatchRecognizeQuantifier PEGTransformerFactory::TransformQuantifierOptional(PEGTransformer &transformer) {
	MatchRecognizeQuantifier result;
	result.min_count = 0;
	result.max_count = 1;
	return result;
}

MatchRecognizeQuantifier
PEGTransformerFactory::TransformQuantifierMinMax(PEGTransformer &transformer,
                                                 unique_ptr<ParsedExpression> number_literal,
                                                 unique_ptr<ParsedExpression> number_literal_1) {
	MatchRecognizeQuantifier result;
	result.min_count = QuantifierCount(number_literal);
	result.max_count = QuantifierCount(number_literal_1);
	if (result.min_count.GetIndex() > result.max_count.GetIndex()) {
		throw ParserException("Min count cannot be larger than max count");
	}
	CheckQuantifierUpperBound(result.max_count);
	return result;
}

MatchRecognizeQuantifier PEGTransformerFactory::TransformQuantifierMin(PEGTransformer &transformer,
                                                                       unique_ptr<ParsedExpression> number_literal) {
	MatchRecognizeQuantifier result;
	result.min_count = QuantifierCount(number_literal);
	return result;
}

MatchRecognizeQuantifier PEGTransformerFactory::TransformQuantifierMax(PEGTransformer &transformer,
                                                                       unique_ptr<ParsedExpression> number_literal) {
	MatchRecognizeQuantifier result;
	result.max_count = QuantifierCount(number_literal);
	CheckQuantifierUpperBound(result.max_count);
	return result;
}

MatchRecognizeQuantifier PEGTransformerFactory::TransformQuantifierExact(PEGTransformer &transformer,
                                                                         unique_ptr<ParsedExpression> number_literal) {
	MatchRecognizeQuantifier result;
	result.min_count = QuantifierCount(number_literal);
	result.max_count = result.min_count;
	CheckQuantifierUpperBound(result.max_count);
	return result;
}

} // namespace duckdb
