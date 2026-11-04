#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"

namespace duckdb {

SetColumnCommentInfo::SetColumnCommentInfo()
    : AlterInfo(AlterType::SET_COLUMN_COMMENT,
                QualifiedName(Identifier::InvalidCatalog(), Identifier::InvalidSchema(), ""),
                OnEntryNotFound::THROW_EXCEPTION),
      catalog_entry_type(CatalogType::INVALID), column_name(""), comment_value(Value()) {
}

SetColumnCommentInfo::SetColumnCommentInfo(QualifiedName qualified_name, Identifier column_name, Value comment_value,
                                           OnEntryNotFound if_not_found)
    : AlterInfo(AlterType::SET_COLUMN_COMMENT, std::move(qualified_name), if_not_found),
      catalog_entry_type(CatalogType::INVALID), column_name(std::move(column_name)),
      comment_value(std::move(comment_value)) {
}

SetColumnCommentInfo::SetColumnCommentInfo(Identifier catalog, Identifier schema, Identifier name,
                                           Identifier column_name, Value comment_value, OnEntryNotFound if_not_found)
    : SetColumnCommentInfo(QualifiedName(std::move(catalog), std::move(schema), std::move(name)),
                           std::move(column_name), std::move(comment_value), if_not_found) {
}

unique_ptr<AlterInfo> SetColumnCommentInfo::Copy() const {
	auto result = make_uniq<SetColumnCommentInfo>(GetQualifiedName(), column_name, comment_value, if_not_found);
	result->type = type;
	return std::move(result);
}

string SetColumnCommentInfo::ToString() const {
	string result = "";

	D_ASSERT(catalog_entry_type == CatalogType::INVALID);
	result += "COMMENT ON COLUMN ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += "." + SQLIdentifier(column_name);
	result += " IS ";
	result += comment_value.ToSQLString();
	result += ";";
	return result;
}

optional_ptr<CatalogEntry> SetColumnCommentInfo::TryResolveCatalogEntry(CatalogEntryRetriever &retriever) {
	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, GetQualifiedName());
	auto entry = retriever.GetEntry(lookup_info, if_not_found);

	if (entry) {
		catalog_entry_type = entry->type;
		return entry;
	}

	return nullptr;
}

// Note: this is a bit of a weird one: the exact type is not yet known: it can be either a view or a column this needs
//       to be resolved at bind time. If type is invalid here, the CommentOnColumnInfo was not properly resolved first
CatalogType SetColumnCommentInfo::GetCatalogType() const {
	if (catalog_entry_type == CatalogType::INVALID) {
		throw InternalException("Attempted to access unresolved ");
	}
	return catalog_entry_type;
}

} // namespace duckdb
