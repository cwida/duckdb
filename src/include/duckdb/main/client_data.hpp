//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"

namespace duckdb {

class AttachedDatabase;
class BufferedFileWriter;
class ClientContext;
class CatalogSearchPath;
class FileOpener;
class FileSystem;
class QueryProfiler;
class PreparedStatementData;
class RandomEngine;
class BufferManager;

struct ClientData {
public:
	explicit ClientData(ClientContext &context);
	~ClientData();

public:
	//! The query profiler of this client.
	shared_ptr<QueryProfiler> profiler;

	//! The set of temporary objects belonging to this client.
	shared_ptr<AttachedDatabase> temporary_objects;
	//! The set of bound prepared statements belonging to this client.
	identifier_map_t<shared_ptr<PreparedStatementData>> prepared_statements;

	//! The random generator used by random().
	//! Its seed value can be set by setseed().
	unique_ptr<RandomEngine> random_engine;

	//! The catalog search path.
	unique_ptr<CatalogSearchPath> catalog_search_path;
	//! The instance's default database as it was when this client connected; later DatabaseManager::SetDefaultDatabase
	//! calls only affect new clients. Empty when there was none.
	Identifier default_database;

	//! The file opener of the client context.
	unique_ptr<FileOpener> file_opener;
	//! The clients' file system wrapper.
	unique_ptr<FileSystem> client_file_system;
	//! The clients' buffer manager wrapper.
	unique_ptr<BufferManager> client_buffer_manager;

	//! The writer used to log queries, if logging is enabled.
	//! DEPRECATED: Now, queries are written to the generic log.
	unique_ptr<BufferedFileWriter> log_query_writer;

public:
	DUCKDB_API static ClientData &Get(ClientContext &context);
	DUCKDB_API static const ClientData &Get(const ClientContext &context);
};

} // namespace duckdb
