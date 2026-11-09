#include "duckdb/common/stacktrace.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#include <cxxabi.h>
#endif

namespace duckdb {

#if defined(__GLIBC__) || defined(__APPLE__)
static string UnmangleSymbol(string symbol) {
	// find the mangled name
	idx_t mangle_start = symbol.size();
	idx_t mangle_end = 0;
	for (idx_t i = 0; i < symbol.size(); ++i) {
		if (symbol[i] == '_') {
			mangle_start = i;
			break;
		}
	}
	for (idx_t i = mangle_start; i < symbol.size(); i++) {
		if (StringUtil::CharacterIsSpace(symbol[i]) || symbol[i] == ')' || symbol[i] == '+') {
			mangle_end = i;
			break;
		}
	}
	if (mangle_start >= mangle_end) {
		return symbol;
	}
	string mangled_symbol = symbol.substr(mangle_start, mangle_end - mangle_start);

	int status;
	auto demangle_result = abi::__cxa_demangle(mangled_symbol.c_str(), nullptr, nullptr, &status);
	if (status != 0 || !demangle_result) {
		return symbol;
	}
	string result;
	result += symbol.substr(0, mangle_start);
	result += demangle_result;
	result += symbol.substr(mangle_end);
	free(demangle_result);
	return result;
}

static string CleanupStackTrace(string symbol) {
#ifdef __APPLE__
	// structure of frame pointers is [depth] [library] [pointer] [symbol]
	// we are only interested in [depth] and [symbol]

	// find the depth
	idx_t start;
	for (start = 0; start < symbol.size(); start++) {
		if (!StringUtil::CharacterIsDigit(symbol[start])) {
			break;
		}
	}

	// now scan forward until we find the frame pointer
	idx_t frame_end = symbol.size();
	for (idx_t i = start; i + 1 < symbol.size(); ++i) {
		if (symbol[i] == '0' && symbol[i + 1] == 'x') {
			idx_t k;
			for (k = i + 2; k < symbol.size(); ++k) {
				if (!StringUtil::CharacterIsHex(symbol[k])) {
					break;
				}
			}
			frame_end = k;
			break;
		}
	}
	static constexpr idx_t STACK_TRACE_INDENTATION = 8;
	if (frame_end == symbol.size() || start >= STACK_TRACE_INDENTATION) {
		// frame pointer not found - just preserve the original frame
		return symbol;
	}
	idx_t space_count = STACK_TRACE_INDENTATION - start;
	return symbol.substr(0, start) + string(space_count, ' ') + symbol.substr(frame_end, symbol.size() - frame_end);
#else
	return symbol;
#endif
}

string StackTrace::GetStacktracePointers(idx_t max_depth) {
	string result;
	auto callstack = unique_ptr<void *[]>(new void *[max_depth]);
	int frames = backtrace(callstack.get(), NumericCast<int32_t>(max_depth));
	// skip two frames (these are always StackTrace::...)
	for (idx_t i = 2; i < NumericCast<idx_t>(frames); i++) {
		if (!result.empty()) {
			result += ";";
		}
		result += to_string(CastPointerToValue(callstack[i]));
	}
	return result;
}

string StackTrace::ResolveStacktraceSymbols(const string &pointers) {
	auto splits = StringUtil::Split(pointers, ";");
	idx_t frame_count = splits.size();
	auto callstack = unique_ptr<void *[]>(new void *[frame_count]);
	for (idx_t i = 0; i < frame_count; i++) {
		callstack[i] = cast_uint64_to_pointer(StringUtil::ToUnsigned(splits[i]));
	}
	char **strs = backtrace_symbols(callstack.get(), NumericCast<int>(frame_count));
	if (!strs) {
		// backtrace_symbols could not allocate, so the stack trace stays unresolved
		return string();
	}
	string result;
	for (idx_t i = 0; i < frame_count; i++) {
		result += CleanupStackTrace(UnmangleSymbol(strs[i]));
		result += "\n";
	}
	free(reinterpret_cast<void *>(strs));
	return "\n" + result;
}

#else
string StackTrace::GetStacktracePointers(idx_t max_depth) {
	return string();
}

string StackTrace::ResolveStacktraceSymbols(const string &pointers) {
	return string();
}
#endif

} // namespace duckdb
