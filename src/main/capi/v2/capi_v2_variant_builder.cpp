#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"

#include <unordered_set>

// The VARIANT builder: the C API counterpart of the engine's single-pass VariantBuilder. Every row's value tree is
// emitted into the builder's growable buffers as it arrives; the vector is written once in Finish, the way
// BuildVariant does it for the engine's own converters.

namespace duckdb::capiv2 {
namespace {

//! A primitive payload handed to the engine's VariantBuilder::EmitPrimitiveNode
struct CV2ScalarPayload {
	const_data_ptr_t data = nullptr;
	string_t str;
	VariantDecimalProperties decimal {0, 0};

	template <class T>
	T GetData() const {
		return Load<T>(data);
	}
	string_t GetString() const {
		return str;
	}
	VariantDecimalProperties GetDecimalProperties() const {
		return decimal;
	}
};

//! An open OBJECT or ARRAY of the row being built
struct CV2VariantFrame {
	bool is_object = false;
	//! Where the container's child slots start in the builder's children buffers
	idx_t block = 0;
	//! The announced child count, and how many children are complete / have their key
	idx_t child_count = 0;
	idx_t children_assigned = 0;
	bool key_pending = false;
	//! The dictionary indices of the keys given so far (duplicate detection)
	idx_t keys_start = 0;
	unique_ptr<std::unordered_set<uint32_t>> key_set;
};

class CV2VariantBuilder {
public:
	CV2VariantBuilder(Vector &vector_p, idx_t row_count_p)
	    : target(vector_p), row_count(row_count_p),
	      dictionary(StringVector::GetStringAllocator(ListVector::GetChildMutable(VariantVector::GetKeys(vector_p)))),
	      builder(dictionary), keys_entries(row_count_p), children_entries(row_count_p), values_entries(row_count_p),
	      blob_entries(row_count_p), row_state(row_count_p, RowState::MISSING) {
	}

public:
	void BeginRow(idx_t row) {
		CheckNotFinished();
		if (in_row) {
			throw InvalidInputException("duckdb_v2_variant_builder_begin_row: row %llu is still open; end it first",
			                            current_row);
		}
		if (row >= row_count) {
			throw InvalidInputException("duckdb_v2_variant_builder_begin_row: row %llu is out of range, the builder "
			                            "holds %llu rows",
			                            row, row_count);
		}
		if (row_state[row] != RowState::MISSING) {
			throw InvalidInputException("duckdb_v2_variant_builder_begin_row: row %llu was begun before", row);
		}
		in_row = true;
		current_row = row;
		root_emitted = false;
		builder.row_values = builder.type_ids.size();
		builder.row_children = builder.child_value_ids.size();
		builder.row_keys = builder.key_slots.size();
		blob_start = builder.blob.size();
	}

