//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/base_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/scanner_boundary.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_byte_skipper.hpp"

namespace duckdb {

class CSVFileScan;

//! Class that keeps track of line starts, used for line size verification
class LinePosition {
public:
	LinePosition() {
	}
	LinePosition(idx_t buffer_idx_p, idx_t buffer_pos_p, idx_t buffer_size_p)
	    : buffer_pos(buffer_pos_p), buffer_size(buffer_size_p), buffer_idx(buffer_idx_p) {
	}

	idx_t operator-(const LinePosition &other) const {
		if (other.buffer_idx == buffer_idx) {
			return buffer_pos - other.buffer_pos;
		}
		return other.buffer_size - other.buffer_pos + buffer_pos;
	}

	bool operator==(const LinePosition &other) const {
		return buffer_pos == other.buffer_pos && buffer_idx == other.buffer_idx && buffer_size == other.buffer_size;
	}

	idx_t GetGlobalPosition(idx_t requested_buffer_size, bool first_char_nl = false) const {
		return requested_buffer_size * buffer_idx + buffer_pos + first_char_nl;
	}
	idx_t buffer_pos = 0;
	idx_t buffer_size = 0;
	idx_t buffer_idx = 0;
};

class ScannerResult {
public:
	ScannerResult(CSVStates &states, CSVStateMachine &state_machine, idx_t result_size);

	static inline void SetQuoted(ScannerResult &result, idx_t quoted_position) {
		if (!result.quoted) {
			result.quoted_position = quoted_position;
		}
		result.quoted = true;
		result.unquoted = true;
	}

	static inline void SetUnquoted(ScannerResult &result) {
		if (result.states.states[0] == CSVState::UNQUOTED && result.states.states[1] == CSVState::UNQUOTED &&
		    result.state_machine.dialect_options.state_machine_options.escape != '\0') {
			// This means we touched an unescaped quote, we must go through the remove escape code to remove it.
			result.escaped = true;
		}
		result.quoted = true;
	}

	static inline void SetEscaped(ScannerResult &result) {
		result.escaped = true;
	}
	static inline void SetComment(ScannerResult &result, idx_t buffer_pos) {
		result.comment = true;
	}
	static inline bool UnsetComment(ScannerResult &result, idx_t buffer_pos) {
		result.comment = false;
		return false;
	}
	static inline bool IsCommentSet(const ScannerResult &result) {
		return result.comment == true;
	}

	inline bool IsStateCurrent(CSVState state) const {
		return states.states[1] == state;
	}

	//! Variable to keep information regarding quoted and escaped values
	bool quoted = false;
	//! If the current quoted value is unquoted
	bool unquoted = false;
	//! If the current value has been escaped
	bool escaped = false;
	//! Variable to keep track if we are in a comment row. Hence, won't add it
	bool comment = false;
	idx_t quoted_position = 0;

	LinePosition last_position;

	//! Size of the result
	const idx_t result_size;

	CSVStateMachine &state_machine;
	bool cur_line_starts_as_comment = false;

	void Print() const {
		state_machine.Print();
	}

protected:
	CSVStates &states;
};

//! This is the base of our CSV scanners.
//! Scanners differ on what they are used for, and consequently have different performance benefits.
class BaseScanner {
public:
	explicit BaseScanner(shared_ptr<CSVBufferManager> buffer_manager, shared_ptr<CSVStateMachine> state_machine,
	                     shared_ptr<CSVErrorHandler> error_handler, bool sniffing = false,
	                     shared_ptr<CSVFileScan> csv_file_scan = nullptr, const CSVIterator &iterator = {});

	virtual ~BaseScanner() = default;

	void Print() const;

	//! Returns true if the scanner is finished
	bool FinishedFile() const;

	//! Parses data into an output_chunk
	virtual ScannerResult &ParseChunk();

	//! Returns the result from the last Parse call. Shouts at you if you call it wrong
	virtual ScannerResult &GetResult();

	CSVIterator &GetIterator();

	void SetIterator(const CSVIterator &it);

	idx_t GetBoundaryIndex() const {
		return iterator.GetBoundaryIdx();
	}

	idx_t GetLinesRead() const {
		return lines_read;
	}

	CSVPosition GetIteratorPosition() const {
		return iterator.pos;
	}

	CSVStateMachine &GetStateMachine() const;

	//! Removes thousands separator
	static string RemoveSeparator(const char *value_ptr, const idx_t size, char thousands_separator);

	shared_ptr<CSVFileScan> csv_file_scan;

	//! If this scanner is being used for sniffing
	bool sniffing = false;
	//! The guy that handles errors
	shared_ptr<CSVErrorHandler> error_handler;

