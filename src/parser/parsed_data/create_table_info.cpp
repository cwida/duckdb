#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"

namespace duckdb {

CreateTableInfo::CreateTableInfo() : CreateInfo(CatalogType::TABLE_ENTRY, Identifier::InvalidSchema()) {
}

CreateTableInfo::CreateTableInfo(QualifiedName qualified_name_p) : CreateInfo(CatalogType::TABLE_ENTRY) {
	SetQualifiedName(std::move(qualified_name_p));
}

CreateTableInfo::CreateTableInfo(SchemaCatalogEntry &schema, const Identifier &name_p)
    : CreateTableInfo(schema.GetQualifiedName(name_p)) {
}

unique_ptr<CreateInfo> CreateTableInfo::Copy() const {
	auto result = make_uniq<CreateTableInfo>(GetQualifiedName());
	CopyProperties(*result);
	result->columns = columns.Copy();
	for (auto &constraint : constraints) {
		result->constraints.push_back(constraint->Copy());
	}
	for (auto &partition : partition_keys) {
		result->partition_keys.push_back(partition->Copy());
	}
	for (auto &order : sort_keys) {
		result->sort_keys.push_back(order->Copy());
	}
	for (auto &option : options) {
		result->options.emplace(option.first, option.second->Copy());
	}
	if (query) {
		result->query = unique_ptr_cast<SQLStatement, SelectStatement>(query->Copy());
	}
	return std::move(result);
}

optional_idx CreateTableInfo::FindNotNullConstraint(LogicalIndex column) const {
	for (idx_t constraint_idx = 0; constraint_idx < constraints.size(); constraint_idx++) {
		auto &constraint = *constraints[constraint_idx];
		if (constraint.type == ConstraintType::NOT_NULL && constraint.Cast<NotNullConstraint>().index == column) {
			return constraint_idx;
		}
	}
	return optional_idx();
}

string CreateTableInfo::ExtraOptionsToString() const {
	string ret;
	if (!partition_keys.empty()) {
		ret += " PARTITIONED BY (";
		for (auto &partition : partition_keys) {
			ret += partition->ToString() + ",";
		}
		ret.pop_back();
		ret += ")";
	}
	if (!sort_keys.empty()) {
		ret += " SORTED BY (";
		for (auto &order : sort_keys) {
			ret += order->ToString() + ",";
		}
		ret.pop_back();
		ret += ")";
	}
	if (!options.empty()) {
		ret += " WITH (";
		for (auto &entry : options) {
			ret += SQLString(entry.first) + "=" + entry.second->ToString() + ",";
		}
		ret.pop_back();
		ret += ")";
	}
	return ret;
}

string CreateTableInfo::ToString() const {
	string ret = GetCreatePrefix("TABLE");
	ret += QualifiedNameToString();

	if (query != nullptr) {
		ret += TableCatalogEntry::ColumnNamesToSQL(columns);
		ret += ExtraOptionsToString();
		ret += " AS " + query->ToString();
	} else {
		ret += TableCatalogEntry::ColumnsToSQL(columns, constraints);
		ret += ExtraOptionsToString();
		ret += ";";
	}
	return ret;
}

} // namespace duckdb
