#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_load.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include <algorithm>

namespace duckdb {

BoundStatement Binder::Bind(LoadStatement &stmt) {
	BoundStatement result;
	result.types = LoadInfo::GetResultTypes(stmt.info->load_type);
	result.names = LoadInfo::GetResultNames(stmt.info->load_type);

	// Ensure the repository exists if it's an alias
	if (!stmt.info->repository.empty() && stmt.info->repo_is_alias) {
		auto &db = DatabaseInstance::GetDatabase(context);
		auto &fs = FileSystem::GetLocal(db);
		ExtensionRepository repository;
		auto repository_url = ExtensionRepository::TryGetRepositoryUrl(stmt.info->repository);
		if (repository_url.empty() &&
		    !ExtensionRepositoryManager::TryGetRepository(db, fs, stmt.info->repository, repository)) {
			throw BinderException("'%s' is not a known repository name. Are you trying to query from a repository by "
			                      "path? Use single quotes: `FROM '%s'`",
			                      stmt.info->repository, stmt.info->repository);
		}
	}

	auto load_type = stmt.info->load_type;
	result.plan = make_uniq<LogicalLoad>(std::move(stmt.info));

	auto &properties = GetStatementProperties();
	properties.return_type =
	    load_type == LoadType::CREATE_REPOSITORY ? StatementReturnType::QUERY_RESULT : StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
