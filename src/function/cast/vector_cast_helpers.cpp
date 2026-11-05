#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/stack.hpp"
#include "duckdb/common/typedefs.hpp"

namespace {

struct StringCastInputState {
public:
	StringCastInputState(const char *buf, duckdb::idx_t &pos, duckdb::idx_t &len) : buf(buf), pos(pos), len(len) {
	}

public:
	const char *buf;
	duckdb::idx_t &pos;
	duckdb::idx_t &len;
	bool escaped = false;
};

} // namespace

namespace duckdb {

// ------- Helper functions for splitting string nested types  -------
static bool IsNull(const char *buf, idx_t pos, idx_t end_pos) {
	if (pos + 4 != end_pos) {
		return false;
	}
	return StringUtil::CIEquals(buf + pos, 4, "null", 4);
}

inline static void SkipWhitespace(StringCastInputState &input_state) {
	auto &buf = input_state.buf;
	auto &pos = input_state.pos;
	auto &len = input_state.len;
	while (pos < len && StringUtil::CharacterIsSpace(buf[pos])) {
		pos++;
		input_state.escaped = false;
	}
}

static bool SkipToCloseQuotes(StringCastInputState &input_state) {
	auto &buf = input_state.buf;
	auto &pos = input_state.pos;
	auto &len = input_state.len;
	auto &escaped = input_state.escaped;

	char quote = buf[pos];
	pos++;

	while (pos < len) {
		bool set_escaped = false;
		if (buf[pos] == '\\') {
			if (!escaped) {
				set_escaped = true;
			}
		} else {
			if (buf[pos] == quote && !escaped) {
				return true;
			}
		}
		escaped = set_escaped;
		pos++;
	}
	return false;
}

static bool SkipToClose(StringCastInputState &input_state) {
	auto &idx = input_state.pos;
	auto &buf = input_state.buf;
	auto &len = input_state.len;

	D_ASSERT(buf[idx] == '{' || buf[idx] == '[' || buf[idx] == '(');

	vector<char> brackets;
	while (idx < len) {
		bool set_escaped = false;
		if (buf[idx] == '"' || buf[idx] == '\'') {
			if (!input_state.escaped) {
				if (!SkipToCloseQuotes(input_state)) {
					return false;
				}
			}
		} else if (buf[idx] == '{') {
			brackets.push_back('}');
		} else if (buf[idx] == '(') {
			brackets.push_back(')');
		} else if (buf[idx] == '[') {
			brackets.push_back(']');
		} else if (buf[idx] == brackets.back()) {
			brackets.pop_back();
			if (brackets.empty()) {
				return true;
			}
		} else if (buf[idx] == '\\') {
			//! Note that we don't treat `\\` special here, backslashes can't be escaped outside of quotes
			//! backslashes within quotes will not be encountered in this function
			set_escaped = true;
		}
		input_state.escaped = set_escaped;
		idx++;
	}
	return false;
}

struct CountPartOperation {
	idx_t count = 0;

