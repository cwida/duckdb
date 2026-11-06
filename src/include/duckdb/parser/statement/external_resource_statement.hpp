//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/statement/external_resource_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

//! The durable external-resource verbs. CREATE provisions and registers a resource under a local name;
//! REGISTER registers an already-provisioned resource from a handle value; DESTROY tears one down and
//! deregisters it; SHOW lists the registered resources.
enum class ExternalResourceOperation : uint8_t { CREATE, REGISTER, DESTROY };

class ExternalResourceStatement : public SQLStatement {
public:
	static constexpr const StatementType TYPE = StatementType::EXTERNAL_RESOURCE_STATEMENT;

public:
	explicit ExternalResourceStatement(ExternalResourceOperation operation);

	ExternalResourceOperation operation;
	//! The resource type ('<recipe>'): a string literal, so it is fixed at parse time. Set for CREATE and REGISTER.
	string type;
	//! The local name: the `AS <name>` alias for CREATE/REGISTER (optional), or the target for DESTROY.
	Identifier name;
	//! Create params `(k v, ...)`. Set for CREATE.
	case_insensitive_map_t<unique_ptr<ParsedExpression>> options;
	//! The `FROM <value>` handle expression. Set for REGISTER.
	unique_ptr<ParsedExpression> handle;

protected:
	ExternalResourceStatement(const ExternalResourceStatement &other);

public:
	unique_ptr<SQLStatement> Copy() const override;
	string ToString() const override;
};

} // namespace duckdb
