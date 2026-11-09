//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/checkpoint_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/checkpoint_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

struct CheckpointOptions {
	CheckpointOptions()
	    : wal_action(CheckpointWALAction::DONT_DELETE_WAL), action(CheckpointAction::CHECKPOINT_IF_REQUIRED),
	      type(CheckpointType::FULL_CHECKPOINT), visibility_bound(VisibilityBound::IncludingUncommitted()) {
	}

	CheckpointWALAction wal_action;
	CheckpointAction action;
	CheckpointType type;
	//! Identifies this checkpoint. Compared for equality only; empty when no checkpoint is running
	optional_idx checkpoint_id;
	//! What this checkpoint sees: timestamps below this bound are written
	VisibilityBound visibility_bound;
	//! The commit lock, when the committing transaction holds it for the whole checkpoint (checkpoint instead of WAL)
	optional_ptr<unique_lock<mutex>> commit_lock;
};

} // namespace duckdb
