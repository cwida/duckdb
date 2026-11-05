//===----------------------------------------------------------------------===//
//                         DuckDB
//
// autocomplete_catalog_provider.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/peg/compiled_grammar.hpp"

namespace duckdb {

//! Parameters for autocomplete suggestion generation.
struct AutoCompleteParameters {
	idx_t max_suggestion_count = 20;
	idx_t max_file_suggestion_count = 1;
	idx_t max_exact_suggestion_count = 100;
	bool suggestion_contains_files = false;
};

//! Abstract interface for providing catalog-aware autocomplete suggestions.
//! The PEG parser and keyword matcher work without this — catalog suggestions
//! are additive (table names, column names, function names, etc.).
class AutoCompleteCatalogProvider {
public:
	virtual ~AutoCompleteCatalogProvider() = default;

	virtual vector<AutoCompleteCandidate> SuggestCatalogName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestSchemaName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestTableName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestType() = 0;
	virtual vector<AutoCompleteCandidate> SuggestColumnName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestFileName(string &prefix, idx_t &last_pos) = 0;
	virtual vector<AutoCompleteCandidate> SuggestScalarFunctionName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestTableFunctionName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestPragmaName() = 0;
	virtual vector<AutoCompleteCandidate> SuggestSettingName() = 0;

	//! Get the parser cache for syntax-level autocomplete.
	virtual shared_ptr<CompiledGrammar> GetCompiledGrammar() = 0;
};

//! Empty provider — returns no catalog suggestions.
//! Keywords and SQL syntax completion still work via the PEG grammar.
class EmptyCatalogProvider : public AutoCompleteCatalogProvider {
public:
	vector<AutoCompleteCandidate> SuggestCatalogName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestSchemaName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestTableName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestType() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestColumnName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestFileName(string &, idx_t &) override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestScalarFunctionName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestTableFunctionName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestPragmaName() override {
		return {};
	}
	vector<AutoCompleteCandidate> SuggestSettingName() override {
		return {};
	}
	shared_ptr<CompiledGrammar> GetCompiledGrammar() override {
		if (!compiled_grammar) {
			compiled_grammar = CompiledGrammar::Create();
		}
		return compiled_grammar;
	}

private:
	shared_ptr<CompiledGrammar> compiled_grammar;
};

//! Get a human-readable string for a suggestion type.
string GetSuggestionType(SuggestionState type);

//! Generate autocomplete suggestions for the given SQL text.
//! Uses the PEG grammar for syntax and the provider for catalog awareness.
vector<AutoCompleteSuggestion> GenerateAutoCompleteSuggestions(AutoCompleteCatalogProvider &provider, const string &sql,
                                                               AutoCompleteParameters &parameters);

} // namespace duckdb
