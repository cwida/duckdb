#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/statement/call_statement.hpp"
#include "duckdb/parser/statement/vacuum_statement.hpp"

namespace duckdb {

static unique_ptr<SQLStatement> TransformVacuumFullCall() {
	auto result = make_uniq<CallStatement>();
	vector<unique_ptr<ParsedExpression>> children;
	auto function = make_uniq<FunctionExpression>("vacuum_full", std::move(children));
	function->SetQualifiedName(
	    QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), function->GetQualifiedName().Name()));
	result->function = std::move(function);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformVacuumStatement(PEGTransformer &transformer,
                                                                         const optional<VacuumOptions> &vacuum_options,
                                                                         optional<AnalyzeTarget> analyze_target) {
	VacuumOptions options;
	if (vacuum_options) {
		options = *vacuum_options;
	}
	if (options.full) {
		if (options.analyze) {
			throw NotImplementedException("VACUUM FULL ANALYZE is not yet implemented");
		}
		if (analyze_target && analyze_target->ref) {
			throw NotImplementedException("VACUUM FULL with a table name is not yet implemented");
		}
		return TransformVacuumFullCall();
	}
	auto result = make_uniq<VacuumStatement>(options);
	if (analyze_target && analyze_target->ref) {
		result->info->columns = analyze_target->columns;
		result->info->ref = std::move(analyze_target->ref);
		result->info->has_table = true;
	}
	return std::move(result);
}

VacuumOptions PEGTransformerFactory::TransformVacuumLegacyOptions(PEGTransformer &transformer,
                                                                  const optional<string> &opt_full,
                                                                  const optional<string> &opt_freeze,
                                                                  const optional<string> &opt_verbose,
                                                                  const optional<string> &opt_analyze) {
	VacuumOptions options;
	options.vacuum = true;
	options.analyze = opt_analyze.has_value();
	if (opt_full) {
		options.full = true;
	}
	if (opt_freeze) {
		throw NotImplementedException("FREEZE is not yet implemented");
	}
	if (opt_verbose) {
		throw NotImplementedException("VERBOSE is not yet implemented");
	}
	return options;
}

VacuumOptions PEGTransformerFactory::TransformVacuumParensOptions(PEGTransformer &transformer,
                                                                  const vector<string> &vacuum_option) {
	VacuumOptions options;
	options.vacuum = true;
	for (auto &option : vacuum_option) {
		if (StringUtil::CIEquals(option, "disable_page_skipping")) {
			throw NotImplementedException("Disable Page Skipping vacuum option");
		}
		if (StringUtil::CIEquals(option, "freeze")) {
			throw NotImplementedException("FREEZE is not yet implemented");
		}
		if (StringUtil::CIEquals(option, "full")) {
			options.full = true;
			continue;
		}
		if (StringUtil::CIEquals(option, "verbose")) {
			throw NotImplementedException("VERBOSE is not yet implemented");
		}
		if (StringUtil::CIEquals(option, "analyze")) {
			options.analyze = true;
		} else {
			throw ParserException("unrecognized VACUUM option \"%s\"", option);
		}
	}
	return options;
}

vector<string> PEGTransformerFactory::TransformNameList(PEGTransformer &transformer, const vector<Identifier> &col_id) {
	return IdentifiersToStrings(col_id);
}

string PEGTransformerFactory::TransformOptAnalyze(PEGTransformer &transformer, const Identifier &analyze_keyword) {
	return "analyze";
}

string PEGTransformerFactory::TransformOptFull(PEGTransformer &transformer) {
	return "full";
}

string PEGTransformerFactory::TransformOptFreeze(PEGTransformer &transformer) {
	return "freeze";
}

string PEGTransformerFactory::TransformOptVerbose(PEGTransformer &transformer) {
	return "verbose";
}

} // namespace duckdb
