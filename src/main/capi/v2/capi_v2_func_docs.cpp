#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_function_docs_set_description(duckdb_v2_function_docs_handle docs,
                                                        const duckdb_v2_str *description,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(docs);
	DUCKDB_CHECK_ARG(description);
	return WithErrorHandler(err, [&]() { Convert(docs)->description = duckdb::string(Convert(description)); });
}

DUCKDB_V2_ERROR duckdb_v2_function_docs_add_example(duckdb_v2_function_docs_handle docs, const duckdb_v2_str *title,
                                                    const duckdb_v2_str *example, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(docs);
	if (title) {
		DUCKDB_CHECK_ARG(title);
	}
	DUCKDB_CHECK_ARG(example);
	return WithErrorHandler(err, [&]() {
		duckdb::string title_text = title ? duckdb::string(Convert(title)) : duckdb::string();
		Convert(docs)->examples.push_back({std::move(title_text), duckdb::string(Convert(example))});
	});
}

DUCKDB_V2_ERROR duckdb_v2_function_docs_add_category(duckdb_v2_function_docs_handle docs, const duckdb_v2_str *category,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(docs);
	DUCKDB_CHECK_ARG(category);
	return WithErrorHandler(err, [&]() { Convert(docs)->categories.emplace_back(Convert(category)); });
}
