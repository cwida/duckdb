#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/regexp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/common/pair.hpp"

#include "utf8proc_wrapper.hpp"
#include "utf8proc.hpp"

namespace duckdb {

struct StringSplitIterator {
public:
	explicit StringSplitIterator(idx_t size) : size(size) {
	}
	virtual ~StringSplitIterator() {
	}

	idx_t size;

public:
	virtual idx_t Next(const char *input) = 0;
	bool HasNext() {
		return offset < size;
	}
	idx_t Start() {
		return start;
	}

protected:
	idx_t start = 0;  // end of last place a delim match was found
	idx_t offset = 0; // current position
};

struct AsciiStringSplitIterator : virtual public StringSplitIterator {
public:
	AsciiStringSplitIterator(size_t size, const char *delim, const size_t delim_size)
	    : StringSplitIterator(size), delim(delim), delim_size(delim_size) {
	}
	idx_t Next(const char *input) override {
		// special case: separate by empty delimiter
		if (delim_size == 0) {
			offset++;
			start = offset;
			return offset;
		}
		for (offset = start; HasNext(); offset++) {
			// potential delimiter match
			if (input[offset] == delim[0] && offset + delim_size <= size) {
				idx_t i;
				for (i = 1; i < delim_size; i++) {
					if (input[offset + i] != delim[i]) {
						break;
					}
				}
				// delimiter found: skip start over delimiter
				if (i == delim_size) {
					start = offset + delim_size;
					return offset;
				}
			}
		}
		return offset;
	}

protected:
	const char *delim;
	size_t delim_size;
};

struct UnicodeStringSplitIterator : virtual public StringSplitIterator {
public:
	UnicodeStringSplitIterator(size_t input_size, const char *delim, const size_t delim_size)
	    : StringSplitIterator(input_size), delim_size(delim_size) {
		int cp_sz;
		for (idx_t i = 0; i < delim_size; i += cp_sz) {
			delim_cps.push_back(utf8proc_codepoint(delim, cp_sz));
		}
	}
	idx_t Next(const char *input) override {
		// special case: separate by empty delimiter
		if (delim_size == 0) {
			offset = utf8proc_next_grapheme(input, size, offset);
			start = offset;
			return offset;
		}
		int cp_sz;
		for (offset = start; HasNext(); offset = utf8proc_next_grapheme(input, size, offset)) {
			// potential delimiter match
			if (utf8proc_codepoint(&input[offset], cp_sz) == delim_cps[0] && offset + delim_size <= size) {
				idx_t delim_offset = cp_sz;
				for (idx_t i = 1; i < delim_cps.size(); i++) {
					if (utf8proc_codepoint(&input[offset + delim_offset], cp_sz) != delim_cps[i]) {
						break;
					}
					delim_offset += cp_sz;
				}
				// delimiter found: skip start over delimiter
				if (delim_offset == delim_size) {
					start = offset + delim_size;
					return offset;
				}
			}
		}
		return offset;
	}

protected:
	vector<utf8proc_int32_t> delim_cps;
	size_t delim_size;
};

struct RegexStringSplitIterator : virtual public StringSplitIterator {
public:
	RegexStringSplitIterator(size_t input_size, unique_ptr<RE2> re, const bool ascii_only)
	    : StringSplitIterator(input_size), re(move(re)), ascii_only(ascii_only) {
	}
	idx_t Next(const char *input) override {
		duckdb_re2::StringPiece input_sp(input, size);
		duckdb_re2::StringPiece match;
		if (re->Match(input_sp, start, size, RE2::UNANCHORED, &match, 1)) {
			offset = match.data() - input;
			// special case: 0 length match
			if (match.empty() && start < size) {
				if (ascii_only) {
					offset++;
				} else {
					offset = utf8proc_next_grapheme(input, size, offset);
				}
				start = offset;
			} else {
				start = offset + match.size();
			}
		} else {
			offset = size;
		}
		return offset;
	}

protected:
	unique_ptr<RE2> re;
	bool ascii_only;
};

void BaseStringSplitFunction(const char *input, StringSplitIterator &iter, Vector &result) {
	// special case: empty string
	if (iter.size == 0) {
		Value val = StringVector::AddString(ListVector::GetEntry(result), &input[0], 0);
		ListVector::PushBack(result, val);
		return;
	}
	while (iter.HasNext()) {
		idx_t start = iter.Start();
		idx_t end = iter.Next(input);
		size_t length = end - start;
		Value to_insert(StringVector::AddString(ListVector::GetEntry(result), &input[start], length));
		ListVector::PushBack(result, to_insert);
	}
}

unique_ptr<Vector> BaseStringSplitFunction(string_t input, string_t delim, const bool regex) {
	const char *input_data = input.GetDataUnsafe();
	size_t input_size = input.GetSize();
	const char *delim_data = delim.GetDataUnsafe();
	size_t delim_size = delim.GetSize();

	bool ascii_only = Utf8Proc::Analyze(input_data, input_size) == UnicodeType::ASCII;

	auto list_type = LogicalType::LIST(LogicalType::VARCHAR);
	auto output = make_unique<Vector>(list_type);
	unique_ptr<StringSplitIterator> iter;
	if (regex) {
		auto re = make_unique<RE2>(duckdb_re2::StringPiece(delim_data, delim_size));
		if (!re->ok()) {
			throw Exception(re->error());
		}
		iter = make_unique_base<StringSplitIterator, RegexStringSplitIterator>(input_size, move(re), ascii_only);
	} else if (ascii_only) {
		iter = make_unique_base<StringSplitIterator, AsciiStringSplitIterator>(input_size, delim_data, delim_size);
	} else {
		iter = make_unique_base<StringSplitIterator, UnicodeStringSplitIterator>(input_size, delim_data, delim_size);
	}
	BaseStringSplitFunction(input_data, *iter, *output);

	return output;
}

static void StringSplitExecutor(DataChunk &args, ExpressionState &state, Vector &result, const bool regex) {
	VectorData input_data;
	args.data[0].Orrify(args.size(), input_data);
	auto inputs = (string_t *)input_data.data;

	VectorData delim_data;
	args.data[1].Orrify(args.size(), delim_data);
	auto delims = (string_t *)delim_data.data;

	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	ListVector::SetListSize(result, 0);

	auto list_struct_data = FlatVector::GetData<list_entry_t>(result);
	auto list_vector_type = LogicalType::LIST(LogicalType::VARCHAR);

	idx_t total_len = 0;
	for (idx_t i = 0; i < args.size(); i++) {
		auto input_idx = input_data.sel->get_index(i);
		auto delim_idx = delim_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(input_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		string_t input = inputs[input_idx];

		unique_ptr<Vector> split_input;
		if (!delim_data.validity.RowIsValid(delim_idx)) {
			// special case: delimiter is NULL
			split_input = make_unique<Vector>(list_vector_type);
			Value val(input);
			ListVector::PushBack(*split_input, val);
		} else {
			string_t delim = delims[delim_idx];
			split_input = BaseStringSplitFunction(input, delim, regex);
		}
		list_struct_data[i].length = ListVector::GetListSize(*split_input);
		list_struct_data[i].offset = total_len;
		total_len += ListVector::GetListSize(*split_input);
		ListVector::Append(result, ListVector::GetEntry(*split_input), ListVector::GetListSize(*split_input));
	}

	D_ASSERT(ListVector::GetListSize(result) == total_len);
	if (args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR &&
	    args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void StringSplitFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	StringSplitExecutor(args, state, result, false);
}

static void StringSplitRegexFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	StringSplitExecutor(args, state, result, true);
}

void StringSplitFun::RegisterFunction(BuiltinFunctions &set) {
	auto varchar_list_type = LogicalType::LIST(LogicalType::VARCHAR);

	set.AddFunction(
	    {"string_split", "str_split", "string_to_array", "split"},
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, varchar_list_type, StringSplitFunction));
	set.AddFunction(
	    {"string_split_regex", "str_split_regex", "regexp_split_to_array"},
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, varchar_list_type, StringSplitRegexFunction));
}

} // namespace duckdb
