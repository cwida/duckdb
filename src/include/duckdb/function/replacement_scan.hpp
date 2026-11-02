//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/replacement_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"

namespace duckdb {

class ClientContext;
class TableRef;

struct ReplacementScanData {
public:
	virtual ~ReplacementScanData() {
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct ReplacementScanInput {
public:
	explicit ReplacementScanInput(QualifiedName name_p)
	    : name(std::move(name_p)), catalog_name(name.Catalog().GetIdentifierName()),
	      schema_name(name.Schema().GetIdentifierName()), table_name(name.Name().GetIdentifierName()) {
	}

public:
	//! The (optionally qualified) name being looked up
	QualifiedName name;
	//! Backwards-compatible accessors pointing into the QualifiedName above
	const string &catalog_name;
	const string &schema_name;
	const string &table_name;
};

typedef unique_ptr<TableRef> (*replacement_scan_t)(ClientContext &context, ReplacementScanInput &input,
                                                   optional_ptr<ReplacementScanData> data);

//! Replacement table scans are automatically attempted when a table name cannot be found in the schema
//! This allows you to do e.g. SELECT * FROM 'filename.csv', and automatically convert this into a CSV scan
struct ReplacementScan {
	explicit ReplacementScan(replacement_scan_t function, unique_ptr<ReplacementScanData> data_p = nullptr)
	    : function(function), data(std::move(data_p)) {
	}

	static bool CanReplace(const string &table_name, const vector<string> &extensions) {
		auto lower_name = StringUtil::Lower(table_name);

		if (StringUtil::EndsWith(lower_name, CompressionExtensionFromType(FileCompressionType::GZIP))) {
			lower_name = lower_name.substr(0, lower_name.size() - 3);
		} else if (StringUtil::EndsWith(lower_name, CompressionExtensionFromType(FileCompressionType::ZSTD))) {
			lower_name = lower_name.substr(0, lower_name.size() - 4);
		}

		for (auto &extension : extensions) {
			if (StringUtil::EndsWith(lower_name, "." + extension) ||
			    StringUtil::Contains(lower_name, "." + extension + "?")) {
				return true;
			}
		}

		return false;
	}

	static string GetFullPath(const string &catalog, const string &schema, const string &table) {
		string table_name = catalog;
		if (!schema.empty()) {
			table_name += (!table_name.empty() ? "." : "") + schema;
		}
		table_name += (!table_name.empty() ? "." : "") + table;
		return table_name;
	}

	//! Joins every component of the name, which can be qualified deeper than [catalog, schema]
	static string GetFullPath(const QualifiedName &name) {
		string table_name;
		for (auto &component : name.Path()) {
			if (component.empty()) {
				continue;
			}
			table_name += (!table_name.empty() ? "." : "") + component.GetIdentifierName();
		}
		return table_name;
	}

	static string GetFullPath(ReplacementScanInput &input) {
		return GetFullPath(input.name);
	}

	replacement_scan_t function;
	unique_ptr<ReplacementScanData> data;
};

} // namespace duckdb
