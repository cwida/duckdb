#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/default/default_views.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

class OpenFileDefaultGenerator : public DefaultGenerator {
public:
	OpenFileDefaultGenerator(Catalog &catalog, SchemaCatalogEntry &schema, const identifier_set_t &view_names_p,
	                         string file_p)
	    : DefaultGenerator(catalog), schema(schema), file(std::move(file_p)) {
		for (auto &view_name : view_names_p) {
			view_names.emplace_back(view_name);
		}
	}

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override {
		for (auto &entry : view_names) {
			if (entry_name == entry) {
				auto result = make_uniq<CreateViewInfo>();
				result->SetQualifiedName(QualifiedName({Identifier::DefaultSchema()}, entry));
				result->sql = StringUtil::Format("SELECT * FROM %s", SQLString(file));
				auto parser = Parser::GetBuiltinParser();
				auto view_info = CreateViewInfo::FromSelect(parser, std::move(result));
				return make_uniq_base<CatalogEntry, ViewCatalogEntry>(catalog, schema, *view_info);
			}
		}
		return nullptr;
	}

	vector<Identifier> GetDefaultEntries() override {
		return view_names;
	}

private:
	SchemaCatalogEntry &schema;
	vector<Identifier> view_names;
	string file;
};

unique_ptr<Catalog> OpenFileStorageAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &attach_options) {
	auto file = info.path;
	// open an in-memory database
	info.path = ":memory:";
	auto catalog = make_uniq<DuckCatalog>(db);
	catalog->Initialize(false);

	identifier_set_t view_names;
	view_names.insert("file");
	view_names.insert(Identifier(name));

	// set up the default view generator for "file" and the derived name of the file
	auto system_transaction = CatalogTransaction::GetSystemTransaction(db.GetDatabase());
	auto &schema = catalog->GetSchema(system_transaction, Identifier::DefaultSchema());
	auto &duck_schema = schema.Cast<DuckSchemaEntry>();
	auto &catalog_set = duck_schema.GetCatalogSet(CatalogType::VIEW_ENTRY);
	auto default_generator = make_uniq<OpenFileDefaultGenerator>(*catalog, schema, view_names, std::move(file));
	catalog_set.SetDefaultGenerator(std::move(default_generator));

	return std::move(catalog);
}

unique_ptr<TransactionManager> OpenFileStorageTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                 AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<DuckTransactionManager>(db);
}

shared_ptr<StorageExtension> OpenFileStorageExtension::Create() {
	auto result = make_shared_ptr<StorageExtension>();
	result->attach = OpenFileStorageAttach;
	result->create_transaction_manager = OpenFileStorageTransactionManager;
	return result;
}

} // namespace duckdb
