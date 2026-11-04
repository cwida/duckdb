#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/common/local_file_system.hpp"
#include <iostream>

namespace duckdb {

static string GetSeparator(const string_t &input) {
	string option = input.GetString();

	// system's path separator
	auto fs = FileSystem::CreateLocal();
	auto system_sep = fs->PathSeparator(option);

	string separator;
	if (option == "system") {
		separator = system_sep;
	} else if (option == "forward_slash") {
		separator = "/";
	} else if (option == "backslash") {
		separator = "\\";
	} else { // both_slash (default)
		separator = "/\\";
	}
	return separator;
}

static bool IsIdxValid(const idx_t &i, const idx_t &sentence_size) {
	if (i > sentence_size || i == DConstants::INVALID_INDEX) {
		return false;
	}
	return true;
}

static idx_t Find(const char *input_data, idx_t input_size, const string &sep_data) {
	if (sep_data.empty()) {
		return 0;
	}
	auto pos = FindStrInStr(const_uchar_ptr_cast(input_data), input_size, const_uchar_ptr_cast(&sep_data[0]), 1);
	// both_slash option
	if (sep_data.size() > 1) {
		auto sec_pos =
		    FindStrInStr(const_uchar_ptr_cast(input_data), input_size, const_uchar_ptr_cast(&sep_data[1]), 1);
		// choose the leftmost valid position
		if (sec_pos != DConstants::INVALID_INDEX && (sec_pos < pos || pos == DConstants::INVALID_INDEX)) {
			return sec_pos;
		}
	}
	return pos;
}

static idx_t FindLast(const char *data_ptr, idx_t input_size, const string &sep_data) {
	idx_t start = 0;
	while (input_size > 0) {
		auto pos = Find(data_ptr, input_size, sep_data);
		if (!IsIdxValid(pos, input_size)) {
			break;
		}
		start += (pos + 1);
		data_ptr += (pos + 1);
		input_size -= (pos + 1);
	}
	if (start < 1) {
		return DConstants::INVALID_INDEX;
	}
	return start - 1;
}

template <class CB>
static idx_t SplitPath(string_t input, const string &sep, CB &&emit) {
	auto input_data = input.GetData();
	auto input_size = input.GetSize();
	if (!input_size) {
		return 0;
	}
	idx_t list_idx = 0;
	while (input_size > 0) {
		auto pos = Find(input_data, input_size, sep);
		if (!IsIdxValid(pos, input_size)) {
			break;
		}

		D_ASSERT(input_size >= pos);
		if (pos == 0) {
			if (list_idx == 0) { // first character in path is separator
				emit(input_data, 1);
				list_idx++;
				if (input_size == 1) { // special case: the only character in path is a separator
					return list_idx;
				}
			} // else: separator is in the path
		} else {
			emit(input_data, pos);
			list_idx++;
		}
		input_data += (pos + 1);
		input_size -= (pos + 1);
	}
	if (input_size > 0) {
		emit(input_data, input_size);
		list_idx++;
	}
	return list_idx;
}

//! The optional separator and trim_extension arguments - NULL values use the default options
struct PathOptionArguments {
	optional_ptr<const Vector> separator;
	optional_ptr<const Vector> trim_extension;
};

static PathOptionArguments GetPathOptionArguments(DataChunk &args, const bool front_trim) {
	PathOptionArguments result;
	switch (args.ColumnCount()) {
	case 1:
		// use default values
		break;
	case 2:
		switch (args.data[1].GetType().id()) {
		case LogicalTypeId::VARCHAR:
			result.separator = args.data[1];
			break;
		case LogicalTypeId::BOOLEAN: // parse_path and parse_dirname won't get in here
			result.trim_extension = args.data[1];
			break;
		default:
			throw InvalidInputException("Invalid argument type");
		}
		break;
	case 3:
		if (front_trim) {
			throw InvalidInputException("Invalid number of arguments");
		}
		result.trim_extension = args.data[1];
		result.separator = args.data[2];
		break;
	default:
		throw InvalidInputException("Invalid number of arguments");
	}
	return result;
}

template <class OP>
static void ExecutePathFunction(DataChunk &args, Vector &result, const bool front_trim, OP &&op) {
	const auto count = args.size();
	auto options = GetPathOptionArguments(args, front_trim);
	Vector default_separator(string_t("default"), count_t(count));
	Vector default_trim_extension(Value::BOOLEAN(false), count_t(count));
	auto paths = args.data[0].Values<string_t>();
	auto separators = (options.separator ? *options.separator : default_separator).Values<string_t>();
	auto trim_extensions = (options.trim_extension ? *options.trim_extension : default_trim_extension).Values<bool>();

	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto path = paths[i];
		if (!path.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto separator = separators[i];
		auto sep = GetSeparator(separator.IsValid() ? separator.GetValue() : string_t("default"));
		auto trim_extension = trim_extensions[i];
		writer.WriteValue(op(path.GetValue(), sep, trim_extension.IsValid() && trim_extension.GetValue()));
	}
}

template <bool FRONT_TRIM>
static void TrimPathFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecutePathFunction(args, result, FRONT_TRIM, [&](const string_t &input, const string &sep, bool trim_extension) {
		auto data = input.GetData();
		auto input_size = input.GetSize();

		// find the beginning idx and the size of the result string
		idx_t begin = 0;
		idx_t new_size = input_size;
		if (FRONT_TRIM) { // left trim
			auto pos = Find(data, input_size, sep);
			if (pos == 0) { // path starts with separator
				pos = 1;
			}
			new_size = (IsIdxValid(pos, input_size)) ? pos : 0;
		} else { // right trim
			auto idx_last_sep = FindLast(data, input_size, sep);
			if (IsIdxValid(idx_last_sep, input_size)) {
				begin = idx_last_sep + 1;
			}
			if (trim_extension) {
				auto idx_extension_sep = FindLast(data, input_size, ".");
				if (begin <= idx_extension_sep && IsIdxValid(idx_extension_sep, input_size)) {
					new_size = idx_extension_sep;
				}
			}
		}
		D_ASSERT(begin <= new_size);
		return string_t(data + begin, UnsafeNumericCast<uint32_t>(new_size - begin));
	});
}

