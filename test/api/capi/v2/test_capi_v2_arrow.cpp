#include "test_capi_v2.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 Arrow C Data Interface tests.
//
// The importer and exporter both need a context, which only a callback has, so
// the value round-trips run through two harnesses registered on the connection:
// arrow_roundtrip(x), a scalar function pushing its argument out to Arrow and
// straight back, and arrow_roundtrip_range(n), a table function doing the same
// for multi-column chunks. A value survives iff arrow_roundtrip(x) IS NOT
// DISTINCT FROM x, so the assertions are ordinary SQL.
//
// Callbacks must not use Catch assertions: a REQUIRE would throw through the C
// boundary into the engine. They populate the error slot and return instead, and
// the failure surfaces as a query error.
//
// The Arrow structs come from duckdb_v2.h; this file includes no Arrow header.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
namespace {

duckdb_v2_identifier_t ArrowIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

// Builds a type inside a callback. Returns null after populating the error slot.
duckdb_v2_logical_type_handle ArrowTypeInCallback(duckdb_v2_context_handle context, DUCKDB_V2_LOGICAL_TYPE_ID id,
                                                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle type = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, id, nullptr, nullptr, 0, &type, err) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	return type;
}

// The two halves of a round-trip, carried as bind data: an exporter for the column list, and an
// importer resolved from the schema that exporter reports.
struct ArrowRoundtrip {
	duckdb_v2_arrow_exporter_handle exporter = nullptr;
	duckdb_v2_arrow_importer_handle importer = nullptr;
};

void ArrowRoundtripDestroy(void *ptr) {
	auto *rt = static_cast<ArrowRoundtrip *>(ptr);
	duckdb_v2_arrow_exporter_destroy(&rt->exporter);
	duckdb_v2_arrow_importer_destroy(&rt->importer);
	delete rt;
}

