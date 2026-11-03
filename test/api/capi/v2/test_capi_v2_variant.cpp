#include "test_capi_v2.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 VARIANT layout tests: a VARIANT vector exposes its canonical layout as
// four children (keys, children, values, data). A small writer builds rows
// into them the way a producer would, a table function hands them to SQL, and
// the rows are compared with the engine's own casts. The read side checks the
// children of a VARIANT a query produced.
//
// The callbacks avoid Catch assertions: on failure they populate the error
// slot and return, and the failure surfaces as a query error.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_str Str(const char *s) {
	return duckdb_v2_str {s, std::strlen(s)};
}

void PutVarint(std::vector<uint8_t> &out, uint64_t value) {
	do {
		auto byte = static_cast<uint8_t>(value & 127);
		value >>= 7;
		if (value) {
			byte |= 128;
		}
		out.push_back(byte);
	} while (value);
}

uint64_t GetVarint(const uint8_t *&ptr) {
	uint64_t result = 0;
	uint8_t shift = 0;
	while (true) {
		auto byte = *ptr++;
		result |= static_cast<uint64_t>(byte & 127) << shift;
		if (!(byte & 128)) {
			return result;
		}
		shift += 7;
	}
}

// Accumulates the rows of one VARIANT vector in the canonical layout and writes them through the C API in Finish.
// A value is appended with Scalar / Bytes / Null, a container with BeginObject / Key / BeginArray and the matching
// End; the child count is stored ahead of the children, so it is given up front.
struct VariantWriter {
	struct Row {
		std::vector<std::string> keys; // in the order first seen; sorted and remapped in EndRow
		std::vector<uint32_t> child_keys;
		std::vector<uint32_t> child_values;
		std::vector<uint8_t> type_ids;
		std::vector<uint32_t> byte_offsets;
		std::vector<uint8_t> blob;
		bool is_null = true;
	};
	struct Frame {
		bool is_object;
		idx_t block;
		idx_t count;
		idx_t assigned = 0;
	};
	std::vector<Row> rows;
	Row *row = nullptr;
	std::vector<Frame> stack;

	explicit VariantWriter(idx_t row_count) : rows(row_count) {
	}

