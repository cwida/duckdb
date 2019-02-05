//===----------------------------------------------------------------------===//
//                         DuckDB
//
// planner/operator/logical_prepare.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "planner/logical_operator.hpp"

#include <unordered_set>

namespace duckdb {

class TableCatalogEntry;

class LogicalPrepare : public LogicalOperator {
public:
	LogicalPrepare(string name, StatementType statement_type, unique_ptr<LogicalOperator> logical_plan)
	    : LogicalOperator(LogicalOperatorType::PREPARE), name(name), statement_type(statement_type) {
		children.push_back(move(logical_plan));
	}

	string name;
	StatementType statement_type;

	vector<string> GetNames() override {
		return {"Success"};
	}

	void GetTableBindings(std::unordered_set<TableCatalogEntry *> &result_list);

protected:
	void ResolveTypes() override {
		types.push_back(TypeId::BOOLEAN);
	}
};
} // namespace duckdb
