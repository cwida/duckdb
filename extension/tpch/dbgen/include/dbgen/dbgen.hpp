//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// dbgen.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {
class ClientContext;
}

namespace tpch {

struct DBGenWrapper {
	//! Create the TPC-H tables in the given catalog / schema with the given suffix
	static void CreateTPCHSchema(duckdb::ClientContext &context, const duckdb::Identifier &catalog,
	                             const duckdb::Identifier &schema, std::string suffix);
	//! Load the TPC-H data of the given scale factor
	static void LoadTPCHData(duckdb::ClientContext &context, double sf, const duckdb::Identifier &catalog,
	                         const duckdb::Identifier &schema, std::string suffix, int children, int step);

	//! Gets the specified TPC-H Query number as a string
	static std::string GetQuery(int query);
	//! Gets the specified TPC-H Query number with the substitution parameters for the given scale factor
	static std::string GetQuery(int query, double sf);
	//! Returns the CSV answer of a TPC-H query
	static std::string GetAnswer(double sf, int query);
};

class DBGenGenerator {
public:
	virtual ~DBGenGenerator();

	//! Generate the next batch of data, returning true when generation is complete
	virtual bool GenerateNext() = 0;
	//! Returns the progress percentage in [0, 100]
	virtual double Progress() const = 0;
};

duckdb::unique_ptr<DBGenGenerator> CreateDBGenGenerator(duckdb::ClientContext &context, double sf,
                                                        const duckdb::Identifier &catalog,
                                                        const duckdb::Identifier &schema, std::string suffix,
                                                        int children, int step);

} // namespace tpch
