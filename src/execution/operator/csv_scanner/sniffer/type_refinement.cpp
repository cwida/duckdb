#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_casting.hpp"

namespace duckdb {
bool CSVSniffer::TryCastVector(Vector &parse_chunk_col, idx_t size, const LogicalType &sql_type) const {
	auto &sniffing_state_machine = best_candidate->GetStateMachine();
	// try vector-cast from string to sql_type
	Vector dummy_result(sql_type, size);
	if (!sniffing_state_machine.dialect_options.date_format[LogicalTypeId::DATE].GetValue().Empty() &&
	    sql_type.id() == LogicalTypeId::DATE) {
		// use the date format to cast the chunk
		string error_message;
		CastParameters parameters(false, &error_message);
		idx_t line_error;
		return CSVCast::TryCastDateVector(sniffing_state_machine.dialect_options.date_format, parse_chunk_col,
		                                  dummy_result, size, parameters, line_error);
	}
	if (!sniffing_state_machine.dialect_options.date_format[LogicalTypeId::TIMESTAMP].GetValue().Empty() &&
	    sql_type.id() == LogicalTypeId::TIMESTAMP) {
		// use the timestamp format to cast the chunk
		string error_message;
		CastParameters parameters(false, &error_message);
		return CSVCast::TryCastTimestampVector(sniffing_state_machine.dialect_options.date_format, parse_chunk_col,
		                                       dummy_result, size, parameters);
	}
	if ((sql_type.id() == LogicalTypeId::DOUBLE || sql_type.id() == LogicalTypeId::FLOAT) &&
	    options.decimal_separator == ",") {
		string error_message;
		CastParameters parameters(false, &error_message);
		idx_t line_error;
		return CSVCast::TryCastFloatingVectorCommaSeparated(options, parse_chunk_col, dummy_result, size, parameters,
		                                                    sql_type, line_error);
	}
	if (sql_type.id() == LogicalTypeId::DECIMAL && options.decimal_separator == ",") {
		string error_message;
		CastParameters parameters(false, &error_message);
		idx_t line_error;
		return CSVCast::TryCastDecimalVectorCommaSeparated(options, parse_chunk_col, dummy_result, size, parameters,
		                                                   sql_type, line_error);
	}
	if (sql_type.id() == LogicalTypeId::BIGNUM) {
		auto vector_data = FlatVector::GetData<string_t>(parse_chunk_col);
		auto &validity = FlatVector::ValidityMutable(parse_chunk_col);
		string strip_thousands;
		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			if (validity.RowIsValid(row_idx)) {
				auto value_ptr = vector_data[row_idx].GetData();
				auto value_size = vector_data[row_idx].GetSize();
				if (sniffing_state_machine.options.thousands_separator != '\0') {
					strip_thousands = BaseScanner::RemoveSeparator(value_ptr, value_size,
					                                               sniffing_state_machine.options.thousands_separator);
					value_ptr = strip_thousands.c_str();
					value_size = strip_thousands.size();
				}
				if (!CSVSniffer::CanYouCastBignum(value_ptr, value_size)) {
					return false;
				}
			}
		}
		return true;
	}
	// target type is not varchar: perform a cast
	string error_message;
	return VectorOperations::DefaultTryCast(parse_chunk_col, dummy_result, size, &error_message, true);
}

void CSVSniffer::RefineTypes() {
	auto &sniffing_state_machine = best_candidate->GetStateMachine();
	// if data types were provided, exit here if the number of columns does not match
	detected_types.assign(sniffing_state_machine.dialect_options.num_cols, LogicalType::VARCHAR);
	const auto all_varchar = sniffing_state_machine.options.all_varchar;
	for (idx_t i = 1; i < sniffing_state_machine.options.sample_size_chunks; i++) {
		if (best_candidate->FinishedFile()) {
			break;
		}
		best_candidate->SetResultLimit(sniffing_state_machine.options.GetSampleChunkSize(i));
		auto &parse_chunk = best_candidate->ParseChunk().ToChunk();
		// Include each subsequent type sample once, even with only one dialect candidate.
		lines_sniffed += parse_chunk.size();

		for (idx_t col = 0; !all_varchar && col < parse_chunk.ColumnCount(); col++) {
			vector<LogicalType> &col_type_candidates = best_sql_types_candidates_per_column_idx[col];
			bool is_bool_type = col_type_candidates.back() == LogicalType::BOOLEAN;
			while (col_type_candidates.size() > 1) {
				const auto &sql_type = col_type_candidates.back();
				if (TryCastVector(parse_chunk.data[col], parse_chunk.size(), sql_type)) {
					break;
				}
				if (col_type_candidates.back() == LogicalType::BOOLEAN && is_bool_type) {
					// If we thought this was a boolean value (i.e., T,F, True, False) and it is not, we
					// immediately pop to varchar.
					while (col_type_candidates.back() != LogicalType::VARCHAR) {
						col_type_candidates.pop_back();
					}
					break;
				}
				col_type_candidates.pop_back();
			}
		}
		// reset parse chunk for the next iteration
		parse_chunk.Reset();
	}
	if (all_varchar) {
		return;
	}
	detected_types.clear();
	// set sql types
	for (idx_t column_idx = 0; column_idx < best_sql_types_candidates_per_column_idx.size(); column_idx++) {
		LogicalType d_type = best_sql_types_candidates_per_column_idx[column_idx].back();
		if (best_sql_types_candidates_per_column_idx[column_idx].size() ==
		        best_candidate->GetStateMachine().options.auto_type_candidates.size() &&
		    default_null_to_varchar && !best_candidate->FinishedFile()) {
			// We only default SQLNull to Varchar if we haven't finished the file yet.
			d_type = LogicalType::VARCHAR;
		}
		detected_types.push_back(d_type);
	}
}
} // namespace duckdb
