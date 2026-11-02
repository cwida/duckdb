#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_type_extension.hpp"
#include "duckdb/common/arrow/arrow_buffer.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/common/arrow/appender/append_data.hpp"
#include "duckdb/common/arrow/appender/list.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// ArrowAppender
//===--------------------------------------------------------------------===//

ArrowAppender::ArrowAppender(vector<LogicalType> types_p, const idx_t initial_capacity, ClientProperties options,
                             unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_type_cast)
    : types(std::move(types_p)), options(options) {
	for (idx_t i = 0; i < types.size(); i++) {
		// Pass any explicit per-column extension override through to InitializeChild; when none
		// is supplied it auto-resolves the extension (and applies the bitshift_boolean gate) so
		// children of nested types pick up the same extension SetArrowFormat uses for the schema.
		auto extension_it = extension_type_cast.find(i);
		shared_ptr<ArrowTypeExtensionData> extension =
		    extension_it != extension_type_cast.end() ? extension_it->second : nullptr;
		root_data.push_back(InitializeChild(types[i], initial_capacity, options, extension));
	}
}

ArrowAppender::~ArrowAppender() {
}

//! Append a data chunk to the underlying arrow array
void ArrowAppender::Append(DataChunk &input, const idx_t from, const idx_t to, const idx_t input_size) {
	D_ASSERT(types == input.GetTypes());
	D_ASSERT(to >= from);
	for (idx_t i = 0; i < input.ColumnCount(); i++) {
		root_data[i]->AppendChild(input.data[i], from, to, input_size);
	}
	row_count += to - from;
}

idx_t ArrowAppender::RowCount() const {
	return row_count;
}

idx_t ArrowAppender::ByteSize() const {
	idx_t bytes = 0;
	for (auto &data : root_data) {
		bytes += data->ByteSize();
	}
	return bytes;
}

void ArrowAppender::ReleaseArray(ArrowArray *array) {
	if (!array || !array->release) {
		return;
	}
	auto holder = static_cast<ArrowAppendData *>(array->private_data);
	for (int64_t i = 0; i < array->n_children; i++) {
		auto child = array->children[i];
		if (!child->release) {
			// Child was moved out of the array
			continue;
		}
		child->release(child);
		D_ASSERT(!child->release);
	}
	if (array->dictionary && array->dictionary->release) {
		array->dictionary->release(array->dictionary);
	}
	array->release = nullptr;
	delete holder;
}

//===--------------------------------------------------------------------===//
// Finalize Arrow Child
//===--------------------------------------------------------------------===//
ArrowArray *ArrowAppender::FinalizeChild(const LogicalType &type, unique_ptr<ArrowAppendData> append_data_p) {
	auto result = make_uniq<ArrowArray>();

	auto &append_data = *append_data_p;
	result->private_data = append_data_p.release();
	result->release = ReleaseArray;
	result->n_children = 0;
	result->null_count = 0;
	result->offset = 0;
	result->dictionary = nullptr;
	result->buffers = append_data.buffers.data();
	result->null_count = NumericCast<int64_t>(append_data.null_count);
	result->length = NumericCast<int64_t>(append_data.row_count);
	result->buffers[0] = append_data.GetValidityBuffer().data();

	if (append_data.finalize) {
		// An extension type's appender tree was initialized (and appended) with the extension's INTERNAL
		// (storage) type — see InitializeChild — so it must also be finalized with that type: a nested
		// finalizer (struct/list/map) sizes its child walk from the type it is given, and the LOGICAL
		// type's shape need not match the storage-built child_data (e.g. a canonical Arrow extension over
		// struct storage whose logical type has a different child count).
		if (append_data.extension_data) {
			append_data.finalize(append_data, append_data.extension_data->GetInternalType(), result.get());
		} else {
			append_data.finalize(append_data, type, result.get());
		}
	}

	append_data.array = std::move(result);
	return append_data.array.get();
}

//! Returns the underlying arrow array
ArrowArray ArrowAppender::Finalize() {
	D_ASSERT(root_data.size() == types.size());
	auto root_holder = make_uniq<ArrowAppendData>(options);

	ArrowArray result;
	AddChildren(*root_holder, types.size());
	result.children = root_holder->child_pointers.data();
	result.n_children = NumericCast<int64_t>(types.size());

	// Configure root array
	result.length = NumericCast<int64_t>(row_count);
	result.n_buffers = 1;
	result.buffers = root_holder->buffers.data(); // there is no actual buffer there since we don't have NULLs
	result.offset = 0;
	result.null_count = 0; // needs to be 0
	result.dictionary = nullptr;
	root_holder->child_data = std::move(root_data);

	for (idx_t i = 0; i < root_holder->child_data.size(); i++) {
		root_holder->child_arrays[i] = *ArrowAppender::FinalizeChild(types[i], std::move(root_holder->child_data[i]));
	}

	// Release ownership to caller
	result.private_data = root_holder.release();
	result.release = ArrowAppender::ReleaseArray;
	return result;
}

