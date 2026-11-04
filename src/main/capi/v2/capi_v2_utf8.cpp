#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "utf8proc_wrapper.hpp"

namespace duckdb::capiv2 {

auto ConvertIdentifierName(const char *name, idx_t name_length) -> std::string_view {
	auto bytes = Convert(name, name_length);
	if (!Utf8Proc::IsValid(bytes.data(), bytes.size())) {
		throw InvalidInputException("Identifier is not valid UTF-8");
	}
	return bytes;
}

std::string_view string_view_array::operator[](idx_t i) const {
	D_ASSERT(i < count);
	if (!ptrs) {
		return std::string_view();
	}
	if (!lengths) {
		throw InvalidInputException("string array lengths cannot be null when the array is not null");
	}
	if (!ptrs[i]) {
		if (lengths[i] > 0) {
			throw InvalidInputException("string array entry %llu is null with a non-zero length", i);
		}
		return std::string_view();
	}
	if (!Utf8Proc::IsValid(ptrs[i], lengths[i])) {
		throw InvalidInputException("string array entry %llu is not valid UTF-8", i);
	}
	return std::string_view(ptrs[i], lengths[i]);
}

} // namespace duckdb::capiv2

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_validate_utf8(const char *text, idx_t text_length, duckdb_v2_error_info_handle *err) {
	return WithErrorHandler(err, [&]() {
		auto bytes = Convert(text, text_length);
		if (!duckdb::Utf8Proc::IsValid(bytes.data(), bytes.size())) {
			throw duckdb::InvalidInputException("Input is not valid UTF-8");
		}
	});
}
