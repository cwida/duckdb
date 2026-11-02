#include "test_capi_v2.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 expression tests. Expressions only ever reach a caller through the filter
// pushdown callback of a table function, so every test registers expr_probe(),
// a table function whose pushdown callback renders each offered predicate into
// a string without claiming it, then queries it with a WHERE clause and checks
// what the callback saw.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// What the pushdown callback recorded for the last query.
struct ExpressionProbe {
	std::vector<std::string> filters;
	std::string root_type;
	std::string failure;
	idx_t misuse_failures = 0;
	bool check_misuse = false;

	void Reset() {
		filters.clear();
		root_type.clear();
		failure.clear();
		misuse_failures = 0;
		check_misuse = false;
	}
};

ExpressionProbe expression_probe;

void ProbeFail(const char *what, duckdb_v2_error_info_handle *err) {
	if (expression_probe.failure.empty()) {
		expression_probe.failure = what;
		if (err && *err) {
			duckdb_v2_str text = {nullptr, 0};
			duckdb_v2_error_info_get_text(*err, &text);
			expression_probe.failure += ": " + Convert(text);
		}
	}
}

// The two-call text protocol, without REQUIRE so it is safe inside a callback.
template <class CALL>
std::string ProbeText(CALL call, bool &ok) {
	idx_t len = 0;
	if (call(nullptr, 0, &len) != DUCKDB_V2_ERROR_NONE) {
		ok = false;
		return std::string();
	}
	std::vector<char> buf(len + 1, '\0');
	if (call(buf.data(), buf.size(), &len) != DUCKDB_V2_ERROR_NONE) {
		ok = false;
		return std::string();
	}
	return std::string(buf.data(), len);
}

std::string ProbeValue(duckdb_v2_value_handle value, bool &ok) {
	return ProbeText(
	    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_value_to_string(value, buf, cap, len, nullptr); }, ok);
}

std::string ProbeType(duckdb_v2_logical_type_handle type, bool &ok) {
	return ProbeText(
	    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_logical_type_to_text(type, buf, cap, len, nullptr); },
	    ok);
}

// A tag for the node types that are not function calls.
const char *ProbeTag(DUCKDB_V2_EXPRESSION_TYPE type) {
	switch (type) {
	case DUCKDB_V2_EXPRESSION_TYPE_OPERATOR_NOT:
		return "not";
	case DUCKDB_V2_EXPRESSION_TYPE_OPERATOR_IS_NULL:
		return "is_null";
	case DUCKDB_V2_EXPRESSION_TYPE_OPERATOR_IS_NOT_NULL:
		return "is_not_null";
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_IN:
		return "in";
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_NOT_IN:
		return "not_in";
	case DUCKDB_V2_EXPRESSION_TYPE_CONJUNCTION_AND:
		return "and";
	case DUCKDB_V2_EXPRESSION_TYPE_CONJUNCTION_OR:
		return "or";
	case DUCKDB_V2_EXPRESSION_TYPE_VALUE_PARAMETER:
		return "param";
	case DUCKDB_V2_EXPRESSION_TYPE_CASE_EXPR:
		return "case";
	case DUCKDB_V2_EXPRESSION_TYPE_OPERATOR_COALESCE:
		return "coalesce";
	case DUCKDB_V2_EXPRESSION_TYPE_INVALID:
		return "invalid";
	default:
		return "?";
	}
}

bool ProbeIsFunction(DUCKDB_V2_EXPRESSION_TYPE type) {
	switch (type) {
	case DUCKDB_V2_EXPRESSION_TYPE_BOUND_FUNCTION:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_EQUAL:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_NOTEQUAL:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_LESSTHAN:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_GREATERTHAN:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_LESSTHANOREQUALTO:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_GREATERTHANOREQUALTO:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_DISTINCT_FROM:
	case DUCKDB_V2_EXPRESSION_TYPE_COMPARE_NOT_DISTINCT_FROM:
		return true;
	default:
		return false;
	}
}

