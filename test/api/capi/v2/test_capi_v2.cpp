#include "test_capi_v2.hpp"

// Include the internal header so we can construct internal objects manually for testing
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

namespace test_capi_v2 {

DUCKDB_V2_ERROR SetErrorInfo(duckdb_v2_error_info_handle *err, DUCKDB_V2_ERROR code, const char *msg) {
	if (err) {
		if (!*err) {
			*err = duckdb::capiv2::Convert(new duckdb::capiv2::CV2ErrorInfo());
		}
		auto &info = *duckdb::capiv2::Convert(*err);
		info.code = code;
		info.message = msg ? msg : "";
		// Directly-set message has no body; clear any from a prior failure.
		info.raw_message.reset();
	}
	return code;
}

} // namespace test_capi_v2