	void EndRow() {
		CheckInRow("duckdb_v2_variant_builder_end_row");
		if (!stack.empty()) {
			throw InvalidInputException("duckdb_v2_variant_builder_end_row: %d container(s) are still open",
			                            static_cast<int>(stack.size()));
		}
		if (!root_emitted) {
			throw InvalidInputException(
			    "duckdb_v2_variant_builder_end_row: the row has no value; append one, or skip the row for NULL");
		}
		auto blob_size = builder.blob.size() - blob_start;
		if (blob_size > NumericLimits<uint32_t>::Maximum()) {
			throw Exception(ExceptionType::OBJECT_SIZE,
			                StringUtil::Format("duckdb_v2_variant_builder_end_row: the encoded row exceeds the maximum "
			                                   "supported %u bytes",
			                                   NumericLimits<uint32_t>::Maximum()));
		}
		auto value_count = builder.LocalValue();
		if (value_count == 1 &&
		    builder.type_ids[builder.row_values] == static_cast<uint8_t>(VariantLogicalType::VARIANT_NULL)) {
			// a lone VARIANT null is a SQL NULL row
			builder.type_ids.pop_back();
			builder.byte_offsets.pop_back();
			row_state[current_row] = RowState::NUL;
			blob_entries[current_row] = list_entry_t(blob_start, 0);
			keys_entries[current_row] = list_entry_t(builder.row_keys, 0);
			children_entries[current_row] = list_entry_t(builder.row_children, 0);
			values_entries[current_row] = list_entry_t(builder.row_values, 0);
			in_row = false;
			return;
		}
		// the engine's builder records offsets into its blob; re-base them on the row
		auto row_blob_start = static_cast<uint32_t>(blob_start);
		for (idx_t i = builder.row_values; i < builder.type_ids.size(); i++) {
			builder.byte_offsets[i] -= row_blob_start;
		}
		row_state[current_row] = RowState::VALUE;
		blob_entries[current_row] = list_entry_t(blob_start, blob_size);
		keys_entries[current_row] = list_entry_t(builder.row_keys, builder.LocalKey());
		children_entries[current_row] = list_entry_t(builder.row_children, builder.LocalChild());
		values_entries[current_row] = list_entry_t(builder.row_values, value_count);
		in_row = false;
	}

	void AppendNull() {
		PrepareValue("duckdb_v2_variant_builder_append_null");
		builder.EmitNull();
	}

	void AppendScalar(LogicalTypeId type_id, const void *data) {
		if (!data) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_scalar: data is NULL");
		}
		auto variant_type = ScalarVariantType(type_id, data);
		PrepareValue("duckdb_v2_variant_builder_append_scalar");
		CV2ScalarPayload payload;
		payload.data = const_data_ptr_cast(data);
		builder.EmitPrimitiveNode(payload, variant_type);
	}

