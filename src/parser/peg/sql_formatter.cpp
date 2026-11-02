//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sql_formatter.cpp
//
// SQL pretty-printer that uses the PEG tokenizer for lexical analysis.
//
//===----------------------------------------------------------------------===//

#include "duckdb/parser/peg/sql_formatter.hpp"
#include "duckdb/parser/peg/keyword_helper/duckdb_keyword_helper.hpp"
#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/common/string_util.hpp"
#include "utf8proc_wrapper.hpp"

#include <unordered_set>

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// SQLFormatter class declaration
// ─────────────────────────────────────────────────────────────────────────────

class SQLFormatter {
public:
	explicit SQLFormatter(const FormatterConfig &config);
	string Format(const string &sql);

private:
	const FormatterConfig &config;

	// Keyword classification helpers
	static bool IsClauseKeyword(const string &kw);
	static bool IsJoinModifier(const string &kw);
	static bool IsClauseKeywordLine(const string &trimmed);
	static bool ShouldUppercase(const string &kw);

	// Token helpers
	static vector<MatcherToken> Tokenize(const string &sql);
	static bool IsParameterStart(const vector<MatcherToken> &tokens, idx_t token_idx);
	static bool SkipNonCodeToken(const vector<MatcherToken> &tokens, idx_t &token_idx, idx_t &pos);

	// Token lookahead helpers
	static string PeekKeyword(const vector<MatcherToken> &tokens, idx_t i, idx_t offset);
	static idx_t DetectCompoundClause(const vector<MatcherToken> &tokens, idx_t i, string &compound_text,
	                                  string &original_text);

	// Line helpers (shared by Phases 2-5)
	static idx_t LeadingSpaces(const string &line);
	static string TrimLeft(const string &line);
	static vector<string> SplitLines(const string &formatted);
	static string JoinLines(const vector<string> &lines);

	// Phase 1: token-stream -> multiline string
	string ApplyCase(const string &upper_kw, const string &orig_kw, bool is_structural = false) const;
	string FormatMultiline(const string &sql, const vector<MatcherToken> &tokens) const;

	// Phase 2: inline short struct literals
	string InlineStructLiterals(const string &formatted) const;

	// Phase 3: inline short clauses
	string MergeShortClauses(const string &formatted) const;

	// Phase 4: lift first WHERE/HAVING predicate onto the keyword's line
	static bool IsConditionClauseLine(const string &trimmed);
	static bool StartsWithConjunction(const string &trimmed);
	string CollapseFirstCondition(const string &formatted) const;

	// Phase 5: expand CREATE TABLE column list to one column per line
	static vector<string> SplitTopLevelCommas(const string &s);
	static idx_t FindMatchingClose(const string &s, idx_t open_pos);
	static idx_t MatchCreateTablePrefix(const string &upper_trimmed);
	string ExpandTableDefinition(const string &formatted) const;

	// Phase 6: expand long CASE expressions to one WHEN/ELSE per line
	static bool MatchKeywordAt(const string &s, idx_t pos, const char *keyword);
	static idx_t FindKeywordAny(const string &s, idx_t from_pos, const char *keyword);
	static idx_t FindCaseEnd(const string &s, idx_t case_pos);
	static vector<string> SplitCaseBranches(const string &content);
	string ExpandCaseExpressions(const string &formatted) const;

