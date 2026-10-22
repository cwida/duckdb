#include "duckdb/common/algorithm.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/passthrough_dialect.hpp"

namespace duckdb {

Parser::Parser(ClientContext &context) : Parser(context.GetParserOptions()) {
}

Parser::Parser(ClientContext &context, IdentifierCaseMode identifier_case_mode) : Parser(context) {
	options.identifier_case_mode = identifier_case_mode;
}

//! The grammar extensions a session parses with: the default-active registered extensions (sorted by
//! name, the registry map is unordered) followed by the explicitly activated list in user order, so
//! explicit overrides win over default-active ones
static vector<string> ComposeSessionGrammarNames(const ExtensionCallbackManager &callback_manager,
                                                 const vector<string> &explicit_names) {
	vector<string> result;
	for (auto &entry : callback_manager.GrammarExtensions()) {
		if (entry.second->ActiveByDefault()) {
			result.push_back(entry.second->Name());
		}
	}
	std::sort(result.begin(), result.end());
	case_insensitive_set_t seen(result.begin(), result.end());
	for (auto &name : explicit_names) {
		if (seen.insert(name).second) {
			result.push_back(name);
		}
	}
	return result;
}

shared_ptr<CompiledGrammar> CompiledGrammar::Get(ClientContext &context) {
	auto &client_config = ClientConfig::GetConfig(context);
	auto &callback_manager = ExtensionCallbackManager::Get(context);
	if (client_config.connected_grammar) {
		// while CONNECT-ed, the database being talked to decides how its statements are parsed
		return client_config.connected_grammar;
	}
	if (client_config.current_dialect) {
		auto dialect_extension = callback_manager.GetDialectExtension(*client_config.current_dialect);
		if (!dialect_extension) {
			throw InternalException("Dialect extension set to '%s' but couldn't be located in the registry",
			                        *client_config.current_dialect);
		}
		return dialect_extension->GetCompiledGrammar(context);
	}
	auto registry_version = callback_manager.GrammarRegistryVersion();
	if (client_config.cached_grammar && client_config.cached_grammar_version == registry_version) {
		return client_config.cached_grammar;
	}
	// recompose: either nothing is cached yet, or a grammar extension registered since (e.g. a LOAD
	// between two statements of this session), so default-active grammar becomes visible here
	auto grammar = ComposeSessionGrammar(context, client_config.active_grammar_extensions);
	client_config.cached_grammar = grammar;
	client_config.cached_grammar_version = registry_version;
	return grammar;
}

shared_ptr<CompiledGrammar> CompiledGrammar::ComposeSessionGrammar(ClientContext &context,
                                                                   const vector<string> &explicit_names) {
	auto &callback_manager = ExtensionCallbackManager::Get(context);
	auto &parser_cache = DatabaseInstance::GetDatabase(context).GetParserCache();
	auto extension_names = ComposeSessionGrammarNames(callback_manager, explicit_names);
	if (extension_names.empty()) {
		return parser_cache.GetMatcher();
	}
	return parser_cache.GetMatcher(context, extension_names);
}

shared_ptr<CompiledGrammar> CompiledGrammar::Create(const ClientContext &context,
                                                    const vector<string> &active_extensions) {
	return context.db->GetParserCache().GetMatcher(context, active_extensions);
}

ParserCache::ParserCache() {
}

ParserCache::~ParserCache() {
}

shared_ptr<CompiledGrammar> ParserCache::GetMatcher() {
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (matcher) {
			return matcher;
		}
	}
	auto new_matcher = CompiledGrammar::DefaultGrammar();

	std::unique_lock<std::mutex> lock(mutex);
	if (!matcher) {
		matcher = std::move(new_matcher);
	}
	return matcher;
}

shared_ptr<CompiledGrammar> ParserCache::GetMatcher(const ClientContext &context,
                                                    const vector<string> &active_extensions) {
	vector<reference<GrammarExtension>> selected_extensions;
	vector<string> extension_names;
	auto &callback_manager = ExtensionCallbackManager::Get(context);
	for (auto &name : active_extensions) {
		auto grammar_extension = callback_manager.FindGrammarExtension(name);
		if (grammar_extension) {
			selected_extensions.emplace_back(*grammar_extension);
			extension_names.push_back(grammar_extension->Name());
		}
	}
	// compile while holding the lock, so concurrent first uses of the same extensions only compile once
	lock_guard<std::mutex> lock(extension_mutex);
	for (idx_t i = 0; i < extension_matchers.size(); i++) {
		if (extension_matchers[i].first == extension_names) {
			// move the entry to the back, which holds the most recently used grammar
			auto entry = std::move(extension_matchers[i]);
			extension_matchers.erase(extension_matchers.begin() + NumericCast<int64_t>(i));
			extension_matchers.push_back(std::move(entry));
			return extension_matchers.back().second;
		}
	}
	// compile before evicting, so a grammar that fails to compile does not evict a valid one
	auto new_matcher = CompiledGrammar::Create(selected_extensions);
	if (extension_matchers.size() >= MAX_CACHED_EXTENSION_MATCHERS) {
		// connections that still use the evicted grammar keep their own reference to it
		extension_matchers.erase(extension_matchers.begin());
	}
	extension_matchers.emplace_back(std::move(extension_names), std::move(new_matcher));
	return extension_matchers.back().second;
}

shared_ptr<CompiledGrammar> ParserCache::GetPassthroughMatcher(const ClientContext &context) {
	lock_guard<std::mutex> lock(passthrough_mutex);
	if (!passthrough_dialect) {
		passthrough_dialect = make_uniq<PassthroughDialect>();
	}
	// the dialect caches the compiled grammar itself
	return passthrough_dialect->GetCompiledGrammar(context);
}

} // namespace duckdb
