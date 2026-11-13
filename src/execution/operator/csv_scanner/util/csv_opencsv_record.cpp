#include "duckdb/execution/operator/csv_scanner/csv_opencsv_record.hpp"

#include <cstring>

namespace duckdb {

OpenCSVRecordParser::OpenCSVRecordParser(string separator_p, char quote_p, char escape_p)
    : separator(std::move(separator_p)), quote(quote_p), escape(escape_p) {
}

bool OpenCSVRecordParser::IsSeparatorAt(const char *record, idx_t size, idx_t position) const {
	if (separator.size() == 1) {
		return position < size && record[position] == separator[0];
	}
	return position + separator.size() <= size && memcmp(record + position, separator.data(), separator.size()) == 0;
}

//! Java's Character.isWhitespace for ASCII, which is what opencsv sees in UTF-8 data
static bool IsJavaWhitespace(char c) {
	return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1C && c <= 0x1F);
}

static bool IsAllJavaWhitespace(const string &text) {
	for (const auto c : text) {
		if (!IsJavaWhitespace(c)) {
			return false;
		}
	}
	return true;
}

idx_t OpenCSVRecordParser::Parse(const char *record, idx_t size, idx_t max_fields) {
	if (max_fields == 0) {
		return 0;
	}
	if (fields.size() < max_fields) {
		fields.resize(max_fields);
	}
	idx_t field_count = 0;
	auto *field = &fields[0];
	field->clear();
	bool in_quotes = false;
	bool in_field = false;
	const auto separator_size = separator.size();
	// the position in UTF-16 code units, as Java counts the characters the embedded quote rule looks at; only
	// whether it is past 2 matters, so the count stops there
	idx_t char_position = 0;
	auto count = [&](idx_t from, idx_t to) {
		for (auto consumed = from; consumed < to && char_position <= 2; consumed++) {
			const auto byte = static_cast<uint8_t>(record[consumed]);
			if ((byte & 0xC0) != 0x80) {
				// a character above the Basic Multilingual Plane is two UTF-16 code units
				char_position += byte >= 0xF0 ? 2 : 1;
			}
		}
	};
	auto separator_before = [&](idx_t position) {
		return position >= separator_size &&
		       memcmp(record + position - separator_size, separator.data(), separator_size) == 0;
	};
	for (idx_t i = 0; i < size;) {
		const auto start = i;
		const auto c = record[i];
		const auto has_next = i + 1 < size;
		if (c == escape) {
			// the escape only escapes the quote and escape characters, and is inert at the start of a field
			if ((in_quotes || in_field) && has_next && (record[i + 1] == quote || record[i + 1] == escape)) {
				field->push_back(record[i + 1]);
				i++;
			}
			i++;
		} else if (c == quote) {
			if ((in_quotes || in_field) && has_next && record[i + 1] == quote) {
				// a doubled quote is one quote, in a quoted field or not
				field->push_back(quote);
				i++;
			} else {
				// a quote in the middle of a field, not next to the separator, is a literal character - while still
				// toggling the quote state, as opencsv's embedded quote rule does
				if (char_position > 2 && !separator_before(i) && has_next && !IsSeparatorAt(record, size, i + 1)) {
					if (!field->empty() && IsAllJavaWhitespace(*field)) {
						// discard whitespace leading up to the quote
						field->clear();
					} else {
						field->push_back(quote);
					}
				}
				in_quotes = !in_quotes;
			}
			in_field = !in_field;
			i++;
		} else if (!in_quotes && IsSeparatorAt(record, size, i)) {
			field_count++;
			if (field_count == max_fields) {
				// the fields already split do not change, so the rest of the record does not matter
				return field_count;
			}
			field = &fields[field_count];
			field->clear();
			in_field = false;
			i += separator_size;
		} else {
			field->push_back(c);
			in_field = true;
			i++;
		}
		count(start, i);
	}
	if (in_quotes) {
		// the record ends inside an open quote: the field in progress is dropped, as opencsv's reader drops the
		// parser's pending field, and it is read as NULL
		return field_count;
	}
	return field_count + 1;
}

} // namespace duckdb