static void ParseDirpathFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecutePathFunction(args, result, true, [&](const string_t &input, const string &sep, bool) {
		auto path = input.GetData();
		auto path_size = input.GetSize();

		auto last_sep = FindLast(path, path_size, sep);
		if (last_sep == 0 && path_size == 1) {
			last_sep = 1;
		}
		idx_t new_size = (IsIdxValid(last_sep, path_size)) ? last_sep : 0;
		return string_t(path, UnsafeNumericCast<uint32_t>(new_size));
	});
}

static void ParsePathFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1 || args.ColumnCount() == 2);
	auto inputs = args.data[0].Values<string_t>();
	Vector default_separator(string_t("default"), count_t(args.size()));
	const auto &separator = args.ColumnCount() == 2 ? args.data[1] : default_separator;
	auto separator_values = separator.Values<string_t>();
	const auto constant_separator = separator.GetVectorType() == VectorType::CONSTANT_VECTOR;
	auto read_separator = [&](idx_t row_idx) {
		auto entry = separator_values[row_idx];
		return GetSeparator(entry.IsValid() ? entry.GetValue() : string_t("default"));
	};
	string sep;
	if (constant_separator) {
		sep = read_separator(0);
	}

	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto list_writer = FlatVector::Writer<VectorListType<string_t>>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		auto input = inputs[i];
		if (!input.IsValid()) {
			list_writer.WriteNull();
			continue;
		}
		if (!constant_separator) {
			sep = read_separator(i);
		}
		auto list = list_writer.WriteDynamicList();
		SplitPath(input.GetValue(), sep, [&](const char *split_data, idx_t split_size) {
			list.WriteElement().WriteValue(string_t(split_data, UnsafeNumericCast<uint32_t>(split_size)));
		});
	}
}