	//! Shared pointer to the state machine, this is used across multiple scanners
	shared_ptr<CSVStateMachine> state_machine;

	//! States
	CSVStates states;

	//! If the scanner ever entered a quoted state
	bool ever_quoted = false;

	//! If the scanner ever entered an escaped state.
	bool ever_escaped = false;

	//! If the scanner ever used advantage of the non-strict mode.
	bool used_unstrictness = false;

	//! Shared pointer to the buffer_manager, this is shared across multiple scanners
	shared_ptr<CSVBufferManager> buffer_manager;

	//! Skips Notes and/or parts of the data, starting from the top.
	//! notes are dirty lines on top of the file, before the actual data
	static CSVIterator SkipCSVRows(shared_ptr<CSVBufferManager> buffer_manager,
	                               const shared_ptr<CSVStateMachine> &state_machine, idx_t rows_to_skip);

protected:
	//! Boundaries of this scanner
	CSVIterator iterator;

	//! Unique pointer to the buffer_handle, this is unique per scanner, since it also contains the necessary counters
	//! To offload buffers to disk if necessary
	shared_ptr<CSVBufferHandle> cur_buffer_handle;

	//! Hold the current buffer ptr
	const char *buffer_handle_ptr = nullptr;

	//! If this scanner has been initialized
	bool initialized = false;
	//! How many lines were read by this scanner
	idx_t lines_read = 0;
	idx_t bytes_read = 0;
	//! Internal Functions used to perform the parsing
	//! Initializes the scanner
	virtual void Initialize();

	//! Skips the content bytes of the current buffer, mutable because the line finder is const
	mutable CSVByteSkipper skipper;

	//! Whether ProcessBlocks may drive this state machine, and the bytes it stops at
	const bool block_scan;
	const uint8_t block_delimiter;
	const uint8_t block_quote;
	const bool block_crlf;
	const bool block_escape_is_quote;

	//! Whether the state machine can hand the current byte to ProcessBlocks
	template <class T>
	bool BlockScanPossible(const T &result) const {
		if (result.quoted || result.escaped || result.comment) {
			return false;
		}
		switch (states.states[1]) {
		case CSVState::STANDARD:
		case CSVState::DELIMITER:
		case CSVState::RECORD_SEPARATOR:
		case CSVState::EMPTY_SPACE:
		case CSVState::NOT_SET:
			return true;
		default:
			return false;
		}
	}

	static bool IsSeparatorState(const CSVState state) {
		return state == CSVState::DELIMITER || state == CSVState::RECORD_SEPARATOR ||
		       state == CSVState::CARRIAGE_RETURN || state == CSVState::NOT_SET;
	}

	//! The state after the data bytes [from, to) given the state `before` them, as the eligible tables have it
	CSVState StateAfterData(const CSVState before, const idx_t from, const idx_t to) const {
		if (from == to) {
			return before;
		}
		if (to - from == 1 && buffer_handle_ptr[from] == ' ' && IsSeparatorState(before)) {
			return CSVState::EMPTY_SPACE;
		}
		return CSVState::STANDARD;
	}

	//! Moves `states` from the pair before byte `last` to the pair before byte `at`, over data bytes
	void SetStatesBefore(const idx_t at, const idx_t last) {
		const auto before = states.states[1];
		const auto previous = at == last ? states.states[0] : StateAfterData(before, last, at - 1);
		D_ASSERT(ReplayStates(last, at).states[1] == StateAfterData(before, last, at));
		D_ASSERT(ReplayStates(last, at).states[0] == previous);
		states.states[1] = StateAfterData(before, last, at);
		states.states[0] = previous;
	}

	//! The pair the state machine reaches from `states` over the bytes [from, to), for the block scan assertions
	CSVStates ReplayStates(const idx_t from, const idx_t to) const {
		CSVStates replay = states;
		for (idx_t i = from; i < to; i++) {
			state_machine->Transition(replay, buffer_handle_ptr[i]);
		}
		return replay;
	}

	//! Moves `states` from the pair before byte `last` to the pair before byte `at`, over quoted data bytes
	void SetQuotedStatesBefore(const idx_t at, const idx_t last) {
		const auto previous = at == last ? states.states[0] : at == last + 1 ? states.states[1] : CSVState::QUOTED;
		D_ASSERT(ReplayStates(last, at).states[0] == previous);
		D_ASSERT(ReplayStates(last, at).states[1] == (at == last ? states.states[1] : CSVState::QUOTED));
		states.states[1] = at == last ? states.states[1] : CSVState::QUOTED;
		states.states[0] = previous;
	}

