#include "duckdb/common/enums/catalog_type.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

// LCOV_EXCL_START
string CatalogTypeToString(CatalogType type) {
	switch (type) {
	case CatalogType::COLLATION_ENTRY:
		return "Collation";
	case CatalogType::COORDINATE_SYSTEM_ENTRY:
		return "Coordinate System";
	case CatalogType::TYPE_ENTRY:
		return "Type";
	case CatalogType::TABLE_ENTRY:
		return "Table";
	case CatalogType::SCHEMA_ENTRY:
		return "Schema";
	case CatalogType::DATABASE_ENTRY:
		return "Database";
	case CatalogType::TABLE_FUNCTION_ENTRY:
		return "Table Function";
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		return "Scalar Function";
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
		return "Aggregate Function";
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		return "Window Function";
	case CatalogType::COPY_FUNCTION_ENTRY:
		return "Copy Function";
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
		return "Pragma Function";
	case CatalogType::MACRO_ENTRY:
		return "Macro Function";
	case CatalogType::TABLE_MACRO_ENTRY:
		return "Table Macro Function";
	case CatalogType::VIEW_ENTRY:
		return "View";
	case CatalogType::INDEX_ENTRY:
		return "Index";
	case CatalogType::PREPARED_STATEMENT:
		return "Prepared Statement";
	case CatalogType::SEQUENCE_ENTRY:
		return "Sequence";
	case CatalogType::TRIGGER_ENTRY:
		return "Trigger";
	case CatalogType::LANGUAGE_ENTRY:
		return "Language";
	case CatalogType::SECRET_ENTRY:
		return "Secret";
	case CatalogType::SECRET_TYPE_ENTRY:
		return "Secret Type";
	case CatalogType::SECRET_FUNCTION_ENTRY:
		return "Secret Function";
	case CatalogType::INVALID:
	case CatalogType::DELETED_ENTRY:
	case CatalogType::RENAMED_ENTRY:
	case CatalogType::DEPENDENCY_ENTRY:
		break;
	}
	return "INVALID";
}

CatalogType CatalogTypeFromString(const string &type) {
	if (type == "Collation") {
		return CatalogType::COLLATION_ENTRY;
	}
	if (type == "Coordinate System") {
		return CatalogType::COORDINATE_SYSTEM_ENTRY;
	}
	if (type == "Type") {
		return CatalogType::TYPE_ENTRY;
	}
	if (type == "Table") {
		return CatalogType::TABLE_ENTRY;
	}
	if (type == "Schema") {
		return CatalogType::SCHEMA_ENTRY;
	}
	if (type == "Database") {
		return CatalogType::DATABASE_ENTRY;
	}
	if (type == "Table Function") {
		return CatalogType::TABLE_FUNCTION_ENTRY;
	}
	if (type == "Scalar Function") {
		return CatalogType::SCALAR_FUNCTION_ENTRY;
	}
	if (type == "Aggregate Function") {
		return CatalogType::AGGREGATE_FUNCTION_ENTRY;
	}
	if (type == "Copy Function") {
		return CatalogType::COPY_FUNCTION_ENTRY;
	}
	if (type == "Pragma Function") {
		return CatalogType::PRAGMA_FUNCTION_ENTRY;
	}
	if (type == "Macro Function") {
		return CatalogType::MACRO_ENTRY;
	}
	if (type == "Table Macro Function") {
		return CatalogType::TABLE_MACRO_ENTRY;
	}
	if (type == "View") {
		return CatalogType::VIEW_ENTRY;
	}
	if (type == "Index") {
		return CatalogType::INDEX_ENTRY;
	}
	if (type == "Prepared Statement") {
		return CatalogType::PREPARED_STATEMENT;
	}
	if (type == "Sequence") {
		return CatalogType::SEQUENCE_ENTRY;
	}
	if (type == "Trigger") {
		return CatalogType::TRIGGER_ENTRY;
	}
	if (type == "Language") {
		return CatalogType::LANGUAGE_ENTRY;
	}
	if (type == "INVALID") {
		return CatalogType::INVALID;
	}
	throw InternalException("Unrecognized CatalogType '%s'", type);
}

// LCOV_EXCL_STOP

} // namespace duckdb