	void AppendDecimal(uint8_t width, uint8_t scale, const void *data) {
		if (!data) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_decimal: data is NULL");
		}
		if (width == 0 || width > DecimalType::MaxWidth()) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_decimal: width %d is not between 1 and %d",
			                            static_cast<int>(width), static_cast<int>(DecimalType::MaxWidth()));
		}
		if (scale > width) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_decimal: scale %d exceeds the width %d",
			                            static_cast<int>(scale), static_cast<int>(width));
		}
		PrepareValue("duckdb_v2_variant_builder_append_decimal");
		CV2ScalarPayload payload;
		payload.data = const_data_ptr_cast(data);
		payload.decimal = VariantDecimalProperties(width, scale);
		builder.EmitPrimitiveNode(payload, VariantLogicalType::DECIMAL);
	}

	void AppendBytes(LogicalTypeId type_id, const uint8_t *data, idx_t length) {
		if (!data && length > 0) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_bytes: data is NULL with a length of %llu",
			                            length);
		}
		if (length >= NumericLimits<uint32_t>::Maximum()) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_bytes: a value of %llu bytes does not fit",
			                            length);
		}
		VariantLogicalType variant_type;
		switch (type_id) {
		case LogicalTypeId::VARCHAR:
			variant_type = VariantLogicalType::VARCHAR;
			break;
		case LogicalTypeId::BLOB:
			variant_type = VariantLogicalType::BLOB;
			break;
		case LogicalTypeId::BIT:
			variant_type = VariantLogicalType::BITSTRING;
			break;
		case LogicalTypeId::BIGNUM:
			variant_type = VariantLogicalType::BIGNUM;
			break;
		case LogicalTypeId::GEOMETRY:
			variant_type = VariantLogicalType::GEOMETRY;
			break;
		default:
			throw InvalidInputException("duckdb_v2_variant_builder_append_bytes: %s is not a text or binary type; "
			                            "expected VARCHAR, BLOB, BIT, BIGNUM or GEOMETRY",
			                            EnumUtil::ToString(type_id));
		}
		PrepareValue("duckdb_v2_variant_builder_append_bytes");
		CV2ScalarPayload payload;
		payload.str = string_t(const_char_ptr_cast(data), NumericCast<uint32_t>(length));
		builder.EmitPrimitiveNode(payload, variant_type);
	}

	void BeginContainer(bool is_object, idx_t child_count) {
		const char *func =
		    is_object ? "duckdb_v2_variant_builder_begin_object" : "duckdb_v2_variant_builder_begin_array";
		if (child_count > NumericLimits<uint32_t>::Maximum()) {
			throw InvalidInputException("%s: a child count of %llu does not fit", func, child_count);
		}
		PrepareValue(func);
		auto type_id = is_object ? VariantLogicalType::OBJECT : VariantLogicalType::ARRAY;
		auto block = builder.BeginContainer(type_id, child_count);
		CV2VariantFrame frame;
		frame.is_object = is_object;
		frame.block = block.IsValid() ? block.GetIndex() : 0;
		frame.child_count = child_count;
		frame.keys_start = builder.key_slots.size();
		stack.push_back(std::move(frame));
	}

	void AppendKey(string_t key) {
		CheckInRow("duckdb_v2_variant_builder_append_key");
		if (stack.empty() || !stack.back().is_object) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_key: no object is open");
		}
		auto &frame = stack.back();
		if (frame.key_pending) {
			throw InvalidInputException(
			    "duckdb_v2_variant_builder_append_key: the previous member still awaits its value");
		}
		if (frame.children_assigned >= frame.child_count) {
			throw InvalidInputException("duckdb_v2_variant_builder_append_key: the object already has all %llu members "
			                            "it was begun with",
			                            frame.child_count);
		}
		auto dictionary_index = VariantBuilderGetOrCreateIndex(dictionary, key);
		if (IsDuplicateKey(frame, dictionary_index)) {
			throw InvalidInputException(
			    "duckdb_v2_variant_builder_append_key: object contains the key \"%s\" more than "
			    "once",
			    key.GetString());
		}
		auto slot = frame.block + frame.children_assigned;
		builder.child_value_ids[slot] = builder.LocalValue();
		builder.child_key_ids[slot] = builder.LocalKey();
		builder.key_slots.push_back(dictionary_index);
		frame.key_pending = true;
	}

	void EndContainer(bool is_object) {
		const char *func = is_object ? "duckdb_v2_variant_builder_end_object" : "duckdb_v2_variant_builder_end_array";
		CheckInRow(func);
		if (stack.empty() || stack.back().is_object != is_object) {
			throw InvalidInputException("%s: no %s is open", func, is_object ? "object" : "array");
		}
		auto &frame = stack.back();
		if (frame.key_pending) {
			throw InvalidInputException("%s: the last member still awaits its value", func);
		}
		if (frame.children_assigned < frame.child_count) {
			throw InvalidInputException("%s: the %s was begun with %llu children but holds %llu", func,
			                            is_object ? "object" : "array", frame.child_count, frame.children_assigned);
		}
		stack.pop_back();
	}

	void Finish() {
		CheckNotFinished();
		if (in_row) {
			throw InvalidInputException("duckdb_v2_variant_builder_finish: row %llu is still open; end it first",
			                            current_row);
		}
		finished = true;
		if (row_count == 0) {
			FlatVector::SetSize(target, 0);
			return;
		}
		auto &keys = VariantVector::GetKeys(target);
		auto &keys_entry = ListVector::GetChildMutable(keys);
		auto &children = VariantVector::GetChildren(target);
		auto &values = VariantVector::GetValues(target);
		auto &blob_vector = VariantVector::GetData(target);

		auto total_keys = builder.key_slots.size();
		auto total_children = builder.child_value_ids.size();
		auto total_values = builder.type_ids.size();

		ListVector::Reserve(keys, total_keys);
		ListVector::SetListSize(keys, total_keys);
		ListVector::Reserve(children, total_children);
		ListVector::SetListSize(children, total_children);
		ListVector::Reserve(values, total_values);
		ListVector::SetListSize(values, total_values);

		auto blob_data = FlatVector::GetDataMutable<string_t>(blob_vector);
		VariantVectorData variant_data(target);
		for (idx_t row = 0; row < row_count; row++) {
			if (row_state[row] == RowState::VALUE) {
				auto &blob = blob_entries[row];
				blob_data[row] =
				    StringVector::AddStringOrBlob(blob_vector, builder.blob.data() + blob.offset, blob.length);
			} else {
				blob_data[row] = string_t();
				FlatVector::SetNull(target, row, true);
			}
			variant_data.keys_data[row] = keys_entries[row];
			variant_data.children_data[row] = children_entries[row];
			variant_data.values_data[row] = values_entries[row];
		}

		if (total_values) {
			memcpy(variant_data.type_ids_data, builder.type_ids.data(), total_values * sizeof(uint8_t));
			memcpy(variant_data.byte_offset_data, builder.byte_offsets.data(), total_values * sizeof(uint32_t));
		}
		for (idx_t i = 0; i < total_children; i++) {
			variant_data.values_index_data[i] = builder.child_value_ids[i];
			if (builder.child_key_ids[i] == VARIANT_INVALID_KEY) {
				variant_data.keys_index_validity.SetInvalid(i);
			} else {
				variant_data.keys_index_data[i] = builder.child_key_ids[i];
			}
		}

		SelectionVector keys_selvec(total_keys);
		for (idx_t i = 0; i < total_keys; i++) {
			keys_selvec.set_index(i, builder.key_slots[i]);
		}
		VariantUtils::FinalizeVariantKeys(target, dictionary, keys_selvec, total_keys);
		keys_entry.Slice(keys_selvec, total_keys);

		FlatVector::SetSize(target, row_count);
		target.Verify();
	}

