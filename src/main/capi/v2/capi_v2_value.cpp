#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"

#include <string>

namespace duckdb::capiv2 {
namespace {

// Common preflight for typed-payload getters: handle must be non-null, value
// id must match, and the value must not be NULL. Throws InvalidInputException
// on any precondition failure so the outer WithErrorHandler routes it.
void RequireTypedValue(duckdb_v2_value_handle value, LogicalTypeId expected) {
	if (!value) {
		throw InvalidInputException("value handle cannot be null");
	}
	auto *v = Convert(value);
	if (v->type().id() != expected) {
		throw InvalidInputException("value is a " + v->type().ToString() + ", not a " +
		                            LogicalType(expected).ToString());
	}
	if (v->IsNull()) {
		throw InvalidInputException("value is NULL");
	}
}
// The child-count view of a value: LIST/ARRAY/STRUCT elements or fields,
// MAP 2 x entries (children alternate key, value), UNION 2 (tag + active
// member). NULL values of any type, and non-composites, report 0.
idx_t CompositeChildCount(const Value &v) {
	if (v.IsNull()) {
		return 0;
	}
	switch (v.type().id()) {
	case LogicalTypeId::LIST:
		return ListValue::GetChildren(v).size();
	case LogicalTypeId::ARRAY:
		return ArrayValue::GetChildren(v).size();
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		return StructValue::GetChildren(v).size();
	case LogicalTypeId::MAP:
		return MapValue::GetChildren(v).size() * 2;
	case LogicalTypeId::UNION:
		return 2;
	default:
		return 0;
	}
}

// Reads a value as T. A matching type id reads the payload straight through;
// anything else is converted on a copy through the default cast set, so a read
// never alters the value it reads, and a conversion the engine cannot do is
// the error.
//
// So these report a *value*, not a storage form: a DECIMAL read through an
// integer getter is converted, fraction and all, rather than handed back as
// its backing integer. value_get_decimal is the storage-form reader, and the
// two borrowing getters use RequireTypedValue directly -- a converted copy
// would not outlive the call.
template <class T>
T ReadAs(duckdb_v2_value_handle value, LogicalTypeId expected) {
	if (!value) {
		throw InvalidInputException("value handle cannot be null");
	}
	auto &v = *Convert(value);
	if (v.IsNull()) {
		throw InvalidInputException("value is NULL");
	}
	if (v.type().id() == expected) {
		return v.GetValueUnsafe<T>();
	}
	string error;
	auto converted = v.DefaultTryCastAs(LogicalType(expected), &error);
	if (!converted) {
		throw InvalidInputException("cannot read a " + v.type().ToString() + " value as " +
		                            LogicalType(expected).ToString() + ": " + error);
	}
	return converted->GetValueUnsafe<T>();
}

// Hands a freshly built value to the caller.
duckdb_v2_value_handle Emit(Value value) {
	return Convert(new Value(std::move(value)));
}

// Gate for the constructors that take a type rather than a payload. ANY is a
// signature wildcard; a value carries data, so reject it.
LogicalType RequireValueType(duckdb_v2_logical_type_handle type) {
	if (!type) {
		throw InvalidInputException("logical type handle cannot be null");
	}
	auto lt_ref = Convert(type);
	auto &lt = *lt_ref;
	if (lt.id() == LogicalTypeId::ANY) {
		throw InvalidInputException("type cannot be ANY");
	}
	return lt;
}

// The DECIMAL storage tier follows from the width, and the engine's hugeint
// constructor covers only the widest one, so the narrower tiers go through the
// int64 form. Width and scale are gated here: an out-of-range pair would build
// a broken type rather than a bad value.
Value BuildDecimal(const duckdb_v2_hugeint_t *in_value, uint8_t width, uint8_t scale) {
	if (width < 1 || width > Decimal::MAX_WIDTH_DECIMAL) {
		throw InvalidInputException(": DECIMAL width must be between 1 and " +
		                            std::to_string(Decimal::MAX_WIDTH_DECIMAL));
	}
	if (scale > width) {
		throw InvalidInputException(": DECIMAL scale cannot exceed the width");
	}
	auto value = Convert(in_value);
	auto type = LogicalType::DECIMAL(width, scale);
	if (type.InternalType() == PhysicalType::INT128) {
		return Value::DECIMAL(value, width, scale);
	}
	int64_t narrow = 0;
	if (!Hugeint::TryCast<int64_t>(value, narrow)) {
		throw InvalidInputException("value does not fit the storage tier of the declared width");
	}
	return Value::DECIMAL(narrow, width, scale);
}

// BIT carries a mandatory padding-header byte, BIGNUM a header plus at least
// one magnitude byte; anything shorter is not addressable storage.
Value BuildBit(const duckdb_v2_str *in_value) {
	auto bytes = Convert(in_value);
	if (bytes.empty()) {
		throw InvalidInputException("the BIT wire form carries a mandatory padding header byte");
	}
	return Value::BIT(const_data_ptr_cast(bytes.data()), bytes.size());
}

Value BuildBignum(const duckdb_v2_str *in_value) {
	auto bytes = Convert(in_value);
	if (bytes.size() <= Bignum::BIGNUM_HEADER_SIZE) {
		throw InvalidInputException("the BIGNUM storage form requires more than " +
		                            std::to_string(Bignum::BIGNUM_HEADER_SIZE) + " bytes");
	}
	return Value::BIGNUM(const_data_ptr_cast(bytes.data()), bytes.size());
}

} // anonymous namespace
} // namespace duckdb::capiv2