	//! Leaves the block scan inside a quoted value at `at`
	bool HandOverQuoted(const idx_t at, const idx_t last) {
		SetQuotedStatesBefore(at, last);
		iterator.pos.buffer_pos = at;
		return false;
	}

	//! The bytes that end a row at `at`, 2 for a CRLF pair that fits before `limit`, 1 for a line feed, else 0
	idx_t RowEndBytes(const uint8_t byte, const idx_t at, const idx_t limit) const {
		if (block_crlf) {
			return byte == '\r' && at + 1 < limit && buffer_handle_ptr[at + 1] == '\n' ? 2 : 0;
		}
		return byte == '\n' ? 1 : 0;
	}

	//! The delimiter at `at`, `before` is the state before it
	template <class T>
	void BlockDelimiter(T &result, const idx_t at, const CSVState before) {
		states.states[0] = before;
		states.states[1] = CSVState::DELIMITER;
		iterator.pos.buffer_pos = at;
		T::AddValue(result, at);
	}

	//! The row end at `at`, `before` is the state before it. Returns true when the result is full
	template <class T>
	bool BlockRowEnd(T &result, const idx_t at, const CSVState before, const bool crlf, uint64_t &candidates,
	                 const idx_t pos) {
		states.states[0] = before;
		states.states[1] = crlf ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
		iterator.pos.buffer_pos = at;
		const bool full = T::AddRow(result, at);
		lines_read++;
		if (full) {
			iterator.pos.buffer_pos = at + 1;
			return true;
		}
		if (crlf) {
			states.states[0] = CSVState::CARRIAGE_RETURN;
			states.states[1] = CSVState::RECORD_SEPARATOR;
			candidates &= ~(uint64_t(1) << (at + 1 - pos));
		}
		return false;
	}

	//! Scans unquoted fields and quoted values in their canonical form, and preserves the states when handing back
	//! to Process. Returns true when the result is full. Repeated SetQuoted calls may be skipped because the setter
	//! is idempotent.
	template <class T>
	bool ProcessBlocks(T &result, const idx_t to_pos) {
		const auto buffer = buffer_handle_ptr;
		// the first byte not consumed, and the first byte after the last structural or special byte
		idx_t pos = iterator.pos.buffer_pos;
		idx_t last = pos;
		bool quoted = false;
		uint64_t candidates;
		idx_t block_end;
		while (pos < to_pos && skipper.BlockCandidates(pos, candidates, block_end)) {
			const idx_t limit = MinValue<idx_t>(block_end, to_pos);
			while (candidates) {
				const idx_t at = pos + CountZeros<uint64_t>::Trailing(candidates);
				if (at >= limit) {
					break;
				}
				candidates &= candidates - 1;
				const auto byte = static_cast<uint8_t>(buffer[at]);
				if (quoted) {
					if (byte == '\n' || byte == '\r') {
						// QUOTED and QUOTED_NEW_LINE alternate on newline bytes inside the quotes
						SetQuotedStatesBefore(at, last);
						const bool new_line_state = states.states[1] == CSVState::QUOTED;
						states.states[0] = states.states[1];
						states.states[1] = new_line_state ? CSVState::QUOTED_NEW_LINE : CSVState::QUOTED;
						iterator.pos.buffer_pos = at;
						if (new_line_state) {
							T::QuotedNewLine(result);
						} else {
							ever_quoted = true;
							T::SetQuoted(result, at);
						}
						last = at + 1;
						continue;
					}
					if (byte != block_quote) {
						continue;
					}
					if (at + 1 >= limit) {
						return HandOverQuoted(at, last);
					}
					const auto next = static_cast<uint8_t>(buffer[at + 1]);
					const bool escape_pair = block_escape_is_quote && next == block_quote;
					const idx_t row_end_bytes = RowEndBytes(next, at + 1, limit);
					if (!escape_pair && next != block_delimiter && row_end_bytes == 0) {
						return HandOverQuoted(at, last);
					}
					SetQuotedStatesBefore(at, last);
					states.states[0] = states.states[1];
					states.states[1] = CSVState::UNQUOTED;
					iterator.pos.buffer_pos = at;
					T::SetUnquoted(result);
					candidates &= ~(uint64_t(1) << (at + 1 - pos));
					if (escape_pair) {
						states.states[0] = CSVState::UNQUOTED;
						states.states[1] = CSVState::QUOTED;
						iterator.pos.buffer_pos = at + 1;
						ever_escaped = true;
						T::SetEscaped(result);
						ever_quoted = true;
						T::SetQuoted(result, at + 1);
						last = at + 2;
						continue;
					}
					quoted = false;
					if (next == block_delimiter) {
						BlockDelimiter(result, at + 1, CSVState::UNQUOTED);
						last = at + 2;
						continue;
					}
					if (BlockRowEnd(result, at + 1, CSVState::UNQUOTED, row_end_bytes == 2, candidates, pos)) {
						return true;
					}
					last = at + 1 + row_end_bytes;
					continue;
				}
				const auto before = StateAfterData(states.states[1], last, at);
				if (byte == block_delimiter) {
					D_ASSERT(ReplayStates(last, at + 1).states[1] == CSVState::DELIMITER);
					D_ASSERT(ReplayStates(last, at + 1).states[0] == before);
					BlockDelimiter(result, at, before);
					last = at + 1;
					continue;
				}
				if (block_quote != '\0' && byte == block_quote) {
					if (before == CSVState::STANDARD) {
						// a quote inside an unquoted value is data
						continue;
					}
					D_ASSERT(ReplayStates(last, at + 1).states[1] == CSVState::QUOTED);
					states.states[0] = before;
					states.states[1] = CSVState::QUOTED;
					iterator.pos.buffer_pos = at;
					ever_quoted = true;
					T::SetQuoted(result, at);
					quoted = true;
					last = at + 1;
					continue;
				}
				const idx_t row_end_bytes = RowEndBytes(byte, at, limit);
				if (row_end_bytes == 0) {
					if (byte == '\n' || byte == '\r') {
						return HandOver(at, last);
					}
					// a false candidate of the mask, data
					continue;
				}
				if (before == CSVState::RECORD_SEPARATOR || before == CSVState::NOT_SET) {
					// an empty line
					return HandOver(at, last);
				}
				D_ASSERT(ReplayStates(last, at + 1).states[0] == before);
				D_ASSERT(ReplayStates(last, at + 1).states[1] ==
				         (row_end_bytes == 2 ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR));
				if (BlockRowEnd(result, at, before, row_end_bytes == 2, candidates, pos)) {
					return true;
				}
				last = at + row_end_bytes;
			}
			pos = limit;
		}
		return quoted ? HandOverQuoted(pos, last) : HandOver(pos, last);
	}

