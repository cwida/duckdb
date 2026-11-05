//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// dsdgen.hpp
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

namespace tpcds {

struct DSDGenWrapper {
	//! Create the TPC-DS tables in the given schema with the given suffix
	static void CreateTPCDSSchema(duckdb::ClientContext &context, const duckdb::Identifier &catalog,
	                              const duckdb::Identifier &schema, std::string suffix, bool keys, bool overwrite);
	//! Generate the TPC-DS data of the given scale factor
	static void DSDGen(double scale, duckdb::ClientContext &context, const duckdb::Identifier &catalog,
	                   const duckdb::Identifier &schema, std::string suffix);

	static uint32_t QueriesCount();
	//! Gets the specified TPC-DS Query number as a string
	static std::string GetQuery(int query);
	//! Gets the specified TPC-DS Query number with the substitution parameters for the given scale factor
	static std::string GetQuery(int query, double sf);
	//! Returns the CSV answer of a TPC-DS query
	static std::string GetAnswer(double sf, int query);
};

class DSDGenGenerator {
public:
	virtual ~DSDGenGenerator();

	//! Generate the next batch of data, returning true when generation is complete
	virtual bool GenerateNext() = 0;
	//! Returns the progress percentage in [0, 100]
	virtual double Progress() const = 0;
	//! Returns whether generation can safely be resumed by a different async task
	virtual bool CanYield() const = 0;
};

duckdb::unique_ptr<DSDGenGenerator> CreateDSDGenGenerator(duckdb::ClientContext &context, double sf,
                                                          const duckdb::Identifier &catalog,
                                                          const duckdb::Identifier &schema, std::string suffix);

} // namespace tpcds
