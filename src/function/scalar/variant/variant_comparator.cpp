#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/common/radix.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/types/variant_comparison.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Variant Comparator Encoding
//===--------------------------------------------------------------------===//
// VARIANT is physically a STRUCT (keys/children/values/data), but ordering it by that physical
// representation does not match the logical ordering of the values it stores. Instead we traverse
// the variant and emit a sort key per value as [type-rank][value]. Sorting is performed by type
// first and value second. Object keys are processed in string-sorted order so that objects that
// only differ in key order (e.g. {'b': 10, 'a': 1} and {'a': 1, 'b': 10}) compare equal.
//
// Types are grouped into "ranks" the way a semi-structured system (e.g. Snowflake) groups them:
// all integers, decimals and bignums fold into a single NUMBER rank (compared by numeric value) and
// FLOAT/DOUBLE fold into a single REAL rank. Everything else keeps its own rank.
//
// The produced key is byte-comparable and self-contained - it is only ever compared against other
// variant_comparator outputs, so the byte constants below only need to be internally consistent
// (they mirror the conventions used by create_sort_key).

//! byte constants for the comparator encoding
constexpr data_t STRING_DELIMITER = 0;
constexpr data_t LIST_DELIMITER = 0;
constexpr data_t BLOB_ESCAPE_CHARACTER = 1;

//! BIT/BITSTRING sort-key bytes (mirrors CreateBitStringSortKey in the core_functions bitstring code):
//! each bit is expanded to one byte, kept above the STRING_DELIMITER (0) so a shorter bitstring (a
//! prefix) sorts first, while preserving 0 < 1
constexpr data_t BIT_SORT_KEY_ZERO = 2;
constexpr data_t BIT_SORT_KEY_ONE = 3;

data_t GetVariantTypeRank(const VariantLogicalType type_id) {
	return static_cast<data_t>(GetVariantComparisonType(type_id));
}

template <class SINK>
void VariantEncodeNumber(SINK &sink, const VariantNumberKey &key) {
	// the class byte is the primary discriminator and is never complemented
	sink.Write(key.number_class);
	if (key.number_class == 1) {
		// zero - the class fully describes the value
		return;
	}
	const bool negative = key.number_class == 0;
	// for negatives we complement every payload byte so that larger magnitudes sort earlier
	auto emit = [&](data_t b) {
		sink.Write(negative ? static_cast<data_t>(~b) : b);
	};

	// adjusted exponent (order-preserving signed encoding) orders magnitude
	data_t exp_buffer[sizeof(int64_t)];
	Radix::EncodeData<int64_t>(exp_buffer, key.adjusted_exponent);
	for (idx_t i = 0; i < sizeof(int64_t); i++) {
		emit(exp_buffer[i]);
	}
	// significant digits (1..10 so the 0x00 terminator is smaller than any digit -> prefix sorts first)
	for (auto c : key.digits) {
		emit(static_cast<data_t>((c - '0') + 1));
	}
	emit(0); // terminator
}

//! Sink that appends the encoded bytes of a variant value to a (growable) buffer
struct VariantComparatorBufferSink {
	explicit VariantComparatorBufferSink(string &buffer) : buffer(buffer) {
	}

	string &buffer;

	inline void Write(data_t b) {
		buffer.push_back(static_cast<char>(b));
	}
	inline void WriteBytes(const_data_ptr_t src, idx_t count) {
		buffer.append(const_char_ptr_cast(src), count);
	}
};

template <class T, class SINK>
void VariantEncodeFixed(SINK &sink, T value) {
	data_t buffer[sizeof(T)];
	Radix::EncodeData<T>(buffer, value);
	sink.WriteBytes(buffer, sizeof(T));
}