// Builds an exporter over `types`/`names` and an importer over the schema it reports, so the pair
// round-trips those columns. Returns null after populating the error slot.
ArrowRoundtrip *ArrowMakeRoundtrip(duckdb_v2_context_handle context, const duckdb_v2_logical_type_handle *types,
                                   const duckdb_v2_str *names, idx_t count, idx_t export_batch, idx_t import_batch,
                                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_arrow_exporter_handle exporter = nullptr;
	if (duckdb_v2_arrow_exporter_create(context, types, names, count, export_batch, &exporter, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	// The importer is resolved from the exporter's own schema, so the two agree by construction.
	ArrowSchema schema {};
	auto rc = duckdb_v2_arrow_exporter_get_schema(exporter, &schema, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_arrow_exporter_destroy(&exporter);
		return nullptr;
	}
	duckdb_v2_arrow_importer_handle importer = nullptr;
	rc = duckdb_v2_arrow_importer_create(context, &schema, import_batch, &importer, err);
	schema.release(&schema);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_arrow_exporter_destroy(&exporter);
		return nullptr;
	}
	return new ArrowRoundtrip {exporter, importer};
}

// Pushes `input` out to Arrow and back, returning the re-imported chunk (null on failure, with the
// error slot populated). Borrows `input`.
duckdb_v2_data_chunk_handle ArrowRoundtripChunk(ArrowRoundtrip &rt, duckdb_v2_data_chunk_handle input,
                                                duckdb_v2_error_info_handle *err) {
	auto borrowed = input;
	if (duckdb_v2_arrow_exporter_append(rt.exporter, &borrowed, false, true, err) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	ArrowArray array {};
	if (duckdb_v2_arrow_exporter_next_array(rt.exporter, &array, err) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	if (!array.release) {
		return nullptr; // no batch size is set, so one append always completes one array
	}
	// Hand the array over: the imported chunk then references its buffers and keeps them alive.
	auto rc = duckdb_v2_arrow_importer_append(rt.importer, &array, true, true, err);
	if (array.release) {
		array.release(&array);
	}
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	duckdb_v2_data_chunk_handle imported = nullptr;
	if (duckdb_v2_arrow_importer_next_chunk(rt.importer, &imported, err) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	return imported;
}

// ---------------------------------------------------------------------------
// arrow_roundtrip(x): declared with an ANY return; the resolve types callback
// matches the return type to the argument type, and the bind callback builds the
// exporter/importer pair. One function therefore covers every type, nested ones
// included.
// ---------------------------------------------------------------------------

void ArrowRtResolveTypes(duckdb_v2_function_bind_info_handle info,
                         duckdb_v2_scalar_function_resolve_types_info_handle result, duckdb_v2_context_handle,
                         duckdb_v2_error_info_handle *err) {
	duckdb_v2_logical_type_handle arg_type = nullptr;
	if (duckdb_v2_function_bind_get_arg_type(info, 0, &arg_type, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	// The result type follows the argument type.
	duckdb_v2_scalar_function_resolve_types_set_return_type(result, arg_type, err);
	duckdb_v2_logical_type_destroy(&arg_type);
}

void ArrowRtBind(duckdb_v2_function_bind_info_handle info, duckdb_v2_scalar_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_logical_type_handle arg_type = nullptr;
	if (duckdb_v2_function_bind_get_arg_type(info, 0, &arg_type, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto name = Convert("x");
	auto *rt = ArrowMakeRoundtrip(context, &arg_type, &name, 1, 0, 0, err);
	duckdb_v2_logical_type_destroy(&arg_type);
	if (!rt) {
		return;
	}
	duckdb_v2_opaque bind_data = {rt, ArrowRoundtripDestroy, nullptr};
	duckdb_v2_scalar_function_bind_set_bind_data(result, &bind_data, err);
}

void ArrowRtExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle context,
                 duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	void *bind_data = nullptr;
	duckdb_v2_vector_handle arg = nullptr;
	duckdb_v2_vector_handle result = nullptr;
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_arg(info, 0, &arg, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &rt = *static_cast<ArrowRoundtrip *>(bind_data);

	// Wrap the argument vector in a one-column chunk, which is what the exporter takes.
	duckdb_v2_logical_type_handle arg_type = nullptr;
	if (duckdb_v2_vector_get_logical_type(arg, &arg_type, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_data_chunk_handle input = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, &arg_type, 1, &input, err);
	duckdb_v2_logical_type_destroy(&arg_type);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_handle input_vector = nullptr;
	if (duckdb_v2_data_chunk_get_vector(input, 0, &input_vector, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_reference(input_vector, arg, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_set_size(input_vector, count, err) != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_destroy(&input);
		return;
	}

	auto imported = ArrowRoundtripChunk(rt, input, err);
	duckdb_v2_data_chunk_destroy(&input);
	if (!imported) {
		return;
	}
	// Reference the re-imported column into the result: zero-copy, and the shared buffers keep
	// the data alive after the imported chunk goes away.
	duckdb_v2_vector_handle imported_vector = nullptr;
	if (duckdb_v2_data_chunk_get_vector(imported, 0, &imported_vector, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_vector_reference(result, imported_vector, err);
	}
	duckdb_v2_data_chunk_destroy(&imported);
}

void RegisterArrowRoundtrip(EnvFixture &fx) {
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Convert("arrow_roundtrip");
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = ArrowIdent("x");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, any, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_scalar_function_set_resolve_types_callback(function, ArrowRtResolveTypes, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_bind_callback(function, ArrowRtBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, ArrowRtExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&any);
}

// ---------------------------------------------------------------------------
// arrow_roundtrip_range(n): emits n rows of (i BIGINT, s VARCHAR) where
// i = row index and s = 'r' || i, every chunk having gone through the round-trip
// first. This drives what the scalar harness cannot: multi-column conversion,
// and an array longer than one vector.
// ---------------------------------------------------------------------------

struct ArrowRangeBind {
	int64_t count = 0;
	ArrowRoundtrip *roundtrip = nullptr;
};
struct ArrowRangeGlobal {
	int64_t emitted = 0;
};

void ArrowRangeDestroyBind(void *ptr) {
	auto *bind = static_cast<ArrowRangeBind *>(ptr);
	ArrowRoundtripDestroy(bind->roundtrip);
	delete bind;
}
void ArrowRangeDestroyGlobal(void *ptr) {
	delete static_cast<ArrowRangeGlobal *>(ptr);
}

void ArrowRangeBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                      duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t count = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &count, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	auto bigint = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	auto varchar = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, err);
	if (!bigint || !varchar) {
		duckdb_v2_logical_type_destroy(&bigint);
		duckdb_v2_logical_type_destroy(&varchar);
		return;
	}
	auto name_str = ArrowIdent("i");
	duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	auto name_str2 = ArrowIdent("s");
	duckdb_v2_table_function_bind_add_result_column(result, &name_str2, varchar, err);

	duckdb_v2_logical_type_handle types[2] = {bigint, varchar};
	duckdb_v2_str names[2] = {Convert("i"), Convert("s")};
	auto *rt = ArrowMakeRoundtrip(context, types, names, 2, 0, 0, err);
	duckdb_v2_logical_type_destroy(&bigint);
	duckdb_v2_logical_type_destroy(&varchar);
	if (!rt) {
		return;
	}
	auto *bind = new ArrowRangeBind {count, rt};
	duckdb_v2_opaque bind_data = {bind, ArrowRangeDestroyBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void ArrowRangeInitCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new ArrowRangeGlobal(), ArrowRangeDestroyGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void ArrowRangeExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle context,
                      duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	void *raw_bind = nullptr;
	void *raw_global = nullptr;
	duckdb_v2_data_chunk_handle output = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &raw_bind, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &raw_global, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &output, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<ArrowRangeBind *>(raw_bind);
	auto &global = *static_cast<ArrowRangeGlobal *>(raw_global);

	duckdb_v2_vector_handle out_i = nullptr;
	duckdb_v2_vector_handle out_s = nullptr;
	if (duckdb_v2_data_chunk_get_vector(output, 0, &out_i, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_chunk_get_vector(output, 1, &out_s, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (global.emitted >= bind.count) {
		duckdb_v2_vector_set_size(out_i, 0, err);
		return;
	}
	auto remaining = static_cast<idx_t>(bind.count - global.emitted);
	auto rows = remaining < STANDARD_VECTOR_SIZE ? remaining : static_cast<idx_t>(STANDARD_VECTOR_SIZE);

	auto bigint = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	auto varchar = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, err);
	if (!bigint || !varchar) {
		duckdb_v2_logical_type_destroy(&bigint);
		duckdb_v2_logical_type_destroy(&varchar);
		return;
	}
	duckdb_v2_logical_type_handle types[2] = {bigint, varchar};
	duckdb_v2_data_chunk_handle input = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, types, 2, &input, err);
	duckdb_v2_logical_type_destroy(&bigint);
	duckdb_v2_logical_type_destroy(&varchar);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_handle in_i = nullptr;
	duckdb_v2_vector_handle in_s = nullptr;
	int64_t *i_data = nullptr;
	if (duckdb_v2_data_chunk_get_vector(input, 0, &in_i, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_chunk_get_vector(input, 1, &in_s, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(in_i, reinterpret_cast<void **>(&i_data), err) != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_destroy(&input);
		return;
	}
	for (idx_t row = 0; row < rows; row++) {
		auto value = global.emitted + static_cast<int64_t>(row);
		i_data[row] = value;
		auto text = "r" + std::to_string(value);
		if (V2VectorAssignString(in_s, row, text.c_str(), text.size(), err) != DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_data_chunk_destroy(&input);
			return;
		}
	}
	duckdb_v2_vector_set_size(in_i, rows, err);
	duckdb_v2_vector_set_size(in_s, rows, err);

	auto imported = ArrowRoundtripChunk(*bind.roundtrip, input, err);
	duckdb_v2_data_chunk_destroy(&input);
	if (!imported) {
		return;
	}
	duckdb_v2_vector_handle imported_i = nullptr;
	duckdb_v2_vector_handle imported_s = nullptr;
	if (duckdb_v2_data_chunk_get_vector(imported, 0, &imported_i, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_data_chunk_get_vector(imported, 1, &imported_s, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_vector_reference(out_i, imported_i, err);
		duckdb_v2_vector_reference(out_s, imported_s, err);
	}
	duckdb_v2_data_chunk_destroy(&imported);
	duckdb_v2_vector_set_size(out_i, rows, err);
	global.emitted += static_cast<int64_t>(rows);
}

void RegisterArrowRoundtripRange(EnvFixture &fx) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Convert("arrow_roundtrip_range");
	REQUIRE(duckdb_v2_table_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_table_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = ArrowIdent("n");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, bigint, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ArrowRangeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, ArrowRangeInitCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ArrowRangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);
}

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

// Runs a single-row single-BOOLEAN query. The queries below aggregate with bool_and over a
// non-empty set, so the row is never NULL.
bool ArrowQueryBool(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.data != nullptr);
	auto value = reinterpret_cast<const bool *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return value;
}

// What a direct exporter/importer probe observed, latched for the test to assert on afterwards.
struct ArrowSplitObserved {
	std::vector<int64_t> array_rows;
	std::vector<idx_t> chunk_rows;
	std::vector<int64_t> values;
	DUCKDB_V2_ERROR second_append_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR type_mismatch_rc = DUCKDB_V2_ERROR_NONE;
};
ArrowSplitObserved arrow_split_observed;
idx_t arrow_split_export_batch = 0;
idx_t arrow_split_import_batch = 0;
idx_t arrow_split_rows = 0;
idx_t arrow_split_appends = 1;
bool arrow_split_flush = true;
// When nonzero, the probe column is the ENUM `arrow_split_enum`
idx_t arrow_split_dict_size = 0;

// Builds a chunk of arrow_split_rows rows, pushes it through an exporter and importer with the
// configured batch sizes, and records the shapes that came out.
void ArrowSplitExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle context,
                    duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_vector_handle result = nullptr;
	if (duckdb_v2_scalar_function_exec_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_logical_type_handle column_type = nullptr;
	if (arrow_split_dict_size) {
		auto text_str = Convert("arrow_split_enum");
		if (duckdb_v2_factory_create_type_from_text(factory, &text_str, &column_type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	} else {
		column_type = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
		if (!column_type) {
			return;
		}
	}
	auto column = Convert("v");
	auto *rt =
	    ArrowMakeRoundtrip(context, &column_type, &column, 1, arrow_split_export_batch, arrow_split_import_batch, err);
	if (!rt) {
		duckdb_v2_logical_type_destroy(&column_type);
		return;
	}

	duckdb_v2_data_chunk_handle input = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, &column_type, 1, &input, err);
	duckdb_v2_logical_type_destroy(&column_type);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		ArrowRoundtripDestroy(rt);
		return;
	}
	duckdb_v2_vector_handle in_v = nullptr;
	void *data = nullptr;
	if (duckdb_v2_data_chunk_get_vector(input, 0, &in_v, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_vector_get_data_mutable(in_v, &data, err) == DUCKDB_V2_ERROR_NONE) {
		for (idx_t i = 0; i < arrow_split_rows; i++) {
			if (arrow_split_dict_size) {
				// An ENUM of at most 255 entries is physically UINT8 (EnumTypeInfo::DictType).
				static_cast<uint8_t *>(data)[i] = static_cast<uint8_t>(i % arrow_split_dict_size);
			} else {
				static_cast<int64_t *>(data)[i] = static_cast<int64_t>(i);
			}
		}
		duckdb_v2_vector_set_size(in_v, arrow_split_rows, err);
	}

	// Feed the same chunk `arrow_split_appends` times, flushing only on the last, so gathering
	// across chunks is exercised when the batch size does not divide them.
	for (idx_t a = 0; a < arrow_split_appends; a++) {
		auto borrowed = input;
		if (duckdb_v2_arrow_exporter_append(rt->exporter, &borrowed, false,
		                                    arrow_split_flush && a + 1 == arrow_split_appends,
		                                    err) != DUCKDB_V2_ERROR_NONE) {
			ArrowRoundtripDestroy(rt);
			duckdb_v2_data_chunk_destroy(&input);
			return;
		}
	}
	std::vector<ArrowArray> arrays;
	while (true) {
		ArrowArray array {};
		if (duckdb_v2_arrow_exporter_next_array(rt->exporter, &array, err) != DUCKDB_V2_ERROR_NONE || !array.release) {
			break;
		}
		arrow_split_observed.array_rows.push_back(array.length);
		arrays.push_back(array);
	}
	// Then push those arrays back through the importer, again flushing only on the last, so its
	// own gathering is exercised too.
	for (idx_t i = 0; i < arrays.size(); i++) {
		if (duckdb_v2_arrow_importer_append(rt->importer, &arrays[i], true, i + 1 == arrays.size(), err) !=
		    DUCKDB_V2_ERROR_NONE) {
			break;
		}
		while (true) {
			duckdb_v2_data_chunk_handle imported = nullptr;
			if (duckdb_v2_arrow_importer_next_chunk(rt->importer, &imported, err) != DUCKDB_V2_ERROR_NONE ||
			    !imported) {
				break;
			}
			idx_t size = 0;
			duckdb_v2_data_chunk_get_size(imported, &size, err);
			arrow_split_observed.chunk_rows.push_back(size);
			duckdb_v2_vector_handle out_v = nullptr;
			duckdb_v2_vector_view view {};
			if (duckdb_v2_data_chunk_get_vector(imported, 0, &out_v, err) == DUCKDB_V2_ERROR_NONE &&
			    duckdb_v2_vector_get_view(out_v, &view, err) == DUCKDB_V2_ERROR_NONE) {
				for (idx_t k = 0; k < size; k++) {
					auto idx = SelAt(view.sel, k);
					if (!arrow_split_dict_size) {
						arrow_split_observed.values.push_back(reinterpret_cast<const int64_t *>(view.data)[idx]);
						continue;
					}

					const auto &str = reinterpret_cast<const duckdb::string_t *>(view.data)[idx];
					auto code = str.GetSize() == 1 ? static_cast<int64_t>(str.GetData()[0] - 'a') : -1;
					if (code < 0 || code >= static_cast<int64_t>(arrow_split_dict_size)) {
						code = -1;
					}
					arrow_split_observed.values.push_back(code);
				}
			}
			duckdb_v2_data_chunk_destroy(&imported);
		}
	}

	// Gathering means the exporter takes more input whenever the caller has it, without requiring
	// the arrays produced so far to be taken first. Run after the measured drain, so the extra
	// rows cannot affect the shapes recorded above.
	auto again = input;
	arrow_split_observed.second_append_rc = duckdb_v2_arrow_exporter_append(rt->exporter, &again, false, true, nullptr);

	// A chunk whose types disagree with the exporter is refused.
	auto varchar = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, nullptr);
	if (varchar) {
		duckdb_v2_data_chunk_handle wrong = nullptr;
		if (duckdb_v2_data_chunk_create(factory, &varchar, 1, &wrong, nullptr) == DUCKDB_V2_ERROR_NONE) {
			arrow_split_observed.type_mismatch_rc =
			    duckdb_v2_arrow_exporter_append(rt->exporter, &wrong, false, false, nullptr);
			duckdb_v2_data_chunk_destroy(&wrong);
		}
		duckdb_v2_logical_type_destroy(&varchar);
	}

	duckdb_v2_data_chunk_destroy(&input);
	ArrowRoundtripDestroy(rt);
	int32_t one = 1;
	void *out = nullptr;
	if (duckdb_v2_vector_get_data_mutable(result, &out, err) == DUCKDB_V2_ERROR_NONE) {
		std::memcpy(out, &one, sizeof(one));
	}
}

// Registers a no-argument-meaning probe that runs `exec` once.
void RegisterArrowProbe(EnvFixture &fx, const char *name, duckdb_v2_scalar_function_exec_callback_fn exec) {
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto fname = Convert(name);
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &fname, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = ArrowIdent("x");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, exec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
}

// Runs a registered probe once.
void RunArrowProbe(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(conn, sql, &r) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(r) == 1);
	duckdb_v2_result_destroy(&r);
}

struct TopLevelValidityObserved {
	std::vector<int64_t> values;
	std::vector<bool> valid;
	std::vector<bool> dict_valid;
	DUCKDB_V2_ERROR append_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR missing_bitmap_rc = DUCKDB_V2_ERROR_NONE;
};
TopLevelValidityObserved arrow_toplevel_validity_observed;

void ArrowTopLevelValidityReleaseArray(ArrowArray *array) {
	array->release = nullptr; // children are borrowed from the exporter's own array
}

// Exports a BIGINT column and a dictionary-encoded ENUM column of 16 rows, then hands the importer a
// wrapper around the exporter's own top-level array carrying a validity bitmap that marks rows 1, 2
// and 9 null. The exporter never sets buffers[0] on the top-level array, but the Arrow C Data
// Interface spec allows a producer of the top-level struct array to do so.
void ArrowTopLevelValidityExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle context,
                               duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	arrow_toplevel_validity_observed = {};
	duckdb_v2_vector_handle result = nullptr;
	if (duckdb_v2_scalar_function_exec_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto bigint = ArrowTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	duckdb_v2_logical_type_handle mood = nullptr;
	auto text_str = Convert("mood");
	if (!bigint || duckdb_v2_factory_create_type_from_text(factory, &text_str, &mood, err) != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_logical_type_destroy(&bigint);
		return;
	}
	duckdb_v2_logical_type_handle types[2] = {bigint, mood};
	duckdb_v2_str names[2] = {Convert("i"), Convert("v")};
	auto *rt = ArrowMakeRoundtrip(context, types, names, 2, 0, 0, err);
	if (!rt) {
		duckdb_v2_logical_type_destroy(&bigint);
		duckdb_v2_logical_type_destroy(&mood);
		return;
	}

	constexpr idx_t rows = 16;
	duckdb_v2_data_chunk_handle input = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, types, 2, &input, err);
	duckdb_v2_logical_type_destroy(&bigint);
	duckdb_v2_logical_type_destroy(&mood);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		ArrowRoundtripDestroy(rt);
		return;
	}
	duckdb_v2_vector_handle in_v = nullptr;
	duckdb_v2_vector_handle in_m = nullptr;
	void *data = nullptr;
	void *dict = nullptr;
	if (duckdb_v2_data_chunk_get_vector(input, 0, &in_v, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_data_chunk_get_vector(input, 1, &in_m, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_vector_get_data_mutable(in_v, &data, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_vector_get_data_mutable(in_m, &dict, err) == DUCKDB_V2_ERROR_NONE) {
		for (idx_t i = 0; i < rows; i++) {
			static_cast<int64_t *>(data)[i] = static_cast<int64_t>(i);
			static_cast<uint8_t *>(dict)[i] = static_cast<uint8_t>(i % 3); // a 3-member enum is UINT8
		}
		duckdb_v2_vector_set_size(in_v, rows, err);
		duckdb_v2_vector_set_size(in_m, rows, err);
	}

	auto borrowed = input;
	if (duckdb_v2_arrow_exporter_append(rt->exporter, &borrowed, false, true, err) != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_destroy(&input);
		ArrowRoundtripDestroy(rt);
		return;
	}
	ArrowArray real_array {};
	if (duckdb_v2_arrow_exporter_next_array(rt->exporter, &real_array, err) != DUCKDB_V2_ERROR_NONE ||
	    !real_array.release) {
		duckdb_v2_data_chunk_destroy(&input);
		ArrowRoundtripDestroy(rt);
		return;
	}

	// Rows 1, 2 and 9 null (bit clear == null); the rest valid.
	uint8_t mask[2] = {0xF9, 0xFD};
	const void *wrapped_buffers[1] = {mask};
	ArrowArray wrapped_array {};
	wrapped_array.length = real_array.length;
	wrapped_array.null_count = 3;
	wrapped_array.offset = 0;
	wrapped_array.n_buffers = 1;
	wrapped_array.n_children = real_array.n_children;
	wrapped_array.buffers = wrapped_buffers;
	wrapped_array.children = real_array.children;
	wrapped_array.release = ArrowTopLevelValidityReleaseArray;

	arrow_toplevel_validity_observed.append_rc =
	    duckdb_v2_arrow_importer_append(rt->importer, &wrapped_array, false, true, err);
	if (arrow_toplevel_validity_observed.append_rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle imported = nullptr;
		if (duckdb_v2_arrow_importer_next_chunk(rt->importer, &imported, err) == DUCKDB_V2_ERROR_NONE && imported) {
			duckdb_v2_vector_handle out_v = nullptr;
			duckdb_v2_vector_handle out_m = nullptr;
			duckdb_v2_vector_view view {};
			duckdb_v2_vector_view dict_view {};
			if (duckdb_v2_data_chunk_get_vector(imported, 0, &out_v, err) == DUCKDB_V2_ERROR_NONE &&
			    duckdb_v2_data_chunk_get_vector(imported, 1, &out_m, err) == DUCKDB_V2_ERROR_NONE &&
			    duckdb_v2_vector_get_view(out_v, &view, err) == DUCKDB_V2_ERROR_NONE &&
			    duckdb_v2_vector_get_view(out_m, &dict_view, err) == DUCKDB_V2_ERROR_NONE) {
				for (idx_t i = 0; i < rows; i++) {
					auto idx = SelAt(view.sel, i);
					arrow_toplevel_validity_observed.values.push_back(
					    reinterpret_cast<const int64_t *>(view.data)[idx]);
					arrow_toplevel_validity_observed.valid.push_back(RowValid(view, idx));
					arrow_toplevel_validity_observed.dict_valid.push_back(RowValid(dict_view, SelAt(dict_view.sel, i)));
				}
			}
			duckdb_v2_data_chunk_destroy(&imported);
		}
	}
	// A record batch that reports nulls but carries no validity bitmap is malformed: the append must
	// refuse it rather than import wrong data. The error goes to a scratch slot: this expected failure
	// must not poison the callback's own error slot, which the engine reads after exec returns.
	ArrowArray no_bitmap {};
	no_bitmap.length = real_array.length;
	no_bitmap.null_count = 1;
	no_bitmap.n_children = real_array.n_children;
	no_bitmap.children = real_array.children;
	no_bitmap.release = ArrowTopLevelValidityReleaseArray;
	arrow_toplevel_validity_observed.missing_bitmap_rc =
	    duckdb_v2_arrow_importer_append(rt->importer, &no_bitmap, false, true, nullptr);

	real_array.release(&real_array);

	duckdb_v2_data_chunk_destroy(&input);
	ArrowRoundtripDestroy(rt);
	int32_t one = 1;
	void *out = nullptr;
	if (duckdb_v2_vector_get_data_mutable(result, &out, err) == DUCKDB_V2_ERROR_NONE) {
		std::memcpy(out, &one, sizeof(one));
	}
}

} // namespace

// ===========================================================================
// Value round-trips: data chunk -> Arrow array -> data chunk.
// ===========================================================================

TEST_CASE("V2 arrow: a flat round-trip preserves values", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowRoundtrip(fx);

	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(i) IS NOT DISTINCT FROM i) "
	                                "FROM range(-100, 100) t(i)"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(d) IS NOT DISTINCT FROM d) "
	                                "FROM (SELECT i * 1.5 AS d FROM range(50) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(s) IS NOT DISTINCT FROM s) "
	                                "FROM (SELECT 'value_' || i AS s FROM range(50) t(i))"));
	// Booleans, dates and blobs travel through their own Arrow layouts.
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(b) IS NOT DISTINCT FROM b) "
	                                "FROM (SELECT i % 2 = 0 AS b FROM range(20) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(d) IS NOT DISTINCT FROM d) "
	                                "FROM (SELECT DATE '2020-01-01' + i::INTEGER AS d FROM range(20) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(x) IS NOT DISTINCT FROM x) "
	                                "FROM (SELECT ('blob_' || i)::BLOB AS x FROM range(20) t(i))"));
}

TEST_CASE("V2 arrow: NULLs survive a round-trip", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowRoundtrip(fx);

	// Every third value is NULL, so validity has to cross both ways.
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(v) IS NOT DISTINCT FROM v) "
	                                "FROM (SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i END AS v FROM range(60) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(v) IS NOT DISTINCT FROM v) "
	                                "FROM (SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE 's' || i END AS v "
	                                "FROM range(60) t(i))"));
	// An all-NULL column, where the array may carry no buffers at all.
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(v) IS NULL) "
	                                "FROM (SELECT NULL::INTEGER AS v FROM range(10) t(i))"));
}

TEST_CASE("V2 arrow: nested values survive a round-trip", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowRoundtrip(fx);

	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(l) IS NOT DISTINCT FROM l) "
	                                "FROM (SELECT [i, i + 1, i + 2] AS l FROM range(30) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(s) IS NOT DISTINCT FROM s) "
	                                "FROM (SELECT {'a': i, 'b': 'x' || i} AS s FROM range(30) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(m) IS NOT DISTINCT FROM m) "
	                                "FROM (SELECT MAP {'k' || i: i} AS m FROM range(30) t(i))"));
	// A list of structs, and a struct holding a list with NULLs inside.
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(v) IS NOT DISTINCT FROM v) "
	                                "FROM (SELECT [{'a': i}, {'a': i + 1}] AS v FROM range(20) t(i))"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(arrow_roundtrip(v) IS NOT DISTINCT FROM v) "
	                                "FROM (SELECT {'l': [i, NULL, i + 2]} AS v FROM range(20) t(i))"));
}

