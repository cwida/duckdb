#include "duckdb/main/client_data.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {

ClientData::ClientData(ClientContext &context) : catalog_search_path(make_uniq<CatalogSearchPath>(context)) {
	auto &db = DatabaseInstance::GetDatabase(context);

	default_database = DatabaseManager::Get(db).GetDefaultDatabase();
	temporary_objects = make_shared_ptr<AttachedDatabase>(db, AttachedDatabaseType::TEMP_DATABASE);
	temporary_objects->oid = DatabaseManager::Get(db).NextOid();
	random_engine = make_uniq<RandomEngine>();

	temporary_objects->Initialize();
}

ClientData::~ClientData() {
	// This needs to be destroyed first because PreparedStatementData holds a PhysicalPlan,
	// and the PhysicalPlan holds objects that might rely on the client file opener/file system/buffer manager
	prepared_statements.clear();
}

ClientData &ClientData::Get(ClientContext &context) {
	return *context.client_data;
}

const ClientData &ClientData::Get(const ClientContext &context) {
	return *context.client_data;
}

RandomEngine &RandomEngine::Get(ClientContext &context) {
	return *ClientData::Get(context).random_engine;
}

} // namespace duckdb