template <class SINK>
void VariantEncodeString(SINK &sink, const string_t &str, bool is_varchar) {
	auto input_data = const_data_ptr_cast(str.GetData());
	auto input_size = str.GetSize();
	if (is_varchar) {
		// VARCHAR is valid UTF-8 (never contains 0xFF) so we use the +1 / null-delimiter scheme
		for (idx_t i = 0; i < input_size; i++) {
			sink.Write(static_cast<data_t>(input_data[i] + 1));
		}
	} else {
		// arbitrary bytes - escape \x00 and \x01 so they cannot collide with the delimiter
		for (idx_t i = 0; i < input_size; i++) {
			if (input_data[i] <= 1) {
				sink.Write(BLOB_ESCAPE_CHARACTER);
			}
			sink.Write(input_data[i]);
		}
	}
	sink.Write(STRING_DELIMITER);
}

template <class SINK>
void EncodeVariantValue(const VariantNode &it, SINK &sink) {
	const auto type_id = it.GetTypeId();
	const auto comparison_type = GetVariantComparisonType(type_id);
	// write the type rank - this guarantees values are ordered by type first
	sink.Write(GetVariantTypeRank(type_id));

	switch (comparison_type) {
	case VariantComparisonType::NULL_VALUE:
		// no payload - the rank fully describes the value
		break;
	case VariantComparisonType::BOOLEAN:
		sink.Write(type_id == VariantLogicalType::BOOL_TRUE ? 1 : 0);
		break;
	// all integers, decimals and bignums fold into the NUMBER rank and compare by numeric value
	case VariantComparisonType::NUMBER:
		VariantEncodeNumber(sink, VariantGetNumberKey(type_id, it));
		break;
	case VariantComparisonType::REAL:
		VariantEncodeFixed<double>(sink, VariantGetRealValue(type_id, it));
		break;
	case VariantComparisonType::UUID:
		VariantEncodeFixed<hugeint_t>(sink, it.GetData<hugeint_t>());
		break;
	// DATE and all (non-tz) TIMESTAMP precisions fold into the TIMESTAMP rank and compare as the number
	// of nanoseconds since the epoch (DATE compares as midnight of that day, TIMESTAMP WITH TIME ZONE
	// as its UTC instant). int128 is used so that the conversion to nanoseconds is lossless across the
	// full range of each type. The type rank already separates the tz / non-tz groups.
	case VariantComparisonType::TIMESTAMP:
		VariantEncodeFixed<hugeint_t>(sink, VariantGetTimestampValue(type_id, it));
		break;
	case VariantComparisonType::TIMESTAMP_TZ:
		VariantEncodeFixed<hugeint_t>(sink, VariantGetTimestampTZValue(type_id, it));
		break;
	// TIME precisions fold into the TIME rank and compare as nanoseconds since midnight (fits in int64)
	case VariantComparisonType::TIME:
		VariantEncodeFixed<int64_t>(sink, VariantGetTimeValue(type_id, it));
		break;
	case VariantComparisonType::TIME_TZ:
		// TIME WITH TIME ZONE requires a dedicated byte-comparable transform
		VariantEncodeFixed<uint64_t>(sink, it.GetData<dtime_tz_t>().sort_key());
		break;
	case VariantComparisonType::INTERVAL:
		// normalize the interval so that equal intervals encode identically
		VariantEncodeFixed<interval_t>(sink, it.GetData<interval_t>().Normalize());
		break;
	case VariantComparisonType::VARCHAR:
		VariantEncodeString(sink, it.GetString(), true);
		break;
	case VariantComparisonType::BLOB:
	case VariantComparisonType::GEOMETRY:
		VariantEncodeString(sink, it.GetString(), false);
		break;
	case VariantComparisonType::BITSTRING: {
		// BIT is ordered by its logical bit sequence, not by its raw bytes - expand each bit to a sort
		// key byte (matching the bitstring_byte_comparable collation), terminated so a shorter bitstring
		// (a prefix) sorts before a longer one
		auto bitstring = it.GetString();
		auto bit_length = Bit::BitLength(bitstring);
		for (idx_t bit_idx = 0; bit_idx < bit_length; bit_idx++) {
			sink.Write(Bit::GetBit(bitstring, bit_idx) ? BIT_SORT_KEY_ONE : BIT_SORT_KEY_ZERO);
		}
		sink.Write(STRING_DELIMITER);
		break;
	}
	case VariantComparisonType::ARRAY: {
		for (auto child : it.GetArrayChildren()) {
			EncodeVariantValue(child, sink);
		}
		sink.Write(LIST_DELIMITER);
		break;
	}
	case VariantComparisonType::OBJECT: {
		// process the children in string-sorted key order so that objects that only differ in key order
		// compare equal
		for (auto &entry : it.GetObjectChildren(VariantIterationOrder::LEXICOGRAPHIC)) {
			VariantEncodeString(sink, entry.key, true);
			EncodeVariantValue(entry.value, sink);
		}
		sink.Write(LIST_DELIMITER);
		break;
	}
	default:
		throw NotImplementedException("Variant type %s is not supported in variant_comparator",
		                              EnumUtil::ToString(type_id));
	}
}

