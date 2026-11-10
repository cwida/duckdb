#include "duckdb/catalog/catalog_access_intent.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

string CatalogAccessIntent::ToString() const {
	static constexpr const char *NAMES[] = {"READ_METADATA", "READ_DATA",       "INSERT_DATA", "UPDATE_DATA",
	                                        "DELETE_DATA",   "MODIFY_METADATA", "DROP_ENTRY"};
	vector<string> set_bits;
	for (idx_t bit = 0; bit < sizeof(NAMES) / sizeof(NAMES[0]); bit++) {
		if (value & (1ULL << bit)) {
			set_bits.emplace_back(NAMES[bit]);
		}
	}
	if (set_bits.empty()) {
		return "NONE";
	}
	return StringUtil::Join(set_bits, " | ");
}

} // namespace duckdb
