//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parser_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/allow_parser_override.hpp"
#include "duckdb/common/enums/identifier_case_mode.hpp"
#include "duckdb/common/enums/regex_match_operator_semantics.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class ExtensionCallbackManager;
class ParserExtension;
struct CompiledGrammar;

struct ParserOptions {
private:
	ParserOptions() {
	}
	friend class ClientContext;

public:
	//! Explicit configuration for parsing without a client context.
	DUCKDB_API static ParserOptions Builtin();

public:
	IdentifierCaseMode identifier_case_mode = IdentifierCaseMode::PRESERVE_CASE;
	bool integer_division = false;
	RegexMatchOperatorSemantics regex_match_operator_semantics = RegexMatchOperatorSemantics::PARTIAL;
	idx_t max_expression_depth = 1000;
	optional_ptr<const ExtensionCallbackManager> extensions;
	AllowParserOverride parser_override_setting = AllowParserOverride::DEFAULT_OVERRIDE;
	shared_ptr<CompiledGrammar> compiled_grammar;
	//! The client context the statement is parsed for (not set when parsing without a client context)
	optional_ptr<ClientContext> context;
};

} // namespace duckdb
