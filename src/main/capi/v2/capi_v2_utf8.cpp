#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "utf8proc_wrapper.hpp"

namespace duckdb::capiv2 {

auto ConvertIdentifierName(duckdb_v2_identifier_t name) -> std::string_view {
	auto bytes = Convert(name);
	if (!Utf8Proc::IsValid(bytes.data(), bytes.size())) {
		throw InvalidInputException("Identifier is not valid UTF-8");
	}
	return bytes;
}

} // namespace duckdb::capiv2

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_validate_utf8(const duckdb_v2_str *text, duckdb_v2_error_info_handle *err) {
	return WithErrorHandler(err, [&]() {
		auto bytes = Convert(text);
		if (!duckdb::Utf8Proc::IsValid(bytes.data(), bytes.size())) {
			throw duckdb::InvalidInputException("Input is not valid UTF-8");
		}
	});
}
