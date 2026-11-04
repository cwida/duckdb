#include "benchmark_runner.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/fstream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"

#include <sstream>

namespace duckdb {

enum class ParserWorkload : uint8_t {
	KEYWORD_IDENTIFIERS,
	WIDE_SELECT,
	NESTED_EXPRESSIONS,
	MALFORMED_SELECT,
	STATEMENTS,
	TPCH,
	TPCDS,
	FLUMMI,
	AOC,
	STRESS,
	VALUES_LIST
};

struct ParserBenchmarkState : public BenchmarkState {
	ParserOptions options = ParserOptions::Builtin();
	vector<string> queries;
	idx_t statements_parsed = 0;
	idx_t parser_errors = 0;
	bool valid_statement_counts = true;
	atomic<bool> interrupted {false};
};

class ParserMicroBenchmark : public Benchmark {
public:
	ParserMicroBenchmark(const string &name, ParserWorkload workload_p, idx_t iterations_p, idx_t statement_count_p = 1)
	    : Benchmark(true, name, "[parser]"), workload(workload_p), iterations(iterations_p),
	      statement_count(statement_count_p) {
	}

	unique_ptr<BenchmarkState> Initialize(BenchmarkConfiguration &config) override {
		auto state = make_uniq<ParserBenchmarkState>();
		state->queries = LoadQueries();
		// Keep malformed-input backtracking under the runner's timeout in Run.
		if (workload == ParserWorkload::MALFORMED_SELECT) {
			return std::move(state);
		}
		for (idx_t i = 0; i < state->queries.size(); i++) {
			Parser parser(state->options);
			parser.ParseQuery(state->queries[i]);
			if (parser.statements.size() != statement_count) {
				throw InvalidInputException("Parser benchmark '%s' input %llu expected %llu statements, got %llu", name,
				                            i + 1, statement_count, parser.statements.size());
			}
		}
		return std::move(state);
	}

	void Run(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserBenchmarkState &>(*state_p);
		state.statements_parsed = 0;
		state.parser_errors = 0;
		state.valid_statement_counts = true;
		for (idx_t i = 0; i < iterations; i++) {
			for (auto &query : state.queries) {
				if (state.interrupted.load()) {
					return;
				}
				if (workload == ParserWorkload::MALFORMED_SELECT) {
					state.parser_errors += RejectMalformedQuery(state.options, query);
					continue;
				}
				Parser parser(state.options);
				parser.ParseQuery(query);
				state.statements_parsed += parser.statements.size();
				state.valid_statement_counts &= parser.statements.size() == statement_count;
			}
		}
	}

	void Cleanup(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserBenchmarkState &>(*state_p);
		state.interrupted.store(false);
	}

	string Verify(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserBenchmarkState &>(*state_p);
		if (workload == ParserWorkload::MALFORMED_SELECT) {
			return state.parser_errors == iterations * QueryCount() ? string()
			                                                        : "Expected a ParserException on every call";
		}
		if (!state.valid_statement_counts || state.statements_parsed != iterations * QueryCount() * statement_count) {
			return "Unexpected number of parsed statements";
		}
		return string();
	}

	void Interrupt(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserBenchmarkState &>(*state_p);
		state.interrupted.store(true);
	}

	string GetLogOutput(BenchmarkState *state) override {
		return string();
	}

	string DisplayName() override {
		if (workload == ParserWorkload::MALFORMED_SELECT) {
			return StringUtil::Format("%s (%llu expected ParserExceptions/run)", name, iterations);
		}
		return StringUtil::Format("%s (%llu inputs x %llu repetitions, %llu statements/input)", name, QueryCount(),
		                          iterations, statement_count);
	}

	string BenchmarkInfo() override {
		if (workload == ParserWorkload::MALFORMED_SELECT) {
			return StringUtil::Format("Parser::ParseQuery, %llu rejected inputs/run; reused compiled grammar, "
			                          "includes failed matching, error construction and cleanup",
			                          iterations);
		}
		return StringUtil::Format("Parser::ParseQuery, %llu calls/run, %llu statements/call; "
		                          "default parser options, reused compiled grammar, no query execution",
		                          iterations * QueryCount(), statement_count);
	}

	string GetQuery() override {
		switch (workload) {
		case ParserWorkload::KEYWORD_IDENTIFIERS:
			return "SELECT abort, action, comment, database, first, last FROM source_table "
			       "WHERE action IS NOT NULL AND comment <> 'value' ORDER BY first, last";
		case ParserWorkload::WIDE_SELECT: {
			string query = "SELECT ";
			for (idx_t i = 0; i < 128; i++) {
				if (i > 0) {
					query += ", ";
				}
				query += "column_" + to_string(i) + " + " + to_string(i) + " AS alias_" + to_string(i);
			}
			return query + " FROM source_table";
		}
		case ParserWorkload::NESTED_EXPRESSIONS: {
			string expression = "value_column";
			for (idx_t i = 0; i < 32; i++) {
				expression = "coalesce(" + expression + ", " + to_string(i) + ")";
			}
			return "SELECT " + expression + " FROM source_table";
		}
		case ParserWorkload::MALFORMED_SELECT:
			return "select (((((((((((((;";
		case ParserWorkload::STATEMENTS: {
			string query;
			for (idx_t i = 0; i < 32; i++) {
				query += "SELECT " + to_string(i) + " AS \"quoted column\"; -- statement boundary\n";
			}
			return query;
		}
		case ParserWorkload::TPCH:
		case ParserWorkload::TPCDS:
		case ParserWorkload::FLUMMI:
		case ParserWorkload::AOC:
		case ParserWorkload::STRESS:
		case ParserWorkload::VALUES_LIST:
			return StringUtil::Join(LoadQueries(), "\n");
		default:
			throw InternalException("Unknown parser benchmark workload");
		}
	}

private:
	static bool RejectMalformedQuery(const ParserOptions &options, const string &query) {
		Parser parser(options);
		try {
			parser.ParseQuery(query);
		} catch (const ParserException &) {
			return true;
		}
		return false;
	}