private:
	enum class RowState : uint8_t { MISSING, NUL, VALUE };

	void CheckNotFinished() const {
		if (finished) {
			throw InvalidInputException("the variant builder is finished; only destroy may follow");
		}
	}

	void CheckInRow(const char *func) const {
		CheckNotFinished();
		if (!in_row) {
			throw InvalidInputException("%s: no row is open; call duckdb_v2_variant_builder_begin_row first", func);
		}
	}

	//! Accounts for the value about to be emitted: the row's root, or the next child of the open container
	void PrepareValue(const char *func) {
		CheckInRow(func);
		if (stack.empty()) {
			if (root_emitted) {
				throw InvalidInputException("%s: the row already holds its value; a row holds exactly one", func);
			}
			root_emitted = true;
			return;
		}
		auto &frame = stack.back();
		if (frame.children_assigned >= frame.child_count) {
			throw InvalidInputException("%s: the open %s already has all %llu children it was begun with", func,
			                            frame.is_object ? "object" : "array", frame.child_count);
		}
		if (frame.is_object) {
			if (!frame.key_pending) {
				throw InvalidInputException("%s: an object member needs duckdb_v2_variant_builder_append_key first",
				                            func);
			}
			frame.key_pending = false;
		} else {
			builder.AssignArrayChild(frame.block, frame.children_assigned);
		}
		frame.children_assigned++;
	}

	bool IsDuplicateKey(CV2VariantFrame &frame, uint32_t dictionary_index) {
		static constexpr idx_t LINEAR_SCAN_LIMIT = 32;
		if (frame.child_count <= LINEAR_SCAN_LIMIT) {
			for (idx_t i = frame.keys_start; i < builder.key_slots.size(); i++) {
				if (builder.key_slots[i] == dictionary_index) {
					return true;
				}
			}
			return false;
		}
		if (!frame.key_set) {
			frame.key_set = make_uniq<std::unordered_set<uint32_t>>();
		}
		return !frame.key_set->insert(dictionary_index).second;
	}

	static VariantLogicalType ScalarVariantType(LogicalTypeId type_id, const void *data) {
		switch (type_id) {
		case LogicalTypeId::BOOLEAN:
			return Load<bool>(const_data_ptr_cast(data)) ? VariantLogicalType::BOOL_TRUE
			                                             : VariantLogicalType::BOOL_FALSE;
		case LogicalTypeId::TINYINT:
			return VariantLogicalType::INT8;
		case LogicalTypeId::SMALLINT:
			return VariantLogicalType::INT16;
		case LogicalTypeId::INTEGER:
			return VariantLogicalType::INT32;
		case LogicalTypeId::BIGINT:
			return VariantLogicalType::INT64;
		case LogicalTypeId::HUGEINT:
			return VariantLogicalType::INT128;
		case LogicalTypeId::UTINYINT:
			return VariantLogicalType::UINT8;
		case LogicalTypeId::USMALLINT:
			return VariantLogicalType::UINT16;
		case LogicalTypeId::UINTEGER:
			return VariantLogicalType::UINT32;
		case LogicalTypeId::UBIGINT:
			return VariantLogicalType::UINT64;
		case LogicalTypeId::UHUGEINT:
			return VariantLogicalType::UINT128;
		case LogicalTypeId::FLOAT:
			return VariantLogicalType::FLOAT;
		case LogicalTypeId::DOUBLE:
			return VariantLogicalType::DOUBLE;
		case LogicalTypeId::DATE:
			return VariantLogicalType::DATE;
		case LogicalTypeId::TIME:
			return VariantLogicalType::TIME_MICROS;
		case LogicalTypeId::TIME_NS:
			return VariantLogicalType::TIME_NANOS;
		case LogicalTypeId::TIME_TZ:
			return VariantLogicalType::TIME_MICROS_TZ;
		case LogicalTypeId::TIMESTAMP_SEC:
			return VariantLogicalType::TIMESTAMP_SEC;
		case LogicalTypeId::TIMESTAMP_MS:
			return VariantLogicalType::TIMESTAMP_MILIS;
		case LogicalTypeId::TIMESTAMP:
			return VariantLogicalType::TIMESTAMP_MICROS;
		case LogicalTypeId::TIMESTAMP_NS:
			return VariantLogicalType::TIMESTAMP_NANOS;
		case LogicalTypeId::TIMESTAMP_TZ:
			return VariantLogicalType::TIMESTAMP_MICROS_TZ;
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return VariantLogicalType::TIMESTAMP_NANOS_TZ;
		case LogicalTypeId::INTERVAL:
			return VariantLogicalType::INTERVAL;
		case LogicalTypeId::UUID:
			return VariantLogicalType::UUID;
		case LogicalTypeId::DECIMAL:
			throw InvalidInputException("duckdb_v2_variant_builder_append_scalar: DECIMAL goes through "
			                            "duckdb_v2_variant_builder_append_decimal");
		case LogicalTypeId::VARCHAR:
		case LogicalTypeId::BLOB:
		case LogicalTypeId::BIT:
		case LogicalTypeId::BIGNUM:
		case LogicalTypeId::GEOMETRY:
			throw InvalidInputException(
			    "duckdb_v2_variant_builder_append_scalar: %s goes through duckdb_v2_variant_builder_append_bytes",
			    EnumUtil::ToString(type_id));
		default:
			throw InvalidInputException("duckdb_v2_variant_builder_append_scalar: %s is not a fixed-width scalar type",
			                            EnumUtil::ToString(type_id));
		}
	}

