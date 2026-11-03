//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/trampoline_branch_options.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

//! How a trampoline branch receives the rows routed to it
enum class TrampolineBranchInputMode : uint8_t {
	//! Each producing execution's routed rows arrive as their own wave
	DEFAULT,
	//! Round-wide admission against all rows ever routed; one admitted wave per round
	DISTINCT,
	//! The round's routed rows arrive as one bag wave, coalesced without admission
	MERGE
};

//! Options of one trampoline branch, fixed at parse time
struct TrampolineBranchOptions {
	TrampolineBranchInputMode input_mode = TrampolineBranchInputMode::DEFAULT;
	//! Every row routed to the branch is also emitted (BRANCH n TEE)
	bool tee = false;

	bool Distinct() const {
		return input_mode == TrampolineBranchInputMode::DISTINCT;
	}
	bool Merge() const {
		return input_mode == TrampolineBranchInputMode::MERGE;
	}
	//! The branch coalesces its round at the boundary (DISTINCT or MERGE)
	bool Coalescing() const {
		return input_mode != TrampolineBranchInputMode::DEFAULT;
	}

	bool operator==(const TrampolineBranchOptions &other) const {
		return input_mode == other.input_mode && tee == other.tee;
	}
	bool operator!=(const TrampolineBranchOptions &other) const {
		return !(*this == other);
	}

	void Serialize(Serializer &serializer) const;
	static TrampolineBranchOptions Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