ScalarFunctionSet ParseDirnameFun::GetFunctions() {
	ScalarFunctionSet parse_dirname;
	ScalarFunction func({}, LogicalType::VARCHAR, TrimPathFunction<true>, nullptr, nullptr, nullptr,
	                    LogicalType::INVALID, FunctionStability::CONSISTENT, FunctionNullHandling::SPECIAL_HANDLING);
	func.GetSignature().AddParameter("path", LogicalType::VARCHAR);
	parse_dirname.AddFunction(func);
	// separator options
	func.GetSignature().AddParameter("separator", LogicalType::VARCHAR);
	parse_dirname.AddFunction(func);
	return parse_dirname;
}

ScalarFunctionSet ParseDirpathFun::GetFunctions() {
	ScalarFunctionSet parse_dirpath;
	ScalarFunction func({}, LogicalType::VARCHAR, ParseDirpathFunction, nullptr, nullptr, nullptr, LogicalType::INVALID,
	                    FunctionStability::CONSISTENT, FunctionNullHandling::SPECIAL_HANDLING);
	func.GetSignature().AddParameter("path", LogicalType::VARCHAR);
	parse_dirpath.AddFunction(func);
	// separator options
	func.GetSignature().AddParameter("separator", LogicalType::VARCHAR);
	parse_dirpath.AddFunction(func);
	return parse_dirpath;
}

ScalarFunctionSet ParseFilenameFun::GetFunctions() {
	ScalarFunctionSet parse_filename;

	ScalarFunction base({}, LogicalType::VARCHAR, TrimPathFunction<false>, nullptr, nullptr, nullptr,
	                    LogicalType::INVALID, FunctionStability::CONSISTENT, FunctionNullHandling::SPECIAL_HANDLING);
	base.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	parse_filename.AddFunction(base);

	ScalarFunction with_separator({}, LogicalType::VARCHAR, TrimPathFunction<false>, nullptr, nullptr, nullptr,
	                              LogicalType::INVALID, FunctionStability::CONSISTENT,
	                              FunctionNullHandling::SPECIAL_HANDLING);
	with_separator.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("separator", LogicalType::VARCHAR);
	parse_filename.AddFunction(with_separator);

	ScalarFunction with_trim({}, LogicalType::VARCHAR, TrimPathFunction<false>, nullptr, nullptr, nullptr,
	                         LogicalType::INVALID, FunctionStability::CONSISTENT,
	                         FunctionNullHandling::SPECIAL_HANDLING);
	with_trim.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("trim_extension", LogicalType::BOOLEAN);
	parse_filename.AddFunction(with_trim);

	ScalarFunction with_trim_and_separator({}, LogicalType::VARCHAR, TrimPathFunction<false>, nullptr, nullptr, nullptr,
	                                       LogicalType::INVALID, FunctionStability::CONSISTENT,
	                                       FunctionNullHandling::SPECIAL_HANDLING);
	with_trim_and_separator.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("trim_extension", LogicalType::BOOLEAN)
	    .AddParameter("separator", LogicalType::VARCHAR);
	parse_filename.AddFunction(with_trim_and_separator);

	return parse_filename;
}

ScalarFunctionSet ParsePathFun::GetFunctions() {
	auto varchar_list_type = LogicalType::LIST(LogicalType::VARCHAR);
	ScalarFunctionSet parse_path;
	ScalarFunction func({}, varchar_list_type, ParsePathFunction, nullptr, nullptr, nullptr, LogicalType::INVALID,
	                    FunctionStability::CONSISTENT, FunctionNullHandling::SPECIAL_HANDLING);
	func.GetSignature().AddParameter("path", LogicalType::VARCHAR);
	parse_path.AddFunction(func);
	// separator options
	func.GetSignature().AddParameter("separator", LogicalType::VARCHAR);
	parse_path.AddFunction(func);
	return parse_path;
}

} // namespace duckdb
