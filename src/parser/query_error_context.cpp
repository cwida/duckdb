#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

#include "utf8proc_wrapper.hpp"

namespace duckdb {

string QueryErrorContext::Format(const string &query, const string &error_message, int error_loc) {
	if (error_loc < 0 || size_t(error_loc) >= query.size()) {
		// no location in query provided
		return error_message;
	}
	idx_t error_location = idx_t(error_loc);
	// count the line numbers until the error location
	// and set the start position as the first character of that line
	idx_t start_pos = 0;
	idx_t line_number = 1;
	for (idx_t i = 0; i < error_location; i++) {
		if (StringUtil::CharacterIsNewline(query[i])) {
			line_number++;
			start_pos = i + 1;
		}
	}
	// now find either the next newline token after the query, or find the end of string
	// this is the initial end position
	idx_t end_pos = query.size();
	for (idx_t i = error_location; i < query.size(); i++) {
		if (StringUtil::CharacterIsNewline(query[i])) {
			end_pos = i;
			break;
		}
	}
	// now start scanning from the start pos
	// we want to figure out the start and end pos of what we are going to render
	// we want to render at most 80 characters in total, with the error_location located in the middle
	const char *buf = query.c_str() + start_pos;
	idx_t len = end_pos - start_pos;
	vector<idx_t> render_widths;
	vector<idx_t> positions;
	if (Utf8Proc::IsValid(buf, len)) {
		// for unicode awareness, we traverse the graphemes of the current line and keep track of their render widths
		// and of their position in the string
		for (idx_t cpos = 0; cpos < len;) {
			auto char_render_width = Utf8Proc::RenderWidth(buf, len, cpos);
			positions.push_back(cpos);
			render_widths.push_back(char_render_width);
			cpos = Utf8Proc::NextGraphemeCluster(buf, len, cpos);
		}
	} else { // LCOV_EXCL_START
		// invalid utf-8, we can't do much at this point
		// we just assume every character is a character, and every character has a render width of 1
		for (idx_t cpos = 0; cpos < len; cpos++) {
			positions.push_back(cpos);
			render_widths.push_back(1);
		}
	} // LCOV_EXCL_STOP
	// now we want to find the (unicode aware) start and end position
	idx_t epos = 0;
	// start by finding the error location inside the array
	for (idx_t i = 0; i < positions.size(); i++) {
		if (positions[i] >= (error_location - start_pos)) {
			epos = i;
			break;
		}
	}
	bool truncate_beginning = false;
	bool truncate_end = false;
	idx_t spos = 0;
	// now we iterate backwards from the error location
	// we show max 40 render width before the error location
	idx_t current_render_width = 0;
	for (idx_t i = epos; i > 0; i--) {
		current_render_width += render_widths[i];
		if (current_render_width >= 40) {
			truncate_beginning = true;
			start_pos = positions[i];
			spos = i;
			break;
		}
	}
	// now do the same, but going forward
	current_render_width = 0;
	for (idx_t i = epos; i < positions.size(); i++) {
		current_render_width += render_widths[i];
		if (current_render_width >= 40) {
			truncate_end = true;
			end_pos = positions[i];
			break;
		}
	}
	string line_indicator = "LINE " + to_string(line_number) + ": ";
	string begin_trunc = truncate_beginning ? "..." : "";
	string end_trunc = truncate_end ? "..." : "";

	// get the render width of the error indicator (i.e. how many spaces we need to insert before the ^)
	idx_t error_render_width = 0;
	for (idx_t i = spos; i < epos; i++) {
		error_render_width += render_widths[i];
	}
	error_render_width += line_indicator.size() + begin_trunc.size();

	// now first print the error message plus the current line (or a subset of the line)
	string result = error_message;
	result += "\n" + line_indicator + begin_trunc + query.substr(start_pos, end_pos - start_pos) + end_trunc;
	// print an arrow pointing at the error location
	result += "\n" + string(error_render_width, ' ') + "^";
	return result;
}

string QueryErrorContext::FormatErrorRecursive(const string &msg, vector<ExceptionFormatValue> &values) {
	string error_message = values.empty() ? msg : ExceptionFormatValue::Format(msg, values);
	if (!statement || query_location >= statement->query.size()) {
		// no statement provided or query location out of range
		return error_message;
	}
	return Format(statement->query, error_message, query_location);
}

} // namespace duckdb
