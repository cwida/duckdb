//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_byte_skipper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/swar.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine_cache.hpp"

namespace duckdb {

//! Skips the content bytes of a buffer through a stop mask computed for one block of 64 bytes at a time
class CSVByteSkipper {
public:
	//! Where a skip ended
	enum class SkippedTo : uint8_t {
		CANDIDATE_FOUND,     //! Stop candidate found
		CANDIDATE_NOT_FOUND, //! No stop candidate found in the whole range
		TAIL                 //! If we have fewer than 64 bytes, the caller needs to check byte by byte
	};

	//! Takes the byte patterns that end a skip, a byte matches a pattern on the bits set in its mask
	explicit CSVByteSkipper(vector<SwarBlock::BytePattern> stop_patterns_p)
	    : stop_patterns(std::move(stop_patterns_p)) {
		if (stop_patterns.size() < SwarBlock::MIN_PATTERNS || stop_patterns.size() > SwarBlock::MAX_PATTERNS) {
			throw InternalException("CSVByteSkipper takes %d to %d byte patterns", SwarBlock::MIN_PATTERNS,
			                        SwarBlock::MAX_PATTERNS);
		}
	}

	//! Points the skipper at a buffer, dropping the block of a previous one
	void SetBuffer(const CSVBufferHandle &buffer_handle) {
		buffer = const_data_ptr_cast(buffer_handle.Ptr());
		buffer_size = buffer_handle.actual_size;
		block_start = 0;
		block_end = 0;
	}

	//! Skips bytes that match no stop pattern, `pos` ends on a candidate stop, on `limit`, or in the buffer tail
	inline SkippedTo SkipToCandidate(const idx_t limit, idx_t &pos) {
		while (pos < limit) {
			if (pos < block_start || pos >= block_end) {
				if (pos + SwarBlock::SIZE > buffer_size) {
					return SkippedTo::TAIL;
				}
				MaskBlock(pos);
			}
			const uint64_t candidates = stop_mask >> (pos - block_start);
			if (candidates) {
				const idx_t candidate = pos + CountZeros<uint64_t>::Trailing(candidates);
				if (candidate < limit) {
					pos = candidate;
					return SkippedTo::CANDIDATE_FOUND;
				}
			}
			pos = MinValue<idx_t>(candidates ? limit : block_end, limit);
		}
		return SkippedTo::CANDIDATE_NOT_FOUND;
	}

	//! Skips every byte `skip_table` skips, `pos` ends on a stop or on the byte before `to_pos`
	inline void SkipToStop(const bool (&skip_table)[StateMachine::NUM_TRANSITIONS], const idx_t to_pos, idx_t &pos) {
		while (pos + 1 < to_pos) {
			const auto skipped_to = SkipToCandidate(to_pos - 1, pos);
			if (skipped_to == SkippedTo::TAIL) {
				while (skip_table[ByteAt(pos)] && pos + 1 < to_pos) {
					pos++;
				}
				return;
			}
			if (skipped_to == SkippedTo::CANDIDATE_NOT_FOUND || !skip_table[ByteAt(pos)]) {
				return;
			}
			// a candidate the skip table skips after all
			pos++;
		}
	}

private:
	uint8_t ByteAt(const idx_t pos) const {
		return buffer[pos];
	}

	//! Computes the stop mask of the 64 bytes that start at `from`
	void MaskBlock(const idx_t from) {
		stop_mask = SwarBlock::MaybeAnyMask(buffer + from, stop_patterns);
		block_start = from;
		block_end = from + SwarBlock::SIZE;
	}

	//! The byte patterns that end a skip
	const vector<SwarBlock::BytePattern> stop_patterns;
	//! The bytes of the buffer and their count, the block address must not depend on a load chain
	const_data_ptr_t buffer = nullptr;
	idx_t buffer_size = 0;
	//! The masked block [block_start, block_end) and its stop mask, bit i is byte block_start + i
	idx_t block_start = 0;
	idx_t block_end = 0;
	uint64_t stop_mask = 0;
};

} // namespace duckdb
