#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/original/std/sstream.hpp"

namespace duckdb {

SchemaCatalogEntry::SchemaCatalogEntry(Catalog &catalog, CreateSchemaInfo &info,
                                       optional_ptr<SchemaCatalogEntry> parent_schema_p)
    : InCatalogEntry(CatalogType::SCHEMA_ENTRY, catalog, info.SchemaName()), parent_schema(parent_schema_p) {
	this->internal = info.internal;
	this->comment = info.comment;
	this->tags = info.tags;
}

CatalogTransaction SchemaCatalogEntry::GetCatalogTransaction(ClientContext &context) {
	return CatalogTransaction(catalog, context);
}

optional_ptr<CatalogEntry> SchemaCatalogEntry::CreateIndex(ClientContext &context, CreateIndexInfo &info,
                                                           TableCatalogEntry &table) {
	return CreateIndex(GetCatalogTransaction(context), info, table);
}

SimilarCatalogEntry SchemaCatalogEntry::GetSimilarEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	SimilarCatalogEntry result;
	Scan(transaction, lookup_info.GetCatalogType(), [&](CatalogEntry &entry) {
		auto entry_score = StringUtil::SimilarityRating(entry.name.GetIdentifierName(), lookup_info.GetEntryName());
		if (entry_score > result.score) {
			result.score = entry_score;
			result.name = Identifier(entry.name.GetIdentifierName());
		}
	});
	return result;
}

optional_ptr<CatalogEntry> SchemaCatalogEntry::GetEntry(CatalogTransaction transaction, CatalogType type,
                                                        const Identifier &name) {
	EntryLookupInfo lookup_info(type, QualifiedName(name));
	return LookupEntry(transaction, lookup_info);
}

//! This should not be used, it's only implemented to not put the burden of implementing it on every derived class of
//! SchemaCatalogEntry
CatalogSet::EntryLookup SchemaCatalogEntry::LookupEntryDetailed(CatalogTransaction transaction,
                                                                const EntryLookupInfo &lookup_info) {
	CatalogSet::EntryLookup result;
	result.result = LookupEntry(transaction, lookup_info);
	if (!result.result) {
		result.reason = CatalogSet::EntryLookup::FailureReason::DELETED;
	} else {
		result.reason = CatalogSet::EntryLookup::FailureReason::SUCCESS;
	}
	return result;
}

vector<Identifier> SchemaCatalogEntry::GetSchemaPath() const {
	vector<Identifier> path;
	optional_ptr<const SchemaCatalogEntry> schema = this;
	while (schema) {
		path.push_back(schema->name);
		schema = schema->GetParentSchema().get();
	}
	std::reverse(path.begin(), path.end());
	return path;
}

QualifiedName SchemaCatalogEntry::GetQualifiedName(const Identifier &entry_name) const {
	return QualifiedName::FromCatalogSchema(catalog.GetName(), GetSchemaPath(), entry_name);
}

string SchemaCatalogEntry::GetSchemaName() const {
	return QualifiedName::FromPath(GetSchemaPath()).ToString();
}

void SchemaCatalogEntry::Scan(CatalogTransaction transaction, CatalogType type,
                              const std::function<void(CatalogEntry &)> &callback) {
	Scan(transaction.GetContext(), type, callback);
}

template <class SCAN>
static void ScanSchemaTreeInternal(SchemaCatalogEntry &root, SCAN scan,
                                   const std::function<void(SchemaCatalogEntry &)> &callback) {
	vector<reference<SchemaCatalogEntry>> pending;
	pending.emplace_back(root);
	while (!pending.empty()) {
		auto &schema = pending.back().get();
		pending.pop_back();
		callback(schema);
		auto child_start = pending.size();
		scan(schema, [&](CatalogEntry &entry) { pending.emplace_back(entry.Cast<SchemaCatalogEntry>()); });
		std::reverse(pending.begin() + NumericCast<int64_t>(child_start), pending.end());
	}
}

void SchemaCatalogEntry::ScanSchemaTree(CatalogTransaction transaction,
                                        const std::function<void(SchemaCatalogEntry &)> &callback) {
	ScanSchemaTreeInternal(
	    *this,
	    [&](SchemaCatalogEntry &schema, const std::function<void(CatalogEntry &)> &visit) {
		    schema.Scan(transaction, CatalogType::SCHEMA_ENTRY, visit);
	    },
	    callback);
}

void SchemaCatalogEntry::ScanSchemaTree(const std::function<void(SchemaCatalogEntry &)> &callback) {
	ScanSchemaTreeInternal(
	    *this,
	    [](SchemaCatalogEntry &schema, const std::function<void(CatalogEntry &)> &visit) {
		    schema.Scan(CatalogType::SCHEMA_ENTRY, visit);
	    },
	    callback);
}

unique_ptr<CreateInfo> SchemaCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateSchemaInfo>();
	result->SetQualifiedName(GetParentSchema() ? GetQualifiedName(Identifier()) : QualifiedName({name}, Identifier()));
	result->comment = comment;
	result->tags = tags;
	return std::move(result);
}

string SchemaCatalogEntry::ToSQL() const {
	auto create_schema_info = GetInfo();
	create_schema_info->StripCatalogQualification();
	return create_schema_info->ToString();
}

} // namespace duckdb