	// Phase 7: expand long parenthesized comma-lists to one value per line
	static idx_t FindLongestParenList(const string &line, idx_t from_pos);
	string ExpandLongParenLists(const string &formatted) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Constructor and top-level Format
// ─────────────────────────────────────────────────────────────────────────────

SQLFormatter::SQLFormatter(const FormatterConfig &config) : config(config) {
}

vector<MatcherToken> SQLFormatter::Tokenize(const string &sql) {
	auto &keyword_helper = DuckDBKeywordHelper::Instance();
	vector<MatcherToken> tokens;
	HighlightTokenizerBehavior behavior(sql, tokens);
	Tokenizer tokenizer(keyword_helper);
	tokenizer.TokenizeInput(behavior);
	if (!tokens.empty()) {
		auto back_type = tokens.back().type;
		if (back_type == TokenType::END_OF_INPUT || back_type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			tokens.pop_back();
		}
	}

	return tokens;
}

bool SQLFormatter::IsParameterStart(const vector<MatcherToken> &tokens, idx_t token_idx) {
	return tokens[token_idx].type == TokenType::OPERATOR && tokens[token_idx].text == "$" &&
	       token_idx + 1 < tokens.size() &&
	       (tokens[token_idx + 1].type == TokenType::IDENTIFIER || tokens[token_idx + 1].type == TokenType::KEYWORD ||
	        tokens[token_idx + 1].type == TokenType::NUMBER_LITERAL);
}

bool SQLFormatter::SkipNonCodeToken(const vector<MatcherToken> &tokens, idx_t &token_idx, idx_t &pos) {
	while (token_idx < tokens.size() && tokens[token_idx].offset + tokens[token_idx].length <= pos) {
		token_idx++;
	}
	if (token_idx == tokens.size()) {
		return false;
	}
	auto &token = tokens[token_idx];
	if (token.offset > pos) {
		return false;
	}
	// Keep parameters opaque in the later text-based formatting passes.
	if (IsParameterStart(tokens, token_idx)) {
		auto &name = tokens[++token_idx];
		pos = name.offset + name.length - 1;
		return true;
	}
	bool quoted_identifier =
	    token.type == TokenType::IDENTIFIER && !token.text.empty() && (token.text[0] == '"' || token.text[0] == '`');
	if (token.type != TokenType::STRING_LITERAL && token.type != TokenType::COMMENT && !quoted_identifier) {
		return false;
	}
	auto length = token.length;
	// Keep the newline ending a line comment as a formatting boundary.
	if (token.type == TokenType::COMMENT && StringUtil::StartsWith(token.text, "--")) {
		while (length > 0 && (token.text[length - 1] == '\n' || token.text[length - 1] == '\r')) {
			length--;
		}
	}
	if (token.offset + length <= pos) {
		return false;
	}
	pos = token.offset + length - 1;
	return true;
}

string SQLFormatter::Format(const string &sql) {
	auto tokens = Tokenize(sql);

	if (tokens.empty()) {
		return sql;
	}

	string multiline = FormatMultiline(sql, tokens);
	string struct_inlined = InlineStructLiterals(multiline);
	string merged = MergeShortClauses(struct_inlined);
	string collapsed = CollapseFirstCondition(merged);
	string expanded = ExpandTableDefinition(collapsed);
	string case_expanded = ExpandCaseExpressions(expanded);
	string result = ExpandLongParenLists(case_expanded);
	// Strip trailing whitespace/newlines from the final output.
	while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
		result.pop_back();
	}
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyword classification helpers
// ─────────────────────────────────────────────────────────────────────────────

//! Returns true if kw (already uppercased) is a structural clause keyword that
//! starts a new line in the formatted output.
bool SQLFormatter::IsClauseKeyword(const string &kw) {
	static const std::unordered_set<string> clause_keywords = {
	    "SELECT",    "FROM",    "WHERE", "HAVING", "LIMIT",    "OFFSET",     "JOIN",  "UNION",
	    "INTERSECT", "EXCEPT",  "WITH",  "INSERT", "UPDATE",   "DELETE",     "SET",   "RETURNING",
	    "VALUES",    "CREATE",  "DROP",  "ALTER",  "TRUNCATE", "QUALIFY",    "PIVOT", "UNPIVOT",
	    "REFRESH",   "INSTALL", "LOAD",  "ATTACH", "DETACH",   "CHECKPOINT", "FORCE", "COPY"};
	return clause_keywords.count(kw) > 0;
}

bool SQLFormatter::IsJoinModifier(const string &kw) {
	return kw == "INNER" || kw == "LEFT" || kw == "RIGHT" || kw == "FULL" || kw == "CROSS" || kw == "NATURAL";
}

//! Complete set of clause keyword *strings* as they appear in the formatted
//! output (single words AND compound forms).  Used by the post-processing pass.
bool SQLFormatter::IsClauseKeywordLine(const string &trimmed) {
	static const std::unordered_set<string> all_clause_strings = {
	    // Single-word clause starters
	    "SELECT", "FROM", "WHERE", "HAVING", "LIMIT", "OFFSET", "JOIN", "UNION", "INTERSECT", "EXCEPT", "WITH",
	    "INSERT", "UPDATE", "DELETE", "SET", "RETURNING", "VALUES", "CREATE", "DROP", "ALTER", "TRUNCATE", "QUALIFY",
	    "PIVOT", "UNPIVOT", "REFRESH", "INSTALL", "LOAD", "ATTACH", "DETACH", "CHECKPOINT", "COPY", "FORCE",
	    // Compound clause keywords
	    "GROUP BY", "ORDER BY", "PARTITION BY", "UNION ALL", "UNION DISTINCT", "INTERSECT ALL", "INTERSECT DISTINCT",
	    "EXCEPT ALL", "EXCEPT DISTINCT", "INNER JOIN", "CROSS JOIN", "NATURAL JOIN", "LEFT JOIN", "LEFT OUTER JOIN",
	    "RIGHT JOIN", "RIGHT OUTER JOIN", "FULL JOIN", "FULL OUTER JOIN", "INSERT INTO", "DELETE FROM", "ON CONFLICT",
	    "WITH RECURSIVE", "FORCE CHECKPOINT",
	    // DDL compound keywords
	    "CREATE TABLE", "CREATE VIEW", "CREATE INDEX", "CREATE UNIQUE INDEX", "CREATE SCHEMA", "CREATE SEQUENCE",
	    "CREATE MACRO", "CREATE FUNCTION", "CREATE TYPE", "CREATE TEMP TABLE", "CREATE TEMP VIEW",
	    "CREATE TEMPORARY TABLE", "CREATE TEMPORARY VIEW", "CREATE OR REPLACE TABLE", "CREATE OR REPLACE VIEW",
	    "CREATE OR REPLACE INDEX", "CREATE OR REPLACE MACRO", "CREATE OR REPLACE FUNCTION", "CREATE OR REPLACE TYPE",
	    "CREATE OR REPLACE TEMP TABLE", "CREATE OR REPLACE TEMP VIEW", "CREATE OR REPLACE TEMPORARY TABLE",
	    "CREATE OR REPLACE TEMPORARY VIEW", "ALTER TABLE"};
	return all_clause_strings.count(StringUtil::Upper(trimmed)) > 0;
}

//! Structural keywords that are always uppercased.  Unreserved keywords used
//! as identifiers (e.g. "name", "value", "type") are excluded so their
//! original casing is preserved.
bool SQLFormatter::ShouldUppercase(const string &kw) {
	static const std::unordered_set<string> uppercase_set = {
	    "SELECT",     "DISTINCT",  "FROM",        "WHERE",      "HAVING",       "LIMIT",      "OFFSET",
	    "GROUP",      "BY",        "ORDER",       "UNION",      "INTERSECT",    "EXCEPT",     "ALL",
	    "JOIN",       "INNER",     "LEFT",        "RIGHT",      "FULL",         "OUTER",      "CROSS",
	    "NATURAL",    "ON",        "USING",       "WITH",       "RECURSIVE",    "INSERT",     "INTO",
	    "UPDATE",     "DELETE",    "MERGE",       "MATCHED",    "SET",          "VALUES",     "RETURNING",
	    "OVERRIDING", "CREATE",    "DROP",        "ALTER",      "TRUNCATE",     "TABLE",      "VIEW",
	    "INDEX",      "SCHEMA",    "DATABASE",    "SEQUENCE",   "MATERIALIZED", "TEMP",       "TEMPORARY",
	    "IF",         "NOT",       "EXISTS",      "OR",         "REPLACE",      "UNIQUE",     "PRIMARY",
	    "KEY",        "FOREIGN",   "REFERENCES",  "DEFAULT",    "CONSTRAINT",   "CHECK",      "GENERATED",
	    "ALWAYS",     "IDENTITY",  "STORED",      "VIRTUAL",    "COLUMN",       "ADD",        "RENAME",
	    "COPY",       "AND",       "OR",          "NOT",        "IS",           "NULL",       "TRUE",
	    "FALSE",      "IN",        "LIKE",        "ILIKE",      "GLOB",         "BETWEEN",    "SIMILAR",
	    "SOME",       "ANY",       "EXISTS",      "OVERLAPS",   "CASE",         "WHEN",       "THEN",
	    "ELSE",       "END",       "AS",          "ASC",        "DESC",         "NULLS",      "FIRST",
	    "LAST",       "OVER",      "PARTITION",   "ROWS",       "RANGE",        "GROUPS",     "UNBOUNDED",
	    "PRECEDING",  "FOLLOWING", "CURRENT",     "ROW",        "EXCLUDE",      "TIES",       "OTHERS",
	    "CAST",       "TRY_CAST",  "QUALIFY",     "PIVOT",      "UNPIVOT",      "IN",         "BEGIN",
	    "COMMIT",     "ROLLBACK",  "TRANSACTION", "SAVEPOINT",  "RELEASE",      "EXPLAIN",    "ANALYZE",
	    "DESCRIBE",   "SHOW",      "PRAGMA",      "ATTACH",     "DETACH",       "CHECKPOINT", "VACUUM",
	    "LOAD",       "INSTALL",   "FORCE",       "POSITIONAL", "ASOF",         "LATERAL",    "TABLESAMPLE",
	    "REPEATABLE", "USING",     "SYSTEM",      "BERNOULLI",  "RESERVOIR",    "NEAREST",    "APPROX",
	    "EXACT",      "DISTANCE",  "SIMILARITY"};
	return uppercase_set.count(kw) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Token lookahead helpers
// ─────────────────────────────────────────────────────────────────────────────

string SQLFormatter::PeekKeyword(const vector<MatcherToken> &tokens, idx_t i, idx_t offset) {
	idx_t j = i + offset;
	if (j < tokens.size() && tokens[j].type == TokenType::KEYWORD) {
		return StringUtil::Upper(tokens[j].text);
	}
	return "";
}

//! Detect a compound clause keyword starting at tokens[i].
//! Returns the number of *additional* tokens consumed (caller advances by 1+return).
//! Returns static_cast<idx_t>(-1) if tokens[i] is NOT a clause keyword.
//! Fills compound_text with the uppercased keyword string (e.g. "GROUP BY").
//! Fills original_text with the original token text joined by spaces (e.g. "group by").
idx_t SQLFormatter::DetectCompoundClause(const vector<MatcherToken> &tokens, idx_t i, string &compound_text,
                                         string &original_text) {
	// clang-format off
	//! Fixed multi-word clause keyword patterns.
	static const vector<vector<const char *>> kFixedCompounds = {
	    // Two-word clauses
	    {"GROUP",     "BY"      },
	    {"ORDER",     "BY"      },
	    {"PARTITION", "BY"      },
	    {"ALTER",     "TABLE"   },
	    {"INSERT",    "INTO"    },
	    {"DELETE",    "FROM"    },
	    {"ON",        "CONFLICT"},
	    {"FORCE",     "CHECKPOINT"},
	    // Set operations with qualifier
	    {"UNION",     "ALL"     }, {"UNION",     "DISTINCT"},
	    {"INTERSECT", "ALL"     }, {"INTERSECT", "DISTINCT"},
	    {"EXCEPT",    "ALL"     }, {"EXCEPT",    "DISTINCT"},
	    // JOIN variants
	    {"INNER",   "JOIN"             },
	    {"CROSS",   "JOIN"             },
	    {"NATURAL", "JOIN"             },
	    {"LEFT",    "JOIN"             }, {"LEFT",  "OUTER", "JOIN"},
	    {"RIGHT",   "JOIN"             }, {"RIGHT", "OUTER", "JOIN"},
	    {"FULL",    "JOIN"             }, {"FULL",  "OUTER", "JOIN"},
	    // WITH RECURSIVE
	    {"WITH",    "RECURSIVE"        },
	    // Special CREATE form
	    {"CREATE",  "UNIQUE", "INDEX"  },
	};
	// CREATE [OR REPLACE] [TEMP | TEMPORARY] <object> — generated from prefixes × objects.
	static const vector<vector<const char *>> kCreatePrefixes = {
	    {"CREATE"},
	    {"CREATE", "TEMP"},
	    {"CREATE", "TEMPORARY"},
	    {"CREATE", "OR", "REPLACE"},
	    {"CREATE", "OR", "REPLACE", "TEMP"},
	    {"CREATE", "OR", "REPLACE", "TEMPORARY"},
	};
	static const vector<const char *> kCreateObjects = {
	    "TABLE", "VIEW", "INDEX", "SCHEMA", "SEQUENCE", "MACRO", "FUNCTION", "TYPE",
	};
	// clang-format on
	static const vector<vector<const char *>> kCompoundKeywords = [&]() {
		auto result = kFixedCompounds;
		for (const auto &prefix : kCreatePrefixes) {
			for (const char *obj : kCreateObjects) {
				auto entry = prefix;
				entry.push_back(obj);
				result.push_back(std::move(entry));
			}
		}
		return result;
	}();

	const string kw = StringUtil::Upper(tokens[i].text);

	// Find the longest compound keyword pattern whose first word matches kw.
	idx_t best_extra = static_cast<idx_t>(-1);
	const vector<const char *> *best_match = nullptr;

	for (const auto &entry : kCompoundKeywords) {
		if (kw != entry[0]) {
			continue;
		}
		bool matched = true;
		for (idx_t k = 1; k < entry.size(); k++) {
			if (PeekKeyword(tokens, i, k) != entry[k]) {
				matched = false;
				break;
			}
		}
		if (matched) {
			idx_t extra = static_cast<idx_t>(entry.size()) - 1;
			if (best_match == nullptr || extra > best_extra) {
				best_extra = extra;
				best_match = &entry;
			}
		}
	}

	if (best_match != nullptr) {
		compound_text = kw;
		original_text = tokens[i].text;
		for (idx_t k = 1; k <= best_extra; k++) {
			compound_text += ' ';
			compound_text += (*best_match)[k];
			original_text += ' ';
			original_text += tokens[i + k].text;
		}
		return best_extra;
	}

	// Single-word clause keyword or join modifier.
	if (IsClauseKeyword(kw) || IsJoinModifier(kw)) {
		compound_text = kw;
		original_text = tokens[i].text;
		return 0;
	}

	return static_cast<idx_t>(-1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Line helpers (used by Phases 2-5)
// ─────────────────────────────────────────────────────────────────────────────

idx_t SQLFormatter::LeadingSpaces(const string &line) {
	idx_t n = 0;
	while (n < line.size() && line[n] == ' ') {
		n++;
	}
	return n;
}

string SQLFormatter::TrimLeft(const string &line) {
	return line.substr(LeadingSpaces(line));
}

vector<string> SQLFormatter::SplitLines(const string &formatted) {
	auto tokens = Tokenize(formatted);
	idx_t token_idx = 0;
	vector<string> lines;
	idx_t start = 0;
	for (idx_t k = 0; k <= formatted.size(); k++) {
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			continue;
		}
		if (k == formatted.size() || formatted[k] == '\n') {
			lines.push_back(formatted.substr(start, k - start));
			start = k + 1;
		}
	}
	return lines;
}

string SQLFormatter::JoinLines(const vector<string> &lines) {
	string result;
	for (idx_t k = 0; k < lines.size(); k++) {
		if (k > 0) {
			result += '\n';
		}
		result += lines[k];
	}
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1: token-stream -> multiline string
// ─────────────────────────────────────────────────────────────────────────────

//! Apply keyword_case to a keyword.  upper_kw is the uppercased form,
//! orig_kw is the original token text (or joined original tokens for compounds).
//! is_structural=true means the keyword is a clause/set-operator keyword that is
//! always uppercased in UPPER mode (bypasses the ShouldUppercase whitelist check).
string SQLFormatter::ApplyCase(const string &upper_kw, const string &orig_kw, bool is_structural) const {
	switch (config.keyword_case) {
	case KeywordCase::UPPER:
		return (is_structural || ShouldUppercase(upper_kw)) ? upper_kw : orig_kw;
	case KeywordCase::LOWER:
		return StringUtil::Lower(upper_kw);
	case KeywordCase::PRESERVE:
		return orig_kw;
	default:
		return upper_kw;
	}
}

string SQLFormatter::FormatMultiline(const string &sql, const vector<MatcherToken> &tokens) const {
	string result;
	result.reserve(tokens.size() * 8);

	bool at_line_start = true;
	bool after_clause = false;
	bool prev_was_keyword = false;
	bool in_between = false; // true after BETWEEN keyword, cleared after next AND
	string last_clause;      // uppercased text of the most recent clause keyword
	// Uppercase text of the most recent keyword token — used to decide whether
	// to insert a space before '('.  Only operator/conditional keywords like IN,
	// EXISTS, NOT, SOME, ANY, ALL need a space; function-like or type keywords
	// (COALESCE, DECIMAL, VARCHAR, CAST, ...) should not get one.
	string prev_keyword;

	// Per-paren level state: tracks indentation and whether clauses were emitted inside.
	struct ParenInfo {
		bool has_clauses;
		int32_t close_indent;        //! indent for the closing ')'
		int32_t inner_clause_indent; //! indent for clause keywords inside this paren
	};
	vector<ParenInfo> paren_stack;

	const auto indent_size = static_cast<int32_t>(config.indent_size);

	auto write_indent = [&](int32_t n) {
		for (int32_t j = 0; j < n; j++) {
			result += ' ';
		}
		at_line_start = false;
	};
	auto write_newline = [&]() {
		result += '\n';
		at_line_start = true;
	};
	auto write_space = [&]() {
		if (!at_line_start && !result.empty() && result.back() != ' ' && result.back() != '(' && result.back() != '.' &&
		    result.back() != '[' && result.back() != ':') {
			result += ' ';
		}
	};
	auto clause_indent = [&]() -> int32_t {
		return paren_stack.empty() ? 0 : paren_stack.back().inner_clause_indent;
	};
	auto content_indent = [&]() -> int32_t {
		return clause_indent() + indent_size;
	};

	auto emit_clause = [&](const string &kw_text, const string &upper_compound, const string &upper_last_word) {
		if (!result.empty() && !at_line_start) {
			write_newline();
			write_indent(clause_indent());
		} else if (at_line_start && clause_indent() > 0) {
			write_indent(clause_indent());
		}
		result += kw_text;
		at_line_start = false;
		after_clause = true;
		prev_was_keyword = true;
		prev_keyword = upper_last_word;
		last_clause = upper_compound;
		if (!paren_stack.empty()) {
			paren_stack.back().has_clauses = true;
		}
	};

	for (idx_t i = 0; i < tokens.size();) {
		const auto &tok = tokens[i];

		if (tok.type == TokenType::TERMINATOR) {
			after_clause = false;
			prev_was_keyword = false;
			in_between = false;
			last_clause.clear();
			result += ';';
			at_line_start = false;
			// A semicolon ends a statement; the next statement starts on a new line.
			// Preserve blank lines from the original SQL between statements.
			if (i + 1 < tokens.size()) {
				write_newline();
				idx_t end_of_semi = tok.offset + tok.length;
				idx_t next_start = tokens[i + 1].offset;
				idx_t newline_count = 0;
				for (idx_t k = end_of_semi; k < next_start && k < sql.size(); k++) {
					if (sql[k] == '\n') {
						newline_count++;
					}
				}
				// Two or more newlines in the gap means there was a blank line.
				if (newline_count >= 2) {
					write_newline();
				}
			}
			i++;
			continue;
		}

		if (tok.type == TokenType::COMMENT) {
			// Strip trailing newline from comment text — we manage newlines ourselves.
			string comment_text = tok.text;
			while (!comment_text.empty() && (comment_text.back() == '\n' || comment_text.back() == '\r')) {
				comment_text.pop_back();
			}
			bool is_block = comment_text.size() >= 2 && comment_text[0] == '/' && comment_text[1] == '*';
			bool is_line = comment_text.size() >= 2 && comment_text[0] == '-' && comment_text[1] == '-';
			// Block comments before the first statement go at column 0.
			if (is_block && result.empty()) {
				// no space prefix
			} else if (is_block && at_line_start) {
				write_indent(clause_indent());
			} else {
				write_space();
			}
			result += comment_text;
			if (is_line || is_block) {
				write_newline();
			}
			after_clause = false;
			prev_was_keyword = false;
			i++;
			continue;
		}

		if (tok.type == TokenType::OPERATOR && tok.text == "(") {
			// Insert a space before '(' for most keywords (e.g. AS, IN, EXISTS,
			// OVER, FILTER, USING ...) but NOT for function-like keywords or type
			// names where no space is conventional (COALESCE, CAST, DECIMAL, ...).
			static const std::unordered_set<string> no_space_before_paren = {
			    "COALESCE",   "NULLIF",    "CAST",          "TRY_CAST",    "EXTRACT", "OVERLAY",       "POSITION",
			    "SUBSTRING",  "TRIM",      "GROUPING",      "GROUPING_ID", "TREAT",   "XMLATTRIBUTES", "XMLCONCAT",
			    "XMLELEMENT", "XMLFOREST", "XMLNAMESPACES", "XMLPARSE",    "XMLPI",   "XMLROOT",       "XMLSERIALIZE",
			    "XMLTABLE",   "XMLEXISTS", "BIGINT",        "BIT",         "BOOLEAN", "CHAR",          "CHARACTER",
			    "DEC",        "DECIMAL",   "FLOAT",         "INT",         "INTEGER", "INTERVAL",      "MAP",
			    "NATIONAL",   "NCHAR",     "NUMERIC",       "PRECISION",   "REAL",    "SMALLINT",      "STRUCT",
			    "TIME",       "TIMESTAMP", "VARCHAR",
			};
			if (prev_was_keyword && !no_space_before_paren.count(prev_keyword) && !at_line_start) {
				write_space();
			}
			after_clause = false;
			prev_was_keyword = false;
			result += '(';
			at_line_start = false;
			int32_t ci = content_indent();
			paren_stack.push_back({false, ci, ci + indent_size});
			i++;
			continue;
		}

		if (tok.type == TokenType::OPERATOR && tok.text == ")") {
			after_clause = false;
			prev_was_keyword = false;
			bool had_clauses = !paren_stack.empty() && paren_stack.back().has_clauses;
			int32_t close_ind = paren_stack.empty() ? 0 : paren_stack.back().close_indent;
			if (!paren_stack.empty()) {
				paren_stack.pop_back();
			}
			if (had_clauses && !at_line_start) {
				write_newline();
				write_indent(close_ind);
			}
			result += ')';
			at_line_start = false;
			i++;
			continue;
		}

		// { (struct literal) — always multiline; phase 2 inlines short ones.
		if (tok.type == TokenType::OPERATOR && tok.text == "{") {
			prev_was_keyword = false;
			if (after_clause) {
				write_newline();
				write_indent(content_indent());
				after_clause = false;
			} else {
				write_space();
			}
			result += '{';
			at_line_start = false;
			int32_t ci = content_indent();
			paren_stack.push_back({true, ci, ci + indent_size});
			after_clause = true;
			i++;
			continue;
		}

		if (tok.type == TokenType::OPERATOR && tok.text == "}") {
			after_clause = false;
			prev_was_keyword = false;
			int32_t close_ind = paren_stack.empty() ? 0 : paren_stack.back().close_indent;
			if (!paren_stack.empty()) {
				paren_stack.pop_back();
			}
			if (!at_line_start) {
				write_newline();
				write_indent(close_ind);
			}
			result += '}';
			at_line_start = false;
			i++;
			continue;
		}

		if (tok.type == TokenType::OPERATOR && tok.text == "[") {
			after_clause = false;
			prev_was_keyword = false;
			if (!at_line_start && !result.empty() && result.back() != ' ' && result.back() != '(' &&
			    result.back() != '[' && result.back() != '.') {
				result += ' ';
			}
			result += '[';
			at_line_start = false;
			// Brackets are array literals — push a paren level to keep commas inline.
			int32_t ci = content_indent();
			paren_stack.push_back({false, ci, ci + indent_size});
			i++;
			continue;
		}

		if (tok.type == TokenType::OPERATOR && tok.text == "]") {
			after_clause = false;
			prev_was_keyword = false;
			if (!paren_stack.empty()) {
				paren_stack.pop_back();
			}
			// Remove trailing space before ']' if present.
			if (!result.empty() && result.back() == ' ') {
				result.pop_back();
			}
			result += ']';
			at_line_start = false;
			i++;
			continue;
		}

		if (tok.type == TokenType::OPERATOR && tok.text == ",") {
			after_clause = false;
			prev_was_keyword = false;
			result += ',';
			at_line_start = false;
			if (paren_stack.empty() || paren_stack.back().has_clauses) {
				write_newline();
				write_indent(content_indent());
			} else {
				result += ' ';
			}
			i++;
			continue;
		}

		if (tok.text == "." && (tok.type == TokenType::OPERATOR || tok.type == TokenType::NUMBER_LITERAL)) {
			after_clause = false;
			prev_was_keyword = false;
			if (!result.empty() && result.back() == ' ') {
				result.pop_back();
			}
			result += '.';
			at_line_start = false;
			i++;
			continue;
		}

		// : (struct literal key-value separator) — no space before, space after.
		if (tok.type == TokenType::OPERATOR && tok.text == ":") {
			after_clause = false;
			prev_was_keyword = false;
			if (!result.empty() && result.back() == ' ') {
				result.pop_back();
			}
			result += ": ";
			at_line_start = false;
			i++;
			continue;
		}

		// :: (typecast) binds tightly — no spaces around it.
		if (tok.type == TokenType::OPERATOR && tok.text == "::") {
			after_clause = false;
			prev_was_keyword = false;
			if (!result.empty() && result.back() == ' ') {
				result.pop_back();
			}
			result += "::";
			at_line_start = false;
			i++;
			continue;
		}

		// A parameter is one lexical unit for formatting. In particular, a keyword
		// used as its name must not become a clause or have its case changed.
		if (IsParameterStart(tokens, i)) {
			if (after_clause) {
				write_newline();
				write_indent(content_indent());
				after_clause = false;
			} else {
				write_space();
			}
			result += "$" + tokens[i + 1].text;
			at_line_start = false;
			prev_was_keyword = false;
			prev_keyword.clear();
			i += 2;
			continue;
		}

		if (tok.type == TokenType::KEYWORD) {
			string compound_text;
			string original_text;
			idx_t extra = DetectCompoundClause(tokens, i, compound_text, original_text);

			if (extra != static_cast<idx_t>(-1)) {
				// COPY tablename FROM — FROM should not start a new clause line.
				if (compound_text == "FROM" && last_clause == "COPY") {
					// Fall through to the regular keyword path below.
				} else {
					auto sp = compound_text.rfind(' ');
					const string last_word = (sp == string::npos) ? compound_text : compound_text.substr(sp + 1);
					emit_clause(ApplyCase(compound_text, original_text, /*is_structural=*/true), compound_text,
					            last_word);
					// WITH / WITH RECURSIVE: keep CTE name on the same line
					if (compound_text == "WITH" || compound_text == "WITH RECURSIVE") {
						after_clause = false;
					}
					i += 1 + extra;
					continue;
				}
			}

			const string upper = StringUtil::Upper(tok.text);

			if (upper == "BETWEEN") {
				in_between = true;
			}

			if (upper == "AND" || upper == "OR") {
				// AND after BETWEEN is part of the BETWEEN expression, not a conjunction.
				if (upper == "AND" && in_between) {
					in_between = false;
					write_space();
					result += ApplyCase(upper, tok.text, /*is_structural=*/true);
					at_line_start = false;
					prev_was_keyword = true;
					prev_keyword = upper;
					i++;
					continue;
				}
				// AND/OR inside parentheses that have no clause keywords are part of
				// an expression (e.g. (a = 1 AND b = 2)), not structural conjunctions.
				bool inside_expression_parens = !paren_stack.empty() && !paren_stack.back().has_clauses;
				if (inside_expression_parens) {
					write_space();
					result += ApplyCase(upper, tok.text, /*is_structural=*/true);
					at_line_start = false;
					prev_was_keyword = true;
					prev_keyword = upper;
					i++;
					continue;
				}
				if (!at_line_start) {
					write_newline();
				}
				write_indent(content_indent());
				result += ApplyCase(upper, tok.text, /*is_structural=*/true);
				at_line_start = false;
				after_clause = false;
				prev_was_keyword = true;
				prev_keyword = upper;
				i++;
				continue;
			}

			if (after_clause) {
				write_newline();
				write_indent(content_indent());
				after_clause = false;
			} else if (!result.empty() && result.back() != '.') {
				write_space();
			}
			result += ApplyCase(upper, tok.text);
			at_line_start = false;
			prev_was_keyword = true;
			prev_keyword = upper;
			i++;
			continue;
		}

		if (after_clause) {
			write_newline();
			write_indent(content_indent());
			after_clause = false;
		} else if (!result.empty() && result.back() != '.') {
			write_space();
		}
		result += tok.text;
		at_line_start = false;
		prev_was_keyword = false;
		prev_keyword.clear();
		i++;
	}

	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2: inline short struct literals
// ─────────────────────────────────────────────────────────────────────────────

//! Walk through the formatted output and inline short struct literals.
//! A struct literal that was expanded to multiple lines ('{' on one line,
//! entries on subsequent lines, '}' on its own line) is collapsed back to
//! a single line when the result fits within inline_threshold.
string SQLFormatter::InlineStructLiterals(const string &formatted) const {
	if (config.inline_threshold == 0) {
		return formatted;
	}

	const vector<string> lines = SplitLines(formatted);
	vector<string> out;
	out.reserve(lines.size());

	for (idx_t i = 0; i < lines.size();) {
		const string &line = lines[i];
		const string trimmed = TrimLeft(line);

		// Look for lines whose trimmed content ends with '{'.
		if (!trimmed.empty() && trimmed.back() == '{') {
			// Find the closing '}' line.
			idx_t close_line = i + 1;
			bool found_close = false;
			while (close_line < lines.size()) {
				const string ctrimmed = TrimLeft(lines[close_line]);
				if (ctrimmed == "}") {
					found_close = true;
					break;
				}
				close_line++;
			}
			if (found_close && close_line > i + 1) {
				// Build the flat content inside the braces.
				string flat;
				for (idx_t k = i + 1; k < close_line; k++) {
					if (!flat.empty()) {
						flat += ' ';
					}
					flat += TrimLeft(lines[k]);
				}
				// Merge: line with '{' + space + flat content + space + '}'
				string merged = line + " " + flat + " }";
				if (Utf8Proc::RenderWidth(merged) <= config.inline_threshold) {
					out.push_back(std::move(merged));
					i = close_line + 1;
					continue;
				}
			}
		}

		out.push_back(line);
		i++;
	}

	return JoinLines(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: inline short clauses
// ─────────────────────────────────────────────────────────────────────────────

//! Post-processing pass: walk through the already-formatted multiline output
//! and merge clause-keyword lines with their content when the combined line
//! fits within config.inline_threshold characters.
//!
//! A clause is only inlined when its content is "simple" — that is, none of
//! the content lines is itself a clause keyword (which would indicate a nested
//! subquery structure that shouldn't be flattened).
string SQLFormatter::MergeShortClauses(const string &formatted) const {
	if (config.inline_threshold == 0) {
		return formatted;
	}

	const vector<string> lines = SplitLines(formatted);
	vector<string> out;
	out.reserve(lines.size());

	for (idx_t i = 0; i < lines.size();) {
		const string &line = lines[i];
		const idx_t clause_ind = LeadingSpaces(line);
		const string trimmed = TrimLeft(line);

		if (!trimmed.empty() && IsClauseKeywordLine(trimmed)) {
			const idx_t content_ind = clause_ind + config.indent_size;
			bool is_simple = true;
			idx_t j = i + 1;
			idx_t content_end = i + 1;

			while (j < lines.size()) {
				const string &cline = lines[j];
				const string ctrimmed = TrimLeft(cline);
				if (ctrimmed.empty()) {
					break;
				}
				const idx_t cind = LeadingSpaces(cline);
				if (cind <= clause_ind) {
					break;
				}
				if (cind > content_ind) {
					is_simple = false;
					break;
				}
				if (IsClauseKeywordLine(ctrimmed)) {
					is_simple = false;
					break;
				}
				content_end = j + 1;
				if (cline.find('\n') != string::npos) {
					break;
				}
				j++;
			}

			if (is_simple && content_end > i + 1) {
				// A line with a -- comment can only be the last content line
				// when inlining, because inlining would make the comment
				// swallow any text that follows it on the merged line.
				bool has_inner_comment = false;
				for (idx_t k = i + 1; k + 1 < content_end; k++) {
					if (lines[k].find("--") != string::npos) {
						has_inner_comment = true;
						break;
					}
				}
				if (!has_inner_comment) {
					string flat;
					for (idx_t k = i + 1; k < content_end; k++) {
						if (!flat.empty()) {
							flat += ' ';
						}
						flat += TrimLeft(lines[k]);
					}
					string merged = string(clause_ind, ' ') + trimmed + ' ' + flat;
					// Always merge when there's only one content line — splitting
					// doesn't save space, it just adds indentation overhead.
					bool single_content = (content_end == i + 2);
					if (single_content || Utf8Proc::RenderWidth(merged) <= config.inline_threshold) {
						out.push_back(std::move(merged));
						i = content_end;
						continue;
					}
				}
			}

			// For CREATE/COPY clauses, always merge at least the first content
			// line so the name stays on the same line as the keyword.
			if (i + 1 < lines.size()) {
				const string upper_trimmed = StringUtil::Upper(trimmed);
				if (upper_trimmed.rfind("CREATE", 0) == 0 || upper_trimmed == "COPY") {
					const string &next = lines[i + 1];
					const idx_t next_ind = LeadingSpaces(next);
					if (next_ind > clause_ind) {
						string merged_first = string(clause_ind, ' ') + trimmed + ' ' + TrimLeft(next);
						out.push_back(std::move(merged_first));
						i += 2;
						continue;
					}
				}
			}
		}

		out.push_back(line);
		i++;
	}

	return JoinLines(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: lift first WHERE/HAVING predicate onto the keyword's line
// ─────────────────────────────────────────────────────────────────────────────

//! True when trimmed is exactly a WHERE or HAVING keyword (any case).
bool SQLFormatter::IsConditionClauseLine(const string &trimmed) {
	const string upper = StringUtil::Upper(trimmed);
	return upper == "WHERE" || upper == "HAVING";
}

//! True when trimmed content starts with an AND/OR conjunction keyword (any case).
bool SQLFormatter::StartsWithConjunction(const string &trimmed) {
	const string upper = StringUtil::Upper(trimmed);
	return (upper.size() >= 4 && upper.substr(0, 4) == "AND ") || (upper.size() >= 3 && upper.substr(0, 3) == "OR ");
}

//! Phase 3: for WHERE/HAVING clauses left multi-line by Phase 2, lift the first
//! predicate onto the keyword line and align subsequent AND/OR conjunctions at
//! clause_indent+2 (so their content starts at the same column as the WHERE content).
//!
//! Only applies when config.inline_threshold > 0 and the lifted first line fits
//! within the threshold.  Clauses containing deeper-nested content (subqueries)
//! are left untouched.
string SQLFormatter::CollapseFirstCondition(const string &formatted) const {
	if (config.inline_threshold == 0) {
		return formatted;
	}

	const vector<string> lines = SplitLines(formatted);
	vector<string> out;
	out.reserve(lines.size());

	for (idx_t i = 0; i < lines.size();) {
		const string &line = lines[i];
		const idx_t clause_ind = LeadingSpaces(line);
		const string trimmed = TrimLeft(line);

		if (!IsConditionClauseLine(trimmed)) {
			out.push_back(line);
			i++;
			continue;
		}

		const idx_t content_ind = clause_ind + config.indent_size;

		bool is_simple = true;
		idx_t j = i + 1;
		while (j < lines.size()) {
			const string ctrimmed = TrimLeft(lines[j]);
			if (ctrimmed.empty()) {
				break;
			}
			const idx_t cind = LeadingSpaces(lines[j]);
			if (cind <= clause_ind) {
				break;
			}
			if (cind > content_ind) {
				is_simple = false;
				break;
			}
			j++;
		}

		const idx_t content_count = j - (i + 1);

		if (!is_simple || content_count == 0) {
			out.push_back(line);
			i++;
			continue;
		}

		const string first_content = TrimLeft(lines[i + 1]);
		const string merged_first = string(clause_ind, ' ') + trimmed + ' ' + first_content;
		if (Utf8Proc::RenderWidth(merged_first) > config.inline_threshold) {
			out.push_back(line);
			i++;
			continue;
		}

		out.push_back(merged_first);

		for (idx_t k = i + 2; k < j; k++) {
			const string ctrimmed = TrimLeft(lines[k]);
			if (StartsWithConjunction(ctrimmed)) {
				out.push_back(string(clause_ind + 2, ' ') + ctrimmed);
			} else {
				out.push_back(lines[k]);
			}
		}

		i = j;
	}

	return JoinLines(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 4: expand CREATE TABLE column list to one column per line
// ─────────────────────────────────────────────────────────────────────────────

//! Split s at top-level commas (depth-0, outside parens and string literals).
vector<string> SQLFormatter::SplitTopLevelCommas(const string &s) {
	vector<string> parts;
	int32_t depth = 0;
	auto tokens = Tokenize(s);
	idx_t token_idx = 0;
	idx_t start = 0;
	for (idx_t k = 0; k < s.size(); k++) {
		const char c = s[k];
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == ',' && depth == 0) {
			string part = s.substr(start, k - start);
			idx_t ts = 0;
			while (ts < part.size() && part[ts] == ' ') {
				ts++;
			}
			idx_t te = part.size();
			while (te > ts && part[te - 1] == ' ') {
				te--;
			}
			if (ts < te) {
				parts.push_back(part.substr(ts, te - ts));
			}
			start = k + 1;
		}
	}
	{
		string part = s.substr(start);
		idx_t ts = 0;
		while (ts < part.size() && part[ts] == ' ') {
			ts++;
		}
		idx_t te = part.size();
		while (te > ts && part[te - 1] == ' ') {
			te--;
		}
		if (ts < te) {
			parts.push_back(part.substr(ts, te - ts));
		}
	}
	return parts;
}

//! Find the closing ')' that matches the '(' at open_pos in s.
//! Returns string::npos if not found.
idx_t SQLFormatter::FindMatchingClose(const string &s, idx_t open_pos) {
	int32_t depth = 0;
	auto tokens = Tokenize(s);
	idx_t token_idx = 0;
	for (idx_t k = open_pos; k < s.size(); k++) {
		const char c = s[k];
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				return k;
			}
		}
	}
	return string::npos;
}

//! Returns the length of a CREATE [OR REPLACE] [TEMP|TEMPORARY] TABLE prefix
//! in upper_trimmed, or 0 if none matches.
idx_t SQLFormatter::MatchCreateTablePrefix(const string &upper_trimmed) {
	static const char *const prefixes[] = {
	    "CREATE OR REPLACE TEMPORARY TABLE",
	    "CREATE OR REPLACE TEMP TABLE",
	    "CREATE OR REPLACE TABLE",
	    "CREATE TEMPORARY TABLE",
	    "CREATE TEMP TABLE",
	    "CREATE TABLE",
	    nullptr,
	};
	for (idx_t p = 0; prefixes[p]; p++) {
		const idx_t len = strlen(prefixes[p]);
		if (upper_trimmed.size() >= len) {
			const char next = (upper_trimmed.size() == len) ? '\0' : upper_trimmed[len];
			if ((next == '\0' || next == ' ' || next == '(') && upper_trimmed.compare(0, len, prefixes[p]) == 0) {
				return len;
			}
		}
	}
	return 0;
}

//! Phase 4: expand CREATE TABLE column list so each column gets its own line.
//! Handles both the inlined form (CREATE TABLE t(a, b)) and the two-line form
//! (CREATE TABLE / <indent>tablename(a, b)) produced by earlier phases.
string SQLFormatter::ExpandTableDefinition(const string &formatted) const {
	const vector<string> lines = SplitLines(formatted);
	vector<string> out;
	out.reserve(lines.size() + 16);

	for (idx_t i = 0; i < lines.size();) {
		const string &line = lines[i];
		const idx_t indent = LeadingSpaces(line);
		const string trimmed = TrimLeft(line);
		const string upper_trimmed = StringUtil::Upper(trimmed);
		const idx_t prefix_len = MatchCreateTablePrefix(upper_trimmed);

		if (prefix_len == 0) {
			out.push_back(line);
			i++;
			continue;
		}

		// CREATE TABLE ... AS SELECT — no column definition list to expand.
		// Check if " AS" appears as a word boundary after the prefix.
		{
			const string after_prefix = upper_trimmed.substr(prefix_len);
			auto as_pos = after_prefix.find(" AS");
			if (as_pos != string::npos) {
				idx_t end_pos = as_pos + 3;
				if (end_pos == after_prefix.size() || after_prefix[end_pos] == ' ') {
					out.push_back(line);
					i++;
					continue;
				}
			}
		}

		idx_t paren_pos = string::npos;
		auto tokens = Tokenize(line);
		idx_t token_idx = 0;
		for (idx_t k = indent + prefix_len; k < line.size(); k++) {
			if (SkipNonCodeToken(tokens, token_idx, k)) {
				continue;
			}
			if (line[k] == '(') {
				paren_pos = k;
				break;
			}
		}

		string prefix_part;
		string col_list;
		string suffix;
		bool consumed_next = false;

		if (paren_pos != string::npos) {
			// Case B: CREATE TABLE ... tablename(cols) all on one line.
			const idx_t close_pos = FindMatchingClose(line, paren_pos);
			if (close_pos == string::npos) {
				out.push_back(line);
				i++;
				continue;
			}
			prefix_part = line.substr(0, paren_pos);
			col_list = line.substr(paren_pos + 1, close_pos - paren_pos - 1);
			suffix = line.substr(close_pos + 1);
		} else if (i + 1 < lines.size()) {
			// Case A: keyword on line i, "tablename(cols)" on line i+1.
			const string &next_line = lines[i + 1];
			idx_t next_paren = string::npos;
			auto next_tokens = Tokenize(next_line);
			idx_t next_token_idx = 0;
			for (idx_t k = 0; k < next_line.size(); k++) {
				if (SkipNonCodeToken(next_tokens, next_token_idx, k)) {
					continue;
				}
				if (next_line[k] == '(') {
					next_paren = k;
					break;
				}
			}
			if (next_paren == string::npos) {
				out.push_back(line);
				i++;
				continue;
			}
			const idx_t close_pos = FindMatchingClose(next_line, next_paren);
			if (close_pos == string::npos) {
				out.push_back(line);
				i++;
				continue;
			}
			const string table_spec = TrimLeft(next_line.substr(0, next_paren));
			prefix_part = string(indent, ' ') + trimmed + ' ' + table_spec;
			col_list = next_line.substr(next_paren + 1, close_pos - next_paren - 1);
			suffix = next_line.substr(close_pos + 1);
			consumed_next = true;
		} else {
			out.push_back(line);
			i++;
			continue;
		}

		const vector<string> cols = SplitTopLevelCommas(col_list);
		if (cols.size() <= 1) {
			out.push_back(prefix_part + "(" + col_list + ")" + suffix);
		} else {
			const string col_indent = string(indent + config.indent_size, ' ');
			out.push_back(prefix_part + "(");
			for (idx_t c = 0; c < cols.size(); c++) {
				string col_line = col_indent + cols[c];
				if (c + 1 < cols.size()) {
					col_line += ',';
				}
				out.push_back(std::move(col_line));
			}
			out.push_back(string(indent, ' ') + ")" + suffix);
		}

		i += consumed_next ? 2 : 1;
	}

	return JoinLines(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 5: expand long CASE expressions to one WHEN/ELSE per line
// ─────────────────────────────────────────────────────────────────────────────

//! Returns true if s[pos..pos+klen) matches keyword (case-insensitive) and
//! both preceding and following characters are non-identifier characters.
//! keyword must be an uppercase ASCII string.
bool SQLFormatter::MatchKeywordAt(const string &s, idx_t pos, const char *keyword) {
	const idx_t klen = strlen(keyword);
	if (pos + klen > s.size()) {
		return false;
	}
	for (idx_t k = 0; k < klen; k++) {
		if (toupper((unsigned char)s[pos + k]) != (unsigned char)keyword[k]) {
			return false;
		}
	}
	if (pos + klen < s.size()) {
		const char c = s[pos + klen];
		if (isalnum((unsigned char)c) || c == '_') {
			return false;
		}
	}
	if (pos > 0) {
		const char c = s[pos - 1];
		if (isalnum((unsigned char)c) || c == '_') {
			return false;
		}
	}
	return true;
}

//! Find the first occurrence of keyword in s starting at from_pos,
//! skipping string literals.  Does NOT skip paren-enclosed content
//! so that CASE inside function calls (e.g. sum(CASE...)) is found.
idx_t SQLFormatter::FindKeywordAny(const string &s, idx_t from_pos, const char *keyword) {
	auto tokens = Tokenize(s);
	idx_t token_idx = 0;
	for (idx_t k = from_pos; k < s.size(); k++) {
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			continue;
		}
		if (MatchKeywordAt(s, k, keyword)) {
			return k;
		}
	}
	return string::npos;
}

//! Find the END keyword that closes the CASE at case_pos, tracking nested
//! CASE...END pairs.  Returns string::npos if no matching END is found.
idx_t SQLFormatter::FindCaseEnd(const string &s, idx_t case_pos) {
	int32_t depth = 0;
	auto tokens = Tokenize(s);
	idx_t token_idx = 0;
	idx_t k = case_pos;
	while (k < s.size()) {
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			k++;
			continue;
		}
		if (MatchKeywordAt(s, k, "CASE")) {
			depth++;
			k += 4;
			continue;
		}
		if (MatchKeywordAt(s, k, "END")) {
			depth--;
			if (depth == 0) {
				return k;
			}
			k += 3;
			continue;
		}
		k++;
	}
	return string::npos;
}

//! Split the text between CASE and END (exclusive, after removing leading/
//! trailing whitespace) into branches at top-level WHEN/ELSE keywords.
//! "Top-level" means paren_depth == 0 and case_depth == 0.
vector<string> SQLFormatter::SplitCaseBranches(const string &content) {
	vector<string> branches;
	int32_t paren_depth = 0;
	int32_t case_depth = 0;
	auto tokens = Tokenize(content);
	idx_t token_idx = 0;

	idx_t start = 0;
	while (start < content.size() && content[start] == ' ') {
		start++;
	}
	idx_t k = start;

	while (k < content.size()) {
		const char c = content[k];
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			k++;
			continue;
		}
		if (c == '(') {
			paren_depth++;
			k++;
			continue;
		}
		if (c == ')') {
			paren_depth--;
			k++;
			continue;
		}
		if (paren_depth == 0) {
			if (MatchKeywordAt(content, k, "CASE")) {
				case_depth++;
				k += 4;
				continue;
			}
			if (MatchKeywordAt(content, k, "END")) {
				case_depth--;
				k += 3;
				continue;
			}
			if (case_depth == 0 && (MatchKeywordAt(content, k, "WHEN") || MatchKeywordAt(content, k, "ELSE"))) {
				if (k > start) {
					idx_t e = k;
					while (e > start && content[e - 1] == ' ') {
						e--;
					}
					if (e > start) {
						branches.push_back(content.substr(start, e - start));
					}
				}
				start = k;
			}
		}
		k++;
	}
	{
		idx_t e = content.size();
		while (e > start && content[e - 1] == ' ') {
			e--;
		}
		if (e > start) {
			branches.push_back(content.substr(start, e - start));
		}
	}
	return branches;
}

//! Phase 5: for each output line that exceeds inline_threshold and contains a
//! CASE...END expression, expand the CASE block so each WHEN/ELSE branch is on
//! its own indented line.  Iterates until no further expansion is needed.
string SQLFormatter::ExpandCaseExpressions(const string &formatted) const {
	if (config.inline_threshold == 0) {
		return formatted;
	}

	vector<string> lines = SplitLines(formatted);

	bool changed = true;
	while (changed) {
		changed = false;
		vector<string> out;
		out.reserve(lines.size());

		for (idx_t i = 0; i < lines.size(); i++) {
			const string &line = lines[i];

			if (Utf8Proc::RenderWidth(line) <= config.inline_threshold) {
				out.push_back(line);
				continue;
			}

			const idx_t line_indent = LeadingSpaces(line);

			const idx_t case_pos = FindKeywordAny(line, 0, "CASE");
			if (case_pos == string::npos) {
				out.push_back(line);
				continue;
			}

			const idx_t end_pos = FindCaseEnd(line, case_pos);
			if (end_pos == string::npos) {
				out.push_back(line);
				continue;
			}

			const string between = line.substr(case_pos + 4, end_pos - case_pos - 4);
			const vector<string> branches = SplitCaseBranches(between);

			if (branches.size() < 2) {
				out.push_back(line);
				continue;
			}

			changed = true;

			// Check if CASE immediately follows '(' — if so, break after '('
			// and indent CASE inside the function call.
			string prefix = line.substr(0, case_pos);
			// Strip trailing spaces from prefix to find preceding char.
			idx_t prefix_end = prefix.size();
			while (prefix_end > 0 && prefix[prefix_end - 1] == ' ') {
				prefix_end--;
			}
			bool case_after_paren = (prefix_end > 0 && prefix[prefix_end - 1] == '(');

			if (case_after_paren) {
				// Break after '(' and put CASE on its own indented line.
				const string open_line = prefix.substr(0, prefix_end);
				const idx_t case_indent = line_indent + config.indent_size;
				const string case_ind(case_indent, ' ');
				const string branch_ind(case_indent + config.indent_size, ' ');
				const string end_ind(case_indent, ' ');

				out.push_back(open_line);
				out.push_back(case_ind + "CASE");

				for (const string &branch : branches) {
					out.push_back(branch_ind + branch);
				}

				const string end_kw = line.substr(end_pos, 3);
				const string suffix = line.substr(end_pos + 3);
				out.push_back(end_ind + end_kw + suffix);
			} else {
				// CASE is not after '(' — expand in place using CASE position.
				const string branch_indent(case_pos + config.indent_size, ' ');
				const string end_indent(case_pos, ' ');

				out.push_back(line.substr(0, case_pos + 4));

				for (const string &branch : branches) {
					out.push_back(branch_indent + branch);
				}

				const string end_kw = line.substr(end_pos, 3);
				const string suffix = line.substr(end_pos + 3);
				out.push_back(end_indent + end_kw + suffix);
			}
		}

		lines = std::move(out);
	}

	return JoinLines(lines);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 6: expand long parenthesized comma-lists to one value per line
// ─────────────────────────────────────────────────────────────────────────────

//! Find the '(' whose comma-separated content spans the most characters on
//! this line.  Only considers top-level parens (not nested).  Returns the
//! position of '(' or string::npos.
idx_t SQLFormatter::FindLongestParenList(const string &line, idx_t from_pos) {
	idx_t best_pos = string::npos;
	idx_t best_len = 0;
	auto tokens = Tokenize(line);
	idx_t token_idx = 0;
	int32_t depth = 0;
	for (idx_t k = from_pos; k < line.size(); k++) {
		char c = line[k];
		if (SkipNonCodeToken(tokens, token_idx, k)) {
			continue;
		}
		if (c == '(' && depth == 0) {
			idx_t close = FindMatchingClose(line, k);
			if (close != string::npos) {
				idx_t span = close - k;
				if (span > best_len) {
					// Only consider if it actually has commas (multiple items).
					string content = line.substr(k + 1, close - k - 1);
					auto parts = SplitTopLevelCommas(content);
					if (parts.size() > 1) {
						best_len = span;
						best_pos = k;
					}
				}
				// Skip past this paren group at depth 0.
				k = close;
			}
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		}
	}
	return best_pos;
}

string SQLFormatter::ExpandLongParenLists(const string &formatted) const {
	if (config.inline_threshold == 0) {
		return formatted;
	}

	vector<string> lines = SplitLines(formatted);
	bool changed = true;
	while (changed) {
		changed = false;
		vector<string> out;
		out.reserve(lines.size());

		for (const auto &line : lines) {
			if (Utf8Proc::RenderWidth(line) <= config.inline_threshold) {
				out.push_back(line);
				continue;
			}

			idx_t open_paren = FindLongestParenList(line, 0);
			if (open_paren == string::npos) {
				out.push_back(line);
				continue;
			}

			idx_t close_paren = FindMatchingClose(line, open_paren);
			if (close_paren == string::npos) {
				out.push_back(line);
				continue;
			}

			string content = line.substr(open_paren + 1, close_paren - open_paren - 1);
			vector<string> values = SplitTopLevelCommas(content);
			if (values.size() <= 1) {
				out.push_back(line);
				continue;
			}

			changed = true;
			string prefix = line.substr(0, open_paren + 1); // up to and including '('
			string suffix = line.substr(close_paren);       // from ')' onwards

			const idx_t line_indent = LeadingSpaces(line);
			const string val_indent(line_indent + config.indent_size, ' ');

			// First line: everything up to and including '('
			out.push_back(prefix);
			// One value per line, closing ')' on the last value line
			for (idx_t v = 0; v < values.size(); v++) {
				string val_line = val_indent + values[v];
				if (v + 1 < values.size()) {
					val_line += ',';
				} else {
					val_line += suffix;
				}
				out.push_back(std::move(val_line));
			}
		}

		lines = std::move(out);
	}

	return JoinLines(lines);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────

string FormatSQL(const string &sql, const FormatterConfig &config) {
	return SQLFormatter(config).Format(sql);
}

} // namespace duckdb