	idx_t QueryCount() const {
		switch (workload) {
		case ParserWorkload::TPCH:
			return 22;
		case ParserWorkload::TPCDS:
			return 99;
		case ParserWorkload::AOC:
			return 25;
		default:
			return 1;
		}
	}

	static string ReadQuery(const string &path) {
		ifstream input(path, ios::binary);
		if (!input.is_open()) {
			throw IOException("Cannot open parser benchmark input '%s'; provide the query files under --root-dir",
			                  path);
		}
		std::ostringstream buffer;
		buffer << input.rdbuf();
		if (input.bad() || buffer.bad()) {
			throw IOException("Cannot read parser benchmark input '%s'", path);
		}
		auto query = buffer.str();
		if (query.empty()) {
			throw InvalidInputException("Parser benchmark input '%s' is empty", path);
		}
		return query;
	}

	vector<string> LoadQueries() {
		string prefix;
		switch (workload) {
		case ParserWorkload::FLUMMI:
			return {ReadQuery("benchmark/recursive_cte/queries/performance/flummi_ray.sql")};
		case ParserWorkload::STRESS:
			return {ReadQuery("benchmark/micro/parser/parser_stress.sql")};
		case ParserWorkload::VALUES_LIST:
			return {ReadQuery("benchmark/micro/parser/parser_values_list.sql")};
		case ParserWorkload::TPCH:
			prefix = "extension/tpch/dbgen/queries/q";
			break;
		case ParserWorkload::TPCDS:
			prefix = "extension/tpcds/dsdgen/queries/";
			break;
		case ParserWorkload::AOC:
			prefix = "benchmark/aoc24/queries/day";
			break;
		default:
			return {GetQuery()};
		}
		vector<string> queries;
		for (idx_t i = 1; i <= QueryCount(); i++) {
			queries.push_back(ReadQuery(prefix + (i < 10 ? "0" : "") + to_string(i) + ".sql"));
		}
		return queries;
	}

private:
	ParserWorkload workload;
	idx_t iterations;
	idx_t statement_count;
};

ParserMicroBenchmark parser_keyword_identifiers("ParserKeywordIdentifiers", ParserWorkload::KEYWORD_IDENTIFIERS, 650);
ParserMicroBenchmark parser_wide_select("ParserWideSelect", ParserWorkload::WIDE_SELECT, 55);
ParserMicroBenchmark parser_nested_expressions("ParserNestedExpressions", ParserWorkload::NESTED_EXPRESSIONS, 250);
ParserMicroBenchmark parser_malformed_select("ParserMalformedSelect", ParserWorkload::MALFORMED_SELECT, 400, 0);
ParserMicroBenchmark parser_statements("ParserStatements", ParserWorkload::STATEMENTS, 700, 32);
ParserMicroBenchmark parser_tpch("ParserTPCH", ParserWorkload::TPCH, 12);
ParserMicroBenchmark parser_tpcds("ParserTPCDS", ParserWorkload::TPCDS, 1);
ParserMicroBenchmark parser_flummi("ParserFlummi", ParserWorkload::FLUMMI, 1);
ParserMicroBenchmark parser_aoc("ParserAoC", ParserWorkload::AOC, 2);
ParserMicroBenchmark parser_stress("ParserStress", ParserWorkload::STRESS, 7);
ParserMicroBenchmark parser_values_list("ParserValuesList", ParserWorkload::VALUES_LIST, 8);

struct ParserGrammarConstructionState : public BenchmarkState {
	idx_t grammars_constructed = 0;
	atomic<bool> interrupted {false};
};

class ParserGrammarConstructionBenchmark : public Benchmark {
public:
	ParserGrammarConstructionBenchmark() : Benchmark(true, "ParserGrammarConstruction", "[parser]") {
	}

	unique_ptr<BenchmarkState> Initialize(BenchmarkConfiguration &config) override {
		return make_uniq<ParserGrammarConstructionState>();
	}

	void Run(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserGrammarConstructionState &>(*state_p);
		state.grammars_constructed = 0;
		for (idx_t i = 0; i < ITERATIONS; i++) {
			if (state.interrupted.load()) {
				return;
			}
			auto grammar = CompiledGrammar::Create();
			state.grammars_constructed += grammar != nullptr;
		}
	}

	void Cleanup(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserGrammarConstructionState &>(*state_p);
		state.interrupted.store(false);
	}

	string Verify(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserGrammarConstructionState &>(*state_p);
		return state.grammars_constructed == ITERATIONS ? string() : "Unexpected number of constructed grammars";
	}

	void Interrupt(BenchmarkState *state_p) override {
		auto &state = static_cast<ParserGrammarConstructionState &>(*state_p);
		state.interrupted.store(true);
	}

	string GetLogOutput(BenchmarkState *state) override {
		return string();
	}

	string DisplayName() override {
		return StringUtil::Format("%s (%llu grammar constructions/run)", name, ITERATIONS);
	}

	string BenchmarkInfo() override {
		return "Construct and destroy the compiled base grammar; includes grammar parsing, keyword tables and matcher "
		       "construction, excludes SQL parsing and grammar extensions";
	}

private:
	static constexpr idx_t ITERATIONS = 250;
};

ParserGrammarConstructionBenchmark parser_grammar_construction;

} // namespace duckdb
