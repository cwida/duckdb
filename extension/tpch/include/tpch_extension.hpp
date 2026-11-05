//===----------------------------------------------------------------------===//
//                         DuckDB
//
// tpch_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class TpchExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;

	//! Gets the specified TPC-H Query number as a string
	static std::string GetQuery(int query);
	//! Gets the specified TPC-H Query number with the substitution parameters for the given scale factor
	static std::string GetQuery(int query, double sf);
	//! Returns the CSV answer of a TPC-H query
	static std::string GetAnswer(double sf, int query);
};

} // namespace duckdb
