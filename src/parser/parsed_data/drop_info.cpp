#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

DropInfo::DropInfo() : ParseInfo(TYPE), cascade(false) {
}

DropInfo::DropInfo(const DropInfo &info)
    : ParseInfo(info.info_type), type(info.type), if_not_found(info.if_not_found), cascade(info.cascade),
      allow_drop_internal(info.allow_drop_internal),
      extra_drop_info(info.extra_drop_info ? info.extra_drop_info->Copy() : nullptr),
      qualified_name(info.qualified_name) {
}

DatabaseModificationType DropInfo::GetModifications() const {
	if (type == CatalogType::TABLE_ENTRY || type == CatalogType::INDEX_ENTRY || cascade) {
		return DatabaseModificationType::DROP_CATALOG_ENTRY;
	}
	// a schema is only dropped without CASCADE when it is empty
	return DatabaseModificationType();
}

unique_ptr<DropInfo> DropInfo::Copy() const {
	return make_uniq<DropInfo>(*this);
}

string DropInfo::ToString() const {
	string result = "";
	if (type == CatalogType::PREPARED_STATEMENT) {
		result += "DEALLOCATE PREPARE ";
		result += SQLIdentifier(GetQualifiedName().Name());
	} else {
		result += "DROP";
		result += " " + ParseInfo::TypeToString(type);
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			result += " IF EXISTS";
		}
		result += " ";
		result += qualified_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
		if (type == CatalogType::TRIGGER_ENTRY && extra_drop_info) {
			auto &trigger_info = extra_drop_info->Cast<ExtraDropTriggerInfo>();
			if (trigger_info.base_table) {
				result += " ON ";
				result += trigger_info.base_table->Cast<BaseTableRef>().ToString();
			}
		}
		if (type == CatalogType::SECRET_ENTRY && extra_drop_info) {
			// Preserve the `FROM <storage>` storage specifier so `DROP SECRET <name> FROM <storage>` round-trips.
			auto &secret_info = extra_drop_info->Cast<ExtraDropSecretInfo>();
			if (!secret_info.secret_storage.empty()) {
				result += " FROM " + SQLIdentifier(secret_info.secret_storage);
			}
		}
		if (cascade) {
			result += " CASCADE";
		}
	}
	result += ";";
	return result;
}

} // namespace duckdb