	//! Leaves the block scan at `at`, with the states before it
	bool HandOver(const idx_t at, const idx_t last) {
		SetStatesBefore(at, last);
		iterator.pos.buffer_pos = at;
		return false;
	}

	//! Process one chunk
	template <class T>
	void Process(T &result) {
		idx_t to_pos;
		const bool has_escaped_value = state_machine->dialect_options.state_machine_options.escape != '\0';
		const idx_t start_pos = iterator.pos.buffer_pos;
		if (iterator.IsBoundarySet()) {
			to_pos = iterator.GetEndPos();
			if (to_pos > cur_buffer_handle->actual_size) {
				to_pos = cur_buffer_handle->actual_size;
			}
		} else {
			to_pos = cur_buffer_handle->actual_size;
		}
		skipper.SetBuffer(*cur_buffer_handle);
		while (iterator.pos.buffer_pos < to_pos) {
			if (T::BLOCK_SCAN && block_scan && BlockScanPossible(result)) {
				if (ProcessBlocks(result, to_pos)) {
					bytes_read = iterator.pos.buffer_pos - start_pos;
					return;
				}
				if (iterator.pos.buffer_pos >= to_pos) {
					break;
				}
			}
			state_machine->Transition(states, buffer_handle_ptr[iterator.pos.buffer_pos]);
			switch (states.states[1]) {
			case CSVState::INVALID:
				T::InvalidState(result);
				iterator.pos.buffer_pos++;
				bytes_read = iterator.pos.buffer_pos - start_pos;
				return;
			case CSVState::RECORD_SEPARATOR:
				if (states.states[0] == CSVState::RECORD_SEPARATOR || states.states[0] == CSVState::NOT_SET) {
					if (T::EmptyLine(result, iterator.pos.buffer_pos)) {
						iterator.pos.buffer_pos++;
						bytes_read = iterator.pos.buffer_pos - start_pos;
						lines_read++;
						return;
					}
					lines_read++;

				} else if (states.states[0] != CSVState::CARRIAGE_RETURN) {
					if (T::IsCommentSet(result)) {
						if (T::UnsetComment(result, iterator.pos.buffer_pos)) {
							iterator.pos.buffer_pos++;
							bytes_read = iterator.pos.buffer_pos - start_pos;
							lines_read++;
							return;
						}
					} else {
						if (T::AddRow(result, iterator.pos.buffer_pos)) {
							iterator.pos.buffer_pos++;
							bytes_read = iterator.pos.buffer_pos - start_pos;
							lines_read++;
							return;
						}
					}
					lines_read++;
				}
				iterator.pos.buffer_pos++;
				break;
			case CSVState::CARRIAGE_RETURN:
				if (states.states[0] == CSVState::RECORD_SEPARATOR || states.states[0] == CSVState::NOT_SET) {
					if (T::EmptyLine(result, iterator.pos.buffer_pos)) {
						iterator.pos.buffer_pos++;
						bytes_read = iterator.pos.buffer_pos - start_pos;
						lines_read++;
						return;
					}
				} else if (states.states[0] != CSVState::CARRIAGE_RETURN) {
					if (T::IsCommentSet(result)) {
						if (T::UnsetComment(result, iterator.pos.buffer_pos)) {
							iterator.pos.buffer_pos++;
							bytes_read = iterator.pos.buffer_pos - start_pos;
							lines_read++;
							return;
						}
					} else {
						if (T::AddRow(result, iterator.pos.buffer_pos)) {
							iterator.pos.buffer_pos++;
							bytes_read = iterator.pos.buffer_pos - start_pos;
							lines_read++;
							return;
						}
					}
				}
				iterator.pos.buffer_pos++;
				lines_read++;
				break;
			case CSVState::DELIMITER:
				T::AddValue(result, iterator.pos.buffer_pos);
				iterator.pos.buffer_pos++;
				break;
			case CSVState::QUOTED: {
				if ((states.states[0] == CSVState::UNQUOTED || states.states[0] == CSVState::MAYBE_QUOTED) &&
				    has_escaped_value) {
					ever_escaped = true;
					T::SetEscaped(result);
				}
				if ((states.states[0] == CSVState::ESCAPE || states.states[0] == CSVState::ESCAPED_RETURN ||
				     states.states[0] == CSVState::UNQUOTED_ESCAPE) &&
				    (buffer_handle_ptr[iterator.pos.buffer_pos] ==
				         state_machine->dialect_options.state_machine_options.quote.GetValue() ||
				     !state_machine->dialect_options.state_machine_options.strict_mode.GetValue())) {
					// We only set the ever escaped variable if this is either a quote char OR strict mode is off
					ever_escaped = true;
					if (states.states[0] == CSVState::UNQUOTED_ESCAPE) {
						used_unstrictness = true;
					}
				}
				ever_quoted = true;
				T::SetQuoted(result, iterator.pos.buffer_pos);
				iterator.pos.buffer_pos++;
				skipper.SkipToStop(state_machine->transition_array.skip_quoted, to_pos, iterator.pos.buffer_pos);
			} break;
			case CSVState::UNQUOTED: {
				if (states.states[0] == CSVState::MAYBE_QUOTED) {
					ever_escaped = true;
					T::SetEscaped(result);
				}
				T::SetUnquoted(result);
				iterator.pos.buffer_pos++;
				break;
			}
			case CSVState::ESCAPE:
			case CSVState::ESCAPED_RETURN:
				T::SetEscaped(result);
				iterator.pos.buffer_pos++;
				break;
			case CSVState::UNQUOTED_ESCAPE:
				T::SetEscaped(result);
				iterator.pos.buffer_pos++;
				used_unstrictness = true;
				break;
			case CSVState::STANDARD:
				iterator.pos.buffer_pos++;
				skipper.SkipToStop(state_machine->transition_array.skip_standard, to_pos, iterator.pos.buffer_pos);
				break;
			case CSVState::QUOTED_NEW_LINE:
				T::QuotedNewLine(result);
				iterator.pos.buffer_pos++;
				break;
			case CSVState::COMMENT:
				T::SetComment(result, iterator.pos.buffer_pos);
				iterator.pos.buffer_pos++;
				skipper.SkipToStop(state_machine->transition_array.skip_comment, to_pos, iterator.pos.buffer_pos);
				break;
			default:
				iterator.pos.buffer_pos++;
				break;
			}
		}
		bytes_read = iterator.pos.buffer_pos - start_pos;
	}

	//! Finalizes the process of the chunk
	virtual void FinalizeChunkProcess();

	//! Internal function for parse chunk
	template <class T>
	void ParseChunkInternal(T &result) {
		if (iterator.done) {
			return;
		}
		if (!initialized) {
			Initialize();
			initialized = true;
		}
		if (!iterator.done && cur_buffer_handle) {
			Process(result);
		}
		FinalizeChunkProcess();
	}
};

} // namespace duckdb