	void BeginRow(idx_t index) {
		row = &rows[index];
		row->is_null = false;
	}
	void EndRow() {
		// the keys sorted; readers look them up by value, so this is a choice, not a requirement
		std::vector<uint32_t> order(row->keys.size());
		for (uint32_t i = 0; i < order.size(); i++) {
			order[i] = i;
		}
		std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return row->keys[a] < row->keys[b]; });
		std::vector<uint32_t> remap(order.size());
		std::vector<std::string> sorted(order.size());
		for (uint32_t i = 0; i < order.size(); i++) {
			remap[order[i]] = i;
			sorted[i] = row->keys[order[i]];
		}
		row->keys = sorted;
		for (auto &key : row->child_keys) {
			if (key != UINT32_MAX) {
				key = remap[key];
			}
		}
		row = nullptr;
	}

	// Registers the next value as the current container's child, returning its values index.
	uint32_t Enter() {
		auto index = static_cast<uint32_t>(row->type_ids.size());
		if (!stack.empty()) {
			auto &frame = stack.back();
			row->child_values[frame.block + frame.assigned] = index;
			frame.assigned++;
		}
		return index;
	}
	void Push(DUCKDB_V2_VARIANT_TYPE type) {
		Enter();
		row->type_ids.push_back(static_cast<uint8_t>(type));
		row->byte_offsets.push_back(static_cast<uint32_t>(row->blob.size()));
	}
	void Null() {
		Push(DUCKDB_V2_VARIANT_TYPE_NULL);
	}
	void Bool(bool value) {
		Push(value ? DUCKDB_V2_VARIANT_TYPE_BOOL_TRUE : DUCKDB_V2_VARIANT_TYPE_BOOL_FALSE);
	}
	template <class T>
	void Scalar(DUCKDB_V2_VARIANT_TYPE type, const T &value) {
		Push(type);
		auto *bytes = reinterpret_cast<const uint8_t *>(&value);
		row->blob.insert(row->blob.end(), bytes, bytes + sizeof(T));
	}
	template <class T>
	void Decimal(uint8_t width, uint8_t scale, const T &unscaled) {
		Push(DUCKDB_V2_VARIANT_TYPE_DECIMAL);
		PutVarint(row->blob, width);
		PutVarint(row->blob, scale);
		auto *bytes = reinterpret_cast<const uint8_t *>(&unscaled);
		row->blob.insert(row->blob.end(), bytes, bytes + sizeof(T));
	}
	void Bytes(DUCKDB_V2_VARIANT_TYPE type, const uint8_t *data, idx_t length) {
		Push(type);
		PutVarint(row->blob, length);
		row->blob.insert(row->blob.end(), data, data + length);
	}
	void Text(const char *text) {
		Bytes(DUCKDB_V2_VARIANT_TYPE_VARCHAR, reinterpret_cast<const uint8_t *>(text), std::strlen(text));
	}
	void BeginContainer(bool is_object, idx_t count) {
		Push(is_object ? DUCKDB_V2_VARIANT_TYPE_OBJECT : DUCKDB_V2_VARIANT_TYPE_ARRAY);
		PutVarint(row->blob, count);
		auto block = row->child_values.size();
		if (count) {
			PutVarint(row->blob, block);
			row->child_values.resize(block + count);
			row->child_keys.resize(block + count, UINT32_MAX);
		}
		stack.push_back(Frame {is_object, block, count});
	}
	void BeginObject(idx_t count) {
		BeginContainer(true, count);
	}
	void BeginArray(idx_t count) {
		BeginContainer(false, count);
	}
	// Names the next member of the open object; the key is interned per row.
	void Key(const char *key) {
		auto &frame = stack.back();
		auto it = std::find(row->keys.begin(), row->keys.end(), key);
		uint32_t index = static_cast<uint32_t>(it - row->keys.begin());
		if (it == row->keys.end()) {
			row->keys.emplace_back(key);
		}
		row->child_keys[frame.block + frame.assigned] = index;
	}
	void End() {
		stack.pop_back();
	}

	// Writes the rows into `vector`; false with `err` set when a C API call failed.
	bool Finish(duckdb_v2_vector_handle vector, duckdb_v2_error_info_handle *err) {
		duckdb_v2_vector_handle keys, children, values, data;
		if (duckdb_v2_vector_get_child(vector, 0, &keys, err) ||
		    duckdb_v2_vector_get_child(vector, 1, &children, err) ||
		    duckdb_v2_vector_get_child(vector, 2, &values, err) || duckdb_v2_vector_get_child(vector, 3, &data, err)) {
			return false;
		}
		idx_t total_keys = 0, total_children = 0, total_values = 0;
		for (auto &r : rows) {
			total_keys += r.keys.size();
			total_children += r.child_values.size();
			total_values += r.type_ids.size();
		}
		auto count = rows.size();
		// size the lists and their elements
		duckdb_v2_vector_handle key_strings, child_entries, value_entries;
		if (duckdb_v2_vector_set_size(vector, count, err) || duckdb_v2_vector_set_size(keys, count, err) ||
		    duckdb_v2_vector_set_size(children, count, err) || duckdb_v2_vector_set_size(values, count, err) ||
		    duckdb_v2_vector_set_size(data, count, err) || duckdb_v2_vector_get_child(keys, 0, &key_strings, err) ||
		    duckdb_v2_vector_get_child(children, 0, &child_entries, err) ||
		    duckdb_v2_vector_get_child(values, 0, &value_entries, err) ||
		    duckdb_v2_vector_set_size(key_strings, total_keys, err) ||
		    duckdb_v2_vector_set_size(child_entries, total_children, err) ||
		    duckdb_v2_vector_set_size(value_entries, total_values, err)) {
			return false;
		}
		duckdb_v2_vector_handle keys_index, values_index, type_id, byte_offset;
		if (duckdb_v2_vector_get_child(child_entries, 0, &keys_index, err) ||
		    duckdb_v2_vector_get_child(child_entries, 1, &values_index, err) ||
		    duckdb_v2_vector_get_child(value_entries, 0, &type_id, err) ||
		    duckdb_v2_vector_get_child(value_entries, 1, &byte_offset, err) ||
		    duckdb_v2_vector_set_size(keys_index, total_children, err) ||
		    duckdb_v2_vector_set_size(values_index, total_children, err) ||
		    duckdb_v2_vector_set_size(type_id, total_values, err) ||
		    duckdb_v2_vector_set_size(byte_offset, total_values, err)) {
			return false;
		}
		void *keys_raw, *children_raw, *values_raw, *key_strings_raw, *keys_index_raw, *values_index_raw, *type_id_raw,
		    *byte_offset_raw, *data_raw;
		uint64_t *keys_index_validity;
		duckdb_v2_arena_handle key_arena, data_arena;
		if (duckdb_v2_vector_get_data_mutable(keys, &keys_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(children, &children_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(values, &values_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(key_strings, &key_strings_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(keys_index, &keys_index_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(values_index, &values_index_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(type_id, &type_id_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(byte_offset, &byte_offset_raw, err) ||
		    duckdb_v2_vector_get_data_mutable(data, &data_raw, err) ||
		    duckdb_v2_vector_flat_get_validity_mutable(keys_index, &keys_index_validity, err) ||
		    duckdb_v2_vector_get_arena(key_strings, &key_arena, err) ||
		    duckdb_v2_vector_get_arena(data, &data_arena, err)) {
			return false;
		}
		auto *keys_entries = static_cast<duckdb_v2_list_entry *>(keys_raw);
		auto *children_entries = static_cast<duckdb_v2_list_entry *>(children_raw);
		auto *values_entries = static_cast<duckdb_v2_list_entry *>(values_raw);
		auto *key_strings_data = static_cast<duckdb_v2_bytes *>(key_strings_raw);
		auto *keys_index_data = static_cast<uint32_t *>(keys_index_raw);
		auto *values_index_data = static_cast<uint32_t *>(values_index_raw);
		auto *type_id_data = static_cast<uint8_t *>(type_id_raw);
		auto *byte_offset_data = static_cast<uint32_t *>(byte_offset_raw);
		auto *data_strings = static_cast<duckdb_v2_bytes *>(data_raw);
		idx_t key_at = 0, child_at = 0, value_at = 0;
		for (idx_t r = 0; r < count; r++) {
			auto &row = rows[r];
			if (row.is_null) {
				keys_entries[r] = children_entries[r] = values_entries[r] = duckdb_v2_list_entry {0, 0};
				if (duckdb_v2_vector_set_null(vector, r, err)) {
					return false;
				}
				continue;
			}
			keys_entries[r] = duckdb_v2_list_entry {key_at, row.keys.size()};
			for (auto &key : row.keys) {
				auto rc = DUCKDB_V2_ERROR_NONE;
				key_strings_data[key_at++] = MakeString(key_arena, key.data(), key.size(), rc, err);
				if (rc) {
					return false;
				}
			}
			children_entries[r] = duckdb_v2_list_entry {child_at, row.child_values.size()};
			for (idx_t i = 0; i < row.child_values.size(); i++, child_at++) {
				values_index_data[child_at] = row.child_values[i];
				if (row.child_keys[i] == UINT32_MAX) {
					keys_index_validity[child_at / 64] &= ~(uint64_t(1) << (child_at % 64));
				} else {
					keys_index_data[child_at] = row.child_keys[i];
				}
			}
			values_entries[r] = duckdb_v2_list_entry {value_at, row.type_ids.size()};
			for (idx_t i = 0; i < row.type_ids.size(); i++, value_at++) {
				type_id_data[value_at] = row.type_ids[i];
				byte_offset_data[value_at] = row.byte_offsets[i];
			}
			auto rc = DUCKDB_V2_ERROR_NONE;
			data_strings[r] =
			    MakeString(data_arena, reinterpret_cast<const char *>(row.blob.data()), row.blob.size(), rc, err);
			if (rc) {
				return false;
			}
		}
		return true;
	}
};

// The rows variant_rows() produces. Row 1 is never begun and reads as SQL NULL.
constexpr idx_t VARIANT_ROW_COUNT = 6;

const char *TYPED_OBJECT_SQL = "{'date': DATE '2024-02-29', 'time': TIME '12:34:56.789', "
                               "'ts': TIMESTAMP '2024-02-29 12:34:56.789', 'dec': 1234.567::DECIMAL(18, 3), "
                               "'dec2': 12.5::DECIMAL(4, 1), 'dec38': 1.2345678901::DECIMAL(38, 10), "
                               "'blob': '\\x00\\x01\\xFF'::BLOB, 'big': -5::BIGINT, 'hug': (2::HUGEINT ^ 70)::HUGEINT, "
                               "'u8': 255::UTINYINT, 'f': 0.5::FLOAT, 'iv': INTERVAL '1 month 2 days 3 microseconds'}"
                               "::VARIANT";

bool BuildRows(duckdb_v2_vector_handle vector, duckdb_v2_error_info_handle *err) {
	VariantWriter w(VARIANT_ROW_COUNT);
	// Row 0: {'name': 'duck', 'n': 42, 'ok': true, 'none': null, 'list': [1.5, 'x', {}], 'empty': []}, keys out of
	// order
	w.BeginRow(0);
	w.BeginObject(6);
	w.Key("name");
	w.Text("duck");
	w.Key("n");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INT32, int32_t(42));
	w.Key("ok");
	w.Bool(true);
	w.Key("none");
	w.Null();
	w.Key("list");
	w.BeginArray(3);
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_DOUBLE, 1.5);
	w.Text("x");
	w.BeginObject(0);
	w.End();
	w.End();
	w.Key("empty");
	w.BeginArray(0);
	w.End();
	w.End();
	w.EndRow();
	// Row 2: one member per typed scalar kind, matching TYPED_OBJECT_SQL
	w.BeginRow(2);
	w.BeginObject(12);
	w.Key("date");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_DATE, int32_t(19782)); // 2024-02-29
	int64_t time = (12 * 3600 + 34 * 60 + 56) * 1000000LL + 789000;
	w.Key("time");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_TIME_MICROS, time);
	w.Key("ts");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_TIMESTAMP_MICROS, int64_t(19782 * 86400000000LL + time));
	w.Key("dec");
	w.Decimal(18, 3, int64_t(1234567));
	w.Key("dec2");
	w.Decimal(4, 1, int16_t(125));
	w.Key("dec38");
	w.Decimal(38, 10, duckdb_v2_hugeint_t {12345678901ULL, 0});
	const uint8_t blob[] = {0x00, 0x01, 0xFF};
	w.Key("blob");
	w.Bytes(DUCKDB_V2_VARIANT_TYPE_BLOB, blob, sizeof(blob));
	w.Key("big");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INT64, int64_t(-5));
	w.Key("hug");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INT128, duckdb_v2_hugeint_t {0, 64}); // 2^70
	w.Key("u8");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_UINT8, uint8_t(255));
	w.Key("f");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_FLOAT, 0.5f);
	w.Key("iv");
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INTERVAL, duckdb_v2_interval_t {1, 2, 3});
	w.End();
	w.EndRow();
	// Row 3: {}
	w.BeginRow(3);
	w.BeginObject(0);
	w.End();
	w.EndRow();
	// Row 4: [[1, 2], [3]]
	w.BeginRow(4);
	w.BeginArray(2);
	w.BeginArray(2);
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INT64, int64_t(1));
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INT64, int64_t(2));
	w.End();
	w.BeginArray(1);
	w.Scalar(DUCKDB_V2_VARIANT_TYPE_INT64, int64_t(3));
	w.End();
	w.End();
	w.EndRow();
	// Row 5: a scalar root
	w.BeginRow(5);
	w.Text("just text");
	w.EndRow();
	return w.Finish(vector, err);
}

