#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/simplified_token.hpp"

#include "utf8proc.hpp"

namespace duckdb {
// Helper function to generate column names
static string GenerateColumnName(const idx_t total_cols, const idx_t col_number, const string &prefix = "column") {
	auto max_digits = NumericHelper::UnsignedLength(total_cols - 1);
	auto digits = NumericHelper::UnsignedLength(col_number);
	string leading_zeros = string(NumericCast<idx_t>(max_digits - digits), '0');
	string value = to_string(col_number);
	return string(prefix + leading_zeros + value);
}

// Helper function for UTF-8 aware space trimming
static string TrimWhitespace(const string &col_name) {
	utf8proc_int32_t codepoint;
	const auto str = reinterpret_cast<const utf8proc_uint8_t *>(col_name.c_str());
	const idx_t size = col_name.size();
	// Find the first character that is not left trimmed
	idx_t begin = 0;
	while (begin < size) {
		auto bytes = utf8proc_iterate(str + begin, NumericCast<utf8proc_ssize_t>(size - begin), &codepoint);
		D_ASSERT(bytes > 0);
		if (utf8proc_category(codepoint) != UTF8PROC_CATEGORY_ZS) {
			break;
		}
		begin += NumericCast<idx_t>(bytes);
	}

	// Find the last character that is not right trimmed
	idx_t end = begin;
	for (auto next = begin; next < col_name.size();) {
		auto bytes = utf8proc_iterate(str + next, NumericCast<utf8proc_ssize_t>(size - next), &codepoint);
		D_ASSERT(bytes > 0);
		next += NumericCast<idx_t>(bytes);
		if (utf8proc_category(codepoint) != UTF8PROC_CATEGORY_ZS) {
			end = next;
		}
	}

	// return the trimmed string
	return col_name.substr(begin, end - begin);
}

bool NormalizeThis(const KeywordCategory category, const string &col_name) {
	return category == KeywordCategory::KEYWORD_UNRESERVED || category == KeywordCategory::KEYWORD_TYPE_FUNC ||
	       category == KeywordCategory::KEYWORD_RESERVED;
}

static string NormalizeColumnName(const string &col_name) {
	// normalize UTF8 characters to NFKD
	auto nfkd = utf8proc_NFKD(reinterpret_cast<const utf8proc_uint8_t *>(col_name.c_str()),
	                          NumericCast<utf8proc_ssize_t>(col_name.size()));
	const string col_name_nfkd = string(const_char_ptr_cast(nfkd), strlen(const_char_ptr_cast(nfkd)));
	free(nfkd);

	// only keep ASCII characters 0-9 a-z A-Z and replace spaces with regular whitespace
	string col_name_ascii = "";
	for (idx_t i = 0; i < col_name_nfkd.size(); i++) {
		if (col_name_nfkd[i] == '_' || (col_name_nfkd[i] >= '0' && col_name_nfkd[i] <= '9') ||
		    (col_name_nfkd[i] >= 'A' && col_name_nfkd[i] <= 'Z') ||
		    (col_name_nfkd[i] >= 'a' && col_name_nfkd[i] <= 'z')) {
			col_name_ascii += col_name_nfkd[i];
		} else if (StringUtil::CharacterIsSpace(col_name_nfkd[i])) {
			col_name_ascii += " ";
		}
	}

	// trim whitespace and replace remaining whitespace by _
	string col_name_trimmed = TrimWhitespace(col_name_ascii);
	string col_name_cleaned = "";
	bool in_whitespace = false;
	for (idx_t i = 0; i < col_name_trimmed.size(); i++) {
		if (col_name_trimmed[i] == ' ') {
			if (!in_whitespace) {
				col_name_cleaned += "_";
				in_whitespace = true;
			}
		} else {
			col_name_cleaned += col_name_trimmed[i];
			in_whitespace = false;
		}
	}

	// don't leave string empty; if not empty, make lowercase
	if (col_name_cleaned.empty()) {
		col_name_cleaned = "_";
	} else {
		col_name_cleaned = StringUtil::Lower(col_name_cleaned);
	}

	// prepend _ if name starts with a digit or is a reserved keyword
	auto keyword = KeywordHelper::KeywordCategoryType(col_name_cleaned);

	if (NormalizeThis(keyword, col_name_cleaned) || (col_name_cleaned[0] >= '0' && col_name_cleaned[0] <= '9')) {
		col_name_cleaned = "_" + col_name_cleaned;
	}
	return col_name_cleaned;
}

static void ReplaceNames(vector<Identifier> &detected_names, CSVStateMachine &state_machine,
                         unordered_map<idx_t, vector<LogicalType>> &best_sql_types_candidates_per_column_idx,
                         CSVReaderOptions &options, const MultiFileOptions &file_options,
                         const vector<HeaderValue> &best_header_row, CSVErrorHandler &error_handler) {
	auto &dialect_options = state_machine.dialect_options;
	if (!options.columns_set) {
		if (file_options.hive_partitioning || file_options.union_by_name || options.multi_file_reader) {
			// Just do the replacement
			for (idx_t i = 0; i < MinValue<idx_t>(detected_names.size(), options.name_list.size()); i++) {
				detected_names[i] = options.name_list[i];
			}
			return;
		}
		if (options.name_list.size() > dialect_options.num_cols) {
			if (options.null_padding) {
				// we increase our types
				idx_t col = 0;
				for (idx_t i = dialect_options.num_cols; i < options.name_list.size(); i++) {
					detected_names.emplace_back(GenerateColumnName(options.name_list.size(), col++));
					best_sql_types_candidates_per_column_idx[i] = {LogicalType::VARCHAR};
				}
				dialect_options.num_cols = options.name_list.size();
			} else {
				// we throw an error
				const auto error = CSVError::HeaderSniffingError(
				    options, best_header_row, options.name_list.size(),
				    state_machine.dialect_options.state_machine_options.delimiter.GetValue());
				error_handler.Error(error);
			}
		}
		if (options.name_list.size() > detected_names.size()) {
			// we throw an error
			const auto error =
			    CSVError::HeaderSniffingError(options, best_header_row, options.name_list.size(),
			                                  state_machine.dialect_options.state_machine_options.delimiter.GetValue());
			error_handler.Error(error);
		} else {
			for (idx_t i = 0; i < options.name_list.size(); i++) {
				detected_names[i] = options.name_list[i];
			}
		}
	}
}

// If our columns were set by the user, we verify if their names match with the first row
bool CSVSniffer::DetectHeaderWithSetColumn(ClientContext &context, vector<HeaderValue> &best_header_row,
                                           const SetColumns &set_columns, CSVReaderOptions &options,
                                           const DialectOptions &scan_dialect_options) {
	bool has_header = true;

	std::ostringstream error;
	// User set the names, we must check if they match the first row
	// We do a +1 to check for situations where the csv file has an extra all null column
	if (set_columns.Size() != best_header_row.size() && set_columns.Size() + 1 != best_header_row.size()) {
		return false;
	}

	// Let's do a match-aroo
	for (idx_t i = 0; i < set_columns.Size(); i++) {
		if (best_header_row[i].IsNull()) {
			return false;
		}
		if (best_header_row[i].value != (*set_columns.names)[i]) {
			error << "Header mismatch at position: " << i << "\n";
			error << "Expected name: \"" << (*set_columns.names)[i] << "\", ";
			error << "Actual name: \"" << best_header_row[i].value << "\"."
			      << "\n";
			has_header = false;
			break;
		}
	}

	if (!has_header) {
		bool all_varchar = true;
		// A row the scan reads is data; for any other row the type detection check decides, as before
		bool scan_reads_row = true;
		bool type_detection_reads_row = true;
		for (idx_t col = 0; col < set_columns.Size(); col++) {
			const auto &sql_type = (*set_columns.types)[col];
			if (sql_type != LogicalType::VARCHAR) {
				all_varchar = false;
				const auto &value = best_header_row[col];
				if (!CSVSniffer::CanYouCastIt(context, value.value, sql_type, scan_dialect_options, value.is_null,
				                              options.decimal_separator[0], options.thousands_separator, false)) {
					scan_reads_row = false;
				}
				if (!CSVSniffer::CanYouCastIt(context, value.value, sql_type, options.dialect_options, value.is_null,
				                              options.decimal_separator[0], options.thousands_separator, true)) {
					type_detection_reads_row = false;
				}
			}
		}
		const bool first_row_consistent = scan_reads_row || type_detection_reads_row;
		if (!first_row_consistent) {
			options.sniffer_user_mismatch_error += error.str();
		}
		if (all_varchar) {
			return true;
		}
		return !first_row_consistent;
	}
	return has_header;
}

bool EmptyHeader(const string &col_name, bool is_null, bool normalize) {
	if (col_name.empty() || is_null) {
		return true;
	}
	if (normalize) {
		// normalize has special logic to trim white spaces and generate names
		return false;
	}
	// check if it's all white spaces
	for (auto &c : col_name) {
		if (!StringUtil::CharacterIsSpace(c)) {
			return false;
		}
	}
	// if we are not normalizing the name and is all white spaces, then we generate a name
	return true;
}

vector<Identifier>
CSVSniffer::DetectHeaderInternal(ClientContext &context, vector<HeaderValue> &best_header_row,
                                 CSVStateMachine &state_machine, const SetColumns &set_columns,
                                 unordered_map<idx_t, vector<LogicalType>> &best_sql_types_candidates_per_column_idx,
                                 CSVReaderOptions &options, const DialectOptions &scan_dialect_options,
                                 const MultiFileOptions &file_options, CSVErrorHandler &error_handler) {
	vector<Identifier> detected_names;
	auto &dialect_options = state_machine.dialect_options;
	dialect_options.num_cols = best_sql_types_candidates_per_column_idx.size();
	if (best_header_row.empty()) {
		dialect_options.header = false;
		for (idx_t col = 0; col < dialect_options.num_cols; col++) {
			detected_names.emplace_back(GenerateColumnName(dialect_options.num_cols, col));
		}
		// If the user provided names, we must replace our header with the user provided names
		ReplaceNames(detected_names, state_machine, best_sql_types_candidates_per_column_idx, options, file_options,
		             best_header_row, error_handler);
		return detected_names;
	}
	// information for header detection
	// check if header row is all null and/or consistent with detected column data types
	// If null-padding is not allowed and there is a mismatch between our header candidate and the number of columns
	// We can't detect the dialect/type options properly
	if (!options.null_padding && best_sql_types_candidates_per_column_idx.size() != best_header_row.size()) {
		if (options.ignore_errors.GetValue()) {
			dialect_options.header = false;
			for (idx_t col = 0; col < dialect_options.num_cols; col++) {
				detected_names.emplace_back(GenerateColumnName(dialect_options.num_cols, col));
			}
			dialect_options.rows_until_header += 1;
			ReplaceNames(detected_names, state_machine, best_sql_types_candidates_per_column_idx, options, file_options,
			             best_header_row, error_handler);
			return detected_names;
		}
		auto error =
		    CSVError::HeaderSniffingError(options, best_header_row, best_sql_types_candidates_per_column_idx.size(),
		                                  state_machine.dialect_options.state_machine_options.delimiter.GetValue());
		error_handler.Error(error);
	}
	bool has_header;

	if (set_columns.IsSet()) {
		has_header = DetectHeaderWithSetColumn(context, best_header_row, set_columns, options, scan_dialect_options);
	} else {
		bool first_row_consistent = true;
		bool all_varchar = true;
		bool first_row_nulls = true;
		for (idx_t col = 0; col < best_header_row.size(); col++) {
			if (!best_header_row[col].IsNull()) {
				first_row_nulls = false;
			}
			// try cast to sql_type of column
			const auto &sql_type = best_sql_types_candidates_per_column_idx[col].back();
			if (sql_type != LogicalType::VARCHAR) {
				all_varchar = false;
				if (!CanYouCastIt(context, best_header_row[col].value, sql_type, dialect_options,
				                  best_header_row[col].IsNull(), options.decimal_separator[0],
				                  options.thousands_separator, true)) {
					first_row_consistent = false;
				}
			}
		}
		// Our header is only false if types are not all varchar, and rows are consistent
		if (all_varchar || first_row_nulls) {
			has_header = true;
		} else {
			has_header = !first_row_consistent;
		}
	}

	if (options.dialect_options.header.IsSetByUser()) {
		// Header is defined by user, use that.
		has_header = options.dialect_options.header.GetValue();
	}
	// update parser info, and read, generate & set col_names based on previous findings
	if (has_header) {
		dialect_options.header = true;
		if (options.null_padding && !options.dialect_options.skip_rows.IsSetByUser()) {
			if (dialect_options.skip_rows.GetValue() > 0) {
				dialect_options.skip_rows = dialect_options.skip_rows.GetValue() - 1;
			}
		}
		case_insensitive_map_t<idx_t> name_collision_count;

		// get header names from CSV
		for (idx_t col = 0; col < best_header_row.size(); col++) {
			string &col_name = best_header_row[col].value;

			// generate name if field is empty
			if (EmptyHeader(col_name, best_header_row[col].is_null, options.normalize_names)) {
				col_name = GenerateColumnName(dialect_options.num_cols, col);
			}

			// normalize names or at least trim whitespace
			if (options.normalize_names) {
				col_name = NormalizeColumnName(col_name);
			} else {
				col_name = TrimWhitespace(col_name);
			}

			// avoid duplicate header names
			while (name_collision_count.find(col_name) != name_collision_count.end()) {
				name_collision_count[col_name] += 1;
				col_name = col_name + "_" + to_string(name_collision_count[col_name]);
			}
			detected_names.emplace_back(col_name);
			name_collision_count[col_name] = 0;
		}
		if (best_header_row.size() < dialect_options.num_cols && options.null_padding) {
			for (idx_t col = best_header_row.size(); col < dialect_options.num_cols; col++) {
				detected_names.emplace_back(GenerateColumnName(dialect_options.num_cols, col));
			}
		} else if (best_header_row.size() < dialect_options.num_cols) {
			throw InternalException("Detected header has number of columns inferior to dialect detection");
		}

	} else {
		dialect_options.header = false;
		for (idx_t col = 0; col < dialect_options.num_cols; col++) {
			detected_names.emplace_back(GenerateColumnName(dialect_options.num_cols, col));
		}
	}

	// If the user provided names, we must replace our header with the user provided names
	ReplaceNames(detected_names, state_machine, best_sql_types_candidates_per_column_idx, options, file_options,
	             best_header_row, error_handler);
	return detected_names;
}
void CSVSniffer::DetectHeader() {
	auto &sniffer_state_machine = best_candidate->GetStateMachine();
	names = DetectHeaderInternal(buffer_manager->context, best_header_row, sniffer_state_machine, set_columns,
	                             best_sql_types_candidates_per_column_idx, options, ScanDialectOptions(), file_options,
	                             *error_handler);
	if (EmptyOrOnlyHeader()) {
		// This file only contains a header, lets default to the lowest type of all.
		detected_types.clear();
		for (idx_t i = 0; i < names.size(); i++) {
			detected_types.push_back(LogicalType::SQLNULL);
		}
	}
	for (idx_t i = detected_types.size(); i < names.size(); i++) {
		detected_types.push_back(LogicalType::VARCHAR);
	}
	max_columns_found = names.size();
}
} // namespace duckdb