TEST_CASE("V2 arrow: a multi-column round-trip preserves rows", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowRoundtripRange(fx);

	REQUIRE(ArrowQueryBool(fx.conn, "SELECT bool_and(s = 'r' || i) FROM arrow_roundtrip_range(100)"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT count(*) = 100 AND min(i) = 0 AND max(i) = 99 "
	                                "FROM arrow_roundtrip_range(100)"));
	// More rows than fit in one vector, so several arrays are converted in sequence.
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT count(*) = 5000 AND sum(i) = 12497500 "
	                                "FROM arrow_roundtrip_range(5000)"));
	REQUIRE(ArrowQueryBool(fx.conn, "SELECT count(*) = 0 FROM arrow_roundtrip_range(0)"));
}

// Latches the DuckDB type an importer resolves for a dictionary-encoded (ENUM) column.
std::string enum_probe_type;

void EnumProbeExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle context,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_vector_handle result = nullptr;
	if (duckdb_v2_scalar_function_exec_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_logical_type_handle mood = nullptr;
	auto text_str = Convert("mood");
	if (duckdb_v2_factory_create_type_from_text(factory, &text_str, &mood, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto col = Convert("v");
	auto *rt = ArrowMakeRoundtrip(context, &mood, &col, 1, 0, 0, err);
	duckdb_v2_logical_type_destroy(&mood);
	if (!rt) {
		return;
	}
	duckdb_v2_schema_handle resolved = nullptr;
	if (duckdb_v2_arrow_importer_get_schema(rt->importer, &resolved, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_str field_name = {nullptr, 0};
		duckdb_v2_logical_type_handle field_type = nullptr;
		if (duckdb_v2_schema_get_field(resolved, 0, &field_name, &field_type, err) == DUCKDB_V2_ERROR_NONE) {
			auto rc2 = DUCKDB_V2_ERROR_NONE;
			enum_probe_type = RenderText(
			    [&](char *buf, idx_t cap, idx_t *len) {
				    return duckdb_v2_logical_type_to_text(field_type, buf, cap, len, nullptr);
			    },
			    rc2);
		}
		duckdb_v2_schema_destroy(&resolved);
	}
	ArrowRoundtripDestroy(rt);
	int32_t one = 1;
	void *data = nullptr;
	if (duckdb_v2_vector_get_data_mutable(result, &data, err) == DUCKDB_V2_ERROR_NONE) {
		std::memcpy(data, &one, sizeof(one));
	}
}

// ===========================================================================
// What the correspondence does and does not preserve.
// ===========================================================================

TEST_CASE("V2 arrow: a dictionary column resolves to VARCHAR by default", "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')");
	enum_probe_type.clear();
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto fname = Convert("enum_probe");
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &fname, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = ArrowIdent("x");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, EnumProbeExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT enum_probe(1)", &r) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(r);
	duckdb_v2_result_destroy(&r);
	auto without_lossless = enum_probe_type;

	enum_probe_type.clear();
	ExecSQL(fx.conn, "SET arrow_lossless_conversion = true");
	REQUIRE(Query(fx.conn, "SELECT enum_probe(1)", &r) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(r);
	duckdb_v2_result_destroy(&r);
	// An Arrow dictionary of strings is exactly that: the ENUM identity is not in the schema, so
	// an importer resolving that schema can only report VARCHAR -- with or without
	// arrow_lossless_conversion, which only tags types that have no Arrow match at all. So an
	// ENUM survives the round-trip by value but not by type.
	REQUIRE(without_lossless == "VARCHAR");
	REQUIRE(enum_probe_type == "VARCHAR");
}

// ===========================================================================
// Batch sizes: a maximum in both directions, never a target.
// ===========================================================================

#if (STANDARD_VECTOR_SIZE >= 8)
TEST_CASE("V2 arrow: a batch size caps the output in both directions", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowProbe(fx, "arrow_split_probe", ArrowSplitExec);

	SECTION("no maximum gives one output per input") {
		arrow_split_observed = {};
		arrow_split_rows = 7;
		arrow_split_export_batch = 0;
		arrow_split_import_batch = 0;
		arrow_split_appends = 1;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		REQUIRE(arrow_split_observed.array_rows == std::vector<int64_t> {7});
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {7});
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 3, 4, 5, 6});
	}

	SECTION("the exporter splits a chunk, the last array being short") {
		arrow_split_observed = {};
		arrow_split_rows = 7;
		arrow_split_export_batch = 3;
		arrow_split_import_batch = 0;
		arrow_split_appends = 1;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		REQUIRE(arrow_split_observed.array_rows == std::vector<int64_t> {3, 3, 1});
		// Each array imports as one chunk, since the importer has no maximum of its own.
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {3, 3, 1});
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 3, 4, 5, 6});
	}

	SECTION("the importer splits an array, the last chunk being short") {
		arrow_split_observed = {};
		arrow_split_rows = 7;
		arrow_split_export_batch = 0;
		arrow_split_import_batch = 2;
		arrow_split_appends = 1;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		REQUIRE(arrow_split_observed.array_rows == std::vector<int64_t> {7});
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {2, 2, 2, 1});
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 3, 4, 5, 6});
	}

	SECTION("gathering fills a batch across two inputs") {
		// Two 3-row inputs with a batch of 4: the exporter joins them into a 4-row array plus a
		// 2-row remainder, and the importer does the same to those two arrays. Neither could
		// reach a full batch from one input alone, which is what gathering is for.
		arrow_split_observed = {};
		arrow_split_rows = 3;
		arrow_split_export_batch = 4;
		arrow_split_import_batch = 4;
		arrow_split_appends = 2;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		REQUIRE(arrow_split_observed.array_rows == std::vector<int64_t> {4, 2});
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {4, 2});
		// The same chunk twice, so the values repeat rather than continuing.
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 0, 1, 2});
	}

	SECTION("without a flush the remainder is held back") {
		// One 3-row input with a batch of 4 and no flush: the rows stay gathered, waiting for
		// input that never comes, so nothing is produced.
		arrow_split_observed = {};
		arrow_split_rows = 3;
		arrow_split_export_batch = 4;
		arrow_split_import_batch = 0;
		arrow_split_appends = 1;
		arrow_split_flush = false;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		arrow_split_flush = true;
		REQUIRE(arrow_split_observed.array_rows.empty());
		REQUIRE(arrow_split_observed.chunk_rows.empty());
	}

	SECTION("a maximum larger than the input leaves it whole") {
		// The defining property of a maximum rather than a target: nothing is gathered to reach it.
		arrow_split_observed = {};
		arrow_split_rows = 5;
		arrow_split_export_batch = 1000;
		arrow_split_import_batch = 1000;
		arrow_split_appends = 1;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		REQUIRE(arrow_split_observed.array_rows == std::vector<int64_t> {5});
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {5});
	}
}

