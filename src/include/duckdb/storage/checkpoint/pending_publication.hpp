//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/pending_publication.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class PendingPublication {
public:
	virtual ~PendingPublication() = default;

	//! Publish prepared state after its backing blocks have been flushed.
	virtual void Publish() = 0;
};

} // namespace duckdb
