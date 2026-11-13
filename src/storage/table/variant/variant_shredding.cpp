#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/storage/table/variant_column_data.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant_visitor.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/function/variant/variant_normalize.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#ifdef DEBUG
#include "duckdb/common/value_operations/value_operations.hpp"
#endif

namespace duckdb {

namespace {

struct VariantStatsVisitor {
	using result_type = void;

	static void VisitNull(VariantShreddingStats &stats, idx_t stats_column_index) {
		return;
	}
	static void VisitBoolean(bool val, VariantShreddingStats &stats, idx_t stats_column_index) {
		return;
	}

	static void VisitMetadata(VariantLogicalType type_id, VariantShreddingStats &stats, idx_t stats_column_index) {
		auto &column_stats = stats.GetColumnStats(stats_column_index);
		column_stats.SetType(type_id);
	}

	template <typename T>
	static void VisitInteger(T val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitFloat(float val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitDouble(double val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitUUID(hugeint_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitDate(date_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitInterval(interval_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTime(dtime_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimeNanos(dtime_ns_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimeTZ(dtime_tz_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimestampSec(timestamp_sec_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimestampMs(timestamp_ms_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimestamp(timestamp_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimestampNanos(timestamp_ns_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimestampTZ(timestamp_tz_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitTimestampTZNanos(timestamp_tz_ns_t val, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void WriteStringInternal(const string_t &str, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitString(const string_t &str, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitBlob(const string_t &blob, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitBignum(const string_t &bignum, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitGeometry(const string_t &geom, VariantShreddingStats &stats, idx_t stats_column_index) {
	}
	static void VisitBitstring(const string_t &bits, VariantShreddingStats &stats, idx_t stats_column_index) {
	}

	template <typename T>
	static void VisitDecimal(T val, uint32_t width, uint32_t scale, VariantShreddingStats &stats,
	                         idx_t stats_column_index) {
		auto &column_stats = stats.GetColumnStats(stats_column_index);

		auto decimal_count = column_stats.type_counts[static_cast<uint8_t>(VariantLogicalType::DECIMAL)];
		D_ASSERT(decimal_count);
		//! Visit is called after VisitMetadata, so even for the first DECIMAL value, count will already be 1
		decimal_count--;

		if (!decimal_count) {
			column_stats.decimal_width = width;
			column_stats.decimal_scale = scale;
			column_stats.decimal_consistent = true;
			return;
		}

		if (!column_stats.decimal_consistent) {
			return;
		}

		if (width != column_stats.decimal_width || scale != column_stats.decimal_scale) {
			column_stats.decimal_consistent = false;
		}
	}

	static void VisitArray(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                       VariantShreddingStats &stats, idx_t stats_column_index) {
		auto &element_stats = stats.GetOrCreateElement(stats_column_index);
		auto index = element_stats.index;
		VariantVisitor<VariantStatsVisitor>::VisitArrayItems(variant, row, nested_data, stats, index);
	}

	static void VisitObject(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                        VariantShreddingStats &stats, idx_t stats_column_index) {
		//! Then visit the fields in sorted order
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto source_children_idx = nested_data.children_idx + i;

			//! Add the key of the field to the result
			auto keys_index = variant.GetKeysIndex(row, source_children_idx);
			auto &key = variant.GetKey(row, keys_index);

			auto &child_stats = stats.GetOrCreateField(stats_column_index, key.GetString());
			auto index = child_stats.index;

			//! Visit the child value
			auto values_index = variant.GetValuesIndex(row, source_children_idx);
			VariantVisitor<VariantStatsVisitor>::Visit(variant, row, values_index, stats, index);
		}
	}

	static void VisitDefault(VariantLogicalType type_id, const_data_ptr_t, VariantShreddingStats &stats,
	                         idx_t stats_column_index) {
		throw InternalException("VariantLogicalType(%s) not handled", EnumUtil::ToString(type_id));
	}
};

static unordered_set<VariantLogicalType> GetVariantType(const LogicalType &type) {
	if (type.id() == LogicalTypeId::ANY) {
		return {};
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		return {VariantLogicalType::OBJECT};
	case LogicalTypeId::LIST:
		return {VariantLogicalType::ARRAY};
	case LogicalTypeId::BOOLEAN:
		return {VariantLogicalType::BOOL_TRUE, VariantLogicalType::BOOL_FALSE};
	case LogicalTypeId::TINYINT:
		return {VariantLogicalType::INT8};
	case LogicalTypeId::SMALLINT:
		return {VariantLogicalType::INT16};
	case LogicalTypeId::INTEGER:
		return {VariantLogicalType::INT32};
	case LogicalTypeId::BIGINT:
		return {VariantLogicalType::INT64};
	case LogicalTypeId::HUGEINT:
		return {VariantLogicalType::INT128};
	case LogicalTypeId::UTINYINT:
		return {VariantLogicalType::UINT8};
	case LogicalTypeId::USMALLINT:
		return {VariantLogicalType::UINT16};
	case LogicalTypeId::UINTEGER:
		return {VariantLogicalType::UINT32};
	case LogicalTypeId::UBIGINT:
		return {VariantLogicalType::UINT64};
	case LogicalTypeId::UHUGEINT:
		return {VariantLogicalType::UINT128};
	case LogicalTypeId::FLOAT:
		return {VariantLogicalType::FLOAT};
	case LogicalTypeId::DOUBLE:
		return {VariantLogicalType::DOUBLE};
	case LogicalTypeId::DECIMAL:
		return {VariantLogicalType::DECIMAL};
	case LogicalTypeId::DATE:
		return {VariantLogicalType::DATE};
	case LogicalTypeId::TIME:
		return {VariantLogicalType::TIME_MICROS};
	case LogicalTypeId::TIME_TZ:
		return {VariantLogicalType::TIME_MICROS_TZ};
	case LogicalTypeId::TIMESTAMP_TZ:
		return {VariantLogicalType::TIMESTAMP_MICROS_TZ};
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return {VariantLogicalType::TIMESTAMP_NANOS_TZ};
	case LogicalTypeId::TIMESTAMP:
		return {VariantLogicalType::TIMESTAMP_MICROS};
	case LogicalTypeId::TIMESTAMP_SEC:
		return {VariantLogicalType::TIMESTAMP_SEC};
	case LogicalTypeId::TIMESTAMP_MS:
		return {VariantLogicalType::TIMESTAMP_MILIS};
	case LogicalTypeId::TIMESTAMP_NS:
		return {VariantLogicalType::TIMESTAMP_NANOS};
	case LogicalTypeId::BLOB:
		return {VariantLogicalType::BLOB};
	case LogicalTypeId::VARCHAR:
		return {VariantLogicalType::VARCHAR};
	case LogicalTypeId::UUID:
		return {VariantLogicalType::UUID};
	case LogicalTypeId::BIGNUM:
		return {VariantLogicalType::BIGNUM};
	case LogicalTypeId::TIME_NS:
		return {VariantLogicalType::TIME_NANOS};
	case LogicalTypeId::INTERVAL:
		return {VariantLogicalType::INTERVAL};
	case LogicalTypeId::BIT:
		return {VariantLogicalType::BITSTRING};
	case LogicalTypeId::GEOMETRY:
		return {VariantLogicalType::GEOMETRY};
	default:
		throw BinderException("Type '%s' can't be translated to a VARIANT type", type.ToString());
	}
}

struct DuckDBVariantShreddingState : public VariantShreddingState {
public:
	DuckDBVariantShreddingState(const LogicalType &type, idx_t total_count)
	    : VariantShreddingState(type, total_count), variant_types(GetVariantType(type)) {
	}
	~DuckDBVariantShreddingState() override {
	}

public:
	const unordered_set<VariantLogicalType> &GetVariantTypes() override {
		return variant_types;
	}

private:
	unordered_set<VariantLogicalType> variant_types;
};

struct UnshreddedValue {
public:
	UnshreddedValue(uint32_t value_index, uint32_t &target_value_index, vector<uint32_t> &&children = {})
	    : source_value_index(value_index), target_value_index(target_value_index),
	      unshredded_children(std::move(children)) {
	}

public:
	uint32_t source_value_index;
	uint32_t &target_value_index;
	vector<uint32_t> unshredded_children;
};

struct DuckDBVariantShredding : public VariantShredding {
public:
	explicit DuckDBVariantShredding(idx_t count) : VariantShredding(), unshredded_values(count) {
	}
	~DuckDBVariantShredding() override = default;

public:
	void WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result, optional_ptr<const SelectionVector> sel,
	                        optional_ptr<const SelectionVector> value_index_sel,
	                        optional_ptr<const SelectionVector> result_sel, idx_t count) override;
	void WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result, idx_t count);
	void AnalyzeVariantValues(UnifiedVariantVectorData &variant, optional_ptr<Vector> untyped_values,
	                          optional_ptr<const SelectionVector> sel,
	                          optional_ptr<const SelectionVector> value_index_sel,
	                          optional_ptr<const SelectionVector> result_sel,
	                          DuckDBVariantShreddingState &shredding_state, idx_t count);

public:
	//! For each row of the variant, the value_index(es) of the values to write to the 'unshredded' Vector
	vector<vector<UnshreddedValue>> unshredded_values;
};

} // namespace

void VariantColumnStatsData::SetType(VariantLogicalType type) {
	type_counts[static_cast<uint8_t>(type)]++;
	total_count++;
}

VariantColumnStatsData &VariantShreddingStats::GetOrCreateElement(idx_t parent_index) {
	auto &parent_column = GetColumnStats(parent_index);

	idx_t element_stats = parent_column.element_stats;
	if (parent_column.element_stats == DConstants::INVALID_INDEX) {
		parent_column.element_stats = columns.size();
		element_stats = parent_column.element_stats;
		columns.emplace_back(element_stats);
	}
	return GetColumnStats(element_stats);
}

VariantColumnStatsData &VariantShreddingStats::GetOrCreateField(idx_t parent_index, const string &name) {
	auto &parent_column = columns[parent_index];
	auto it = parent_column.field_stats.find(name);

	idx_t field_stats;
	if (it == parent_column.field_stats.end()) {
		it = parent_column.field_stats.emplace(name, columns.size()).first;
		field_stats = it->second;
		columns.emplace_back(field_stats);
	} else {
		field_stats = it->second;
	}
	return GetColumnStats(field_stats);
}

VariantColumnStatsData &VariantShreddingStats::GetColumnStats(idx_t index) {
	D_ASSERT(columns.size() > index);
	return columns[index];
}

const VariantColumnStatsData &VariantShreddingStats::GetColumnStats(idx_t index) const {
	D_ASSERT(columns.size() > index);
	return columns[index];
}

static LogicalType ProduceShreddedType(VariantLogicalType type_id) {
	switch (type_id) {
	case VariantLogicalType::BOOL_TRUE:
	case VariantLogicalType::BOOL_FALSE:
		return LogicalTypeId::BOOLEAN;
	case VariantLogicalType::INT8:
		return LogicalTypeId::TINYINT;
	case VariantLogicalType::INT16:
		return LogicalTypeId::SMALLINT;
	case VariantLogicalType::INT32:
		return LogicalTypeId::INTEGER;
	case VariantLogicalType::INT64:
		return LogicalTypeId::BIGINT;
	case VariantLogicalType::INT128:
		return LogicalTypeId::HUGEINT;
	case VariantLogicalType::UINT8:
		return LogicalTypeId::UTINYINT;
	case VariantLogicalType::UINT16:
		return LogicalTypeId::USMALLINT;
	case VariantLogicalType::UINT32:
		return LogicalTypeId::UINTEGER;
	case VariantLogicalType::UINT64:
		return LogicalTypeId::UBIGINT;
	case VariantLogicalType::UINT128:
		return LogicalTypeId::UHUGEINT;
	case VariantLogicalType::FLOAT:
		return LogicalTypeId::FLOAT;
	case VariantLogicalType::DOUBLE:
		return LogicalTypeId::DOUBLE;
	case VariantLogicalType::DECIMAL:
		throw InternalException("Can't shred on DECIMAL");
	case VariantLogicalType::VARCHAR:
		return LogicalTypeId::VARCHAR;
	case VariantLogicalType::BLOB:
		return LogicalTypeId::BLOB;
	case VariantLogicalType::UUID:
		return LogicalTypeId::UUID;
	case VariantLogicalType::DATE:
		return LogicalTypeId::DATE;
	case VariantLogicalType::TIME_MICROS:
		return LogicalTypeId::TIME;
	case VariantLogicalType::TIME_NANOS:
		return LogicalTypeId::TIME_NS;
	case VariantLogicalType::TIMESTAMP_SEC:
		return LogicalTypeId::TIMESTAMP_SEC;
	case VariantLogicalType::TIMESTAMP_MILIS:
		return LogicalTypeId::TIMESTAMP_MS;
	case VariantLogicalType::TIMESTAMP_MICROS:
		return LogicalTypeId::TIMESTAMP;
	case VariantLogicalType::TIMESTAMP_NANOS:
		return LogicalTypeId::TIMESTAMP_NS;
	case VariantLogicalType::TIME_MICROS_TZ:
		return LogicalTypeId::TIME_TZ;
	case VariantLogicalType::TIMESTAMP_MICROS_TZ:
		return LogicalTypeId::TIMESTAMP_TZ;
	case VariantLogicalType::TIMESTAMP_NANOS_TZ:
		return LogicalTypeId::TIMESTAMP_TZ_NS;
	case VariantLogicalType::INTERVAL:
		return LogicalTypeId::INTERVAL;
	case VariantLogicalType::BIGNUM:
		return LogicalTypeId::BIGNUM;
	case VariantLogicalType::BITSTRING:
		return LogicalTypeId::BIT;
	case VariantLogicalType::GEOMETRY:
		return LogicalTypeId::GEOMETRY;
	case VariantLogicalType::OBJECT:
	case VariantLogicalType::ARRAY:
		throw InternalException("Already handled above");
	default:
		throw NotImplementedException("Shredding on VariantLogicalType::%s not supported yet",
		                              EnumUtil::ToString(type_id));
	}
}

static bool CanFlattenShreddedType(const LogicalType &type) {
	if (type.IsNested()) {
		// cannot flatten nested types
		return false;
	}
	return true;
}

static LogicalType SetShreddedType(const LogicalType &typed_value, bool fully_consistent) {
	if (fully_consistent && CanFlattenShreddedType(typed_value)) {
		// fully consistent and this is a primitive type - we can flatten the type entirely
		return typed_value;
	}
	child_list_t<LogicalType> child_types;
	child_types.emplace_back("typed_value", typed_value);
	if (!fully_consistent) {
		child_types.emplace_back("untyped_value_index", LogicalType::UINTEGER);
	}
	return LogicalType::STRUCT(child_types);
}

bool VariantShreddingStats::GetShreddedTypeInternal(const VariantColumnStatsData &column, LogicalType &out_type,
                                                    optional_idx parent_count, bool force_partial) const {
	if (parent_count.IsValid() && column.total_count > parent_count.GetIndex()) {
		throw InternalException("Column count is larger than parent count - this should not be possible");
	}
	auto total_value_count = parent_count.IsValid() ? parent_count.GetIndex() : column.total_count;
	const auto null_count = column.type_counts[0];
	if (null_count == column.total_count) {
		//! All NULL, emit INT32
		auto fully_consistent = !force_partial && null_count == total_value_count;
		out_type = SetShreddedType(LogicalTypeId::INTEGER, fully_consistent);
		return true;
	}

	idx_t max_count = 0;
	uint8_t type_index = 0;
	//! Skip the 'VARIANT_NULL' type, we can't shred on NULL
	for (uint8_t i = 1; i < static_cast<uint8_t>(VariantLogicalType::ENUM_SIZE); i++) {
		if (i == static_cast<uint8_t>(VariantLogicalType::DECIMAL) && !column.decimal_consistent) {
			//! Can't shred on DECIMAL, not consistent
			continue;
		}
		idx_t count = column.type_counts[i] + null_count;
		if (!max_count || count > max_count) {
			max_count = count;
			type_index = i;
		}
	}

	if (!max_count) {
		return false;
	}

	bool fully_consistent = !force_partial && max_count == total_value_count;
	if (type_index == static_cast<uint8_t>(VariantLogicalType::OBJECT)) {
		child_list_t<LogicalType> child_types;
		case_insensitive_string_set_t field_names;
		for (auto &entry : column.field_stats) {
			auto &child_column = GetColumnStats(entry.second);
			if (entry.first.empty()) {
				//! Do not include empty field names in the shredded type!
				continue;
			}
			if (!field_names.emplace(entry.first).second) {
				//! STRUCT field names are case-insensitive, but VARIANT object keys are case-sensitive.
				return false;
			}
			LogicalType child_type;
			if (GetShreddedTypeInternal(child_column, child_type, total_value_count, force_partial)) {
				child_types.emplace_back(entry.first, child_type);
			}
		}
		if (child_types.empty()) {
			return false;
		}
		// always set objects as not being fully consistent
		fully_consistent = false;
		auto shredded_type = LogicalType::STRUCT(child_types);
		out_type = SetShreddedType(shredded_type, fully_consistent);
		return true;
	}
	if (type_index == static_cast<uint8_t>(VariantLogicalType::ARRAY)) {
		D_ASSERT(column.element_stats != DConstants::INVALID_INDEX);
		auto &element_column = GetColumnStats(column.element_stats);
		LogicalType element_type;
		if (!GetShreddedTypeInternal(element_column, element_type, optional_idx(), force_partial)) {
			return false;
		}
		auto shredded_type = LogicalType::LIST(element_type);
		out_type = SetShreddedType(shredded_type, fully_consistent);
		return true;
	}
	if (type_index == static_cast<uint8_t>(VariantLogicalType::DECIMAL)) {
		auto shredded_type = LogicalType::DECIMAL(static_cast<uint8_t>(column.decimal_width),
		                                          static_cast<uint8_t>(column.decimal_scale));
		out_type = SetShreddedType(shredded_type, fully_consistent);
		return true;
	}
	auto type_id = static_cast<VariantLogicalType>(type_index);

	auto shredded_type = ProduceShreddedType(type_id);
	out_type = SetShreddedType(shredded_type, fully_consistent);
	return true;
}

LogicalType VariantShreddingStats::GetShreddedType(bool force_partial) const {
	auto &root_column = GetColumnStats(0);

	child_list_t<LogicalType> child_types;
	child_types.emplace_back("unshredded", VariantShredding::GetUnshreddedType());
	LogicalType shredded_type;
	if (GetShreddedTypeInternal(root_column, shredded_type, optional_idx(), force_partial)) {
		child_types.emplace_back("shredded", shredded_type);
	}
	return LogicalType::STRUCT(child_types);
}

void VariantShreddingStats::Update(const Vector &input, idx_t count) {
	RecursiveUnifiedVectorFormat recursive_format;
	Vector::RecursiveToUnifiedFormat(input, recursive_format);
	UnifiedVariantVectorData variant(recursive_format);

	for (idx_t i = 0; i < count; i++) {
		VariantVisitor<VariantStatsVisitor>::Visit(variant, i, 0, *this, static_cast<idx_t>(0));
	}
}

static void VisitObject(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
                        VariantNormalizerState &state, const vector<uint32_t> &child_indices) {
	D_ASSERT(child_indices.size() <= nested_data.child_count);
	//! First iterate through all fields to populate the map of key -> field
	map<string, idx_t> sorted_fields;
	for (auto &child_idx : child_indices) {
		auto keys_index = variant.GetKeysIndex(row, nested_data.children_idx + child_idx);
		auto &key = variant.GetKey(row, keys_index);
		sorted_fields.emplace(key, child_idx);
	}

	state.blob_size += VarintEncode(sorted_fields.size(), state.GetDestination());
	D_ASSERT(!sorted_fields.empty());

	uint32_t children_idx = state.children_size;
	uint32_t keys_idx = state.keys_size;
	state.blob_size += VarintEncode(children_idx, state.GetDestination());
	state.children_size += sorted_fields.size();
	state.keys_size += sorted_fields.size();

	//! Then visit the fields in sorted order
	for (auto &entry : sorted_fields) {
		auto source_children_idx = nested_data.children_idx + entry.second;

		//! Add the key of the field to the result
		auto keys_index = variant.GetKeysIndex(row, source_children_idx);
		auto &key = variant.GetKey(row, keys_index);
		auto dict_index = state.GetOrCreateIndex(key);
		state.keys_selvec.set_index(state.keys_offset + keys_idx, dict_index);

		//! Visit the child value
		auto values_index = variant.GetValuesIndex(row, source_children_idx);
		state.values_indexes[children_idx] = state.values_size;
		state.keys_indexes[children_idx] = keys_idx;
		children_idx++;
		keys_idx++;
		VariantVisitor<VariantNormalizer>::Visit(variant, row, values_index, state);
	}
}

static vector<uint32_t> UnshreddedObjectChildren(UnifiedVariantVectorData &variant, uint32_t row, uint32_t value_index,
                                                 DuckDBVariantShreddingState &shredding_state) {
	auto nested_data = VariantUtils::DecodeNestedData(variant, row, value_index);

	auto shredded_fields = shredding_state.ObjectFields();
	vector<uint32_t> unshredded_children;
	unshredded_children.reserve(nested_data.child_count);
	for (uint32_t i = 0; i < nested_data.child_count; i++) {
		auto keys_index = variant.GetKeysIndex(row, nested_data.children_idx + i);
		auto &key = variant.GetKey(row, keys_index);
		if (shredded_fields.count(key.GetString())) {
			continue;
		}
		unshredded_children.emplace_back(i);
	}
	return unshredded_children;
}

//! ~~Write the unshredded values~~, also receiving the 'untyped_value_index' Vector to populate
//! Marking the rows that are shredded in the shredding state
void DuckDBVariantShredding::AnalyzeVariantValues(UnifiedVariantVectorData &variant,
                                                  optional_ptr<Vector> untyped_values,
                                                  optional_ptr<const SelectionVector> sel,
                                                  optional_ptr<const SelectionVector> value_index_sel,
                                                  optional_ptr<const SelectionVector> result_sel,
                                                  DuckDBVariantShreddingState &shredding_state, idx_t count) {
	//
	// auto &validity = FlatVector::ValidityMutable(value);
	uint32_t *untyped_data = nullptr;
	if (untyped_values) {
		untyped_data = FlatVector::GetDataMutable<uint32_t>(*untyped_values);
	}

	for (uint32_t i = 0; i < static_cast<uint32_t>(count); i++) {
		uint32_t value_index = 0;
		if (value_index_sel) {
			value_index = static_cast<uint32_t>(value_index_sel->get_index(i));
		}

		uint32_t row = i;
		if (sel) {
			row = static_cast<uint32_t>(sel->get_index(i));
		}

		uint32_t result_index = i;
		if (result_sel) {
			result_index = static_cast<uint32_t>(result_sel->get_index(i));
		}

		if (variant.RowIsValid(row) && shredding_state.ValueIsShredded(variant, row, value_index)) {
			shredding_state.SetShredded(row, value_index, result_index);
			if (shredding_state.type.id() != LogicalTypeId::STRUCT) {
				//! Value is shredded, directly write a `NULL` to the 'value' if the type is not an OBJECT
				if (untyped_values) {
					FlatVector::ValidityMutable(*untyped_values).SetInvalid(result_index);
				}
				continue;
			}

			//! When the type is OBJECT, all excess fields would still need to be written to the 'value'
			auto unshredded_children = UnshreddedObjectChildren(variant, row, value_index, shredding_state);
			if (unshredded_children.empty()) {
				//! Fully shredded object
				if (untyped_values) {
					FlatVector::ValidityMutable(*untyped_values).SetInvalid(result_index);
				}
			} else {
				//! Deal with partially shredded objects
				if (!untyped_data) {
					throw InvalidInputException(
					    "Failed to shred variant value - untyped_value was not set but inconsistent values were found");
				}
				unshredded_values[row].emplace_back(value_index, untyped_data[result_index],
				                                    std::move(unshredded_children));
			}
			continue;
		}

		//! Deal with unshredded values
		if (!variant.RowIsValid(row) || variant.GetTypeId(row, value_index) == VariantLogicalType::VARIANT_NULL) {
			//! NULL is reserved for NULL Variant values
			if (untyped_values) {
				FlatVector::ValidityMutable(*untyped_values).SetInvalid(result_index);
			}
		} else {
			if (!untyped_data) {
				throw InvalidInputException(
				    "Failed to shred variant value - untyped_value was not set but inconsistent values were found");
			}
			unshredded_values[row].emplace_back(value_index, untyped_data[result_index]);
		}
	}
}

//! Receive a 'shredded' result Vector, consisting of the 'typed_value' and the 'untyped_value_index' Vector
void DuckDBVariantShredding::WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result,
                                                optional_ptr<const SelectionVector> sel,
                                                optional_ptr<const SelectionVector> value_index_sel,
                                                optional_ptr<const SelectionVector> result_sel, idx_t count) {
	reference<Vector> typed_value_ref(result);
	optional_ptr<Vector> untyped_value_index;
	if (result.GetType().id() == LogicalTypeId::STRUCT) {
		// "typed_value", "untyped_value"
		auto &child_vectors = StructVector::GetEntries(result);
#ifdef D_ASSERT_IS_ENABLED
		auto &result_type = result.GetType();
		D_ASSERT(result_type.id() == LogicalTypeId::STRUCT);
		auto &child_types = StructType::GetChildTypes(result_type);
		D_ASSERT(child_types.size() == child_vectors.size());
#endif
		typed_value_ref = child_vectors[VariantColumnData::TYPED_VALUE_INDEX];
		if (child_vectors.size() > 1) {
			D_ASSERT(child_vectors.size() == 2);
			untyped_value_index = child_vectors[VariantColumnData::UNTYPED_VALUE_INDEX];
		}
	}
	auto &typed_value = typed_value_ref.get();

	DuckDBVariantShreddingState shredding_state(typed_value.GetType(), count);
	AnalyzeVariantValues(variant, untyped_value_index, sel, value_index_sel, result_sel, shredding_state, count);

	SelectionVector null_values;
	if (shredding_state.count) {
		WriteTypedValues(variant, typed_value, shredding_state.shredded_sel, shredding_state.values_index_sel,
		                 shredding_state.result_sel, shredding_state.count);
		//! Set the rows that aren't shredded to NULL
		idx_t sel_idx = 0;
		for (idx_t i = 0; i < count; i++) {
			auto original_index = result_sel ? result_sel->get_index(i) : i;
			if (sel_idx < shredding_state.count && shredding_state.result_sel[sel_idx] == original_index) {
				sel_idx++;
				continue;
			}
			FlatVector::SetNull(typed_value, original_index, true);
		}
	} else {
		//! Set all rows of the typed_value to NULL, nothing is shredded on
		for (idx_t i = 0; i < count; i++) {
			FlatVector::SetNull(typed_value, result_sel ? result_sel->get_index(i) : i, true);
		}
	}
}

void DuckDBVariantShredding::WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result, idx_t count) {
	// write the top-level variant values
	WriteVariantValues(variant, result, nullptr, nullptr, nullptr, count);
	//! Propagate NULL values from the top-level input variant to the shredded child struct
	for (idx_t row = 0; row < count; row++) {
		if (!variant.RowIsValid(row)) {
			FlatVector::SetNull(result, row, true);
		}
	}
}

void VariantShredding::ShredWithFallback(const Vector &input, Vector &output, idx_t count) {
	RecursiveUnifiedVectorFormat recursive_format;
	Vector::RecursiveToUnifiedFormat(input, recursive_format);
	UnifiedVariantVectorData variant(recursive_format);

	auto &child_vectors = StructVector::GetEntries(output);

	//! First traverse the Variant to write the shredded values and collect the 'untyped_value_index'es
	DuckDBVariantShredding shredding(count);
	shredding.WriteVariantValues(variant, child_vectors[1], count);

	//! Now we can write the unshredded values
	auto &unshredded = child_vectors[0];
	auto original_keys_size = ListVector::GetListSize(VariantVector::GetKeys(input));
	auto original_children_size = ListVector::GetListSize(VariantVector::GetChildren(input));
	auto original_values_size = ListVector::GetListSize(VariantVector::GetValues(input));

	auto &keys = VariantVector::GetKeys(unshredded);
	auto &children = VariantVector::GetChildren(unshredded);
	auto &values = VariantVector::GetValues(unshredded);
	auto &data = VariantVector::GetData(unshredded);

	ListVector::Reserve(keys, original_keys_size);
	ListVector::SetListSize(keys, 0);
	ListVector::Reserve(children, original_children_size);
	ListVector::SetListSize(children, 0);
	ListVector::Reserve(values, original_values_size);
	ListVector::SetListSize(values, 0);

	auto &keys_entry = ListVector::GetChildMutable(keys);
	OrderedOwningStringMap<uint32_t> dictionary(StringVector::GetStringAllocator(keys_entry));
	SelectionVector keys_selvec;
	keys_selvec.Initialize(original_keys_size);

	VariantVectorData variant_data(unshredded);
	for (idx_t row = 0; row < count; row++) {
		auto &unshredded_values = shredding.unshredded_values[row];

		if (unshredded_values.empty()) {
			FlatVector::SetNull(unshredded, row, true);
			continue;
		}

		//! Allocate for the new data, use the same size as source
		auto &blob_data = variant_data.blob_data[row];
		auto original_data = variant.GetData(row);
		blob_data = StringVector::EmptyString(data, original_data.GetSize());

		auto &keys_list_entry = variant_data.keys_data[row];
		keys_list_entry.offset = ListVector::GetListSize(keys);

		auto &children_list_entry = variant_data.children_data[row];
		children_list_entry.offset = ListVector::GetListSize(children);

		auto &values_list_entry = variant_data.values_data[row];
		values_list_entry.offset = ListVector::GetListSize(values);

		VariantNormalizerState normalizer_state(row, variant_data, dictionary, keys_selvec);
		for (idx_t i = 0; i < unshredded_values.size(); i++) {
			auto &unshredded_value = unshredded_values[i];
			auto value_index = unshredded_value.source_value_index;

			unshredded_value.target_value_index = normalizer_state.values_size + 1;
			if (!unshredded_value.unshredded_children.empty()) {
				D_ASSERT(variant.GetTypeId(row, value_index) == VariantLogicalType::OBJECT);
				auto nested_data = VariantUtils::DecodeNestedData(variant, row, value_index);

				normalizer_state.type_ids[normalizer_state.values_size] =
				    static_cast<uint8_t>(VariantLogicalType::OBJECT);
				normalizer_state.byte_offsets[normalizer_state.values_size] = normalizer_state.blob_size;
				normalizer_state.values_size++;
				VisitObject(variant, row, nested_data, normalizer_state, unshredded_value.unshredded_children);
				continue;
			}
			VariantVisitor<VariantNormalizer>::Visit(variant, row, value_index, normalizer_state);
		}
		blob_data.SetSizeAndFinalize(normalizer_state.blob_size, original_data.GetSize());
		keys_list_entry.length = normalizer_state.keys_size;
		children_list_entry.length = normalizer_state.children_size;
		values_list_entry.length = normalizer_state.values_size;

		ListVector::SetListSize(keys, ListVector::GetListSize(keys) + normalizer_state.keys_size);
		ListVector::SetListSize(children, ListVector::GetListSize(children) + normalizer_state.children_size);
		ListVector::SetListSize(values, ListVector::GetListSize(values) + normalizer_state.values_size);
	}

	VariantUtils::FinalizeVariantKeys(unshredded, dictionary, keys_selvec, ListVector::GetListSize(keys));
	keys_entry.Slice(keys_selvec, ListVector::GetListSize(keys));

	if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		unshredded.SetVectorType(VectorType::CONSTANT_VECTOR);
		FlatVector::SetSize(unshredded, count_t(count));
	}

#ifdef DEBUG
	Vector roundtrip_result(LogicalType::VARIANT(), count);
	VariantUtils::UnshredVariantData(output, roundtrip_result, count);

	for (idx_t i = 0; i < count; i++) {
		auto input_val = input.GetValue(i);
		auto roundtripped_val = roundtrip_result.GetValue(i);

		Vector input_vec(input_val, count_t(1));
		Vector roundtripped_vec(roundtripped_val, count_t(1));

		Vector normalized_input(LogicalType::VARIANT(), 1);
		Vector normalized_roundtrip(LogicalType::VARIANT(), 1);
		VariantNormalizer::Normalize(input_vec, normalized_input);
		VariantNormalizer::Normalize(roundtripped_vec, normalized_roundtrip);

		auto normalized_input_value = normalized_input.GetValue(0);
		auto normalized_roundtrip_value = normalized_roundtrip.GetValue(0);
		if (!ValueOperations::NotDistinctFrom(normalized_input_value, normalized_roundtrip_value)) {
			throw InternalException("Shredding roundtrip verification failed for row: %d, expected: %s, actual: %s", i,
			                        input_val.ToString(), roundtripped_val.ToString());
		}
	}

#endif
}

void VariantColumnData::ShredVariantData(const Vector &input, Vector &output, idx_t count) {
	VariantShredding::ShredWithFallback(input, output, count);
}

void VariantColumnData::DebugShred(Vector &variant, idx_t count) {
	D_ASSERT(variant.GetType().id() == LogicalTypeId::VARIANT);
	if (count == 0 || variant.GetVectorType() == VectorType::SHREDDED_VECTOR) {
		//! nothing to do (already shredded, or empty)
		return;
	}

	Vector materialized(LogicalType::VARIANT(), count);
	VectorOperations::Copy(variant, materialized, count, 0, 0);
	FlatVector::SetSize(materialized, count);
	variant.Reference(materialized);

	//! Derive the shredding schema from the *first* value only - subsequent values that don't match it
	//! will be partially shredded (i.e. fall back to the unshredded/overlay component)
	VariantShreddingStats stats;
	stats.Update(materialized, 1);
	//! force_partial keeps the overlay columns so that later values that don't match the first value's
	//! schema are partially shredded instead of failing to shred
	auto shredded_struct_type = stats.GetShreddedType(true);
	if (StructType::GetChildCount(shredded_struct_type) < 2) {
		//! the first value did not yield a shreddable type (only the 'unshredded' component) - leave as-is
		return;
	}

	//! Shred into a STRUCT(unshredded, shredded) and wrap it as a SHREDDED_VECTOR
	Vector shredded_struct(shredded_struct_type, count);
	ShredVariantData(variant, shredded_struct, count);
	FlatVector::SetSize(shredded_struct, count_t(count));
	variant.Shred(shredded_struct, count);
}

} // namespace duckdb