TEST_CASE("V2 arrow: a top-level struct array's own validity bitmap is honored", "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')");
	RegisterArrowProbe(fx, "arrow_toplevel_validity_probe", ArrowTopLevelValidityExec);
	RunArrowProbe(fx.conn, "SELECT arrow_toplevel_validity_probe(1)");

	REQUIRE(arrow_toplevel_validity_observed.append_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(arrow_toplevel_validity_observed.valid.size() == 16);
	REQUIRE(arrow_toplevel_validity_observed.dict_valid.size() == 16);
	// Rows 1, 2 and 9 were marked null in the wrapper's own validity buffer; the rest were not.
	for (idx_t i = 0; i < 16; i++) {
		bool expect_null = (i == 1 || i == 2 || i == 9);
		INFO("row " << i << " value " << arrow_toplevel_validity_observed.values[i]);
		REQUIRE(arrow_toplevel_validity_observed.valid[i] == !expect_null);
		REQUIRE(arrow_toplevel_validity_observed.dict_valid[i] == !expect_null);
	}
	REQUIRE(arrow_toplevel_validity_observed.missing_bitmap_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 arrow: the exporter accepts input without draining first", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowProbe(fx, "arrow_split_probe", ArrowSplitExec);
	arrow_split_observed = {};
	arrow_split_rows = 4;
	arrow_split_export_batch = 0;
	arrow_split_import_batch = 0;
	arrow_split_appends = 1;
	RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
	// The exporter gathers, so it accepts more input without the produced arrays being taken first.
	REQUIRE(arrow_split_observed.second_append_rc == DUCKDB_V2_ERROR_NONE);
	// And a chunk whose types disagree with the exporter never reaches the conversion.
	REQUIRE(arrow_split_observed.type_mismatch_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 arrow: a dictionary column survives an importer split", "[capi_v2][arrow]") {
	EnvFixture fx;
	RegisterArrowProbe(fx, "arrow_split_probe", ArrowSplitExec);
	arrow_split_export_batch = 0;
	arrow_split_appends = 1;

	SECTION("a dictionary smaller than the import batch") {
		ExecSQL(fx.conn, "CREATE TYPE arrow_split_enum AS ENUM ('a', 'b', 'c')");
		arrow_split_dict_size = 3;
		arrow_split_observed = {};
		arrow_split_rows = 7;
		arrow_split_import_batch = 2;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		arrow_split_dict_size = 0;
		REQUIRE(arrow_split_observed.array_rows == std::vector<int64_t> {7});
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {2, 2, 2, 1});
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 0, 1, 2, 0});
	}

	SECTION("a dictionary larger than the import batch") {
		ExecSQL(fx.conn, "CREATE TYPE arrow_split_enum AS ENUM ('a','b','c','d','e','f','g','h','i','j')");
		arrow_split_dict_size = 10;
		arrow_split_observed = {};
		arrow_split_rows = 7;
		arrow_split_import_batch = 3;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		arrow_split_dict_size = 0;
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {3, 3, 1});
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 3, 4, 5, 6});
	}

	SECTION("an exact multiple of the import batch") {
		ExecSQL(fx.conn, "CREATE TYPE arrow_split_enum AS ENUM ('a', 'b', 'c')");
		arrow_split_dict_size = 3;
		arrow_split_observed = {};
		arrow_split_rows = 6;
		arrow_split_import_batch = 2;
		RunArrowProbe(fx.conn, "SELECT arrow_split_probe(1)");
		arrow_split_dict_size = 0;
		REQUIRE(arrow_split_observed.chunk_rows == std::vector<idx_t> {2, 2, 2});
		REQUIRE(arrow_split_observed.values == std::vector<int64_t> {0, 1, 2, 0, 1, 2});
	}
}
#endif

