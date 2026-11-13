//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/vacuum_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/tableref.hpp"

#include "duckdb/common/identifier.hpp"
namespace duckdb {
class Serializer;
class Deserializer;

struct VacuumOptions {
	VacuumOptions() : vacuum(false), analyze(false), full(false) {
	}

	bool vacuum;
	bool analyze;
	bool full;

	void Serialize(Serializer &serializer) const;
	static VacuumOptions Deserialize(Deserializer &deserializer);
};

struct VacuumInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::VACUUM_INFO;

public:
	explicit VacuumInfo(VacuumOptions options);

	const VacuumOptions options;
	vector<Identifier> columns;
	bool has_table;
	unique_ptr<TableRef> ref;

public:
	unique_ptr<VacuumInfo> Copy();
	string ToString() const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
