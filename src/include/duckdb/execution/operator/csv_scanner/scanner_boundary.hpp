//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/scanner_boundary.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_handle.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"

//! We all need boundaries every now and then, CSV Scans also need them
//! This class keeps track of what a scan should read, so which buffer and from where to where
//! As in real life, much like my childhood country, the rules are not really enforced.
//! So the end boundaries of a Scanner Boundary can and will be pushed.
//! In practice this means that a scanner is tolerated to read one line over it's end.
namespace duckdb {

//! Information stored in the buffer
struct CSVBoundary {
	CSVBoundary(idx_t buffer_idx, idx_t buffer_pos, idx_t boundary_idx, idx_t end_pos);
	CSVBoundary();
	void Print() const;
	//! Start Buffer index of the file where we start scanning
	idx_t buffer_idx = 0;
	//! Start Buffer position of the buffer of the file where we start scanning
	//! This position moves as we move through the buffer
	idx_t buffer_pos = 0;
	//! The boundary index relative to the total scan, only used for parallel reading to enforce
	//! Insertion Order
	idx_t boundary_idx = 0;
	//! Last position this iterator should read.
	idx_t end_pos;
};

struct CSVPosition {
	CSVPosition(idx_t buffer_idx, idx_t buffer_pos);
	CSVPosition();
	//! Start Buffer index of the file where we start scanning
	idx_t buffer_idx = 0;
	//! Start Buffer position of the buffer of the file where we start scanning
	//! This position moves as we move through the buffer
	idx_t buffer_pos = 0;
};
struct CSVIterator {
public:
	CSVIterator();

	void Print() const;
	//! Moves the boundary to the next one to be scanned, if there are no next boundaries, it returns False
	//! Otherwise, if there are boundaries, it returns True
	bool Next(CSVBufferManager &buffer_manager, const CSVReaderOptions &reader_options);
	//! If boundary is set
	bool IsBoundarySet() const;

	//! Getters
	idx_t GetEndPos() const;
	idx_t GetBufferIdx() const;
	idx_t GetBoundaryIdx() const;

	void SetCurrentPositionToBoundary();

	void SetCurrentBoundaryToPosition(bool single_threaded, const CSVReaderOptions &reader_options);

	void SetStart(idx_t pos);
	void SetEnd(idx_t pos);

	// Gets the current position for the file
	idx_t GetGlobalCurrentPos() const;

	//! Checks if we are done with this iterator
	void CheckIfDone();

	static idx_t BytesPerThread(const CSVReaderOptions &reader_options);

	static constexpr idx_t ROWS_PER_THREAD = 4;

	CSVPosition pos;

	bool done = false;

	bool first_one = true;

	idx_t buffer_size;

private:
	//! The original setting
	CSVBoundary boundary;
	//! Sometimes life knows no boundaries.
	//! The boundaries don't have to be set for single-threaded execution.
	bool is_set;
};
} // namespace duckdb
