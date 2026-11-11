#include "duckdb/execution/operator/join/outer_join_marker.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

ProgressData OuterJoinGlobalScanState::GetProgress() const {
	const auto total = data->Count();
	if (total == 0) {
		return ProgressData {1.0, 1.0, false};
	}
	return ProgressData {double(rows_scanned.load()), double(total), false};
}

void OuterJoinLocalScanState::Reset() {
	scan_chunk.Reset();
	local_scan = ColumnDataLocalScanState();
}

OuterJoinMarker::OuterJoinMarker(bool enabled_p) : enabled(enabled_p), count(0) {
}

void OuterJoinMarker::Initialize(idx_t count_p) {
	if (!enabled) {
		return;
	}
	this->count = count_p;
	found_match = make_unsafe_uniq_array_uninitialized<bool>(count);
	Reset();
}

void OuterJoinMarker::Reset() {
	if (!enabled) {
		return;
	}
	if (count == 0) {
		return;
	}
	memset(found_match.get(), 0, sizeof(bool) * count);
}

void OuterJoinMarker::SetMatch(idx_t position) {
	if (!enabled) {
		return;
	}
	D_ASSERT(position < count);
	found_match[position] = true;
}

void OuterJoinMarker::SetMatches(const SelectionVector &sel, idx_t count, idx_t base_idx) {
	if (!enabled) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto pos = base_idx + idx;
		D_ASSERT(pos < this->count);
		found_match[pos] = true;
	}
}

void OuterJoinMarker::ConstructLeftJoinResult(DataChunk &left, DataChunk &result) {
	if (!enabled) {
		return;
	}
	D_ASSERT(count == STANDARD_VECTOR_SIZE);
	SelectionVector remaining_sel(STANDARD_VECTOR_SIZE);
	auto true_count = SelectionVector::SelectFromBoolean(
	    found_match.get(), nullptr, left.size(), {}, array_ptr<sel_t>(remaining_sel.data(), remaining_sel.Capacity()));
	auto remaining_count = left.size() - true_count;
	if (remaining_count > 0) {
		result.Slice(left, remaining_sel, remaining_count);
		for (idx_t idx = left.ColumnCount(); idx < result.ColumnCount(); idx++) {
			ConstantVector::SetNull(result.data[idx], count_t(remaining_count));
		}
	}
}

idx_t OuterJoinMarker::MaxThreads() const {
	return count / (STANDARD_VECTOR_SIZE * 10ULL);
}

void OuterJoinMarker::InitializeScan(ColumnDataCollection &data, OuterJoinGlobalScanState &gstate) {
	gstate.data = &data;
	gstate.rows_scanned = 0;
	data.InitializeScan(gstate.global_scan);
}

void OuterJoinMarker::InitializeScan(OuterJoinGlobalScanState &gstate, OuterJoinLocalScanState &lstate) {
	D_ASSERT(gstate.data);
	lstate.match_sel.Initialize(STANDARD_VECTOR_SIZE);
	gstate.data->InitializeScanChunk(lstate.scan_chunk);
}

void OuterJoinMarker::Scan(OuterJoinGlobalScanState &gstate, OuterJoinLocalScanState &lstate, DataChunk &result) {
	D_ASSERT(gstate.data);
	// fill in NULL values for the LHS
	while (gstate.data->Scan(gstate.global_scan, lstate.local_scan, lstate.scan_chunk)) {
		idx_t result_count = 0;
		// figure out which tuples didn't find a match in the RHS
		for (idx_t i = 0; i < lstate.scan_chunk.size(); i++) {
			if (!found_match[lstate.local_scan.current_row_index + i]) {
				lstate.match_sel.set_index(result_count++, i);
			}
		}
		if (result_count > 0) {
			// if there were any tuples that didn't find a match, output them
			idx_t left_column_count = result.ColumnCount() - lstate.scan_chunk.ColumnCount();
			for (idx_t i = 0; i < left_column_count; i++) {
				ConstantVector::SetNull(result.data[i], count_t(result_count));
			}
			result.Slice(lstate.scan_chunk, lstate.match_sel, result_count, left_column_count);
		}
		gstate.rows_scanned.fetch_add(lstate.scan_chunk.size(), std::memory_order_relaxed);
		if (result_count > 0) {
			return;
		}
	}
}

} // namespace duckdb