// ---------------------------------------------------------------------------
// variant_rows(): a VARIANT column "v" with the rows above and their index "i", produced in one batch.
// ---------------------------------------------------------------------------

struct RowsGlobal {
	bool done = false;
};

void DeleteRowsGlobal(void *ptr) {
	delete static_cast<RowsGlobal *>(ptr);
}

void RowsBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle variant = nullptr;
	auto text = Str("VARIANT");
	if (duckdb_v2_factory_create_type_from_text(factory, &text, &variant, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto name = duckdb_v2_identifier_t {"v", 1};
	auto rc = duckdb_v2_table_function_bind_add_result_column(result, &name, variant, err);
	duckdb_v2_logical_type_destroy(&variant);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, 0, &bigint,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto index_name = duckdb_v2_identifier_t {"i", 1};
	rc = duckdb_v2_table_function_bind_add_result_column(result, &index_name, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_bind_set_cardinality(result, VARIANT_ROW_COUNT, true, err);
}

void RowsInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new RowsGlobal(), DeleteRowsGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void RowsExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<RowsGlobal *>(global_ptr);
	duckdb_v2_vector_handle vec = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (global.done) {
		duckdb_v2_vector_set_size(vec, 0, err);
		return;
	}
	global.done = true;
	if (!BuildRows(vec, err)) {
		return;
	}
	duckdb_v2_vector_handle index = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 1, &index, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(index, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < VARIANT_ROW_COUNT; i++) {
		static_cast<int64_t *>(raw)[i] = static_cast<int64_t>(i);
	}
	duckdb_v2_vector_set_size(vec, VARIANT_ROW_COUNT, err);
}

void RegisterVariantRows(EnvFixture &fx) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Str("variant_rows");
	REQUIRE(duckdb_v2_table_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, RowsBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, RowsInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RowsExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

// The rows of a single-column query as text, "NULL" standing in for NULL.
std::vector<std::string> QueryStrings(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql.c_str(), &result, &err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		FAIL("query failed: " << sql << ": " << Convert(text));
	}
	auto varchar = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	std::vector<std::string> rows;
	while (auto chunk = StepChunk(result)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
		for (idx_t i = 0; i < size; i++) {
			duckdb_v2_value_handle value = nullptr;
			REQUIRE(duckdb_v2_vector_get_value(vec, i, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
			bool is_null = false;
			REQUIRE(duckdb_v2_value_is_null(value, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
			if (is_null) {
				rows.push_back("NULL");
			} else {
				duckdb_v2_value_handle text = nullptr;
				REQUIRE(duckdb_v2_value_cast(FactoryOf(conn), value, varchar, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
				rows.push_back(ConsumeValue<std::string>(text));
			}
			duckdb_v2_value_destroy(&value);
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_result_destroy(&result);
	return rows;
}

std::string QueryString(duckdb_v2_connection_handle conn, const std::string &sql) {
	auto rows = QueryStrings(conn, sql);
	REQUIRE(rows.size() == 1);
	return rows[0];
}

} // namespace

TEST_CASE("V2 VARIANT layout: rows written into the children read back through SQL", "[capi_v2][variant]") {
	EnvFixture fx;
	RegisterVariantRows(fx);

	REQUIRE(QueryStrings(fx.conn, "SELECT v IS NULL FROM variant_rows() ORDER BY i") ==
	        std::vector<std::string> {"false", "true", "false", "false", "false", "false"});
	REQUIRE(QueryStrings(fx.conn, "SELECT variant_typeof(v) FROM variant_rows() ORDER BY i") ==
	        std::vector<std::string> {"OBJECT(empty, list, n, name, none, ok)", "VARIANT_NULL",
	                                  "OBJECT(big, blob, date, dec, dec2, dec38, f, hug, iv, time, ts, u8)", "OBJECT()",
	                                  "ARRAY(2)", "VARCHAR"});

	// Row 0: the heterogeneous object, member by member.
	auto member = [&](const char *expr) {
		return QueryString(fx.conn, std::string("SELECT ") + expr + " FROM variant_rows() WHERE i = 0");
	};
	REQUIRE(member("variant_extract(v, 'name')::VARCHAR") == "duck");
	REQUIRE(member("variant_typeof(variant_extract(v, 'n'))") == "INT32");
	REQUIRE(member("variant_extract(v, 'n')::INTEGER") == "42");
	REQUIRE(member("variant_extract(v, 'ok')::BOOLEAN") == "true");
	REQUIRE(member("variant_typeof(variant_extract(v, 'none'))") == "VARIANT_NULL");
	REQUIRE(member("variant_typeof(variant_extract(v, 'list'))") == "ARRAY(3)");
	REQUIRE(member("variant_typeof(variant_extract(variant_extract(v, 'list'), 1))") == "DOUBLE");
	REQUIRE(member("variant_extract(variant_extract(v, 'list'), 2)::VARCHAR") == "x");
	REQUIRE(member("variant_typeof(variant_extract(variant_extract(v, 'list'), 3))") == "OBJECT()");
	REQUIRE(member("variant_typeof(variant_extract(v, 'empty'))") == "ARRAY(0)");

	// Row 2: every typed scalar keeps its type and value; the whole object equals the engine's own cast.
	auto typed = [&](const char *expr) {
		return QueryString(fx.conn, std::string("SELECT ") + expr + " FROM variant_rows() WHERE i = 2");
	};
	REQUIRE(typed("variant_typeof(variant_extract(v, 'date'))") == "DATE");
	REQUIRE(typed("variant_extract(v, 'date')::DATE::VARCHAR") == "2024-02-29");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'time'))") == "TIME_MICROS");
	REQUIRE(typed("variant_extract(v, 'time')::TIME::VARCHAR") == "12:34:56.789");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'ts'))") == "TIMESTAMP_MICROS");
	REQUIRE(typed("variant_extract(v, 'ts')::TIMESTAMP::VARCHAR") == "2024-02-29 12:34:56.789");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'dec'))") == "DECIMAL(18, 3)");
	REQUIRE(typed("variant_extract(v, 'dec')::DECIMAL(18, 3)::VARCHAR") == "1234.567");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'dec2'))") == "DECIMAL(4, 1)");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'dec38'))") == "DECIMAL(38, 10)");
	REQUIRE(typed("variant_extract(v, 'dec38')::VARCHAR") == "1.2345678901");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'blob'))") == "BLOB");
	REQUIRE(typed("variant_extract(v, 'blob')::BLOB::VARCHAR") == "\\x00\\x01\\xFF");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'hug'))") == "INT128");
	REQUIRE(typed("variant_extract(v, 'hug')::HUGEINT::VARCHAR") == "1180591620717411303424");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'u8'))") == "UINT8");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'f'))") == "FLOAT");
	REQUIRE(typed("variant_typeof(variant_extract(v, 'iv'))") == "INTERVAL");
	REQUIRE(typed("variant_extract(v, 'iv')::INTERVAL::VARCHAR") == "1 month 2 days 00:00:00.000003");
	REQUIRE(typed(("v = " + std::string(TYPED_OBJECT_SQL)).c_str()) == "true");
	REQUIRE(typed("v::VARCHAR") == QueryString(fx.conn, std::string("SELECT ") + TYPED_OBJECT_SQL + "::VARCHAR"));

	// Rows 4 and 5: nested arrays and a scalar root render like the engine's casts.
	REQUIRE(QueryString(fx.conn, "SELECT v = [[1, 2], [3]]::BIGINT[][]::VARIANT FROM variant_rows() WHERE i = 4") ==
	        "true");
	REQUIRE(QueryString(fx.conn, "SELECT v::VARCHAR FROM variant_rows() WHERE i = 5") == "just text");

	// The rows survive being stored and read back.
	REQUIRE(RunSql(fx.conn, "CREATE TABLE built AS SELECT v, i FROM variant_rows()") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(QueryString(fx.conn, "SELECT count(*) FROM built WHERE v IS NOT NULL") == "5");
	REQUIRE(QueryString(fx.conn, "SELECT variant_extract(v, 'name')::VARCHAR FROM built WHERE i = 0") == "duck");
}