//! Encodes the *logical* value of a VARIANT into a byte-comparable sort key. VARIANT is physically a
//! STRUCT, but the generic create_sort_key encodes that physical representation (which is reversible,
//! so it can be used by aggregates to store/decode values). For comparison / ordering we instead
//! encode the *logical* value of the variant - this encoding is intentionally not reversible (e.g.
//! all integer widths fold together). NULLs are propagated into the result validity.
void CreateVariantComparator(const Vector &input, idx_t count, Vector &result) {
	auto variant = input.Values<VectorVariantType>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto writer = FlatVector::Writer<string_t>(result, count);

	//! reused growable buffer - the key is encoded once and then copied into the result vector
	string buffer;
	for (idx_t r = 0; r < count; r++) {
		auto root = variant[r];
		// a VARIANT is only NULL at the root via a genuine SQL NULL (never a VARIANT_NULL value)
		if (root.IsNull()) {
			// propagate NULL so that NULL = NULL stays NULL and ORDER BY ... NULLS FIRST/LAST is honored
			writer.WriteNull();
			continue;
		}
		// encode the key once into the (reused) buffer, then hand it to the writer (which copies it).
		// NULLs are handled via validity above, so no NULL/validity prefix byte is needed in the key.
		buffer.clear();
		VariantComparatorBufferSink sink(buffer);
		EncodeVariantValue(root, sink);
		writer.WriteValue(string_t(buffer.data(), NumericCast<uint32_t>(buffer.size())));
	}
}

// variant_comparator produces a binary, byte-comparable sort key for a VARIANT value.
// It is used as a collation for VARIANT so that comparison / ordering / grouping all operate on the
// logical ordering of variant values (see EncodeVariantValue above).
// Unlike the create_sort_key scalar (which uses SPECIAL_HANDLING and encodes NULLs into the key), this
// function propagates NULL - this is required so that NULL = NULL stays NULL and ORDER BY ... NULLS
// FIRST/LAST is handled by the surrounding operator instead of being baked into the key.
void VariantComparatorFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);
	CreateVariantComparator(input.data[0], input.size(), result);
}

//===--------------------------------------------------------------------===//
// Statistics Propagation
//===--------------------------------------------------------------------===//
// When the input VARIANT is fully shredded onto a single primitive type, every (non-NULL) value lives
// in the same comparator "bucket" - all of its sort keys share the type-rank prefix - and the values
// are bounded by the typed min/max of the shredded column. Because the comparator encoding is
// order-preserving, encoding the typed min/max with the exact same encoding yields valid min/max sort
// keys for the BLOB output: min/max are derived by running the real comparator (CreateVariantComparator)
// over a cast of the bound to VARIANT, so the derived bounds can never diverge from the runtime encoding.
// A root that resolves to NULL is always a genuine SQL NULL (VARIANT_NULL is reserved for nested values),
// so its NULL is propagated via validity and never lands in the primitive bucket.

