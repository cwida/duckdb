#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_function_docs_set_description(duckdb_v2_function_docs_handle docs, const char *description,
                                                        idx_t description_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(docs);
	DUCKDB_CHECK_STR_ARG(description, description_length);
	return WithErrorHandler(
	    err, [&]() { Convert(docs)->description = duckdb::string(Convert(description, description_length)); });
}

DUCKDB_V2_ERROR duckdb_v2_function_docs_add_example(duckdb_v2_function_docs_handle docs, const char *title,
                                                    idx_t title_length, const char *example, idx_t example_length,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(docs);
	DUCKDB_CHECK_STR_ARG(title, title_length);
	DUCKDB_CHECK_STR_ARG(example, example_length);
	return WithErrorHandler(err, [&]() {
		duckdb::string title_text(Convert(title, title_length));
		Convert(docs)->examples.push_back({std::move(title_text), duckdb::string(Convert(example, example_length))});
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_docs_add_category(duckdb_v2_function_docs_handle docs, const char *category,
                                                     idx_t category_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(docs);
	DUCKDB_CHECK_STR_ARG(category, category_length);
	return WithErrorHandler(err, [&]() { Convert(docs)->categories.emplace_back(Convert(category, category_length)); });
}
