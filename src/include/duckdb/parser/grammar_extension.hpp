//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/grammar_extension.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/parser/grammar_change.hpp"

namespace duckdb {

class DatabaseInstance;

//! A light-weight composable extension to the grammar of the parser
class GrammarExtension {
public:
	explicit GrammarExtension(string name_p, string description_p)
	    : name(std::move(name_p)), description(std::move(description_p)) {
	}
	virtual ~GrammarExtension() {
	}

public:
	DUCKDB_API static void Register(DatabaseInstance &db, shared_ptr<GrammarExtension> extension);

public:
	const string &Name() const {
		return name;
	}
	const string &Description() const {
		return description;
	}
	virtual vector<GrammarChange> GetChanges() const = 0;

private:
	string name;
	//! Description of the changes made by the extension
	string description;
};

} // namespace duckdb
