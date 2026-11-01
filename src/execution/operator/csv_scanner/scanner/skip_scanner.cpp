#include "duckdb/execution/operator/csv_scanner/skip_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/column_count_scanner.hpp"

namespace duckdb {

SkipResult::SkipResult(CSVStates &states, CSVStateMachine &state_machine, idx_t rows_to_skip_p)
    : ScannerResult(states, state_machine, STANDARD_VECTOR_SIZE), rows_to_skip(rows_to_skip_p) {
}

void SkipResult::AddValue(SkipResult &result, const idx_t buffer_pos) {
	// nop
}

inline void SkipResult::InternalAddRow() {
	row_count++;
}

void SkipResult::CheckLineSize(idx_t position) {
	if (!state_machine.options.dialect_options.skip_rows.IsSetByUser()) {
		return;
	}
	const auto maximum_line_size = state_machine.options.maximum_line_size.GetValue();
	if (position - line_start > maximum_line_size) {
		line_size_exceeded = true;
	}
}

void SkipResult::QuotedNewLine(SkipResult &result) {
	// nop
}

void SkipResult::SetComment(SkipResult &result, idx_t buffer_pos) {
	if (!result.states.WasStandard()) {
		result.cur_line_starts_as_comment = true;
	}
	result.comment = true;
}

bool SkipResult::UnsetComment(SkipResult &result, idx_t buffer_pos) {
	// If we are unsetting a comment, it means this row started with a comment char.
	bool done = false;
	if (!result.cur_line_starts_as_comment) {
		done = result.AddRow(result, buffer_pos);
	} else {
		const auto position = result.buffer_start + buffer_pos + 1;
		result.CheckLineSize(position);
		result.line_start = position;
	}
	result.comment = false;
	result.cur_line_starts_as_comment = false;
	return done;
}

bool SkipResult::AddRow(SkipResult &result, const idx_t buffer_pos) {
	const auto position = result.buffer_start + buffer_pos + 1;
	result.CheckLineSize(position);
	result.line_start = position;
	result.InternalAddRow();
	if (result.row_count >= result.rows_to_skip) {
		// We skipped enough rows
		return true;
	}
	return false;
}

void SkipResult::InvalidState(SkipResult &result) {
	// nop
}

bool SkipResult::EmptyLine(SkipResult &result, const idx_t buffer_pos) {
	return AddRow(result, buffer_pos);
}

SkipScanner::SkipScanner(shared_ptr<CSVBufferManager> buffer_manager, const shared_ptr<CSVStateMachine> &state_machine,
                         shared_ptr<CSVErrorHandler> error_handler, idx_t rows_to_skip)
    : BaseScanner(std::move(buffer_manager), state_machine, std::move(error_handler)),
      result(states, *state_machine, rows_to_skip) {
}

SkipResult &SkipScanner::ParseChunk() {
	ParseChunkInternal(result);
	return result;
}

SkipResult &SkipScanner::GetResult() {
	return result;
}

void SkipScanner::Initialize() {
	states.Initialize();
}

void SkipScanner::FinalizeChunkProcess() {
	// We continue skipping until we skipped enough rows, or we have nothing else to read.
	while (!FinishedFile() && result.row_count < result.rows_to_skip) {
		result.buffer_start += cur_buffer_handle->actual_size;
		result.CheckLineSize(result.buffer_start);
		cur_buffer_handle = buffer_manager->GetBuffer(++iterator.pos.buffer_idx);
		if (cur_buffer_handle) {
			iterator.pos.buffer_pos = 0;
			buffer_handle_ptr = cur_buffer_handle->Ptr();
			Process(result);
		}
	}
	// Skip Carriage Return
	if (state_machine->options.dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON &&
	    states.states[1] == CSVState::CARRIAGE_RETURN) {
		iterator.pos.buffer_pos++;
	}
	iterator.done = FinishedFile();
	if (iterator.done && result.line_size_exceeded) {
		throw InvalidInputException("Maximum line size of %llu bytes exceeded",
		                            state_machine->options.maximum_line_size.GetValue());
	}
}
} // namespace duckdb
