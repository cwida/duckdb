#include "duckdb/execution/operator/csv_scanner/column_count_scanner.hpp"

namespace duckdb {

ColumnCountResult::ColumnCountResult(CSVStates &states, CSVStateMachine &state_machine, idx_t result_size,
                                     CSVErrorHandler &error_handler)
    : ScannerResult(states, state_machine, result_size), error_handler(error_handler) {
	column_counts.resize(result_size);
}

void ColumnCountResult::AddValue(ColumnCountResult &result, idx_t buffer_pos) {
	result.current_column_count++;
}

inline void ColumnCountResult::InternalAddRow() {
	const idx_t column_count = current_column_count + 1;
	column_counts[result_position].number_of_columns = column_count;
	column_counts[result_position].empty_lines = empty_lines;
	rows_per_column_count[column_count]++;
	current_column_count = 0;
}

idx_t ColumnCountResult::GetMostFrequentColumnCount() const {
	if (rows_per_column_count.empty()) {
		return 1;
	}
	idx_t column_count = 0;
	idx_t current_max = 0;
	for (auto &rpc : rows_per_column_count) {
		if (rpc.second > current_max) {
			current_max = rpc.second;
			column_count = rpc.first;
		} else if (rpc.second == current_max) {
			// We pick the largest to untie
			if (rpc.first > column_count) {
				column_count = rpc.first;
			}
		}
	}
	return column_count;
}

bool ColumnCountResult::AddRow(ColumnCountResult &result, idx_t buffer_pos) {
	const LinePosition cur_position(result.cur_buffer_idx, buffer_pos + 1, result.current_buffer_size);
	if (cur_position - result.last_position > result.state_machine.options.maximum_line_size.GetValue() &&
	    buffer_pos != NumericLimits<idx_t>::Maximum()) {
		LinesPerBoundary lines_per_batch;
		FullLinePosition current_line_position;
		current_line_position.begin = result.last_position;
		current_line_position.end = cur_position;
		bool mock = false;
		idx_t line_size = 0;
		string csv_row = current_line_position.ReconstructCurrentLine(mock, result.buffer_handles, true, &line_size);
		auto error = CSVError::LineSizeError(
		    result.state_machine.options, lines_per_batch, csv_row, line_size,
		    result.last_position.GetGlobalPosition(result.state_machine.options.buffer_size_option.GetValue(), false),
		    result.state_machine.options.file_path);
		result.error_handler.Error(error);
		result.error = true;
	}
	result.InternalAddRow();
	result.last_position = cur_position;
	if (!result.states.EmptyLastValue()) {
		idx_t col_count_idx = result.result_position;
		for (idx_t i = 0; i < result.result_position + 1; i++) {
			if (!result.column_counts[col_count_idx].last_value_always_empty) {
				break;
			}
			result.column_counts[col_count_idx--].last_value_always_empty = false;
		}
	}
	result.result_position++;
	if (result.result_position >= result.result_size) {
		// We sniffed enough rows
		return true;
	}
	return false;
}

void ColumnCountResult::SetComment(ColumnCountResult &result, idx_t buffer_pos) {
	if (!result.states.WasStandard() && !result.states.WasState(CSVState::DELIMITER)) {
		result.cur_line_starts_as_comment = true;
	}
	result.comment = true;
}

bool ColumnCountResult::UnsetComment(ColumnCountResult &result, idx_t buffer_pos) {
	// If we are unsetting a comment, it means this row started with a comment char.
	// We add the row but tag it as a comment
	bool done = result.AddRow(result, buffer_pos);
	if (result.cur_line_starts_as_comment) {
		result.column_counts[result.result_position - 1].is_comment = true;
	} else {
		result.column_counts[result.result_position - 1].is_mid_comment = true;
	}
	result.comment = false;
	result.cur_line_starts_as_comment = false;
	return done;
}

void ColumnCountResult::InvalidState(ColumnCountResult &result) {
	result.result_position = 0;
	result.error = true;
}

bool ColumnCountResult::EmptyLine(ColumnCountResult &result, idx_t buffer_pos) {
	result.empty_lines++;
	return false;
}

void ColumnCountResult::QuotedNewLine(ColumnCountResult &result) {
	// nop
}

ColumnCountScanner::ColumnCountScanner(shared_ptr<CSVBufferManager> buffer_manager,
                                       const shared_ptr<CSVStateMachine> &state_machine,
                                       shared_ptr<CSVErrorHandler> error_handler_p, idx_t result_size_p,
                                       CSVIterator iterator)
    : BaseScanner(std::move(buffer_manager), state_machine, std::move(error_handler_p), true, nullptr, iterator),
      result(states, *state_machine, result_size_p, *error_handler), column_count(1), result_size(result_size_p) {
	sniffing = true;
	idx_t actual_size = 0;
	if (cur_buffer_handle) {
		actual_size = cur_buffer_handle->actual_size;
		result.buffer_handles[0] = cur_buffer_handle;
	}
	result.last_position = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, actual_size};
	result.current_buffer_size = actual_size;
	result.cur_buffer_idx = iterator.pos.buffer_idx;
}

