#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

ExplainStatement::ExplainStatement(unique_ptr<SQLStatement> stmt, ExplainType explain_type,
                                   const ProfilerPrintFormat &format)
    : SQLStatement(StatementType::EXPLAIN_STATEMENT), stmt(std::move(stmt)), explain_type(explain_type),
      format(format) {
}

ExplainStatement::ExplainStatement(const ExplainStatement &other)
    : SQLStatement(other), stmt(other.stmt->Copy()), explain_type(other.explain_type),
      allow_unsupported_sql(other.allow_unsupported_sql), format(other.format) {
}

unique_ptr<SQLStatement> ExplainStatement::Copy() const {
	return unique_ptr<ExplainStatement>(new ExplainStatement(*this));
}

string ExplainStatement::OptionsToString() const {
	if (explain_type == ExplainType::EXPLAIN_SQL) {
		return "(SQL)";
	}
	string options;
	if (explain_type == ExplainType::EXPLAIN_ANALYZE) {
		options += "(";
		options += "ANALYZE";
	}
	if (format != ProfilerPrintFormat::Default()) {
		if (options.empty()) {
			options += "(";
		} else {
			options += ", ";
		}
		options += StringUtil::Format("FORMAT %s", StringUtil::Upper(format.ToString()));
	}
	if (!options.empty()) {
		options += ")";
	}
	return options;
}

string ExplainStatement::ToString() const {
	string result = "EXPLAIN";
	auto options = OptionsToString();
	if (!options.empty()) {
		result += " " + options;
	}
	result += " " + stmt->ToString();
	return result;
}

} // namespace duckdb