	bool HandleKey(const char *buf, idx_t start_pos, idx_t pos) {
		count++;
		return true;
	}
	void HandleValue(const char *buf, idx_t start_pos, idx_t pos) {
		count++;
	}
};

template <bool RESPECT_SCOPES = true>
static string_t HandleString(Vector &vec, const char *buf, idx_t start, idx_t end) {
	D_ASSERT(start <= end);
	auto length = end - start;
	auto allocated_string = StringVector::EmptyString(vec, length);
	auto string_data = allocated_string.GetDataWriteable();
	uint32_t copied_count = 0;
	bool escaped = false;

	bool quoted = false;
	// Satisfy GCC warning about uninitialized variable
	char quote_char = '\0';
	stack<char> scopes;
	for (idx_t i = 0; i < length; i++) {
		auto current_char = buf[start + i];
		if (!escaped) {
			if (current_char == '\\') {
				if (quoted || (scopes.empty() && start + i + 1 < end &&
				               (buf[start + i + 1] == '\'' || buf[start + i + 1] == '"'))) {
					//! Start of escape
					escaped = true;
					if (!scopes.empty()) {
						string_data[copied_count++] = current_char;
					}
					continue;
				}
			}
			if (current_char == '\'' || current_char == '"') {
				if (quoted && current_char == quote_char) {
					quoted = false;
					if (scopes.empty()) {
						//! Skip the ending quote
						continue;
					}
				} else if (!quoted) {
					quoted = true;
					quote_char = current_char;
					if (scopes.empty()) {
						//! Skip the starting quote
						continue;
					}
				}
			}
			if (!quoted && !scopes.empty() && current_char == scopes.top()) {
				//! Close scope
				scopes.pop();
			}
			if (!quoted && (current_char == '[' || current_char == '{' || current_char == '(')) {
				if (RESPECT_SCOPES) {
					//! 'RESPECT_SCOPES' is false in things like STRUCT keys, these are regular strings
					//! New scope
					char end_char;
					if (current_char == '[') {
						end_char = ']';
					} else if (current_char == '{') {
						end_char = '}';
					} else {
						D_ASSERT(current_char == '(');
						end_char = ')';
					}
					scopes.push(end_char);
				}
			}
			//! Regular character
			string_data[copied_count++] = current_char;
		} else {
			string_data[copied_count++] = current_char;
			escaped = false;
		}
	}
	return string_t((const char *)string_data, copied_count); // NOLINT
}

// ------- LIST SPLIT -------
struct SplitStringListOperation {
public:
	SplitStringListOperation(string_t *child_data, idx_t &entry_count, Vector &child)
	    : child_data(child_data), entry_count(entry_count), child(child) {
	}

public:
	void HandleValue(const char *buf, idx_t start, idx_t end) {
		if (IsNull(buf, start, end)) {
			FlatVector::SetNull(child, entry_count, true);
			entry_count++;
			return;
		}
		child_data[entry_count] = HandleString(child, buf, start, end);
		entry_count++;
	}

private:
	string_t *child_data;
	idx_t &entry_count;
	Vector &child;
};

static inline bool ValueStateTransition(StringCastInputState &input_state, optional_idx &start_pos, idx_t &end_pos) {
	auto &buf = input_state.buf;
	auto &pos = input_state.pos;

	bool set_escaped = false;
	if (buf[pos] == '"' || buf[pos] == '\'') {
		if (!start_pos.IsValid()) {
			start_pos = pos;
		}
		if (!input_state.escaped) {
			if (!SkipToCloseQuotes(input_state)) {
				return false;
			}
		}
		end_pos = pos;
	} else if (buf[pos] == '{') {
		if (!start_pos.IsValid()) {
			start_pos = pos;
		}
		if (!SkipToClose(input_state)) {
			return false;
		}
		end_pos = pos;
	} else if (buf[pos] == '(') {
		if (!start_pos.IsValid()) {
			start_pos = pos;
		}
		if (!SkipToClose(input_state)) {
			return false;
		}
		end_pos = pos;
	} else if (buf[pos] == '[') {
		if (!start_pos.IsValid()) {
			start_pos = pos;
		}
		if (!SkipToClose(input_state)) {
			return false;
		}
		end_pos = pos;
	} else if (buf[pos] == '\\') {
		if (!start_pos.IsValid()) {
			start_pos = pos;
		}
		set_escaped = true;
		end_pos = pos;
	} else if (!StringUtil::CharacterIsSpace(buf[pos])) {
		if (!start_pos.IsValid()) {
			start_pos = pos;
		}
		end_pos = pos;
	}
	input_state.escaped = set_escaped;
	pos++;

	return true;
}

template <class OP>
static bool SplitStringListInternal(const string_t &input, OP &state) {
	const char *buf = input.GetData();
	idx_t len = input.GetSize();
	idx_t pos = 0;

	StringCastInputState input_state(buf, pos, len);

	SkipWhitespace(input_state);
	if (pos == len || buf[pos] != '[') {
		//! Does not have a valid list start
		return false;
	}

	//! Skip the '['
	pos++;
	SkipWhitespace(input_state);
	bool seen_value = false;
	while (pos < len) {
		optional_idx start_pos;
		idx_t end_pos;

		while (pos < len && (buf[pos] != ',' && buf[pos] != ']')) {
			if (!ValueStateTransition(input_state, start_pos, end_pos)) {
				return false;
			}
		}
		if (pos == len) {
			return false;
		}
		if (buf[pos] != ']' || start_pos.IsValid() || seen_value) {
			if (!start_pos.IsValid()) {
				state.HandleValue(buf, 0, 0);
			} else {
				auto start = start_pos.GetIndex();
				state.HandleValue(buf, start, end_pos + 1);
			}
			seen_value = true;
		}
		if (buf[pos] == ']') {
			break;
		}

		pos++;
		SkipWhitespace(input_state);
	}
	pos++;
	SkipWhitespace(input_state);
	return (pos == len);
}

bool VectorStringToList::SplitStringList(const string_t &input, string_t *child_data, idx_t &child_start,
                                         Vector &child) {
	SplitStringListOperation state(child_data, child_start, child);
	return SplitStringListInternal<SplitStringListOperation>(input, state);
}

idx_t VectorStringToList::CountPartsList(const string_t &input) {
	CountPartOperation state;
	SplitStringListInternal<CountPartOperation>(input, state);
	return state.count;
}

// ------- MAP SPLIT -------
struct SplitStringMapOperation {
	SplitStringMapOperation(string_t *child_key_data, string_t *child_val_data, idx_t &child_start, Vector &varchar_key,
	                        Vector &varchar_val)
	    : child_key_data(child_key_data), child_val_data(child_val_data), child_start(child_start),
	      varchar_key(varchar_key), varchar_val(varchar_val) {
	}