// ---------------------------------------------------------------------------
// Lifecycle + NULL
// ---------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_value_destroy(duckdb_v2_value_handle *value) {
	return WithErrorHandler(nullptr, [&]() {
		if (!value) {
			return;
		}
		if (*value) {
			delete Convert(*value);
			*value = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_value_is_null(duckdb_v2_value_handle value, bool *out_is_null,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(out_is_null);
	return WithErrorHandler(err, [&]() { *out_is_null = Convert(value)->IsNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_logical_type(duckdb_v2_value_handle value, duckdb_v2_logical_type_handle *out_type,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(out_type);
	*out_type = nullptr;
	return WithErrorHandler(err, [&]() { *out_type = Convert(Convert(value)->type()); });
}

DUCKDB_V2_ERROR duckdb_v2_value_to_string(duckdb_v2_value_handle value, char *out_string, idx_t out_capacity,
                                          idx_t *out_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(out_length);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		FillCallerText(out_string, out_capacity, out_length, Convert(value)->ToString(), "duckdb_v2_value_to_string");
	});
}

// ---------------------------------------------------------------------------
// Typed accessors
//
// Each reads exactly its own type id: a getter is not a cast, so a mismatched
// value is refused rather than converted (value_cast is the converting path).
// NULL values have no payload and are refused too.
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_value_get_bool(duckdb_v2_value_handle value, bool *out, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<bool>(value, duckdb::LogicalTypeId::BOOLEAN); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_tinyint(duckdb_v2_value_handle value, int8_t *out,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<int8_t>(value, duckdb::LogicalTypeId::TINYINT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_smallint(duckdb_v2_value_handle value, int16_t *out,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<int16_t>(value, duckdb::LogicalTypeId::SMALLINT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_int(duckdb_v2_value_handle value, int32_t *out, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<int32_t>(value, duckdb::LogicalTypeId::INTEGER); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_bigint(duckdb_v2_value_handle value, int64_t *out,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<int64_t>(value, duckdb::LogicalTypeId::BIGINT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_hugeint(duckdb_v2_value_handle value, duckdb_v2_hugeint_t *out,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = Convert(ReadAs<duckdb::hugeint_t>(value, duckdb::LogicalTypeId::HUGEINT)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_utinyint(duckdb_v2_value_handle value, uint8_t *out,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<uint8_t>(value, duckdb::LogicalTypeId::UTINYINT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_usmallint(duckdb_v2_value_handle value, uint16_t *out,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<uint16_t>(value, duckdb::LogicalTypeId::USMALLINT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_uint(duckdb_v2_value_handle value, uint32_t *out,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<uint32_t>(value, duckdb::LogicalTypeId::UINTEGER); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_ubigint(duckdb_v2_value_handle value, uint64_t *out,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<uint64_t>(value, duckdb::LogicalTypeId::UBIGINT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_uhugeint(duckdb_v2_value_handle value, duckdb_v2_uhugeint_t *out,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = Convert(ReadAs<duckdb::uhugeint_t>(value, duckdb::LogicalTypeId::UHUGEINT)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_float(duckdb_v2_value_handle value, float *out, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<float>(value, duckdb::LogicalTypeId::FLOAT); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_double(duckdb_v2_value_handle value, double *out,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<double>(value, duckdb::LogicalTypeId::DOUBLE); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_varchar(duckdb_v2_value_handle value, duckdb_v2_str *out,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() {
		RequireTypedValue(value, duckdb::LogicalTypeId::VARCHAR);
		*out = Convert(duckdb::StringValue::Get(*Convert(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_value_get_blob(duckdb_v2_value_handle value, duckdb_v2_str *out,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() {
		auto &v = *Convert(value);
		// The byte-string reader: BLOB plus the kinds whose payload is opaque
		// storage bytes (BIT's padding header + data, BIGNUM's bignum_decode
		// input). VARCHAR is text, and has its own getter.
		auto id = v.type().id();
		if (id != duckdb::LogicalTypeId::BLOB && id != duckdb::LogicalTypeId::BIT &&
		    id != duckdb::LogicalTypeId::BIGNUM) {
			throw duckdb::InvalidInputException("duckdb_v2_value_get_blob: value is not of the expected type");
		}
		if (v.IsNull()) {
			throw duckdb::InvalidInputException("duckdb_v2_value_get_blob: value is NULL");
		}
		*out = Convert(duckdb::StringValue::Get(v));
	});
}

// ---------------------------------------------------------------------------
// TYPE values (a logical type carried as a value)
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_value_get_type(duckdb_v2_value_handle value, duckdb_v2_logical_type_handle *out_type,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_type);
	*out_type = nullptr;
	return WithErrorHandler(err, [&]() {
		RequireTypedValue(value, duckdb::LogicalTypeId::TYPE);
		// TypeValue::GetType deserializes the stored type into a fresh copy.
		*out_type = Convert(duckdb::TypeValue::GetType(*Convert(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_value_get_uuid(duckdb_v2_value_handle value, duckdb_v2_hugeint_t *out,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() {
		// UUID shares HUGEINT's storage but not its meaning, so it is read
		// exactly rather than through the converting path.
		RequireTypedValue(value, duckdb::LogicalTypeId::UUID);
		*out = Convert(Convert(value)->GetValueUnsafe<duckdb::hugeint_t>());
	});
}

DUCKDB_V2_ERROR duckdb_v2_value_get_decimal(duckdb_v2_value_handle value, duckdb_v2_hugeint_t *out, uint8_t *out_width,
                                            uint8_t *out_scale, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	DUCKDB_CHECK_ARG(out_width);
	DUCKDB_CHECK_ARG(out_scale);
	return WithErrorHandler(err, [&]() {
		RequireTypedValue(value, duckdb::LogicalTypeId::DECIMAL);
		auto &v = *Convert(value);
		auto &type = v.type();
		*out_width = duckdb::DecimalType::GetWidth(type);
		*out_scale = duckdb::DecimalType::GetScale(type);
		// The storage tier follows from the width; widen it to the one form the
		// caller always gets back.
		switch (type.InternalType()) {
		case duckdb::PhysicalType::INT16:
			*out = Convert(duckdb::hugeint_t(v.GetValueUnsafe<int16_t>()));
			break;
		case duckdb::PhysicalType::INT32:
			*out = Convert(duckdb::hugeint_t(v.GetValueUnsafe<int32_t>()));
			break;
		case duckdb::PhysicalType::INT64:
			*out = Convert(duckdb::hugeint_t(v.GetValueUnsafe<int64_t>()));
			break;
		default:
			*out = Convert(v.GetValueUnsafe<duckdb::hugeint_t>());
			break;
		}
	});
}

// ---------------------------------------------------------------------------
// Temporal accessors
//
// Each reports its type's own unit, the same one the vector plane exposes:
// no calendar conversion happens here.
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_value_get_date(duckdb_v2_value_handle value, int32_t *out, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<duckdb::date_t>(value, duckdb::LogicalTypeId::DATE).days; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_time(duckdb_v2_value_handle value, int64_t *out, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = ReadAs<duckdb::dtime_t>(value, duckdb::LogicalTypeId::TIME).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_time_ns(duckdb_v2_value_handle value, int64_t *out,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err,
	                        [&]() { *out = ReadAs<duckdb::dtime_ns_t>(value, duckdb::LogicalTypeId::TIME_NS).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_time_tz(duckdb_v2_value_handle value, uint64_t *out,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err,
	                        [&]() { *out = ReadAs<duckdb::dtime_tz_t>(value, duckdb::LogicalTypeId::TIME_TZ).bits; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_timestamp(duckdb_v2_value_handle value, int64_t *out,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = ReadAs<duckdb::timestamp_t>(value, duckdb::LogicalTypeId::TIMESTAMP).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_timestamp_sec(duckdb_v2_value_handle value, int64_t *out,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = ReadAs<duckdb::timestamp_sec_t>(value, duckdb::LogicalTypeId::TIMESTAMP_SEC).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_timestamp_ms(duckdb_v2_value_handle value, int64_t *out,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = ReadAs<duckdb::timestamp_ms_t>(value, duckdb::LogicalTypeId::TIMESTAMP_MS).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_timestamp_ns(duckdb_v2_value_handle value, int64_t *out,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = ReadAs<duckdb::timestamp_ns_t>(value, duckdb::LogicalTypeId::TIMESTAMP_NS).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_timestamp_tz(duckdb_v2_value_handle value, int64_t *out,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = ReadAs<duckdb::timestamp_tz_t>(value, duckdb::LogicalTypeId::TIMESTAMP_TZ).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_timestamp_tz_ns(duckdb_v2_value_handle value, int64_t *out,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = ReadAs<duckdb::timestamp_tz_ns_t>(value, duckdb::LogicalTypeId::TIMESTAMP_TZ_NS).value; });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_interval(duckdb_v2_value_handle value, duckdb_v2_interval_t *out,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(
	    err, [&]() { *out = Convert(ReadAs<duckdb::interval_t>(value, duckdb::LogicalTypeId::INTERVAL)); });
}

// ---------------------------------------------------------------------------
// Typed constructors
//
// Gated on the context handle, which is what makes a value's construction
// scoped to a catalog.
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_value_create_bool(duckdb_v2_factory_handle factory, bool in_value,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::BOOLEAN(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_tinyint(duckdb_v2_factory_handle factory, int8_t in_value,
                                               duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::TINYINT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_smallint(duckdb_v2_factory_handle factory, int16_t in_value,
                                                duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::SMALLINT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_int(duckdb_v2_factory_handle factory, int32_t in_value,
                                           duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::INTEGER(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_bigint(duckdb_v2_factory_handle factory, int64_t in_value,
                                              duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::BIGINT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_hugeint(duckdb_v2_factory_handle factory, const duckdb_v2_hugeint_t *in_value,
                                               duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::HUGEINT(Convert(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_utinyint(duckdb_v2_factory_handle factory, uint8_t in_value,
                                                duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::UTINYINT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_usmallint(duckdb_v2_factory_handle factory, uint16_t in_value,
                                                 duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::USMALLINT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_uint(duckdb_v2_factory_handle factory, uint32_t in_value,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::UINTEGER(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_ubigint(duckdb_v2_factory_handle factory, uint64_t in_value,
                                               duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::UBIGINT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_uhugeint(duckdb_v2_factory_handle factory, const duckdb_v2_uhugeint_t *in_value,
                                                duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::UHUGEINT(Convert(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_float(duckdb_v2_factory_handle factory, float in_value,
                                             duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::FLOAT(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_double(duckdb_v2_factory_handle factory, double in_value,
                                              duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::DOUBLE(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_varchar(duckdb_v2_factory_handle factory, const duckdb_v2_str *in_value,
                                               duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value(Convert(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_blob(duckdb_v2_factory_handle factory, const duckdb_v2_str *in_value,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::BLOB_RAW(std::string(Convert(in_value)))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_null(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle type,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value(RequireValueType(type))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_type(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle in_type,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::TYPE(RequireValueType(in_type))); });
}

// ---------------------------------------------------------------------------
// Temporal constructors
//
// The duals of the temporal accessors: same units, no range checking.
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_value_create_date(duckdb_v2_factory_handle factory, int32_t in_value,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::DATE(duckdb::date_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_time(duckdb_v2_factory_handle factory, int64_t in_value,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::TIME(duckdb::dtime_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_time_ns(duckdb_v2_factory_handle factory, int64_t in_value,
                                               duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::TIME_NS(duckdb::dtime_ns_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_time_tz(duckdb_v2_factory_handle factory, uint64_t in_value,
                                               duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::TIMETZ(duckdb::dtime_tz_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_timestamp(duckdb_v2_factory_handle factory, int64_t in_value,
                                                 duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::TIMESTAMP(duckdb::timestamp_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_timestamp_sec(duckdb_v2_factory_handle factory, int64_t in_value,
                                                     duckdb_v2_value_handle *out_value,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *out_value = Emit(duckdb::Value::TIMESTAMPSEC(duckdb::timestamp_sec_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_timestamp_ms(duckdb_v2_factory_handle factory, int64_t in_value,
                                                    duckdb_v2_value_handle *out_value,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err,
	                        [&]() { *out_value = Emit(duckdb::Value::TIMESTAMPMS(duckdb::timestamp_ms_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_timestamp_ns(duckdb_v2_factory_handle factory, int64_t in_value,
                                                    duckdb_v2_value_handle *out_value,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err,
	                        [&]() { *out_value = Emit(duckdb::Value::TIMESTAMPNS(duckdb::timestamp_ns_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_timestamp_tz(duckdb_v2_factory_handle factory, int64_t in_value,
                                                    duckdb_v2_value_handle *out_value,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err,
	                        [&]() { *out_value = Emit(duckdb::Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_timestamp_tz_ns(duckdb_v2_factory_handle factory, int64_t in_value,
                                                       duckdb_v2_value_handle *out_value,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *out_value = Emit(duckdb::Value::TIMESTAMPTZNS(duckdb::timestamp_tz_ns_t(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_interval(duckdb_v2_factory_handle factory, const duckdb_v2_interval_t *in_value,
                                                duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::INTERVAL(Convert(in_value))); });
}

// ---------------------------------------------------------------------------
// DECIMAL, UUID, BIT and BIGNUM constructors
//
// The kinds whose payload needs more than a plain scalar: a scaled integer
// plus its width and scale, an internal 128-bit form, or opaque storage bytes.
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_value_create_decimal(duckdb_v2_factory_handle factory, const duckdb_v2_hugeint_t *in_value,
                                               uint8_t width, uint8_t scale, duckdb_v2_value_handle *out_value,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(BuildDecimal(in_value, width, scale)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_uuid(duckdb_v2_factory_handle factory, const duckdb_v2_hugeint_t *in_value,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(duckdb::Value::UUID(Convert(in_value))); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_bit(duckdb_v2_factory_handle factory, const duckdb_v2_str *in_value,
                                           duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(BuildBit(in_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_bignum(duckdb_v2_factory_handle factory, const duckdb_v2_str *in_value,
                                              duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(BuildBignum(in_value)); });
}

// ---------------------------------------------------------------------------
// Composite construction + descent + cast
// ---------------------------------------------------------------------------

// The children cross as borrowed handles; every composite copies them in.
namespace {

duckdb::vector<duckdb::Value> CollectChildren(const duckdb_v2_value_handle *children, idx_t count) {
	if (count > 0 && !children) {
		throw duckdb::InvalidInputException("'children' cannot be null when count > 0");
	}
	duckdb::vector<duckdb::Value> values;
	values.reserve(count);
	for (idx_t i = 0; i < count; i++) {
		if (!children[i]) {
			throw duckdb::InvalidInputException("child values cannot be null");
		}
		values.push_back(*Convert(children[i]));
	}
	return values;
}

// The common type of a set of children, the same rule a list literal follows.
// An explicit type wins outright; without one an empty set has nothing to
// resolve, which is what makes the empty forms take their type.
duckdb::LogicalType ResolveChildType(CV2Factory &factory, duckdb_v2_logical_type_handle declared,
                                     const duckdb::vector<duckdb::Value> &children, const char *what) {
	if (declared) {
		auto declared_type = Convert(declared);
		return *declared_type;
	}
	if (children.empty()) {
		throw duckdb::InvalidInputException(std::string("cannot resolve the ") + what +
		                                    " of an empty set; pass it explicitly");
	}
	duckdb::vector<duckdb::LogicalType> types;
	types.reserve(children.size());
	for (auto &child : children) {
		types.push_back(child.type());
	}
	return ResolveCommonType(factory, types);
}

duckdb::Value BuildList(CV2Factory &factory, duckdb_v2_logical_type_handle child_type,
                        const duckdb_v2_value_handle *children, idx_t child_count) {
	auto values = CollectChildren(children, child_count);
	auto type = ResolveChildType(factory, child_type, values, "element type");
	return duckdb::Value::LIST(type, std::move(values));
}

duckdb::Value BuildArray(CV2Factory &factory, duckdb_v2_logical_type_handle child_type,
                         const duckdb_v2_value_handle *children, idx_t child_count) {
	auto values = CollectChildren(children, child_count);
	// The engine's minimum array size is 1, so there is no empty ARRAY, with or
	// without a declared element type.
	if (values.empty()) {
		throw duckdb::InvalidInputException("an ARRAY must have at least one element");
	}
	auto type = ResolveChildType(factory, child_type, values, "element type");
	return duckdb::Value::ARRAY(type, std::move(values));
}

duckdb::Value BuildStruct(const duckdb_v2_identifier_t *names, const duckdb_v2_value_handle *children,
                          idx_t field_count) {
	if (field_count > 0 && !names) {
		throw duckdb::InvalidInputException("'names' cannot be null when field_count > 0");
	}
	auto values = CollectChildren(children, field_count);
	// Each field carries its own child's type, so the type is assembled here
	// rather than resolved across the fields.
	duckdb::child_list_t<duckdb::Value> fields;
	fields.reserve(field_count);
	for (idx_t i = 0; i < field_count; i++) {
		if (!names[i].ptr && names[i].len > 0) {
			throw duckdb::InvalidInputException("field names cannot be null");
		}
		fields.emplace_back(std::string(ConvertIdentifierName(names[i])), std::move(values[i]));
	}
	return duckdb::Value::STRUCT(std::move(fields));
}

duckdb::Value BuildTuple(const duckdb_v2_value_handle *children, idx_t field_count) {
	auto values = CollectChildren(children, field_count);
	duckdb::vector<duckdb::LogicalType> types;
	types.reserve(field_count);
	for (auto &value : values) {
		types.push_back(value.type());
	}
	return duckdb::Value::STRUCT(duckdb::LogicalType::TUPLE(std::move(types)), std::move(values));
}

duckdb::Value BuildMap(CV2Factory &factory, duckdb_v2_logical_type_handle key_type,
                       duckdb_v2_logical_type_handle value_type, const duckdb_v2_value_handle *keys,
                       const duckdb_v2_value_handle *values, idx_t entry_count) {
	auto key_values = CollectChildren(keys, entry_count);
	auto value_values = CollectChildren(values, entry_count);
	auto resolved_key = ResolveChildType(factory, key_type, key_values, "key type");
	auto resolved_value = ResolveChildType(factory, value_type, value_values, "value type");
	return duckdb::Value::MAP(resolved_key, resolved_value, std::move(key_values), std::move(value_values));
}

} // namespace

DUCKDB_V2_ERROR duckdb_v2_value_create_list(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle child_type,
                                            const duckdb_v2_value_handle *children, idx_t child_count,
                                            duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *out_value = Emit(BuildList(*Convert(factory), child_type, children, child_count)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_array(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle child_type,
                                             const duckdb_v2_value_handle *children, idx_t child_count,
                                             duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *out_value = Emit(BuildArray(*Convert(factory), child_type, children, child_count)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_struct(duckdb_v2_factory_handle factory, const duckdb_v2_identifier_t *names,
                                              const duckdb_v2_value_handle *children, idx_t field_count,
                                              duckdb_v2_value_handle *out_value, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(BuildStruct(names, children, field_count)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_create_tuple(duckdb_v2_factory_handle factory, const duckdb_v2_value_handle *children,
                                             idx_t field_count, duckdb_v2_value_handle *out_value,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Emit(BuildTuple(children, field_count)); });
}

DUCKDB_V2_ERROR
duckdb_v2_value_create_map(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle key_type,
                           duckdb_v2_logical_type_handle value_type, const duckdb_v2_value_handle *keys,
                           const duckdb_v2_value_handle *values, idx_t entry_count, duckdb_v2_value_handle *out_value,
                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() {
		*out_value = Emit(BuildMap(*Convert(factory), key_type, value_type, keys, values, entry_count));
	});
}

DUCKDB_V2_ERROR duckdb_v2_value_get_child_count(duckdb_v2_value_handle value, idx_t *out_count,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = CompositeChildCount(*Convert(value)); });
}

DUCKDB_V2_ERROR duckdb_v2_value_get_child(duckdb_v2_value_handle value, idx_t index, duckdb_v2_value_handle *out_child,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(out_child);
	*out_child = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &v = *Convert(value);
		if (index >= CompositeChildCount(v)) {
			throw duckdb::InvalidInputException("child index out of range in duckdb_v2_value_get_child");
		}
		duckdb::Value child;
		switch (v.type().id()) {
		case duckdb::LogicalTypeId::LIST:
			child = duckdb::ListValue::GetChildren(v)[index];
			break;
		case duckdb::LogicalTypeId::ARRAY:
			child = duckdb::ArrayValue::GetChildren(v)[index];
			break;
		case duckdb::LogicalTypeId::STRUCT:
		case duckdb::LogicalTypeId::TUPLE:
			child = duckdb::StructValue::GetChildren(v)[index];
			break;
		case duckdb::LogicalTypeId::MAP: {
			// Entries are STRUCT(key, value) internally; surface them
			// alternating, symmetric with value_create.
			auto &entry = duckdb::MapValue::GetChildren(v)[index / 2];
			child = duckdb::StructValue::GetChildren(entry)[index % 2];
			break;
		}
		case duckdb::LogicalTypeId::UNION:
			child =
			    index == 0 ? duckdb::Value::UTINYINT(duckdb::UnionValue::GetTag(v)) : duckdb::UnionValue::GetValue(v);
			break;
		default:
			throw duckdb::InternalException("unreachable: bounds check rejects non-composites");
		}
		*out_child = Convert(new duckdb::Value(std::move(child)));
	});
}

static void CastValueV2(CV2Factory &factory, duckdb_v2_value_handle value, duckdb_v2_logical_type_handle target_type,
                        duckdb_v2_value_handle *out_value) {
	*out_value = nullptr;
	// Non-strict, through the client's cast function set (registered custom
	// casts included), or the built-in casts only. Cast failures propagate.
	auto target = Convert(target_type);
	auto context = factory.TryGetClientContext();
	auto casted = context ? Convert(value)->CastAs(*context, *target) : Convert(value)->DefaultCastAs(*target);
	*out_value = Convert(new duckdb::Value(std::move(casted)));
}

DUCKDB_V2_ERROR duckdb_v2_value_cast(duckdb_v2_factory_handle factory, duckdb_v2_value_handle value,
                                     duckdb_v2_logical_type_handle target_type, duckdb_v2_value_handle *out_value,
                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(target_type);
	DUCKDB_CHECK_ARG(out_value);
	return WithErrorHandler(err, [&]() { CastValueV2(*Convert(factory), value, target_type, out_value); });
}