//===--------------------------------------------------------------------===//
// Initialize Arrow Child
//===--------------------------------------------------------------------===//

template <class OP>
static void InitializeAppenderForType(ArrowAppendData &append_data) {
	append_data.initialize = OP::Initialize;
	append_data.append_vector = OP::Append;
	append_data.finalize = OP::Finalize;
}

static void InitializeFunctionPointers(ArrowAppendData &append_data, const LogicalType &type) {
	// handle special logical types
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		InitializeAppenderForType<ArrowNullData>(append_data);
		break;
	case LogicalTypeId::BOOLEAN:
		InitializeAppenderForType<ArrowBoolData>(append_data);
		break;
	case LogicalTypeId::TINYINT:
		InitializeAppenderForType<ArrowScalarData<int8_t>>(append_data);
		break;
	case LogicalTypeId::SMALLINT:
		InitializeAppenderForType<ArrowScalarData<int16_t>>(append_data);
		break;
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTEGER:
		InitializeAppenderForType<ArrowScalarData<int32_t>>(append_data);
		break;
	case LogicalTypeId::TIME_TZ: {
		if (append_data.options.arrow_lossless_conversion) {
			InitializeAppenderForType<ArrowScalarData<int64_t>>(append_data);
		} else {
			InitializeAppenderForType<ArrowScalarData<int64_t, dtime_tz_t, ArrowTimeTzConverter>>(append_data);
		}
		break;
	}
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::BIGINT:
		InitializeAppenderForType<ArrowScalarData<int64_t>>(append_data);
		break;
	case LogicalTypeId::UUID:
		if (append_data.options.arrow_lossless_conversion) {
			InitializeAppenderForType<ArrowScalarData<hugeint_t, hugeint_t, ArrowUUIDBlobConverter>>(append_data);
		} else {
			if (append_data.options.arrow_offset_size == ArrowOffsetSize::LARGE) {
				InitializeAppenderForType<ArrowVarcharData<hugeint_t, ArrowUUIDConverter>>(append_data);
			} else {
				InitializeAppenderForType<ArrowVarcharData<hugeint_t, ArrowUUIDConverter, int32_t>>(append_data);
			}
		}
		break;
	case LogicalTypeId::HUGEINT:
		InitializeAppenderForType<ArrowScalarData<hugeint_t>>(append_data);
		break;
	case LogicalTypeId::UHUGEINT:
		InitializeAppenderForType<ArrowScalarData<uhugeint_t>>(append_data);
		break;
	case LogicalTypeId::UTINYINT:
		InitializeAppenderForType<ArrowScalarData<uint8_t>>(append_data);
		break;
	case LogicalTypeId::USMALLINT:
		InitializeAppenderForType<ArrowScalarData<uint16_t>>(append_data);
		break;
	case LogicalTypeId::UINTEGER:
		InitializeAppenderForType<ArrowScalarData<uint32_t>>(append_data);
		break;
	case LogicalTypeId::UBIGINT:
		InitializeAppenderForType<ArrowScalarData<uint64_t>>(append_data);
		break;
	case LogicalTypeId::FLOAT:
		InitializeAppenderForType<ArrowScalarData<float>>(append_data);
		break;
	case LogicalTypeId::DOUBLE:
		InitializeAppenderForType<ArrowScalarData<double>>(append_data);
		break;
	case LogicalTypeId::DECIMAL:
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			if (append_data.options.arrow_output_version > ArrowFormatVersion::V1_4) {
				InitializeAppenderForType<ArrowScalarData<int32_t, int16_t>>(append_data);
			} else {
				InitializeAppenderForType<ArrowScalarData<hugeint_t, int16_t>>(append_data);
			}
			break;
		case PhysicalType::INT32:
			if (append_data.options.arrow_output_version > ArrowFormatVersion::V1_4) {
				InitializeAppenderForType<ArrowScalarData<int32_t>>(append_data);
			} else {
				InitializeAppenderForType<ArrowScalarData<hugeint_t, int32_t>>(append_data);
			}
			break;
		case PhysicalType::INT64:
			if (append_data.options.arrow_output_version > ArrowFormatVersion::V1_4) {
				InitializeAppenderForType<ArrowScalarData<int64_t>>(append_data);
			} else {
				InitializeAppenderForType<ArrowScalarData<hugeint_t, int64_t>>(append_data);
			}
			break;
		case PhysicalType::INT128:
			InitializeAppenderForType<ArrowScalarData<hugeint_t>>(append_data);
			break;
		default:
			throw InternalException("Unsupported internal decimal type");
		}
		break;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
		if ((append_data.options.produce_arrow_string_view || type.id() != LogicalTypeId::VARCHAR) &&
		    append_data.options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			InitializeAppenderForType<ArrowVarcharToStringViewData>(append_data);
		} else {
			if (append_data.options.arrow_offset_size == ArrowOffsetSize::LARGE) {
				InitializeAppenderForType<ArrowVarcharData<>>(append_data);
			} else {
				InitializeAppenderForType<ArrowVarcharData<string_t, ArrowVarcharConverter, int32_t>>(append_data);
			}
		}
		break;
	case LogicalTypeId::ENUM:
		switch (type.InternalType()) {
		case PhysicalType::UINT8:
			InitializeAppenderForType<ArrowEnumData<uint8_t>>(append_data);
			break;
		case PhysicalType::UINT16:
			InitializeAppenderForType<ArrowEnumData<uint16_t>>(append_data);
			break;
		case PhysicalType::UINT32:
			InitializeAppenderForType<ArrowEnumData<uint32_t>>(append_data);
			break;
		default:
			throw InternalException("Unsupported internal enum type");
		}
		break;
	case LogicalTypeId::INTERVAL:
		InitializeAppenderForType<ArrowScalarData<ArrowInterval, interval_t, ArrowIntervalConverter>>(append_data);
		break;
	case LogicalTypeId::UNION:
		InitializeAppenderForType<ArrowUnionData>(append_data);
		break;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		InitializeAppenderForType<ArrowStructData>(append_data);
		break;
	case LogicalTypeId::ARRAY:
		InitializeAppenderForType<ArrowFixedSizeListData>(append_data);
		break;
	case LogicalTypeId::LIST: {
		if (append_data.options.arrow_use_list_view &&
		    append_data.options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			if (append_data.options.arrow_offset_size == ArrowOffsetSize::LARGE) {
				InitializeAppenderForType<ArrowListViewData<>>(append_data);
			} else {
				InitializeAppenderForType<ArrowListViewData<int32_t>>(append_data);
			}
		} else {
			if (append_data.options.arrow_offset_size == ArrowOffsetSize::LARGE) {
				InitializeAppenderForType<ArrowListData<>>(append_data);
			} else {
				InitializeAppenderForType<ArrowListData<int32_t>>(append_data);
			}
		}
		break;
	}
	case LogicalTypeId::MAP:
		// Arrow MapArray only supports 32-bit offsets. There is no LargeMapArray type in Arrow.
		InitializeAppenderForType<ArrowMapData<int32_t>>(append_data);
		break;
	default:
		throw NotImplementedException("Unsupported type in DuckDB -> Arrow Conversion: %s\n", type.ToString());
	}
}

