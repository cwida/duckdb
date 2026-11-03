//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/block_packable.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

class BlockHandle;

//! Interface for objects whose data can be packed into partial blocks.
class BlockPackable {
public:
	virtual ~BlockPackable() = default;

public:
	//! Install the packed buffer and byte offset; the handle remains temporary for in-memory checkpoints.
	//! Called when the partial block is flushed.
	virtual void Finalize(shared_ptr<BlockHandle> block_handle, uint32_t offset) = 0;
	//! Read the copy in the packed buffer from now on, so the packable's own buffer can be freed.
	virtual void UsePackedCopy(const shared_ptr<BlockHandle> &block_handle, uint32_t offset) {
	}
};

} // namespace duckdb