	string_t *child_key_data;
	string_t *child_val_data;
	idx_t &child_start;
	Vector &varchar_key;
	Vector &varchar_val;

	bool HandleKey(const char *buf, idx_t start_pos, idx_t pos) {
		if (IsNull(buf, start_pos, pos)) {
			FlatVector::SetNull(varchar_val, child_start, true);
			FlatVector::SetNull(varchar_key, child_start, true);
			child_start++;
			return false;
		}
		child_key_data[child_start] = HandleString(varchar_key, buf, start_pos, pos);
		return true;
	}

	void HandleValue(const char *buf, idx_t start_pos, idx_t pos) {
		if (IsNull(buf, start_pos, pos)) {
			FlatVector::SetNull(varchar_val, child_start, true);
			child_start++;
			return;
		}
		child_val_data[child_start] = HandleString(varchar_val, buf, start_pos, pos);
		child_start++;
	}
};

template <class OP>
static bool SplitStringMapInternal(const string_t &input, OP &state) {
	const char *buf = input.GetData();
	idx_t len = input.GetSize();
	idx_t pos = 0;
	StringCastInputState input_state(buf, pos, len);

	SkipWhitespace(input_state);
	if (pos == len || buf[pos] != '{') {
		return false;
	}
	pos++;
	SkipWhitespace(input_state);
	if (pos == len) {
		return false;
	}
	if (buf[pos] == '}') {
		pos++;
		SkipWhitespace(input_state);
		return pos == len;
	}

	while (pos < len) {
		optional_idx start_pos;
		idx_t end_pos;
		while (pos < len && buf[pos] != '=') {
			if (!ValueStateTransition(input_state, start_pos, end_pos)) {
				return false;
			}
		}
		if (pos == len) {
			return false;
		}
		if (!start_pos.IsValid()) {
			start_pos = 0;
			end_pos = 0;
		} else {
			end_pos++;
		}
		if (!state.HandleKey(buf, start_pos.GetIndex(), end_pos)) {
			return false;
		}
		start_pos = optional_idx();
		pos++;
		SkipWhitespace(input_state);
		while (pos < len && (buf[pos] != ',' && buf[pos] != '}')) {
			if (!ValueStateTransition(input_state, start_pos, end_pos)) {
				return false;
			}
		}
		if (pos == len) {
			return false;
		}
		if (!start_pos.IsValid()) {
			//! Value is empty
			state.HandleValue(buf, 0, 0);
		} else {
			state.HandleValue(buf, start_pos.GetIndex(), end_pos + 1);
		}
		if (buf[pos] == '}') {
			break;
		}
		pos++;
		SkipWhitespace(input_state);
	}
	pos++;
	SkipWhitespace(input_state);
	return (pos == len);
}

bool VectorStringToMap::SplitStringMap(const string_t &input, string_t *child_key_data, string_t *child_val_data,
                                       idx_t &child_start, Vector &varchar_key, Vector &varchar_val) {
	SplitStringMapOperation state(child_key_data, child_val_data, child_start, varchar_key, varchar_val);
	return SplitStringMapInternal<SplitStringMapOperation>(input, state);
}

idx_t VectorStringToMap::CountPartsMap(const string_t &input) {
	CountPartOperation state;
	SplitStringMapInternal<CountPartOperation>(input, state);
	return state.count;
}

// ------- STRUCT SPLIT -------
bool VectorStringToStruct::SplitStruct(const string_t &input, vector<Vector> &varchar_vectors, idx_t &row_idx,
                                       string_map_t<idx_t> &child_names, vector<reference<ValidityMask>> &child_masks) {
	const char *buf = input.GetData();
	idx_t len = input.GetSize();
	idx_t pos = 0;
	idx_t child_idx;

	Vector temp_vec(LogicalType::VARCHAR);
	StringCastInputState input_state(buf, pos, len);

	SkipWhitespace(input_state);
	if (pos == len || (buf[pos] != '{' && buf[pos] != '(')) {
		return false;
	}
	auto end_char = buf[pos] == '{' ? '}' : ')';
	pos++;
	SkipWhitespace(input_state);
	if (pos == len) {
		return false;
	}
	if (buf[pos] == end_char) {
		pos++;
		SkipWhitespace(input_state);
		return (pos == len);
	}

	if (end_char == '}') {
		//! Regular struct, in the form of `{name: value, name_2: value_2, ...}`
		vector<bool> seen_children(varchar_vectors.size(), false);
		while (pos < len) {
			optional_idx start_pos;
			idx_t end_pos;
			while (pos < len && buf[pos] != ':') {
				bool set_escaped = false;

				if (input_state.escaped) {
					if (!start_pos.IsValid()) {
						start_pos = pos;
					}
					end_pos = pos;
				} else if (buf[pos] == '"' || buf[pos] == '\'') {
					if (!start_pos.IsValid()) {
						start_pos = pos;
					}
					if (!SkipToCloseQuotes(input_state)) {
						return false;
					}
					end_pos = pos;
				} else if (buf[pos] == '\\') {
					if (!start_pos.IsValid()) {
						start_pos = pos;
					}
					set_escaped = true;
					end_pos = pos;
				} else if (!StringUtil::CharacterIsSpace(buf[pos])) {
					if (!start_pos.IsValid()) {
						start_pos = pos;
					}
					end_pos = pos;
				}
				input_state.escaped = set_escaped;
				pos++;
			}
			if (pos == len) {
				return false;
			}
			if (!start_pos.IsValid()) {
				//! Key can not be empty
				return false;
			}
			idx_t key_start = start_pos.GetIndex();
			end_pos++;
			if (IsNull(buf, key_start, end_pos)) {
				//! Key can not be NULL
				return false;
			}
			auto child_name = HandleString<false>(temp_vec, buf, key_start, end_pos);
			auto it = child_names.find(child_name);
			if (it == child_names.end()) {
				return false; // false key
			}
			child_idx = it->second;
			if (seen_children[child_idx]) {
				return false; // duplicate key
			}
			seen_children[child_idx] = true;

			start_pos = optional_idx();
			pos++;
			SkipWhitespace(input_state);
			while (pos < len && (buf[pos] != ',' && buf[pos] != '}')) {
				if (!ValueStateTransition(input_state, start_pos, end_pos)) {
					return false;
				}
			}
			if (pos == len) {
				return false;
			}
			auto &child_vec = varchar_vectors[child_idx];
			auto string_data = FlatVector::GetDataMutable<string_t>(child_vec);
			auto &child_mask = child_masks[child_idx].get();

			if (!start_pos.IsValid()) {
				start_pos = 0;
				end_pos = 0;
			} else {
				end_pos++;
			}
			auto value_start = start_pos.GetIndex();
			if (IsNull(buf, value_start, end_pos)) {
				child_mask.SetInvalid(row_idx);
			} else {
				string_data[row_idx] = HandleString(child_vec, buf, value_start, end_pos);
				child_mask.SetValid(row_idx);
			}

			if (buf[pos] == '}') {
				break;
			}
			pos++;
			SkipWhitespace(input_state);
		}
	} else {
		//! This is an unnamed struct in the form of `(value, value_2, ...)`
		D_ASSERT(end_char == ')');
		idx_t child_idx = 0;
		while (pos < len) {
			if (child_idx == child_masks.size()) {
				return false;
			}

			optional_idx start_pos;
			idx_t end_pos;
			while (pos < len && (buf[pos] != ',' && buf[pos] != ')')) {
				if (!ValueStateTransition(input_state, start_pos, end_pos)) {
					return false;
				}
			}
			if (pos == len) {
				return false;
			}
			auto &child_vec = varchar_vectors[child_idx];
			auto string_data = FlatVector::GetDataMutable<string_t>(child_vec);
			auto &child_mask = child_masks[child_idx].get();

			if (!start_pos.IsValid()) {
				start_pos = 0;
				end_pos = 0;
			} else {
				end_pos++;
			}
			auto value_start = start_pos.GetIndex();
			if (IsNull(buf, value_start, end_pos)) {
				child_mask.SetInvalid(row_idx);
			} else {
				string_data[row_idx] = HandleString(child_vec, buf, value_start, end_pos);
				child_mask.SetValid(row_idx);
			}

			if (buf[pos] == ')') {
				break;
			}
			child_idx++;
			pos++;
			SkipWhitespace(input_state);
			if (pos < len && buf[pos] == ')') {
				// allow a trailing comma, e.g. the single-element tuple "(1,)"
				break;
			}
		}
		(void)child_idx;
	}
	pos++;
	SkipWhitespace(input_state);
	return (pos == len);
}

} // namespace duckdb
