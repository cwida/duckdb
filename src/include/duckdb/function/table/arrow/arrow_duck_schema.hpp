//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/arrow/arrow_duck_schema.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/validity_mask.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/function/table/arrow/arrow_type_info.hpp"
#include "duckdb/common/arrow/arrow.hpp"

namespace duckdb {
struct DBConfig;

struct ArrowArrayScanState;

typedef void (*cast_arrow_duck_t)(ClientContext &context, Vector &source, Vector &result, idx_t count);

typedef void (*cast_duck_arrow_t)(ClientContext &context, const Vector &source, Vector &result, idx_t count);

class ArrowTypeExtensionData {
public:
	explicit ArrowTypeExtensionData(const LogicalType &duckdb_type, const LogicalType &internal_type_p,
	                                cast_arrow_duck_t arrow_to_duckdb = nullptr,
	                                cast_duck_arrow_t duckdb_to_arrow = nullptr)
	    : arrow_to_duckdb(arrow_to_duckdb), duckdb_to_arrow(duckdb_to_arrow), duckdb_type(duckdb_type),
	      internal_type(internal_type_p) {
	}

	explicit ArrowTypeExtensionData(const LogicalType &duckdb_type)
	    : duckdb_type(duckdb_type), internal_type(duckdb_type) {
	}

	//! (Optional) Callback to function that converts an Arrow Array to a DuckDB Vector
	cast_arrow_duck_t arrow_to_duckdb = nullptr;
	//! (Optional) Callback to function that converts a DuckDB Vector to an Arrow Array
	cast_duck_arrow_t duckdb_to_arrow = nullptr;

	LogicalType GetInternalType() const;

	//! This function returns possible extension types to given DuckDB types
	static unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>>
	GetExtensionTypes(ClientContext &context, const vector<LogicalType> &duckdb_types);
	LogicalType GetDuckDBType() const;

private:
	//! Original DuckDB Type (e.g., UHUGEINT)
	LogicalType duckdb_type;
	//! Internal type is a type that refers to the produced duckdb vector when consuming the arrow format
	LogicalType internal_type;
};

class ArrowType {
public:
	//! From a DuckDB type
	explicit ArrowType(LogicalType type_p, unique_ptr<ArrowTypeInfo> type_info = nullptr)
	    : type(std::move(type_p)), type_info(std::move(type_info)) {
	}
	explicit ArrowType(string error_message_p, bool not_implemented_p = false)
	    : type(LogicalTypeId::INVALID), type_info(nullptr), error_message(std::move(error_message_p)),
	      not_implemented(not_implemented_p) {
	}

public:
	LogicalType GetDuckType(bool use_dictionary = false) const;

	void SetDictionary(unique_ptr<ArrowType> dictionary);
	bool HasDictionary() const;
	const ArrowType &GetDictionary() const;

	bool RunEndEncoded() const;
	void SetRunEndEncoded();

	template <class T>
	const T &GetTypeInfo() const {
		return type_info->Cast<T>();
	}
	void ThrowIfInvalid() const;

	static unique_ptr<ArrowType> GetTypeFromFormat(string &format);
	static unique_ptr<ArrowType> GetTypeFromFormat(ClientContext &context, ArrowSchema &schema, string &format);

	static unique_ptr<ArrowType> GetTypeFromSchema(ClientContext &context, ArrowSchema &schema);

	static unique_ptr<ArrowType> CreateListType(ClientContext &context, ArrowSchema &child,
	                                            ArrowVariableSizeType size_type, bool view);

	static unique_ptr<ArrowType> GetArrowLogicalType(ClientContext &context, ArrowSchema &schema);
	//! A producer may omit a field name as NULL, which the C data interface defines to mean the same as an empty name
	static string GetFieldName(const ArrowSchema &schema);

	bool HasExtension() const;

	ArrowArrayPhysicalType GetPhysicalType() const;

	//! The Arrow Type Extension data, if any
	shared_ptr<ArrowTypeExtensionData> extension_data;

protected:
	LogicalType type;
	//! Hold the optional type if the array is a dictionary
	unique_ptr<ArrowType> dictionary_type;
	//! Is run-end-encoded
	bool run_end_encoded = false;
	unique_ptr<ArrowTypeInfo> type_info;
	//! Error message in case of an invalid type (i.e., from an unsupported extension)
	string error_message;
	//! In case of an error do we throw not implemented?
	bool not_implemented = false;
};

using arrow_column_map_t = unordered_map<idx_t, shared_ptr<ArrowType>>;

struct ArrowTableSchema {
public:
	void AddColumn(idx_t index, shared_ptr<ArrowType> type, const string &name);
	const arrow_column_map_t &GetColumns() const;
	vector<LogicalType> &GetTypes();
	vector<string> &GetNames();

private:
	arrow_column_map_t arrow_convert_data;
	vector<LogicalType> types;
	vector<string> column_names;
};

} // namespace duckdb
