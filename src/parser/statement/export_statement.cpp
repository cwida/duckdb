#include "duckdb/parser/statement/export_statement.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

ExportStatement::ExportStatement(unique_ptr<CopyInfo> info)
    : SQLStatement(StatementType::EXPORT_STATEMENT), info(std::move(info)) {
}

ExportStatement::ExportStatement(const ExportStatement &other)
    : SQLStatement(other), info(other.info->Copy()), database(other.database) {
}

unique_ptr<SQLStatement> ExportStatement::Copy() const {
	return unique_ptr<ExportStatement>(new ExportStatement(*this));
}

string ExportStatement::ToString() const {
	string result = "";
	result += "EXPORT DATABASE";
	if (!database.empty()) {
		result += " " + SQLIdentifier(database) + " TO";
	}
	auto &path = info->file_path;
	D_ASSERT(info->is_from == false);
	result += " " + SQLString(path);
	result += info->CopyOptionsToString();
	result += ";";
	return result;
}

} // namespace duckdb