unique_ptr<ArrowAppendData> ArrowAppender::InitializeChild(const LogicalType &type, const idx_t capacity,
                                                           ClientProperties &options,
                                                           const shared_ptr<ArrowTypeExtensionData> &extension_type) {
	auto result = make_uniq<ArrowAppendData>(options);

	// Resolve the effective extension. An explicit override (from the top-level appender) wins.
	// Otherwise auto-resolve from DBConfig so nested children use the same extension SetArrowFormat
	// declares in the schema. BOOLEAN stays plain bit-packed when arrow_lossless_conversion is off
	// (the bitshift_boolean gate), applied here so it holds at every nesting level.
	shared_ptr<ArrowTypeExtensionData> effective_extension = extension_type;
	const bool bitshift_boolean = type.id() == LogicalTypeId::BOOLEAN && !options.arrow_lossless_conversion;
	if (bitshift_boolean) {
		effective_extension = nullptr;
	} else if (!effective_extension && options.client_context) {
		const auto &db_config = DBConfig::GetConfig(*options.client_context);
		if (db_config.HasArrowExtension(type)) {
			effective_extension = db_config.GetArrowExtension(type).GetTypeExtension();
		}
	}

	LogicalType array_type = type;
	if (effective_extension) {
		array_type = effective_extension->GetInternalType();
	}
	InitializeFunctionPointers(*result, array_type);
	result->extension_data = effective_extension;

	const auto byte_count = (capacity + 7) / 8;
	result->GetValidityBuffer().reserve(byte_count);
	result->initialize(*result, array_type, capacity);
	return result;
}

void ArrowAppender::AddChildren(ArrowAppendData &data, const idx_t count) {
	data.child_pointers.resize(count);
	data.child_arrays.resize(count);
	for (idx_t i = 0; i < count; i++) {
		data.child_pointers[i] = &data.child_arrays[i];
	}
}

} // namespace duckdb