unique_ptr<StringValueScanner> ColumnCountScanner::UpgradeToStringValueScanner() {
	idx_t rows_to_skip = state_machine->dialect_options.skip_rows.GetValue();
	auto iterator = SkipCSVRows(buffer_manager, state_machine, rows_to_skip);
	if (iterator.done) {
		CSVIterator it {};
		return make_uniq<StringValueScanner>(0U, buffer_manager, state_machine, error_handler, nullptr, true, it,
		                                     result_size);
	}
	return make_uniq<StringValueScanner>(0U, buffer_manager, state_machine, error_handler, nullptr, true, iterator,
	                                     result_size);
}

ColumnCountResult &ColumnCountScanner::ParseChunk() {
	result.result_position = 0;
	column_count = 1;
	if (cur_buffer_handle) {
		result.current_buffer_size = cur_buffer_handle->actual_size;
	}
	ParseChunkInternal(result);
	return result;
}

ColumnCountResult &ColumnCountScanner::GetResult() {
	return result;
}

void ColumnCountScanner::Initialize() {
	states.Initialize();
}

void ColumnCountScanner::FinalizeChunkProcess() {
	if (result.result_position == result.result_size || result.error) {
		// We are done
		return;
	}
	// We run until we have a full chunk, or we are done scanning
	while (!FinishedFile() && result.result_position < result.result_size && !result.error) {
		if (iterator.pos.buffer_pos == cur_buffer_handle->actual_size) {
			// Move to next buffer
			cur_buffer_handle = buffer_manager->GetBuffer(++iterator.pos.buffer_idx);

			if (!cur_buffer_handle) {
				buffer_handle_ptr = nullptr;
				if (states.IsQuotedCurrent() && !states.IsUnquoted()) {
					// We are finishing our file on a quoted value that is never unquoted, straight to jail.
					result.error = true;
					return;
				}
				if (states.EmptyLine() || states.NewRow() || states.IsCurrentNewRow() || states.IsNotSet()) {
					return;
				}
				// This means we reached the end of the file, we must add a last line if there is any to be added
				if (result.comment) {
					// If it's a comment we add the last line via unset comment
					result.UnsetComment(result, NumericLimits<idx_t>::Maximum());
				} else {
					// OW, we do a regular AddRow
					result.AddRow(result, NumericLimits<idx_t>::Maximum());
				}
				return;
			} else {
				result.buffer_handles[iterator.pos.buffer_idx] = cur_buffer_handle;
				result.cur_buffer_idx = iterator.pos.buffer_idx;
				result.current_buffer_size = cur_buffer_handle->actual_size;
				// Do a quick check that the line is still sane
				const LinePosition cur_position(result.cur_buffer_idx, 0, result.current_buffer_size);
				LinesPerBoundary lines_per_batch;
				if (cur_position - result.last_position > result.state_machine.options.maximum_line_size.GetValue()) {
					FullLinePosition current_line_position;
					current_line_position.begin = result.last_position;
					current_line_position.end = cur_position;
					bool mock = false;
					idx_t line_size = 0;
					string csv_row =
					    current_line_position.ReconstructCurrentLine(mock, result.buffer_handles, true, &line_size);
					auto error =
					    CSVError::LineSizeError(result.state_machine.options, lines_per_batch, csv_row, line_size,
					                            result.last_position.GetGlobalPosition(
					                                result.state_machine.options.buffer_size_option.GetValue(), false),
					                            result.state_machine.options.file_path);
					error_handler->Error(error);
					result.error = true;
					return;
				}
			}
			if (result.buffer_handles.size() > 2) {
				// pop lowest value
				result.buffer_handles.erase(result.buffer_handles.begin());
			}
			iterator.pos.buffer_pos = 0;
			buffer_handle_ptr = cur_buffer_handle->Ptr();
		}
		Process(result);
	}
}
} // namespace duckdb