private:
	Vector &target;
	const idx_t row_count;
	//! Interns the keys of every row; backed by the string heap of the vector's keys so Finish can hand them over
	OrderedOwningStringMap<uint32_t> dictionary;
	VariantBuilder builder;
	//! Per-row spans into the builder's buffers, filled by EndRow
	vector<list_entry_t> keys_entries;
	vector<list_entry_t> children_entries;
	vector<list_entry_t> values_entries;
	vector<list_entry_t> blob_entries;
	vector<RowState> row_state;
	//! The open containers of the current row, innermost last
	vector<CV2VariantFrame> stack;
	bool in_row = false;
	bool root_emitted = false;
	bool finished = false;
	idx_t current_row = 0;
	idx_t blob_start = 0;
};

auto Convert(duckdb_v2_variant_builder_handle builder) -> CV2VariantBuilder * {
	return reinterpret_cast<CV2VariantBuilder *>(builder);
}
auto Convert(CV2VariantBuilder *builder) -> duckdb_v2_variant_builder_handle {
	return reinterpret_cast<duckdb_v2_variant_builder_handle>(builder);
}

} // anonymous namespace
} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------
using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_variant_builder_create(duckdb_v2_vector_handle vector, idx_t row_count,
                                                 duckdb_v2_variant_builder_handle *out_builder,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_builder);
	*out_builder = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &vec = *Convert(vector);
		if (vec.GetType().id() != duckdb::LogicalTypeId::VARIANT) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_variant_builder_create: the vector is of type %s, not VARIANT", vec.GetType().ToString());
		}
		if (vec.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_variant_builder_create: the vector is not flat; call duckdb_v2_vector_flatten first");
		}
		vec.Reserve(row_count);
		auto builder = duckdb::make_uniq<CV2VariantBuilder>(vec, row_count);
		*out_builder = Convert(builder.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_destroy(duckdb_v2_variant_builder_handle *builder) {
	return WithErrorHandler(nullptr, [&]() {
		if (!builder) {
			return;
		}
		if (*builder) {
			delete Convert(*builder);
			*builder = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_begin_row(duckdb_v2_variant_builder_handle builder, idx_t row,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->BeginRow(row); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_end_row(duckdb_v2_variant_builder_handle builder,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->EndRow(); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_append_null(duckdb_v2_variant_builder_handle builder,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->AppendNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_append_scalar(duckdb_v2_variant_builder_handle builder,
                                                        DUCKDB_V2_LOGICAL_TYPE_ID type_id, const void *data,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(
	    err, [&]() { Convert(builder)->AppendScalar(static_cast<duckdb::LogicalTypeId>(type_id), data); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_append_decimal(duckdb_v2_variant_builder_handle builder, uint8_t width,
                                                         uint8_t scale, const void *data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->AppendDecimal(width, scale, data); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_append_bytes(duckdb_v2_variant_builder_handle builder,
                                                       DUCKDB_V2_LOGICAL_TYPE_ID type_id, const uint8_t *data,
                                                       idx_t length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(
	    err, [&]() { Convert(builder)->AppendBytes(static_cast<duckdb::LogicalTypeId>(type_id), data, length); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_begin_object(duckdb_v2_variant_builder_handle builder, idx_t child_count,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->BeginContainer(true, child_count); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_append_key(duckdb_v2_variant_builder_handle builder, const duckdb_v2_str *key,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	DUCKDB_CHECK_ARG(key);
	return WithErrorHandler(err, [&]() {
		if (!key->ptr && key->len > 0) {
			throw duckdb::InvalidInputException("duckdb_v2_variant_builder_append_key: key->ptr is NULL");
		}
		Convert(builder)->AppendKey(duckdb::string_t(key->ptr, duckdb::NumericCast<uint32_t>(key->len)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_end_object(duckdb_v2_variant_builder_handle builder,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->EndContainer(true); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_begin_array(duckdb_v2_variant_builder_handle builder, idx_t child_count,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->BeginContainer(false, child_count); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_end_array(duckdb_v2_variant_builder_handle builder,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->EndContainer(false); });
}

DUCKDB_V2_ERROR duckdb_v2_variant_builder_finish(duckdb_v2_variant_builder_handle builder,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(builder);
	return WithErrorHandler(err, [&]() { Convert(builder)->Finish(); });
}
