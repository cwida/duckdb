//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/statement/passthrough_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

//! Unparsed SQL forwarded as-is to the CONNECT-ed database; the text lives in `query`
class PassthroughStatement : public SQLStatement {
public:
	static constexpr const StatementType TYPE = StatementType::PASSTHROUGH_STATEMENT;

public:
	PassthroughStatement();

protected:
	PassthroughStatement(const PassthroughStatement &other) = default;

public:
	unique_ptr<SQLStatement> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
