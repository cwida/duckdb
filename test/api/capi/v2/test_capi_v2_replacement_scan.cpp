#include "test_capi_v2.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 replacement scan tests: a callback the binder consults when a table name
// cannot be resolved, which claims the name by naming a table function, a
// column data collection, or a query to read instead.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. They latch what they observe into
// file-scope statics, which the test asserts on after the query.
//
// All file-scope names carry a Repl prefix: this directory is a unity build,
// so they must be unique across test files.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t ReplIdent(const char *s) {
	return Convert(s);
}

// What the callback saw, for the test to assert on afterwards.
struct ReplObserved {
	std::vector<std::string> parts;
	std::string rendered;
	int calls = 0;
	// Return codes latched from calls the callback expects to fail.
	DUCKDB_V2_ERROR mixed_form_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR bare_argument_rc = DUCKDB_V2_ERROR_NONE;
};
ReplObserved repl_observed;

void ReplReset() {
	repl_observed = ReplObserved();
}

// Reads the unresolved name into the observation record. The name is owned, so it is destroyed here.
void ReplRecordName(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_qname_handle name = nullptr;
	if (duckdb_v2_replacement_scan_get_name(info, &name, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	repl_observed.calls++;
	repl_observed.parts.clear();

	idx_t count = 0;
	if (duckdb_v2_qname_get_part_count(name, &count, err) == DUCKDB_V2_ERROR_NONE) {
		for (idx_t i = 0; i < count; i++) {
			duckdb_v2_identifier_t part = {nullptr, 0};
			if (duckdb_v2_qname_get_part(name, i, &part, err) != DUCKDB_V2_ERROR_NONE) {
				break;
			}
			repl_observed.parts.push_back(Convert(part));
		}
	}
	idx_t length = 0;
	if (duckdb_v2_qname_render(name, nullptr, 0, &length, err) == DUCKDB_V2_ERROR_NONE) {
		std::vector<char> buffer(length + 1, '\0');
		if (duckdb_v2_qname_render(name, buffer.data(), buffer.size(), &length, err) == DUCKDB_V2_ERROR_NONE) {
			repl_observed.rendered = std::string(buffer.data(), length);
		}
	}
	duckdb_v2_qname_destroy(&name);
}

// Claims by an unqualified function name, which now means building a one-part qualified name.
DUCKDB_V2_ERROR ReplClaimFunction(duckdb_v2_replacement_scan_info_handle info, const char *function_name,
                                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_identifier_t parts[1] = {Convert(function_name)};
	duckdb_v2_qname_handle name = nullptr;
	auto rc = duckdb_v2_qname_create(parts, 1, &name, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	rc = duckdb_v2_replacement_scan_set_function_name(info, name, err);
	duckdb_v2_qname_destroy(&name);
	return rc;
}

// Claims every name with range(2).
void ReplClaimRange(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle context,
                    duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	ReplRecordName(info, err);
	if (ReplClaimFunction(info, "range", err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_value_create_bigint(factory, 2, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto rc = duckdb_v2_replacement_scan_add_argument(info, value, err);
	// Destroyed immediately: the argument is copied at the call.
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto alias_str = ReplIdent("claimed");
	duckdb_v2_replacement_scan_set_alias(info, &alias_str, err);
}

// Records the name and declines.
void ReplDecline(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	ReplRecordName(info, err);
}

// Claims only names ending in ".csv", so it can be shown to outrank the built-in CSV scan.
void ReplClaimCsv(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle context,
                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_qname_handle qname = nullptr;
	if (duckdb_v2_replacement_scan_get_name(info, &qname, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_identifier_t part = {nullptr, 0};
	auto rc = duckdb_v2_qname_get_part(qname, 0, &part, err);
	const auto name = rc == DUCKDB_V2_ERROR_NONE ? Convert(part) : std::string();
	duckdb_v2_qname_destroy(&qname);
	if (name.size() < 4 || name.compare(name.size() - 4, 4, ".csv") != 0) {
		return;
	}
	ReplClaimRange(info, context, err);
}

// Fails with a specific code.
void ReplFail(duckdb_v2_replacement_scan_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("the replacement scan refused");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// Claims a function that does not exist.
void ReplClaimUnknown(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	ReplClaimFunction(info, "no_such_table_function", err);
}

// Exercises the claim-form rules, then claims by function.
void ReplClaimRules(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle context,
                    duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	// An argument before a function name is refused. The error slot is not touched by a failing call, so these
	// probes pass nullptr and read the return code instead.
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_value_create_bigint(factory, 1, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	repl_observed.bare_argument_rc = duckdb_v2_replacement_scan_add_argument(info, value, nullptr);
	duckdb_v2_value_destroy(&value);

	// A first claim, then the same form again (allowed, last wins), then a different form (refused).
	if (ReplClaimFunction(info, "not_this_one", err) != DUCKDB_V2_ERROR_NONE ||
	    ReplClaimFunction(info, "range", err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto sql_str = Convert("SELECT 1");
	repl_observed.mixed_form_rc = duckdb_v2_replacement_scan_set_subquery(info, &sql_str, nullptr);

	duckdb_v2_value_handle two = nullptr;
	if (duckdb_v2_value_create_bigint(factory, 2, &two, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_replacement_scan_add_argument(info, two, err);
	duckdb_v2_value_destroy(&two);
}

// Claims with a subquery.
void ReplClaimSubquery(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	auto sql_str = Convert("SELECT 41 + 1 AS v");
	duckdb_v2_replacement_scan_set_subquery(info, &sql_str, err);
}

// Latches the rejections the subquery form makes.
DUCKDB_V2_ERROR repl_multi_statement_rc = DUCKDB_V2_ERROR_NONE;
DUCKDB_V2_ERROR repl_non_select_rc = DUCKDB_V2_ERROR_NONE;
DUCKDB_V2_ERROR repl_bad_syntax_rc = DUCKDB_V2_ERROR_NONE;

void ReplClaimBadSubqueries(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle,
                            duckdb_v2_error_info_handle *err) {
	auto sql_str = Convert("SELECT 1; SELECT 2");
	repl_multi_statement_rc = duckdb_v2_replacement_scan_set_subquery(info, &sql_str, nullptr);
	auto sql_str2 = Convert("CREATE TABLE x (i INTEGER)");
	repl_non_select_rc = duckdb_v2_replacement_scan_set_subquery(info, &sql_str2, nullptr);
	auto sql_str3 = Convert("SELECT FROM WHERE");
	repl_bad_syntax_rc = duckdb_v2_replacement_scan_set_subquery(info, &sql_str3, nullptr);
	// Still claim something valid, so the query itself succeeds.
	auto sql_str4 = Convert("SELECT 7 AS v");
	duckdb_v2_replacement_scan_set_subquery(info, &sql_str4, err);
}

// Counts how many times a user data destructor ran.
int repl_destroy_calls = 0;
void ReplDestroyUserData(void *data) {
	repl_destroy_calls++;
	delete static_cast<std::string *>(data);
}

// Reads the user data and claims range(2) when it is what was planted.
void ReplClaimWithUserData(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle context,
                           duckdb_v2_error_info_handle *err) {
	void *user_data = nullptr;
	if (duckdb_v2_replacement_scan_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *tag = static_cast<std::string *>(user_data);
	if (!tag || *tag != "planted") {
		duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
		auto text_str = Convert("user data did not reach the replacement scan");
		duckdb_v2_error_info_set_text(*err, &text_str);
		return;
	}
	ReplClaimRange(info, context, err);
}

// Creates a scan on the connection with the given callback, registers it, and destroys the handle.
void ReplRegisterOnConnection(duckdb_v2_connection_handle conn, duckdb_v2_replacement_scan_callback_fn callback) {
	duckdb_v2_replacement_scan_handle scan = nullptr;
	REQUIRE(duckdb_v2_replacement_scan_create(FactoryOf(conn), &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_set_callback(scan, callback, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_replacement_scan(conn, scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_destroy(&scan) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(scan == nullptr);
}

// Collects a single BIGINT column.
std::vector<int64_t> ReplQueryI64(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	std::vector<int64_t> out;
	while (auto chunk = StepChunk(result)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		duckdb_v2_vector_view view {};
		duckdb_v2_vector_get_view(vec, &view, nullptr);
		for (idx_t i = 0; i < size; i++) {
			out.push_back(static_cast<const int64_t *>(view.data)[SelAt(view.sel, i)]);
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
	return out;
}

// Runs a query to exhaustion, returning the code the failure surfaced with.
DUCKDB_V2_ERROR ReplQueryError(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	auto rc = Query(conn, sql, &result);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_result_destroy(&result);
		return rc;
	}
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED ||
		    status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	duckdb_v2_result_destroy(&result);
	return rc;
}

// ---------------------------------------------------------------------------
// The collection claim. This is how a caller makes its own buffered data
// queryable: the scan's user data holds a name -> collection registry, and the
// callback hands back whichever collection matches the unresolved name.
// ---------------------------------------------------------------------------

// A single-column BIGINT collection holding the given values.
duckdb_v2_column_data_collection_handle ReplMakeCollection(duckdb_v2_factory_handle factory,
                                                           const std::vector<int64_t> &values) {
	auto bigint = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_logical_type_handle types[1] = {bigint};

	duckdb_v2_column_data_collection_handle cdc = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_create(factory, types, 1, &cdc, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto chunk_rc = duckdb_v2_data_chunk_create(factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&bigint);
	REQUIRE(chunk_rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, values.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	if (!values.empty()) {
		void *raw = nullptr;
		REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
		std::memcpy(raw, values.data(), values.size() * sizeof(int64_t));
	}

	duckdb_v2_column_data_collection_append_state_handle state = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_append_state_create(cdc, &state, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto append_rc = duckdb_v2_column_data_collection_append(cdc, state, chunk, nullptr);
	duckdb_v2_column_data_collection_append_state_destroy(&state);
	duckdb_v2_data_chunk_destroy(&chunk);
	REQUIRE(append_rc == DUCKDB_V2_ERROR_NONE);
	return cdc;
}

// What a caller would keep to make its collections queryable by name.
struct ReplRegistry {
	std::string name;
	duckdb_v2_column_data_collection_handle collection = nullptr;
	std::vector<std::string> column_names;
	// Latched from the calls the test expects the setter to refuse.
	DUCKDB_V2_ERROR wrong_count_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR empty_name_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR null_names_rc = DUCKDB_V2_ERROR_NONE;
	bool probe_refusals = false;
};

void ReplClaimCollection(duckdb_v2_replacement_scan_info_handle info, duckdb_v2_context_handle,
                         duckdb_v2_error_info_handle *err) {
	void *user_data = nullptr;
	duckdb_v2_qname_handle qname = nullptr;
	if (duckdb_v2_replacement_scan_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_replacement_scan_get_name(info, &qname, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_identifier_t part = {nullptr, 0};
	idx_t part_count = 0;
	duckdb_v2_qname_get_part_count(qname, &part_count, err);
	auto part_rc = duckdb_v2_qname_get_part(qname, 0, &part, err);
	const auto table_name = part_rc == DUCKDB_V2_ERROR_NONE ? Convert(part) : std::string();
	duckdb_v2_qname_destroy(&qname);

	auto &registry = *static_cast<ReplRegistry *>(user_data);
	if (part_count != 1 || table_name != registry.name) {
		return; // not ours: decline
	}

	std::vector<duckdb_v2_identifier_t> names;
	for (auto &name : registry.column_names) {
		names.push_back(Convert(name));
	}

	if (registry.probe_refusals) {
		duckdb_v2_identifier_t too_many[3] = {Convert("a"), Convert("b"), Convert("c")};
		registry.wrong_count_rc =
		    duckdb_v2_replacement_scan_set_collection(info, registry.collection, too_many, 3, nullptr);
		duckdb_v2_identifier_t empty[1] = {Convert("")};
		registry.empty_name_rc =
		    duckdb_v2_replacement_scan_set_collection(info, registry.collection, empty, 1, nullptr);
		registry.null_names_rc =
		    duckdb_v2_replacement_scan_set_collection(info, registry.collection, nullptr, 1, nullptr);
	}

	duckdb_v2_replacement_scan_set_collection(info, registry.collection, names.empty() ? nullptr : names.data(),
	                                          names.size(), err);
}

// Registers a collection-serving scan whose user data is the caller-owned registry.
void ReplRegisterRegistry(duckdb_v2_connection_handle conn, ReplRegistry &registry) {
	duckdb_v2_replacement_scan_handle scan = nullptr;
	REQUIRE(duckdb_v2_replacement_scan_create(FactoryOf(conn), &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_set_callback(scan, ReplClaimCollection, nullptr) == DUCKDB_V2_ERROR_NONE);
	// The registry outlives the database, so nothing to destroy.
	duckdb_v2_opaque user_data = {&registry, nullptr, nullptr};
	REQUIRE(duckdb_v2_replacement_scan_set_user_data(scan, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_replacement_scan(conn, scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_replacement_scan_destroy(&scan);
}

} // namespace

TEST_CASE("V2 replacement scan: claims a table function", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	ReplReset();
	ReplRegisterOnConnection(fx.conn, ReplClaimRange);

	REQUIRE(ReplQueryI64(fx.conn, "SELECT * FROM not_a_table") == std::vector<int64_t> {0, 1});
	REQUIRE(repl_observed.calls == 1);
	REQUIRE(repl_observed.parts == std::vector<std::string> {"not_a_table"});
	// The scan's alias names the binding, so a qualified reference resolves.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT claimed.range FROM not_a_table") == std::vector<int64_t> {0, 1});
	// A query-written alias wins over the scan's.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT t.range FROM not_a_table AS t") == std::vector<int64_t> {0, 1});
}

TEST_CASE("V2 replacement scan: reports the unresolved name and can decline", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	ReplReset();
	ReplRegisterOnConnection(fx.conn, ReplDecline);

	// Declining leaves the reference unresolved, so the normal catalog error surfaces.
	REQUIRE(ReplQueryError(fx.conn, "SELECT * FROM missing_table") != DUCKDB_V2_ERROR_NONE);
	REQUIRE(repl_observed.calls == 1);
	// An unqualified reference is a single part: absence is a shorter path, never an empty placeholder.
	REQUIRE(repl_observed.parts == std::vector<std::string> {"missing_table"});
	REQUIRE(repl_observed.rendered == "missing_table");

	ReplReset();
	REQUIRE(ReplQueryError(fx.conn, "SELECT * FROM memory.main.missing_table") != DUCKDB_V2_ERROR_NONE);
	REQUIRE(repl_observed.calls == 1);
	REQUIRE(repl_observed.parts == std::vector<std::string> {"memory", "main", "missing_table"});
	REQUIRE(repl_observed.rendered == "memory.main.missing_table");
}

TEST_CASE("V2 replacement scan: not consulted for names the catalog resolves", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	ReplReset();
	ReplRegisterOnConnection(fx.conn, ReplClaimRange);

	ExecSQL(fx.conn, "CREATE TABLE real_table (i BIGINT)");
	ExecSQL(fx.conn, "INSERT INTO real_table VALUES (7), (8)");
	REQUIRE(ReplQueryI64(fx.conn, "SELECT i FROM real_table ORDER BY i") == std::vector<int64_t> {7, 8});
	REQUIRE(repl_observed.calls == 0);
}

TEST_CASE("V2 replacement scan: connection scope and precedence", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	duckdb_v2_connection_handle other = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &other, nullptr) == DUCKDB_V2_ERROR_NONE);

	ReplReset();
	ReplRegisterOnConnection(fx.conn, ReplClaimRange);

	// Visible on the registering connection only.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT * FROM anything") == std::vector<int64_t> {0, 1});
	REQUIRE(ReplQueryError(other, "SELECT * FROM anything") != DUCKDB_V2_ERROR_NONE);

	// An instance-scoped scan reaches every connection, including ones opened afterwards.
	duckdb_v2_replacement_scan_handle instance_scan = nullptr;
	REQUIRE(duckdb_v2_replacement_scan_create(fx.factory, &instance_scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_set_callback(instance_scan, ReplClaimRange, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_instance_register_replacement_scan(fx.instance, instance_scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_replacement_scan_destroy(&instance_scan);

	REQUIRE(ReplQueryI64(other, "SELECT * FROM anything") == std::vector<int64_t> {0, 1});
	duckdb_v2_connection_handle later = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &later, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ReplQueryI64(later, "SELECT * FROM anything") == std::vector<int64_t> {0, 1});
	duckdb_v2_connection_destroy(&later);

	duckdb_v2_connection_destroy(&other);
}

TEST_CASE("V2 replacement scan: outranks the built-in file scans", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	duckdb_v2_connection_handle other = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &other, nullptr) == DUCKDB_V2_ERROR_NONE);

	ReplReset();
	ReplRegisterOnConnection(fx.conn, ReplClaimCsv);

	// Without the scan the CSV replacement takes the name and fails on the missing file.
	REQUIRE(ReplQueryError(other, "SELECT * FROM 'no_such_file.csv'") != DUCKDB_V2_ERROR_NONE);
	// With it, the connection-scoped scan claims the name first.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT * FROM 'no_such_file.csv'") == std::vector<int64_t> {0, 1});
	// Names it declines still reach the built-ins.
	REQUIRE(ReplQueryError(fx.conn, "SELECT * FROM 'no_such_file.parquet'") != DUCKDB_V2_ERROR_NONE);

	duckdb_v2_connection_destroy(&other);
}

TEST_CASE("V2 replacement scan: registration order, first claim wins", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	ReplReset();
	// The claiming scan is registered first, so the recording one is never consulted.
	ReplRegisterOnConnection(fx.conn, ReplClaimRange);
	ReplRegisterOnConnection(fx.conn, ReplDecline);
	REQUIRE(ReplQueryI64(fx.conn, "SELECT * FROM anything") == std::vector<int64_t> {0, 1});
	REQUIRE(repl_observed.calls == 1); // only the claiming scan recorded a call
}

TEST_CASE("V2 replacement scan: callback errors and unknown functions", "[capi_v2][replacement_scan]") {
	SECTION("an error reported by the callback surfaces with its code") {
		EnvFixture fx;
		ReplRegisterOnConnection(fx.conn, ReplFail);
		REQUIRE(ReplQueryError(fx.conn, "SELECT * FROM anything") == DUCKDB_V2_ERROR_IO_GENERAL);
	}
	SECTION("claiming a function that does not exist surfaces a catalog error") {
		EnvFixture fx;
		ReplRegisterOnConnection(fx.conn, ReplClaimUnknown);
		REQUIRE(ReplQueryError(fx.conn, "SELECT * FROM anything") == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	}
}

TEST_CASE("V2 replacement scan: claim form rules", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	ReplReset();
	ReplRegisterOnConnection(fx.conn, ReplClaimRules);

	// The last set_function_name wins, so the query still resolves to range(2).
	REQUIRE(ReplQueryI64(fx.conn, "SELECT * FROM anything") == std::vector<int64_t> {0, 1});
	// An argument before a function name, and a second claim form, are both refused.
	REQUIRE(repl_observed.bare_argument_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(repl_observed.mixed_form_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 replacement scan: claims a subquery", "[capi_v2][replacement_scan]") {
	SECTION("a single SELECT is read instead") {
		EnvFixture fx;
		ReplRegisterOnConnection(fx.conn, ReplClaimSubquery);
		REQUIRE(ReplQueryI64(fx.conn, "SELECT v::BIGINT FROM anything") == std::vector<int64_t> {42});
	}
	SECTION("anything else is rejected by the setter") {
		EnvFixture fx;
		repl_multi_statement_rc = DUCKDB_V2_ERROR_NONE;
		repl_non_select_rc = DUCKDB_V2_ERROR_NONE;
		repl_bad_syntax_rc = DUCKDB_V2_ERROR_NONE;
		ReplRegisterOnConnection(fx.conn, ReplClaimBadSubqueries);

		REQUIRE(ReplQueryI64(fx.conn, "SELECT v::BIGINT FROM anything") == std::vector<int64_t> {7});
		REQUIRE(repl_multi_statement_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(repl_non_select_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(repl_bad_syntax_rc == DUCKDB_V2_ERROR_QUERY_PARSER);
	}
}

TEST_CASE("V2 replacement scan: user data flows and is destroyed once", "[capi_v2][replacement_scan]") {
	repl_destroy_calls = 0;
	{
		EnvFixture fx;
		duckdb_v2_replacement_scan_handle scan = nullptr;
		REQUIRE(duckdb_v2_replacement_scan_create(fx.factory, &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_replacement_scan_set_callback(scan, ReplClaimWithUserData, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_opaque user_data = {new std::string("planted"), ReplDestroyUserData, nullptr};
		REQUIRE(duckdb_v2_replacement_scan_set_user_data(scan, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_replacement_scan(fx.conn, scan, nullptr) == DUCKDB_V2_ERROR_NONE);
		// Destroying the builder does not affect the registered scan or free the user data.
		REQUIRE(duckdb_v2_replacement_scan_destroy(&scan) == DUCKDB_V2_ERROR_NONE);

		REQUIRE(ReplQueryI64(fx.conn, "SELECT * FROM anything") == std::vector<int64_t> {0, 1});
		REQUIRE(repl_destroy_calls == 0);
	}
	// A connection-scoped scan is released with its connection.
	REQUIRE(repl_destroy_calls == 1);
}

TEST_CASE("V2 replacement scan: registration refusals", "[capi_v2][replacement_scan]") {
	EnvFixture fx;

	// No callback.
	{
		duckdb_v2_replacement_scan_handle scan = nullptr;
		REQUIRE(duckdb_v2_replacement_scan_create(fx.factory, &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_replacement_scan(fx.conn, scan, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_replacement_scan_destroy(&scan);
	}
	// Registering twice from one handle.
	{
		duckdb_v2_replacement_scan_handle scan = nullptr;
		REQUIRE(duckdb_v2_replacement_scan_create(fx.factory, &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_replacement_scan_set_callback(scan, ReplDecline, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_replacement_scan(fx.conn, scan, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_replacement_scan(fx.conn, scan, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_replacement_scan_destroy(&scan);
	}
}

TEST_CASE("V2 replacement scan: null arguments and destroy null-safety", "[capi_v2][replacement_scan]") {
	EnvFixture fx;

	duckdb_v2_replacement_scan_handle scan = nullptr;
	REQUIRE(duckdb_v2_replacement_scan_create(nullptr, &scan, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(scan == nullptr);
	REQUIRE(duckdb_v2_replacement_scan_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_replacement_scan(nullptr, scan, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_instance_register_replacement_scan(nullptr, scan, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_extension_register_replacement_scan(nullptr, scan, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_replacement_scan_create(fx.factory, &scan, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_set_callback(nullptr, ReplDecline, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_replacement_scan_set_user_data(scan, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_replacement_scan(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// The info accessors reject a null handle and a null out-parameter alike.
	void *data = nullptr;
	duckdb_v2_qname_handle name = nullptr;
	REQUIRE(duckdb_v2_replacement_scan_get_user_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_replacement_scan_get_name(nullptr, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_replacement_scan_set_function_name(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_replacement_scan_add_argument(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto sql_str = Convert("SELECT 1");
	REQUIRE(duckdb_v2_replacement_scan_set_subquery(nullptr, &sql_str, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto alias_str = ReplIdent("x");
	REQUIRE(duckdb_v2_replacement_scan_set_alias(nullptr, &alias_str, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_replacement_scan_destroy(&scan) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(scan == nullptr);
	REQUIRE(duckdb_v2_replacement_scan_destroy(&scan) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_replacement_scan_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// The collection claim carries the load of the "register my data under a name"
// workflow: no dedicated API, just a scan over a registry the caller owns.
// ===========================================================================

TEST_CASE("V2 replacement scan: claims a column data collection", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	auto cdc = ReplMakeCollection(fx.factory, {10, 20});

	ReplRegistry registry;
	registry.name = "my_batch";
	registry.collection = cdc;
	ReplRegisterRegistry(fx.conn, registry);

	// Readable like a table, with the default col1..colN naming.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT col1 FROM my_batch ORDER BY col1") == std::vector<int64_t> {10, 20});

	// The motivating case: a client-side buffer as the source of an INSERT.
	ExecSQL(fx.conn, "CREATE TABLE sink (v BIGINT)");
	ExecSQL(fx.conn, "INSERT INTO sink SELECT * FROM my_batch");
	REQUIRE(ReplQueryI64(fx.conn, "SELECT v FROM sink ORDER BY v") == std::vector<int64_t> {10, 20});

	// Joined against a real table, and re-read after the collection changed underneath.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT s.v FROM sink s JOIN my_batch b ON s.v = b.col1 ORDER BY s.v") ==
	        std::vector<int64_t> {10, 20});

	// Names it does not recognise are declined, so the normal catalog error surfaces.
	REQUIRE(ReplQueryError(fx.conn, "SELECT * FROM some_other_name") != DUCKDB_V2_ERROR_NONE);

	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2 replacement scan: collection column names", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	auto cdc = ReplMakeCollection(fx.factory, {5, 6});

	ReplRegistry registry;
	registry.name = "named_batch";
	registry.collection = cdc;
	registry.column_names = {"amount"};
	ReplRegisterRegistry(fx.conn, registry);

	REQUIRE(ReplQueryI64(fx.conn, "SELECT amount FROM named_batch ORDER BY amount") == std::vector<int64_t> {5, 6});
	// The alias defaults to the reference's own name, so a qualified read resolves too.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT named_batch.amount FROM named_batch ORDER BY 1") ==
	        std::vector<int64_t> {5, 6});
	// And a query-written column alias still applies.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT renamed FROM named_batch t(renamed) ORDER BY renamed") ==
	        std::vector<int64_t> {5, 6});

	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2 replacement scan: a prepared collection claim caches its borrow", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	auto cdc = ReplMakeCollection(fx.factory, {10, 20});

	ReplRegistry registry;
	registry.name = "cached_batch";
	registry.collection = cdc;
	ReplRegisterRegistry(fx.conn, registry);

	duckdb_v2_statement_iterator_handle iter = nullptr;
	REQUIRE(duckdb_v2_parse_sql(fx.conn, "SELECT col1 FROM cached_batch ORDER BY col1", true, &iter, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_handle stmt = nullptr;
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_statement_iterator_destroy(&iter);

	duckdb_v2_prepared_statement_handle prepared = nullptr;
	REQUIRE(duckdb_v2_prepared_statement_create(fx.conn, stmt, false, &prepared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_destroy(&stmt);

	// A collection claim reads no database, so nothing invalidates the plan: it is reused,
	// which means the scan callback is NOT consulted again and the plan keeps the borrow it
	// took at prepare time.
	bool reuses = false;
	REQUIRE(duckdb_v2_prepared_statement_reuses_plan(prepared, &reuses, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(reuses);

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(duckdb_v2_prepared_statement_execute(prepared, nullptr, nullptr, 0, &r, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 2);
	duckdb_v2_result_destroy(&r);

	duckdb_v2_prepared_statement_destroy(&prepared);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2 replacement scan: empty collection binds and yields no rows", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	auto cdc = ReplMakeCollection(fx.factory, {});

	ReplRegistry registry;
	registry.name = "empty_batch";
	registry.collection = cdc;
	ReplRegisterRegistry(fx.conn, registry);

	REQUIRE(ReplQueryI64(fx.conn, "SELECT col1 FROM empty_batch").empty());
	REQUIRE(ReplQueryI64(fx.conn, "SELECT count(*)::BIGINT FROM empty_batch") == std::vector<int64_t> {0});

	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2 replacement scan: collection column name validation", "[capi_v2][replacement_scan]") {
	EnvFixture fx;
	auto cdc = ReplMakeCollection(fx.factory, {1, 2});

	ReplRegistry registry;
	registry.name = "probe_batch";
	registry.collection = cdc;
	registry.probe_refusals = true;
	ReplRegisterRegistry(fx.conn, registry);

	// The valid claim at the end still lands, so the query itself succeeds.
	REQUIRE(ReplQueryI64(fx.conn, "SELECT col1 FROM probe_batch ORDER BY col1") == std::vector<int64_t> {1, 2});
	// More names than columns, an empty name, and a null array with a non-zero count are all refused.
	REQUIRE(registry.wrong_count_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(registry.empty_name_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(registry.null_names_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_column_data_collection_destroy(&cdc);
}

} // namespace test_capi_v2