//! Encode a single bound value into its comparator sort key by reusing the exact comparator encoding.
bool TryEncodeBoundKey(ClientContext &context, const Value &bound, string &result_key) {
	if (bound.IsNull()) {
		return false;
	}
	auto variant_value = bound.TryCastAs(context, LogicalType::VARIANT());
	if (!variant_value) {
		return false;
	}
	if (variant_value->IsNull()) {
		return false;
	}
	Vector input(LogicalType::VARIANT(), 1);
	input.SetValue(0, *variant_value);
	Vector sort_key(LogicalType::BLOB, 1);
	CreateVariantComparator(input, 1, sort_key);
	auto key = sort_key.GetValue(0);
	if (key.IsNull()) {
		return false;
	}
	result_key = StringValue::Get(key);
	return true;
}

//! Extract the lower/upper bound of the (primitive) typed stats as Values - leaves a bound as NULL when
//! it is not available.
void GetTypedBounds(const BaseStatistics &typed_stats, Value &min_bound, Value &max_bound) {
	switch (typed_stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		if (NumericStats::HasMin(typed_stats)) {
			min_bound = NumericStats::Min(typed_stats);
		}
		if (NumericStats::HasMax(typed_stats)) {
			max_bound = NumericStats::Max(typed_stats);
		}
		break;
	case StatisticsType::STRING_STATS:
		// only VARCHAR has UTF-8 ordered min/max we can turn into valid lower/upper bounds
		if (typed_stats.GetType().id() == LogicalTypeId::VARCHAR) {
			if (StringStats::HasMin(typed_stats)) {
				min_bound = StringStats::TryGetValidMin(typed_stats);
			}
			if (StringStats::HasMax(typed_stats)) {
				max_bound = StringStats::TryGetValidMax(typed_stats);
			}
		}
		break;
	default:
		break;
	}
}

unique_ptr<BaseStatistics> VariantComparatorStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &variant_stats = input.child_stats[0];
	// Keep the (loosest) validity baseline - the BLOB sort key is NULL only when the input variant is a
	// SQL NULL, but the input validity is not propagated here: this code only narrows the value bounds,
	// and over-claiming "cannot be NULL" off of incomplete child stats would prune valid rows.
	auto result = BaseStatistics::CreateUnknown(input.expr.GetReturnType());

	if (variant_stats.GetStatsType() != StatisticsType::VARIANT_STATS || !VariantStats::IsShredded(variant_stats)) {
		return result.ToUnique();
	}
	auto &shredded_stats = VariantStats::GetShreddedStats(variant_stats);
	if (!VariantShreddedStats::IsFullyShredded(shredded_stats)) {
		// values can live in the unshredded component with arbitrary type ranks - no bucket
		return result.ToUnique();
	}
	auto &typed_stats = VariantStats::GetTypedStats(shredded_stats);
	if (typed_stats.GetType().IsNested()) {
		// only a primitive shredding maps every value into a single comparator bucket
		return result.ToUnique();
	}

	Value min_bound, max_bound;
	GetTypedBounds(typed_stats, min_bound, max_bound);

	string min_key;
	if (TryEncodeBoundKey(context, min_bound, min_key)) {
		StringStats::SetMin(result, string_t(min_key.data(), NumericCast<uint32_t>(min_key.size())),
		                    StringStatsType::TRUNCATED_STATS);
	}
	string max_key;
	if (TryEncodeBoundKey(context, max_bound, max_key)) {
		StringStats::SetMax(result, string_t(max_key.data(), NumericCast<uint32_t>(max_key.size())),
		                    StringStatsType::TRUNCATED_STATS);
	}
	return result.ToUnique();
}

} // namespace

ScalarFunction VariantComparatorFun::GetFunction() {
	auto variant_type = LogicalType::VARIANT();
	ScalarFunction function("variant_comparator", {}, LogicalType::BLOB, VariantComparatorFunction);
	function.GetSignature().AddParameter("input_variant", variant_type);
	function.SetStatisticsCallback(VariantComparatorStats);
	return function;
}

} // namespace duckdb