// ===========================================================================
// Argument rejection.
// ===========================================================================

TEST_CASE("V2 arrow: functions guard null arguments", "[capi_v2][arrow]") {
	ArrowSchema schema {};
	duckdb_v2_arrow_importer_handle importer = nullptr;
	REQUIRE(duckdb_v2_arrow_importer_create(nullptr, &schema, 0, &importer, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(importer == nullptr);
	duckdb_v2_schema_handle resolved = nullptr;
	REQUIRE(duckdb_v2_arrow_importer_get_schema(nullptr, &resolved, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(resolved == nullptr);
	ArrowArray array {};
	REQUIRE(duckdb_v2_arrow_importer_append(nullptr, &array, true, false, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	REQUIRE(duckdb_v2_arrow_importer_next_chunk(nullptr, &chunk, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(chunk == nullptr);

	duckdb_v2_arrow_exporter_handle exporter = nullptr;
	duckdb_v2_str name = Convert("a");
	REQUIRE(duckdb_v2_arrow_exporter_create(nullptr, nullptr, &name, 0, 0, &exporter, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(exporter == nullptr);
	REQUIRE(duckdb_v2_arrow_exporter_get_schema(nullptr, &schema, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_exporter_append(nullptr, &chunk, false, false, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_exporter_next_array(nullptr, &array, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Both destroys are null-safe and idempotent.
	REQUIRE(duckdb_v2_arrow_importer_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_arrow_importer_destroy(&importer) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_arrow_exporter_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_arrow_exporter_destroy(&exporter) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Query results as Arrow arrays.
// ===========================================================================

namespace {

struct ArrowResult {
	duckdb_v2_arrow_result_handle handle = nullptr;
	ArrowResult() = default;
	ArrowResult(const ArrowResult &) = delete;
	ArrowResult &operator=(const ArrowResult &) = delete;
	~ArrowResult() {
		duckdb_v2_arrow_result_destroy(&handle);
	}
	duckdb_v2_arrow_result_handle *operator&() {
		return &handle;
	}
	operator duckdb_v2_arrow_result_handle() const {
		return handle;
	}
};

struct OwnedSchema {
	ArrowSchema schema {};
	~OwnedSchema() {
		if (schema.release) {
			schema.release(&schema);
		}
	}
};

struct OwnedStream {
	ArrowArrayStream stream {};
	~OwnedStream() {
		if (stream.release) {
			stream.release(&stream);
		}
	}
};

//! Arrays collected from a result, released together.
struct ArrowBatches {
	std::vector<ArrowArray> arrays;
	ArrowBatches() = default;
	ArrowBatches(const ArrowBatches &) = delete;
	ArrowBatches &operator=(const ArrowBatches &) = delete;
	~ArrowBatches() {
		for (auto &array : arrays) {
			if (array.release) {
				array.release(&array);
			}
		}
	}
	idx_t RowCount() const {
		idx_t rows = 0;
		for (auto &array : arrays) {
			rows += static_cast<idx_t>(array.length);
		}
		return rows;
	}
	idx_t MaxLength() const {
		idx_t longest = 0;
		for (auto &array : arrays) {
			longest = std::max<idx_t>(longest, static_cast<idx_t>(array.length));
		}
		return longest;
	}
};

duckdb_v2_sql_statement_handle ParseOne(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_statement_iterator_handle iter = nullptr;
	REQUIRE(duckdb_v2_parse_sql(conn, sql, true, &iter, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_handle statement = nullptr;
	auto rc = duckdb_v2_statement_iterator_next(iter, &statement, nullptr);
	duckdb_v2_statement_iterator_destroy(&iter);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(statement != nullptr);
	return statement;
}

DUCKDB_V2_ERROR QueryArrow(duckdb_v2_connection_handle conn, const char *sql, idx_t batch_size,
                           duckdb_v2_arrow_result_handle *out_result, duckdb_v2_error_info_handle *err = nullptr,
                           const duckdb_v2_identifier_t *names = nullptr,
                           const duckdb_v2_value_handle *values = nullptr, idx_t value_count = 0) {
	auto statement = ParseOne(conn, sql);
	auto rc =
	    duckdb_v2_statement_execute_arrow(conn, statement, names, values, value_count, batch_size, out_result, err);
	duckdb_v2_sql_statement_destroy(&statement);
	return rc;
}

//! One assertion however many arrays the result has, so the assertion count does not depend on timing.
void FetchAll(duckdb_v2_arrow_result_handle result, ArrowBatches &out) {
	auto rc = DUCKDB_V2_ERROR_NONE;
	while (true) {
		ArrowArray array {};
		rc = duckdb_v2_arrow_result_fetch_array(result, &array, nullptr);
		if (rc != DUCKDB_V2_ERROR_NONE || !array.release) {
			break;
		}
		out.arrays.push_back(array);
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
}

void StepAll(duckdb_v2_arrow_result_handle result, ArrowBatches &out) {
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	bool array_matches_status = true;
	while (true) {
		ArrowArray array {};
		rc = duckdb_v2_arrow_result_step(result, &array, &status, nullptr);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		array_matches_status =
		    array_matches_status && (status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK) == !!array.release;
		if (array.release) {
			out.arrays.push_back(array);
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED || status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_arrow_result_wait(result, nullptr);
			if (rc != DUCKDB_V2_ERROR_NONE) {
				break;
			}
		}
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED);
	REQUIRE(array_matches_status);
}

void StreamAll(ArrowArrayStream &stream, ArrowBatches &out) {
	int rc = 0;
	while (true) {
		ArrowArray array {};
		rc = stream.get_next(&stream, &array);
		if (rc != 0 || !array.release) {
			break;
		}
		out.arrays.push_back(array);
	}
	REQUIRE(rc == 0);
}

idx_t RowIndex(const ArrowArray &batch, idx_t column, idx_t row) {
	return static_cast<idx_t>(batch.offset + batch.children[column]->offset) + row;
}

int64_t Int64At(const ArrowArray &batch, idx_t column, idx_t row) {
	return static_cast<const int64_t *>(batch.children[column]->buffers[1])[RowIndex(batch, column, row)];
}

bool IsValidAt(const ArrowArray &batch, idx_t column, idx_t row) {
	auto bits = static_cast<const uint8_t *>(batch.children[column]->buffers[0]);
	auto index = RowIndex(batch, column, row);
	return !bits || ((bits[index / 8] >> (index % 8)) & 1);
}

//! An array in the "u" format: 32-bit offsets.
std::string StringValue(const ArrowArray &strings, idx_t index) {
	auto offsets = static_cast<const int32_t *>(strings.buffers[1]);
	auto data = static_cast<const char *>(strings.buffers[2]);
	return std::string(data + offsets[index], static_cast<size_t>(offsets[index + 1] - offsets[index]));
}

std::string StringAt(const ArrowArray &batch, idx_t column, idx_t row) {
	return StringValue(*batch.children[column], RowIndex(batch, column, row));
}

//! Whether column 0, a BIGINT, counts up from `first` across all arrays.
bool CountsUpFrom(const ArrowBatches &batches, int64_t first) {
	auto expected = first;
	for (auto &array : batches.arrays) {
		for (idx_t row = 0; row < static_cast<idx_t>(array.length); row++) {
			if (Int64At(array, 0, row) != expected++) {
				return false;
			}
		}
	}
	return true;
}

std::vector<std::string> ChildFormats(const ArrowSchema &schema) {
	std::vector<std::string> formats;
	for (int64_t i = 0; i < schema.n_children; i++) {
		formats.emplace_back(schema.children[i]->format);
	}
	return formats;
}

std::vector<std::string> ChildNames(const ArrowSchema &schema) {
	std::vector<std::string> names;
	for (int64_t i = 0; i < schema.n_children; i++) {
		names.emplace_back(schema.children[i]->name);
	}
	return names;
}

bool SameText(const char *a, const char *b) {
	return (!a && !b) || (a && b && std::strcmp(a, b) == 0);
}

bool SameSchema(const ArrowSchema &a, const ArrowSchema &b) {
	if (!SameText(a.format, b.format) || !SameText(a.name, b.name) || a.flags != b.flags ||
	    a.n_children != b.n_children || !a.dictionary != !b.dictionary || !a.metadata != !b.metadata) {
		return false;
	}
	for (int64_t i = 0; i < a.n_children; i++) {
		if (!SameSchema(*a.children[i], *b.children[i])) {
			return false;
		}
	}
	return !a.dictionary || SameSchema(*a.dictionary, *b.dictionary);
}

void UnusedStreamRelease(ArrowArrayStream *) {
}

std::string ErrorText(duckdb_v2_error_info_handle err) {
	duckdb_v2_str text = {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(err, &text) == DUCKDB_V2_ERROR_NONE);
	return std::string(text.ptr ? text.ptr : "", text.len);
}

} // namespace

TEST_CASE("V2 arrow result: rows arrive in order, in arrays of at most batch_size rows", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE 'v' || i END AS s FROM range(10000) t(i)",
	                   1000, &r) == DUCKDB_V2_ERROR_NONE);

	ArrowBatches batches;
	FetchAll(r, batches);
	REQUIRE(batches.RowCount() == 10000);
	REQUIRE(batches.MaxLength() <= 1000);
	REQUIRE(CountsUpFrom(batches, 0));
	bool strings_match = true;
	idx_t i = 0;
	for (auto &array : batches.arrays) {
		for (idx_t row = 0; row < static_cast<idx_t>(array.length); row++, i++) {
			bool valid = i % 3 != 0;
			strings_match = strings_match && IsValidAt(array, 1, row) == valid &&
			                (!valid || StringAt(array, 1, row) == "v" + std::to_string(i));
		}
	}
	REQUIRE(strings_match);

	ArrowArray after_end {};
	REQUIRE(duckdb_v2_arrow_result_fetch_array(r, &after_end, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(after_end.release == nullptr);
}

TEST_CASE("V2 arrow result: step and wait deliver every array", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(5000) t(i)", 700, &r) == DUCKDB_V2_ERROR_NONE);

	ArrowBatches batches;
	StepAll(r, batches);
	REQUIRE(batches.RowCount() == 5000);
	REQUIRE(batches.MaxLength() <= 700);
	REQUIRE(CountsUpFrom(batches, 0));

	for (int i = 0; i < 3; i++) {
		ArrowArray array {};
		auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		REQUIRE(duckdb_v2_arrow_result_step(r, &array, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED);
		REQUIRE(array.release == nullptr);
	}
}

TEST_CASE("V2 arrow result: a batch size of 0 means 131072 rows", "[capi_v2][arrow]") {
	EnvFixture fx;
	// One thread gives exact batch sizes.
	ExecSQL(fx.conn, "SET threads = 1");
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(300000) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);

	ArrowBatches batches;
	FetchAll(r, batches);
	std::vector<int64_t> lengths;
	for (auto &array : batches.arrays) {
		lengths.push_back(array.length);
	}
	REQUIRE(lengths == std::vector<int64_t> {131072, 131072, 37856});
	REQUIRE(CountsUpFrom(batches, 0));
}

TEST_CASE("V2 arrow result: the schema describes the arrays before, during and after the rows", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT 42::BIGINT AS answer, 'x' AS label", 0, &r) == DUCKDB_V2_ERROR_NONE);

	OwnedSchema before;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &before.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(before.schema.format) == "+s");
	REQUIRE(ChildNames(before.schema) == std::vector<std::string> {"answer", "label"});
	REQUIRE(ChildFormats(before.schema) == std::vector<std::string> {"l", "u"});

	auto result_type = DUCKDB_V2_RESULT_TYPE_NOTHING;
	REQUIRE(duckdb_v2_arrow_result_get_result_type(r, &result_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(result_type == DUCKDB_V2_RESULT_TYPE_QUERY_RESULT);
	auto statement_type = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_arrow_result_get_statement_type(r, &statement_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(statement_type == DUCKDB_V2_STATEMENT_TYPE_SELECT);

	ArrowBatches batches;
	FetchAll(r, batches);
	REQUIRE(batches.RowCount() == 1);
	REQUIRE(batches.arrays[0].n_children == 2);
	REQUIRE(Int64At(batches.arrays[0], 0, 0) == 42);
	REQUIRE(StringAt(batches.arrays[0], 1, 0) == "x");

	// Every copy is independent of the result and of the other copies.
	OwnedSchema after;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &after.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_arrow_result_destroy(&r);
	REQUIRE(ChildNames(after.schema) == ChildNames(before.schema));
	REQUIRE(ChildFormats(after.schema) == ChildFormats(before.schema));
}

TEST_CASE("V2 arrow result: parameters bind by position and by name", "[capi_v2][arrow]") {
	EnvFixture fx;
	auto value = MakeInt64Value(fx.factory, 10);

	ArrowResult positional;
	REQUIRE(QueryArrow(fx.conn, "SELECT $1::BIGINT + i FROM range(3) t(i)", 0, &positional, nullptr, nullptr, &value,
	                   1) == DUCKDB_V2_ERROR_NONE);
	ArrowBatches positional_rows;
	FetchAll(positional, positional_rows);
	REQUIRE(positional_rows.RowCount() == 3);
	REQUIRE(CountsUpFrom(positional_rows, 10));
	duckdb_v2_arrow_result_destroy(&positional);

	auto name = ArrowIdent("base");
	ArrowResult named;
	REQUIRE(QueryArrow(fx.conn, "SELECT $base::BIGINT + i FROM range(3) t(i)", 0, &named, nullptr, &name, &value, 1) ==
	        DUCKDB_V2_ERROR_NONE);
	ArrowBatches named_rows;
	FetchAll(named, named_rows);
	REQUIRE(named_rows.RowCount() == 3);
	REQUIRE(CountsUpFrom(named_rows, 10));

	duckdb_v2_value_destroy(&value);
}

TEST_CASE("V2 arrow result: a prepared statement executes as an Arrow result, repeatedly", "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t (i BIGINT)");

	auto statement = ParseOne(fx.conn, "SELECT i + $1::BIGINT FROM range(4) t(i)");
	duckdb_v2_prepared_statement_handle prepared = nullptr;
	REQUIRE(duckdb_v2_prepared_statement_create(fx.conn, statement, false, &prepared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_destroy(&statement);
	for (int64_t base : {0, 100}) {
		auto value = MakeInt64Value(fx.factory, base);
		ArrowResult r;
		REQUIRE(duckdb_v2_prepared_statement_execute_arrow(prepared, nullptr, &value, 1, 0, &r, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		duckdb_v2_value_destroy(&value);
		auto statement_type = DUCKDB_V2_STATEMENT_TYPE_INVALID;
		REQUIRE(duckdb_v2_arrow_result_get_statement_type(r, &statement_type, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(statement_type == DUCKDB_V2_STATEMENT_TYPE_SELECT);
		ArrowBatches batches;
		FetchAll(r, batches);
		REQUIRE(batches.RowCount() == 4);
		REQUIRE(CountsUpFrom(batches, base));
	}
	duckdb_v2_prepared_statement_destroy(&prepared);

	statement = ParseOne(fx.conn, "INSERT INTO t SELECT * FROM range($1::BIGINT)");
	REQUIRE(duckdb_v2_prepared_statement_create(fx.conn, statement, false, &prepared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_destroy(&statement);
	auto value = MakeInt64Value(fx.factory, 17);
	ArrowResult r;
	REQUIRE(duckdb_v2_prepared_statement_execute_arrow(prepared, nullptr, &value, 1, 0, &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&value);
	idx_t rows_changed = 0;
	REQUIRE(duckdb_v2_arrow_result_drain(r, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 17);
	duckdb_v2_arrow_result_destroy(&r);
	duckdb_v2_prepared_statement_destroy(&prepared);
}

TEST_CASE("V2 arrow result: an INSERT reports its changed rows, DDL reports nothing", "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t (i BIGINT)");

	ArrowResult insert;
	REQUIRE(QueryArrow(fx.conn, "INSERT INTO t SELECT * FROM range(1234)", 0, &insert) == DUCKDB_V2_ERROR_NONE);
	auto result_type = DUCKDB_V2_RESULT_TYPE_NOTHING;
	REQUIRE(duckdb_v2_arrow_result_get_result_type(insert, &result_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(result_type == DUCKDB_V2_RESULT_TYPE_CHANGED_ROWS);
	auto statement_type = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_arrow_result_get_statement_type(insert, &statement_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(statement_type == DUCKDB_V2_STATEMENT_TYPE_INSERT);
	idx_t rows_changed = 0;
	REQUIRE(duckdb_v2_arrow_result_drain(insert, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 1234);
	// The count was consumed by the first drain.
	REQUIRE(duckdb_v2_arrow_result_drain(insert, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 0);
	duckdb_v2_arrow_result_destroy(&insert);

	// Fetched instead of drained, the count is a one-row BIGINT array.
	ArrowResult fetched;
	REQUIRE(QueryArrow(fx.conn, "INSERT INTO t VALUES (1), (2), (3), (4), (5)", 0, &fetched) == DUCKDB_V2_ERROR_NONE);
	OwnedSchema schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(fetched, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ChildNames(schema.schema) == std::vector<std::string> {"Count"});
	REQUIRE(ChildFormats(schema.schema) == std::vector<std::string> {"l"});
	ArrowBatches batches;
	FetchAll(fetched, batches);
	REQUIRE(batches.RowCount() == 1);
	REQUIRE(Int64At(batches.arrays[0], 0, 0) == 5);
	duckdb_v2_arrow_result_destroy(&fetched);

	ArrowResult ddl;
	REQUIRE(QueryArrow(fx.conn, "CREATE TABLE u (x INTEGER)", 0, &ddl) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_arrow_result_get_result_type(ddl, &result_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(result_type == DUCKDB_V2_RESULT_TYPE_NOTHING);
	OwnedSchema ddl_schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(ddl, &ddl_schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ddl_schema.schema.n_children == 1);
	REQUIRE(duckdb_v2_arrow_result_drain(ddl, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 0);
	duckdb_v2_arrow_result_destroy(&ddl);

	QueryResult count;
	REQUIRE(Query(fx.conn, "SELECT count(*) FROM t, u", &count) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(count) == 1);
}

TEST_CASE("V2 arrow result: a CALL statement returns its rows", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "CALL range(5000)", 1000, &r) == DUCKDB_V2_ERROR_NONE);
	auto statement_type = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_arrow_result_get_statement_type(r, &statement_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(statement_type == DUCKDB_V2_STATEMENT_TYPE_CALL);
	OwnedSchema schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ChildFormats(schema.schema) == std::vector<std::string> {"l"});

	ArrowBatches batches;
	StepAll(r, batches);
	REQUIRE(batches.RowCount() == 5000);
	REQUIRE(batches.MaxLength() <= 1000);
	REQUIRE(CountsUpFrom(batches, 0));
}

TEST_CASE("V2 arrow result: an expanding statement reports its schema once stepped", "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE sales (city VARCHAR, year INT, amount INT)");
	ExecSQL(fx.conn,
	        "INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20), ('rtm', 2023, 30), ('rtm', 2024, 40)");

	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "PIVOT sales ON year USING sum(amount)", 0, &r) == DUCKDB_V2_ERROR_NONE);
	ArrowSchema deferred {};
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &deferred, nullptr) == DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED);
	REQUIRE(deferred.release == nullptr);
	auto result_type = DUCKDB_V2_RESULT_TYPE_NOTHING;
	REQUIRE(duckdb_v2_arrow_result_get_result_type(r, &result_type, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED);

	ArrowBatches batches;
	FetchAll(r, batches);
	REQUIRE(batches.RowCount() == 2);
	OwnedSchema schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ChildNames(schema.schema) == std::vector<std::string> {"city", "2023", "2024"});

	// A stream made before any step reaches the schema itself.
	ArrowResult unstepped;
	REQUIRE(QueryArrow(fx.conn, "PIVOT sales ON year USING sum(amount)", 0, &unstepped) == DUCKDB_V2_ERROR_NONE);
	OwnedStream stream;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&unstepped, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
	OwnedSchema stream_schema;
	REQUIRE(stream.stream.get_schema(&stream.stream, &stream_schema.schema) == 0);
	REQUIRE(ChildNames(stream_schema.schema) == ChildNames(schema.schema));
	ArrowBatches streamed;
	StreamAll(stream.stream, streamed);
	REQUIRE(streamed.RowCount() == 2);
}

TEST_CASE("V2 arrow result: an expanding statement without rows reports the Success schema once drained",
          "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t AS SELECT range AS i FROM range(10)");

	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()", 0, &r) == DUCKDB_V2_ERROR_NONE);
	ArrowSchema deferred {};
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &deferred, nullptr) == DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED);

	ArrowBatches batches;
	FetchAll(r, batches);
	REQUIRE(batches.RowCount() == 0);
	OwnedSchema schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ChildNames(schema.schema) == std::vector<std::string> {"Success"});
	auto result_type = DUCKDB_V2_RESULT_TYPE_QUERY_RESULT;
	REQUIRE(duckdb_v2_arrow_result_get_result_type(r, &result_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(result_type == DUCKDB_V2_RESULT_TYPE_NOTHING);
}

TEST_CASE("V2 arrow result: an execution error is sticky", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT error('boom arrow') FROM range(5) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);

	for (int i = 0; i < 2; i++) {
		ArrowArray array {};
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_arrow_result_fetch_array(r, &array, &err) != DUCKDB_V2_ERROR_NONE);
		REQUIRE(array.release == nullptr);
		REQUIRE(ErrorText(err).find("boom arrow") != std::string::npos);
		duckdb_v2_error_info_destroy(&err);
	}
	ArrowArray array {};
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	REQUIRE(duckdb_v2_arrow_result_step(r, &array, &status, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(array.release == nullptr);

	// The error ended the query, so the connection is free.
	QueryResult next;
	REQUIRE(Query(fx.conn, "SELECT 1", &next) == DUCKDB_V2_ERROR_NONE);
}

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2 arrow result: cancellation is a step status and a fetch_array error", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(10000000) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto rc = DUCKDB_V2_ERROR_NONE;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	for (int i = 0; i < 1000 && rc == DUCKDB_V2_ERROR_NONE && status != DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED; i++) {
		ArrowArray array {};
		rc = duckdb_v2_arrow_result_step(r, &array, &status, nullptr);
		if (array.release) {
			array.release(&array);
		}
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);

	ArrowArray array {};
	REQUIRE(duckdb_v2_arrow_result_step(r, &array, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED);
	REQUIRE(array.release == nullptr);
	REQUIRE(duckdb_v2_arrow_result_fetch_array(r, &array, nullptr) == DUCKDB_V2_ERROR_RUNTIME_INTERRUPT);
	REQUIRE(array.release == nullptr);
}
#endif

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2 arrow result: an interrupted C stream fails get_next", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(10000000) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);
	OwnedStream stream;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_interrupt(fx.conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	ArrowArray array {};
	REQUIRE(stream.stream.get_next(&stream.stream, &array) == EIO);
	REQUIRE(array.release == nullptr);
	auto message = stream.stream.get_last_error(&stream.stream);
	REQUIRE(message != nullptr);
	REQUIRE(std::string(message).find("Interrupted") != std::string::npos);
}
#endif

TEST_CASE("V2 arrow result: arrays and schemas outlive the result and the database", "[capi_v2][arrow]") {
	struct LifetimeCase {
		const char *sql;
		int64_t columns;
	};
	// SELECT streams its arrays; CALL completes first and hands out shared exports of retained arrays.
	for (auto &lifetime_case :
	     {LifetimeCase {"SELECT i, 'v' || i AS s FROM range(3000) t(i)", 2}, LifetimeCase {"CALL range(3000)", 1}}) {
		duckdb_v2_environment_handle env = nullptr;
		duckdb_v2_instance_handle instance = nullptr;
		duckdb_v2_connection_handle conn = nullptr;
		REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_create(instance, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

		ArrowBatches batches;
		OwnedSchema schema;
		{
			ArrowResult r;
			REQUIRE(QueryArrow(conn, lifetime_case.sql, 1000, &r) == DUCKDB_V2_ERROR_NONE);
			REQUIRE(duckdb_v2_arrow_result_get_schema(r, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
			FetchAll(r, batches);
		}
		duckdb_v2_connection_destroy(&conn);
		duckdb_v2_instance_destroy(&instance);
		REQUIRE(duckdb_v2_environment_destroy(&env) == DUCKDB_V2_ERROR_NONE);

		REQUIRE(schema.schema.n_children == lifetime_case.columns);
		REQUIRE(ChildFormats(schema.schema)[0] == "l");
		REQUIRE(batches.RowCount() == 3000);
		REQUIRE(CountsUpFrom(batches, 0));
		if (lifetime_case.columns == 2) {
			bool strings_match = true;
			idx_t i = 0;
			for (auto &array : batches.arrays) {
				for (idx_t row = 0; row < static_cast<idx_t>(array.length); row++, i++) {
					strings_match = strings_match && StringAt(array, 1, row) == "v" + std::to_string(i);
				}
			}
			REQUIRE(strings_match);
		}
	}
}

TEST_CASE("V2 arrow result: a connection runs one result at a time, in either format", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult arrow;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(100000) t(i)", 0, &arrow) == DUCKDB_V2_ERROR_NONE);
	QueryResult chunks;
	REQUIRE(Query(fx.conn, "SELECT 1", &chunks) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	ArrowResult second;
	REQUIRE(QueryArrow(fx.conn, "SELECT 1", 0, &second) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	REQUIRE(second.handle == nullptr);

	duckdb_v2_arrow_result_destroy(&arrow);
	REQUIRE(arrow.handle == nullptr);
	REQUIRE(Query(fx.conn, "SELECT 1", &chunks) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(QueryArrow(fx.conn, "SELECT 1", 0, &second) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	duckdb_v2_result_destroy(&chunks.handle);
	REQUIRE(QueryArrow(fx.conn, "SELECT 1", 0, &second) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 arrow result: a C stream continues where fetching left off", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(10000) t(i)", 1000, &r) == DUCKDB_V2_ERROR_NONE);
	OwnedSchema result_schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &result_schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	ArrowBatches fetched;
	ArrowArray first {};
	REQUIRE(duckdb_v2_arrow_result_fetch_array(r, &first, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(first.release != nullptr);
	fetched.arrays.push_back(first);

	OwnedStream stream;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(r.handle == nullptr);
	OwnedSchema stream_schema;
	REQUIRE(stream.stream.get_schema(&stream.stream, &stream_schema.schema) == 0);
	REQUIRE(ChildNames(stream_schema.schema) == ChildNames(result_schema.schema));
	REQUIRE(ChildFormats(stream_schema.schema) == ChildFormats(result_schema.schema));

	ArrowBatches streamed;
	StreamAll(stream.stream, streamed);
	REQUIRE(fetched.RowCount() + streamed.RowCount() == 10000);
	REQUIRE(CountsUpFrom(streamed, static_cast<int64_t>(fetched.RowCount())));
	ArrowArray after_end {};
	REQUIRE(stream.stream.get_next(&stream.stream, &after_end) == 0);
	REQUIRE(after_end.release == nullptr);
	REQUIRE(stream.stream.get_last_error(&stream.stream) == nullptr);

	stream.stream.release(&stream.stream);
	REQUIRE(stream.stream.release == nullptr);
	QueryResult next;
	REQUIRE(Query(fx.conn, "SELECT 1", &next) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 arrow result: releasing an unfinished C stream frees the connection", "[capi_v2][arrow]") {
	EnvFixture fx;
	for (bool fetch_first : {false, true}) {
		ArrowResult r;
		REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(10000000) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);
		OwnedStream stream;
		REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
		if (fetch_first) {
			ArrowArray array {};
			REQUIRE(stream.stream.get_next(&stream.stream, &array) == 0);
			REQUIRE(array.release != nullptr);
			array.release(&array);
		}
		stream.stream.release(&stream.stream);
		QueryResult next;
		REQUIRE(Query(fx.conn, "SELECT 1", &next) == DUCKDB_V2_ERROR_NONE);
	}
}

TEST_CASE("V2 arrow result: a C stream reports an execution error through get_last_error", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT error('boom stream') FROM range(5) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);
	OwnedStream stream;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);

	for (int i = 0; i < 2; i++) {
		ArrowArray array {};
		REQUIRE(stream.stream.get_next(&stream.stream, &array) != 0);
		REQUIRE(array.release == nullptr);
		auto message = stream.stream.get_last_error(&stream.stream);
		REQUIRE(message != nullptr);
		REQUIRE(std::string(message).find("boom stream") != std::string::npos);
	}
}

TEST_CASE("V2 arrow result: an unfinished result survives disconnect", "[capi_v2][arrow]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_create(instance, &conn, nullptr) == DUCKDB_V2_ERROR_NONE);

	ArrowResult r;
	REQUIRE(QueryArrow(conn, "SELECT i FROM range(100000) t(i)", 0, &r) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_connection_destroy(&conn);
	duckdb_v2_instance_destroy(&instance);

	ArrowBatches batches;
	FetchAll(r, batches);
	REQUIRE(batches.RowCount() == 100000);
	REQUIRE(CountsUpFrom(batches, 0));
	duckdb_v2_arrow_result_destroy(&r);
	duckdb_v2_environment_destroy(&env);
}

TEST_CASE("V2 arrow result: a result without rows ends without an array", "[capi_v2][arrow]") {
	EnvFixture fx;
	const char *sql = "SELECT i FROM range(100) t(i) WHERE i < 0";

	ArrowResult fetched;
	REQUIRE(QueryArrow(fx.conn, sql, 0, &fetched) == DUCKDB_V2_ERROR_NONE);
	OwnedSchema schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(fetched, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ChildFormats(schema.schema) == std::vector<std::string> {"l"});
	ArrowBatches fetched_batches;
	FetchAll(fetched, fetched_batches);
	REQUIRE(fetched_batches.arrays.empty());
	duckdb_v2_arrow_result_destroy(&fetched);

	ArrowResult stepped;
	REQUIRE(QueryArrow(fx.conn, sql, 0, &stepped) == DUCKDB_V2_ERROR_NONE);
	ArrowBatches stepped_batches;
	StepAll(stepped, stepped_batches);
	REQUIRE(stepped_batches.arrays.empty());
	duckdb_v2_arrow_result_destroy(&stepped);

	ArrowResult streamed;
	REQUIRE(QueryArrow(fx.conn, sql, 0, &streamed) == DUCKDB_V2_ERROR_NONE);
	OwnedStream stream;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&streamed, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
	OwnedSchema stream_schema;
	REQUIRE(stream.stream.get_schema(&stream.stream, &stream_schema.schema) == 0);
	REQUIRE(SameSchema(schema.schema, stream_schema.schema));
	ArrowBatches streamed_batches;
	StreamAll(stream.stream, streamed_batches);
	REQUIRE(streamed_batches.arrays.empty());
}

TEST_CASE("V2 arrow result: nested, dictionary and decimal columns keep their structure", "[capi_v2][arrow]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')");
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn,
	                   "SELECT [i, i + 1] AS l, {'a': i, 'b': 'x' || i} AS s, "
	                   "(['sad', 'ok', 'happy'][i % 3 + 1])::mood AS m, (i / 4)::DECIMAL(18, 3) AS d "
	                   "FROM range(100) t(i)",
	                   0, &r) == DUCKDB_V2_ERROR_NONE);

	OwnedSchema schema;
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, &schema.schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ChildNames(schema.schema) == std::vector<std::string> {"l", "s", "m", "d"});
	auto &list = *schema.schema.children[0];
	REQUIRE(std::string(list.format) == "+l");
	REQUIRE(ChildFormats(list) == std::vector<std::string> {"l"});
	auto &record = *schema.schema.children[1];
	REQUIRE(std::string(record.format) == "+s");
	REQUIRE(ChildNames(record) == std::vector<std::string> {"a", "b"});
	REQUIRE(ChildFormats(record) == std::vector<std::string> {"l", "u"});
	auto &mood = *schema.schema.children[2];
	REQUIRE(std::string(mood.format) == "C");
	REQUIRE(mood.dictionary != nullptr);
	REQUIRE(std::string(mood.dictionary->format) == "u");
	REQUIRE(std::string(schema.schema.children[3]->format).rfind("d:18,3", 0) == 0);

	// The stream's schema is a second copy: the same structure, none of the same nodes.
	OwnedStream stream;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
	OwnedSchema stream_schema;
	REQUIRE(stream.stream.get_schema(&stream.stream, &stream_schema.schema) == 0);
	REQUIRE(SameSchema(schema.schema, stream_schema.schema));
	REQUIRE(stream_schema.schema.children[2]->dictionary != mood.dictionary);

	ArrowBatches batches;
	StreamAll(stream.stream, batches);
	REQUIRE(batches.RowCount() == 100);
	bool values_match = true;
	idx_t i = 0;
	for (auto &array : batches.arrays) {
		auto &lists = *array.children[0];
		auto list_offsets = static_cast<const int32_t *>(lists.buffers[1]);
		auto &elements = *lists.children[0];
		auto element_values = static_cast<const int64_t *>(elements.buffers[1]);
		auto &records = *array.children[1];
		auto &record_a = *records.children[0];
		auto a_values = static_cast<const int64_t *>(record_a.buffers[1]);
		auto &moods = *array.children[2];
		auto mood_indexes = static_cast<const uint8_t *>(moods.buffers[1]);
		for (idx_t row = 0; row < static_cast<idx_t>(array.length); row++, i++) {
			auto list_index = static_cast<idx_t>(array.offset + lists.offset) + row;
			auto first = static_cast<idx_t>(list_offsets[list_index] + elements.offset);
			values_match = values_match && list_offsets[list_index + 1] - list_offsets[list_index] == 2 &&
			               element_values[first] == static_cast<int64_t>(i) &&
			               element_values[first + 1] == static_cast<int64_t>(i + 1);
			auto a_index = static_cast<idx_t>(array.offset + records.offset + record_a.offset) + row;
			values_match = values_match && a_values[a_index] == static_cast<int64_t>(i);
			auto mood_index = static_cast<idx_t>(array.offset + moods.offset) + row;
			values_match = values_match && mood_indexes[mood_index] == i % 3;
		}
	}
	REQUIRE(values_match);
	auto &dictionary = *batches.arrays[0].children[2]->dictionary;
	REQUIRE(dictionary.length == 3);
	REQUIRE(StringValue(dictionary, 0) == "sad");
	REQUIRE(StringValue(dictionary, 2) == "happy");
}

TEST_CASE("V2 arrow result: a prepared Arrow result honours its batch size and converts to a stream",
          "[capi_v2][arrow]") {
	EnvFixture fx;
	auto statement = ParseOne(fx.conn, "SELECT i FROM range(5000) t(i)");
	duckdb_v2_prepared_statement_handle prepared = nullptr;
	REQUIRE(duckdb_v2_prepared_statement_create(fx.conn, statement, false, &prepared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_destroy(&statement);
	{
		ArrowResult r;
		REQUIRE(duckdb_v2_prepared_statement_execute_arrow(prepared, nullptr, nullptr, 0, 700, &r, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		ArrowResult busy;
		REQUIRE(duckdb_v2_prepared_statement_execute_arrow(prepared, nullptr, nullptr, 0, 700, &busy, nullptr) ==
		        DUCKDB_V2_ERROR_RESOURCE_IN_USE);
		REQUIRE(busy.handle == nullptr);
		ArrowBatches batches;
		FetchAll(r, batches);
		REQUIRE(batches.RowCount() == 5000);
		REQUIRE(batches.MaxLength() <= 700);
		REQUIRE(CountsUpFrom(batches, 0));
	}
	{
		ArrowResult r;
		REQUIRE(duckdb_v2_prepared_statement_execute_arrow(prepared, nullptr, nullptr, 0, 700, &r, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		OwnedStream stream;
		REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, &stream.stream, nullptr) == DUCKDB_V2_ERROR_NONE);
		ArrowBatches streamed;
		StreamAll(stream.stream, streamed);
		REQUIRE(streamed.RowCount() == 5000);
		REQUIRE(streamed.MaxLength() <= 700);
		REQUIRE(CountsUpFrom(streamed, 0));
	}
	duckdb_v2_prepared_statement_destroy(&prepared);
}

TEST_CASE("V2 arrow result: drain discards unread rows and frees the connection", "[capi_v2][arrow]") {
	EnvFixture fx;
	ArrowResult r;
	REQUIRE(QueryArrow(fx.conn, "SELECT i FROM range(100000) t(i)", 1000, &r) == DUCKDB_V2_ERROR_NONE);
	ArrowArray first {};
	REQUIRE(duckdb_v2_arrow_result_fetch_array(r, &first, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(first.release != nullptr);
	first.release(&first);

	idx_t rows_changed = 7;
	REQUIRE(duckdb_v2_arrow_result_drain(r, &rows_changed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows_changed == 0);
	ArrowArray after_drain {};
	REQUIRE(duckdb_v2_arrow_result_fetch_array(r, &after_drain, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(after_drain.release == nullptr);
	QueryResult next;
	REQUIRE(Query(fx.conn, "SELECT 1", &next) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 arrow result: functions guard null arguments", "[capi_v2][arrow]") {
	EnvFixture fx;
	auto statement = ParseOne(fx.conn, "SELECT 1");
	duckdb_v2_arrow_result_handle out = nullptr;
	REQUIRE(duckdb_v2_statement_execute_arrow(nullptr, statement, nullptr, nullptr, 0, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_statement_execute_arrow(fx.conn, nullptr, nullptr, nullptr, 0, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_statement_execute_arrow(fx.conn, statement, nullptr, nullptr, 1, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_statement_execute_arrow(fx.conn, statement, nullptr, nullptr, 0, 0, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(duckdb_v2_prepared_statement_execute_arrow(nullptr, nullptr, nullptr, 0, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);

	ArrowArray array {};
	ArrowSchema schema {};
	ArrowArrayStream stream {};
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	idx_t rows_changed = 0;
	auto result_type = DUCKDB_V2_RESULT_TYPE_NOTHING;
	auto statement_type = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	REQUIRE(duckdb_v2_arrow_result_step(nullptr, &array, &status, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_fetch_array(nullptr, &array, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_wait(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_drain(nullptr, &rows_changed, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_get_result_type(nullptr, &result_type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_get_statement_type(nullptr, &statement_type, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_get_schema(nullptr, &schema, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	stream.release = UnusedStreamRelease;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(nullptr, &stream, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(stream.release == nullptr);
	stream.release = UnusedStreamRelease;
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&out, &stream, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(stream.release == nullptr);

	ArrowResult r;
	REQUIRE(duckdb_v2_statement_execute_arrow(fx.conn, statement, nullptr, nullptr, 0, 0, &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_destroy(&statement);
	REQUIRE(duckdb_v2_arrow_result_step(r, nullptr, &status, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_step(r, &array, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_fetch_array(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_drain(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_get_result_type(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_get_statement_type(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_arrow_result_get_schema(r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// A rejected conversion leaves the result with the caller.
	REQUIRE(duckdb_v2_arrow_result_to_arrow_c_stream(&r, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(r.handle != nullptr);
	ArrowBatches batches;
	FetchAll(r, batches);
	REQUIRE(batches.RowCount() == 1);

	REQUIRE(duckdb_v2_arrow_result_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_arrow_result_destroy(&out) == DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