TEST_CASE("V2 VARIANT layout: the children of a VARIANT a query produced", "[capi_v2][variant]") {
	EnvFixture fx;
	QueryResult r;
	REQUIRE(Query(fx.conn, "SELECT {'b': 1, 'a': [true, NULL]}::VARIANT AS v", &r) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	idx_t count = 0;
	REQUIRE(duckdb_v2_vector_get_child_count(vec, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 4);
	duckdb_v2_vector_handle keys, children, values, data, none = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &keys, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(vec, 1, &children, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(vec, 2, &values, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(vec, 3, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(vec, 4, &none, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(none == nullptr);

	// keys: ['b', 'a'], in the order the struct's fields were given
	duckdb_v2_vector_view keys_view {};
	REQUIRE(duckdb_v2_vector_get_view(keys, &keys_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto keys_entry = static_cast<const duckdb_v2_list_entry *>(keys_view.data)[SelAt(keys_view.sel, 0)];
	REQUIRE(keys_entry.length == 2);
	duckdb_v2_vector_handle key_strings = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(keys, 0, &key_strings, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view key_view {};
	REQUIRE(duckdb_v2_vector_get_view(key_strings, &key_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *key_data = static_cast<const duckdb_v2_bytes *>(key_view.data);
	REQUIRE(Convert(Convert(key_data[SelAt(key_view.sel, keys_entry.offset)])) == "b");
	REQUIRE(Convert(Convert(key_data[SelAt(key_view.sel, keys_entry.offset + 1)])) == "a");

	// values: the object, then its members and the array's two elements
	duckdb_v2_vector_view values_view {};
	REQUIRE(duckdb_v2_vector_get_view(values, &values_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto values_entry = static_cast<const duckdb_v2_list_entry *>(values_view.data)[SelAt(values_view.sel, 0)];
	REQUIRE(values_entry.length == 5);
	duckdb_v2_vector_handle value_entries = nullptr, type_id = nullptr, byte_offset = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(values, 0, &value_entries, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(value_entries, 0, &type_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(value_entries, 1, &byte_offset, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view type_view {}, offset_view {};
	REQUIRE(duckdb_v2_vector_get_view(type_id, &type_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_view(byte_offset, &offset_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *types = static_cast<const uint8_t *>(type_view.data);
	auto *offsets = static_cast<const uint32_t *>(offset_view.data);
	auto type_at = [&](idx_t i) {
		return types[SelAt(type_view.sel, values_entry.offset + i)];
	};
	REQUIRE(type_at(0) == DUCKDB_V2_VARIANT_TYPE_OBJECT);

	// the object's payload: two members starting at children entry 0
	duckdb_v2_vector_view data_view {};
	REQUIRE(duckdb_v2_vector_get_view(data, &data_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto blob = Convert(static_cast<const duckdb_v2_bytes *>(data_view.data)[SelAt(data_view.sel, 0)]);
	auto *payload = reinterpret_cast<const uint8_t *>(blob.ptr) + offsets[SelAt(offset_view.sel, values_entry.offset)];
	REQUIRE(GetVarint(payload) == 2);
	REQUIRE(GetVarint(payload) == 0);

	// children: member 'b' (keys_index 0) and 'a' (keys_index 1), then the array's two elements with NULL keys
	duckdb_v2_vector_view children_view {};
	REQUIRE(duckdb_v2_vector_get_view(children, &children_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto children_entry = static_cast<const duckdb_v2_list_entry *>(children_view.data)[SelAt(children_view.sel, 0)];
	REQUIRE(children_entry.length == 4);
	duckdb_v2_vector_handle child_entries = nullptr, keys_index = nullptr, values_index = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(children, 0, &child_entries, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(child_entries, 0, &keys_index, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(child_entries, 1, &values_index, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view keys_index_view {}, values_index_view {};
	REQUIRE(duckdb_v2_vector_get_view(keys_index, &keys_index_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_view(values_index, &values_index_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *key_ids = static_cast<const uint32_t *>(keys_index_view.data);
	auto *value_ids = static_cast<const uint32_t *>(values_index_view.data);
	auto child = [&](idx_t i) {
		return SelAt(keys_index_view.sel, children_entry.offset + i);
	};
	REQUIRE(RowValid(keys_index_view, child(0)));
	REQUIRE(key_ids[child(0)] == 0);
	REQUIRE(RowValid(keys_index_view, child(1)));
	REQUIRE(key_ids[child(1)] == 1);
	REQUIRE_FALSE(RowValid(keys_index_view, child(2)));
	REQUIRE_FALSE(RowValid(keys_index_view, child(3)));
	// member 'b' is the integer, member 'a' the array, whose elements are true and a VARIANT null
	auto value_of = [&](idx_t i) {
		return value_ids[SelAt(values_index_view.sel, children_entry.offset + i)];
	};
	REQUIRE(type_at(value_of(0)) == DUCKDB_V2_VARIANT_TYPE_INT32);
	REQUIRE(type_at(value_of(1)) == DUCKDB_V2_VARIANT_TYPE_ARRAY);
	REQUIRE(type_at(value_of(2)) == DUCKDB_V2_VARIANT_TYPE_BOOL_TRUE);
	REQUIRE(type_at(value_of(3)) == DUCKDB_V2_VARIANT_TYPE_NULL);

	// A non-nested kind still has no children.
	duckdb_v2_vector_handle other = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(type_id, 0, &other, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_data_chunk_destroy(&chunk);
}

} // namespace test_capi_v2