// Renders a predicate: constants as their text, column references as "col<declared index>", plain function calls
// as "qualified.name(children)", comparisons as "op(children)", casts as "cast(child)::TYPE", and everything else
// as "tag(children)".
std::string ProbeRender(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_expression_handle expr,
                        duckdb_v2_error_info_handle *err, bool &ok) {
	DUCKDB_V2_EXPRESSION_TYPE type = DUCKDB_V2_EXPRESSION_TYPE_INVALID;
	if (duckdb_v2_expression_get_type(expr, &type, err) != DUCKDB_V2_ERROR_NONE) {
		ok = false;
		return "";
	}

	if (type == DUCKDB_V2_EXPRESSION_TYPE_VALUE_CONSTANT) {
		duckdb_v2_value_handle value = nullptr;
		if (duckdb_v2_expression_constant_get_value(expr, &value, err) != DUCKDB_V2_ERROR_NONE) {
			ok = false;
			return "";
		}
		auto text = ProbeValue(value, ok);
		duckdb_v2_value_destroy(&value);
		return text;
	}
	if (type == DUCKDB_V2_EXPRESSION_TYPE_BOUND_COLUMN_REF) {
		idx_t index = 0;
		idx_t declared = 0;
		if (duckdb_v2_expression_column_ref_get_index(expr, &index, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_table_function_filter_pushdown_get_column_index(info, index, &declared, err) !=
		        DUCKDB_V2_ERROR_NONE) {
			ok = false;
			return "";
		}
		return "col" + std::to_string(declared);
	}

	idx_t child_count = 0;
	if (duckdb_v2_expression_get_child_count(expr, &child_count, err) != DUCKDB_V2_ERROR_NONE) {
		ok = false;
		return "";
	}
	std::string children;
	for (idx_t i = 0; i < child_count; i++) {
		duckdb_v2_expression_handle child = nullptr;
		if (duckdb_v2_expression_get_child(expr, i, &child, err) != DUCKDB_V2_ERROR_NONE) {
			ok = false;
			return "";
		}
		if (i > 0) {
			children += ", ";
		}
		children += ProbeRender(info, child, err, ok);
	}

	if (type == DUCKDB_V2_EXPRESSION_TYPE_OPERATOR_CAST) {
		DUCKDB_V2_CAST_MODE mode = DUCKDB_V2_CAST_MODE_NORMAL;
		duckdb_v2_logical_type_handle target = nullptr;
		if (duckdb_v2_expression_cast_get_mode(expr, &mode, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_get_return_type(expr, &target, err) != DUCKDB_V2_ERROR_NONE) {
			ok = false;
			return "";
		}
		auto target_text = ProbeType(target, ok);
		duckdb_v2_logical_type_destroy(&target);
		return std::string(mode == DUCKDB_V2_CAST_MODE_TRY ? "try_cast" : "cast") + "(" + children +
		       ")::" + target_text;
	}
	if (type == DUCKDB_V2_EXPRESSION_TYPE_COMPARE_BETWEEN) {
		return "between(" + children + ")";
	}
	if (type == DUCKDB_V2_EXPRESSION_TYPE_BOUND_FUNCTION) {
		duckdb_v2_qname_handle qname = nullptr;
		if (duckdb_v2_expression_function_get_qname(expr, &qname, err) != DUCKDB_V2_ERROR_NONE) {
			ok = false;
			return "";
		}
		auto text = ProbeText(
		    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_qname_render(qname, buf, cap, len, nullptr); },
		    ok);
		duckdb_v2_qname_destroy(&qname);
		return text + "(" + children + ")";
	}
	if (ProbeIsFunction(type)) {
		duckdb_v2_identifier_t name = {nullptr, 0};
		if (duckdb_v2_expression_function_get_name(expr, &name, err) != DUCKDB_V2_ERROR_NONE) {
			ok = false;
			return "";
		}
		return Convert(name) + "(" + children + ")";
	}
	std::string rendered = ProbeTag(type);
	if (child_count > 0) {
		rendered += "(" + children + ")";
	}
	return rendered;
}

// Counts how many of the type-specific accessors refuse a node they do not apply to. Runs on a "a < 5" predicate:
// the root is a comparison, its first child a column reference, its second a constant.
void ProbeMisuse(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_expression_handle root) {
	duckdb_v2_expression_handle column = nullptr;
	duckdb_v2_expression_handle constant = nullptr;
	if (duckdb_v2_expression_get_child(root, 0, &column, nullptr) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_expression_get_child(root, 1, &constant, nullptr) != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	auto count_failure = [&](DUCKDB_V2_ERROR rc, duckdb_v2_error_info_handle *err) {
		if (rc != DUCKDB_V2_ERROR_NONE && *err) {
			expression_probe.misuse_failures++;
		}
		duckdb_v2_error_info_destroy(err);
	};
	duckdb_v2_error_info_handle err = nullptr;

	duckdb_v2_value_handle value = nullptr;
	count_failure(duckdb_v2_expression_constant_get_value(root, &value, &err), &err);
	idx_t index = 0;
	count_failure(duckdb_v2_expression_column_ref_get_index(root, &index, &err), &err);
	duckdb_v2_identifier_t name = {nullptr, 0};
	count_failure(duckdb_v2_expression_function_get_name(constant, &name, &err), &err);
	duckdb_v2_qname_handle qname = nullptr;
	count_failure(duckdb_v2_expression_function_get_qname(constant, &qname, &err), &err);
	DUCKDB_V2_CAST_MODE mode = DUCKDB_V2_CAST_MODE_NORMAL;
	count_failure(duckdb_v2_expression_cast_get_mode(root, &mode, &err), &err);
	duckdb_v2_expression_handle child = nullptr;
	count_failure(duckdb_v2_expression_get_child(root, 2, &child, &err), &err);
	count_failure(duckdb_v2_expression_get_child(column, 0, &child, &err), &err);

	duckdb_v2_expression_handle filter = nullptr;
	count_failure(duckdb_v2_table_function_filter_pushdown_get_filter(info, 99, &filter, &err), &err);
	count_failure(duckdb_v2_table_function_filter_pushdown_get_column_index(info, 99, &index, &err), &err);
	count_failure(duckdb_v2_table_function_filter_pushdown_accept(info, 99, &err), &err);
}

void ProbePushdownCb(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	// The optimizer may offer the (unclaimed) predicates more than once per query: keep the last round.
	expression_probe.filters.clear();
	idx_t count = 0;
	if (duckdb_v2_table_function_filter_pushdown_get_filter_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		ProbeFail("get_filter_count", err);
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_expression_handle filter = nullptr;
		if (duckdb_v2_table_function_filter_pushdown_get_filter(info, i, &filter, err) != DUCKDB_V2_ERROR_NONE) {
			ProbeFail("get_filter", err);
			return;
		}
		bool ok = true;
		auto rendered = ProbeRender(info, filter, err, ok);
		if (!ok) {
			ProbeFail("render", err);
			return;
		}
		expression_probe.filters.push_back(rendered);

		if (i == 0) {
			duckdb_v2_logical_type_handle type = nullptr;
			if (duckdb_v2_expression_get_return_type(filter, &type, err) != DUCKDB_V2_ERROR_NONE) {
				ProbeFail("get_return_type", err);
				return;
			}
			expression_probe.root_type = ProbeType(type, ok);
			duckdb_v2_logical_type_destroy(&type);
			if (expression_probe.check_misuse) {
				ProbeMisuse(info, filter);
			}
		}
	}
	std::sort(expression_probe.filters.begin(), expression_probe.filters.end());
}

// ---------------------------------------------------------------------------
// expr_probe(): columns a INTEGER, b VARCHAR, c BIGINT with rows (i, str(i), i*10) for i in 0..3.
// ---------------------------------------------------------------------------

constexpr int64_t PROBE_ROWS = 4;

struct ProbeGlobal {
	int64_t position = 0;
};

void DeleteProbeGlobal(void *ptr) {
	delete static_cast<ProbeGlobal *>(ptr);
}

void ProbeBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	struct {
		const char *name;
		DUCKDB_V2_LOGICAL_TYPE_ID id;
	} columns[] = {{"a", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER},
	               {"b", DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR},
	               {"c", DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT}};
	for (auto &column : columns) {
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_factory_create_type_from_id(factory, column.id, nullptr, nullptr, 0, &type, err) !=
		    DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_identifier_t name_str = {column.name, std::strlen(column.name)};
		auto rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, type, err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
}

void ProbeInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new ProbeGlobal {}, DeleteProbeGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void ProbeExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<ProbeGlobal *>(global_ptr);

	duckdb_v2_vector_handle vectors[3] = {nullptr, nullptr, nullptr};
	for (idx_t i = 0; i < 3; i++) {
		if (duckdb_v2_data_chunk_get_vector(chunk, i, &vectors[i], err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
	// Batches are capped by the vector size so the probe also serves builds with a tiny STANDARD_VECTOR_SIZE.
	auto produced = PROBE_ROWS - global.position;
	if (produced > static_cast<int64_t>(STANDARD_VECTOR_SIZE)) {
		produced = static_cast<int64_t>(STANDARD_VECTOR_SIZE);
	}
	if (produced <= 0) {
		duckdb_v2_vector_set_size(vectors[0], 0, err);
		return;
	}

	void *a_raw = nullptr;
	void *c_raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(vectors[0], &a_raw, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vectors[2], &c_raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (int64_t i = 0; i < produced; i++) {
		const auto row = global.position + i;
		static_cast<int32_t *>(a_raw)[i] = static_cast<int32_t>(row);
		static_cast<int64_t *>(c_raw)[i] = row * 10;
		auto text = std::to_string(row);
		if (V2VectorAssignString(vectors[1], static_cast<idx_t>(i), text.data(), text.size(), err) !=
		    DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
	global.position += produced;
	duckdb_v2_vector_set_size(vectors[0], static_cast<idx_t>(produced), err);
}

void RegisterProbe(duckdb_v2_connection_handle conn) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Convert("expr_probe");
	REQUIRE(duckdb_v2_table_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ProbeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, ProbeInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ProbeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_filter_pushdown_callback(function, ProbePushdownCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

// Runs a single-BIGINT-cell query against the probe and returns the rendered predicates it saw.
std::vector<std::string> Probe(duckdb_v2_connection_handle conn, const char *sql, int64_t expected) {
	expression_probe.Reset();
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto cell = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	REQUIRE(cell == expected);
	REQUIRE(expression_probe.failure.empty());
	return expression_probe.filters;
}

using Filters = std::vector<std::string>;

} // namespace

TEST_CASE("V2 expression: comparison, column and constant nodes", "[capi_v2][expression]") {
	EnvFixture fx;
	RegisterProbe(fx.conn);

	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE a < 3", 3) == Filters {"<(col0, 3)"});
	REQUIRE(expression_probe.root_type == "BOOLEAN");
	// A query without predicates offers nothing.
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe()", 4).empty());
}

TEST_CASE("V2 expression: type-specific accessors refuse other node types", "[capi_v2][expression]") {
	EnvFixture fx;
	RegisterProbe(fx.conn);

	expression_probe.Reset();
	expression_probe.check_misuse = true;
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM expr_probe() WHERE a < 3", &result) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(result) == 1);
	duckdb_v2_result_destroy(&result);
	REQUIRE(expression_probe.failure.empty());
	REQUIRE(expression_probe.filters == Filters {"<(col0, 3)"});
	// Seven accessor misuses on the expression plus three out-of-bounds indices on the pushdown info, counted on
	// every round the optimizer offered the predicate.
	REQUIRE(expression_probe.misuse_failures % 10 == 0);
	REQUIRE(expression_probe.misuse_failures > 0);
}

TEST_CASE("V2 expression: conjunctions, operators and functions", "[capi_v2][expression]") {
	EnvFixture fx;
	RegisterProbe(fx.conn);

	// A top-level AND arrives as separate predicates.
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE a > 1 AND b = '3'", 1) ==
	        Filters {"=(col1, 3)", ">(col0, 1)"});
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE a = 1 OR c IS NULL", 1) ==
	        Filters {"or(=(col0, 1), is_null(col2))"});
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE c IS NOT NULL", 4) ==
	        Filters {"is_not_null(col2)"});
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE lower(b) = '2'", 1) ==
	        Filters {"=(\"system\".main.lower(col1), 2)"});
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE a IN (1, 2)", 2) == Filters {"in(col0, 1, 2)"});
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE a BETWEEN 1 AND 2", 2) ==
	        Filters {"between(col0, 1, 2)"});
}

TEST_CASE("V2 expression: casts report their mode and target", "[capi_v2][expression]") {
	EnvFixture fx;
	RegisterProbe(fx.conn);

	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE CAST(b AS INTEGER) = 3", 1) ==
	        Filters {"=(cast(col1)::INTEGER, 3)"});
	REQUIRE(Probe(fx.conn, "SELECT count(*) FROM expr_probe() WHERE TRY_CAST(b AS INTEGER) = 3", 1) ==
	        Filters {"=(try_cast(col1)::INTEGER, 3)"});
}

TEST_CASE("V2 expression: column references resolve to declared columns", "[capi_v2][expression]") {
	EnvFixture fx;
	RegisterProbe(fx.conn);

	// Only c is read, so the predicate's column reference is the first (and only) scanned column, which the
	// pushdown info resolves back to the third declared column.
	REQUIRE(Probe(fx.conn, "SELECT sum(c) FROM expr_probe() WHERE c > 10", 50) == Filters {">(col2, 10)"});
}

TEST_CASE("V2 expression: null arguments", "[capi_v2][expression]") {
	DUCKDB_V2_EXPRESSION_TYPE type = DUCKDB_V2_EXPRESSION_TYPE_INVALID;
	duckdb_v2_logical_type_handle logical_type = nullptr;
	idx_t count = 0;
	duckdb_v2_expression_handle child = nullptr;
	duckdb_v2_value_handle value = nullptr;
	duckdb_v2_identifier_t name = {nullptr, 0};
	DUCKDB_V2_CAST_MODE mode = DUCKDB_V2_CAST_MODE_NORMAL;

	REQUIRE(duckdb_v2_expression_get_type(nullptr, &type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_get_return_type(nullptr, &logical_type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_get_child_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_get_child(nullptr, 0, &child, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_constant_get_value(nullptr, &value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_column_ref_get_index(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_function_get_name(nullptr, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_qname_handle qname = nullptr;
	REQUIRE(duckdb_v2_expression_function_get_qname(nullptr, &qname, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_expression_cast_get_mode(nullptr, &mode, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_table_function_filter_pushdown_get_filter_count(nullptr, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_filter_pushdown_get_filter(nullptr, 0, &child, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_filter_pushdown_accept(nullptr, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_filter_pushdown_get_column_count(nullptr, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_filter_pushdown_get_column_index(nullptr, 0, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2
