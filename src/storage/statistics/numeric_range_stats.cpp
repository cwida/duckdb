#include "duckdb/storage/statistics/numeric_range_stats.hpp"

#include "duckdb/common/types/numeric_blob.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/storage/statistics/stats_struct_fields.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

NumericRangeStats &NumericRangeStats::Get(const BaseStatistics &stats) {
	D_ASSERT(stats.GetType().id() == LogicalTypeId::NUMERIC);
	D_ASSERT(stats.extra_data);
	return stats.extra_data->Cast<NumericRangeStats>();
}

void NumericRangeStats::Construct(BaseStatistics &stats) {
	stats.extra_data = make_uniq<NumericRangeStats>();
}

void NumericRangeStats::Set(BaseStatistics &stats, const NumericRange &range) {
	auto &range_stats = Get(stats);
	range_stats.range = range;
	range_stats.has_range = true;
}

optional_ptr<const NumericRange> NumericRangeStats::TryGet(const BaseStatistics &stats) {
	auto &range_stats = Get(stats);
	if (!range_stats.has_range) {
		return nullptr;
	}
	return &range_stats.range;
}

LogicalType NumericRangeStats::GetRepresentation(const BaseStatistics &stats) {
	auto range = TryGet(stats);
	if (!range) {
		return LogicalType::INVALID;
	}
	return range->ResolveType();
}

void NumericRangeStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	auto &range_stats = Get(stats);
	auto &other_stats = Get(other);
	range_stats.has_range = other_stats.has_range;
	range_stats.range = other_stats.range;
}

void NumericRangeStats::Merge(BaseStatistics &stats, const BaseStatistics &other) {
	auto &range_stats = Get(stats);
	auto other_range = TryGet(other);
	if (!range_stats.has_range || !other_range) {
		// either side has values of unknown representation
		range_stats.has_range = false;
		return;
	}
	range_stats.range.Merge(*other_range);
}

void NumericRangeStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto range = TryGet(stats);
	serializer.WritePropertyWithDefault<bool>(104, "has_numeric_range", range != nullptr, false);
	if (range) {
		serializer.WriteObject(105, "numeric_range", [&](Serializer &object) { range->Serialize(object); });
	}
}

void NumericRangeStats::Deserialize(Deserializer &deserializer, BaseStatistics &stats) {
	auto has_range = deserializer.ReadPropertyWithExplicitDefault<bool>(104, "has_numeric_range", false);
	if (!has_range) {
		Get(stats).has_range = false;
		return;
	}
	NumericRange range;
	deserializer.ReadObject(105, "numeric_range",
	                        [&](Deserializer &object) { range = NumericRange::Deserialize(object); });
	Set(stats, range);
}

void NumericRangeStats::ToStruct(const BaseStatistics &stats, child_list_t<Value> &children) {
	auto range = TryGet(stats);
	if (!range) {
		return;
	}
	auto representation = range->ResolveType();
	children.emplace_back("representation",
	                      Value(NumericType::IsExact(representation) ? representation.ToString() : "CANONICAL"));
	if (range->HasValues()) {
		// the endpoints keep the representation they were collected in (an INTEGER or a DECIMAL of the row group that
		// holds them, which depends on the order the row groups merge): print the number, not the representation
		children.emplace_back("exact_min", Value(NumericBlob::NormalizeText(range->Min().ToString())));
		children.emplace_back("exact_max", Value(NumericBlob::NormalizeText(range->Max().ToString())));
	}
}

//! The representation a statistics struct names: an integer type, a DECIMAL(width, scale), or CANONICAL for the
//! canonical storage type
static bool TryParseRepresentation(const string &text, LogicalType &result) {
	auto name = StringUtil::Upper(text);
	StringUtil::Trim(name);
	if (name == "CANONICAL") {
		result = NumericType::CanonicalStorageType();
		return true;
	}
	auto open = name.find('(');
	if (open == string::npos) {
		auto id = TransformStringToLogicalTypeId(name);
		if (id == LogicalTypeId::DECIMAL || !NumericType::IsExact(LogicalType(id))) {
			return false;
		}
		result = LogicalType(id);
		return true;
	}
	if (name.substr(0, open) != "DECIMAL" || name.back() != ')') {
		return false;
	}
	auto args = StringUtil::Split(name.substr(open + 1, name.size() - open - 2), ',');
	if (args.size() != 2) {
		return false;
	}
	// a width is at most 255: it fits a UTINYINT
	auto width = Value(args[0]).DefaultTryCastAs(LogicalType::UTINYINT, nullptr, true);
	auto scale = Value(args[1]).DefaultTryCastAs(LogicalType::UTINYINT, nullptr, true);
	if (!width || !scale || width->GetValue<uint8_t>() == 0 ||
	    scale->GetValue<uint8_t>() > width->GetValue<uint8_t>()) {
		return false;
	}
	result = LogicalType::DECIMAL(width->GetValue<uint8_t>(), scale->GetValue<uint8_t>());
	return true;
}

//! An endpoint of the exact range, written as a number or as its text
static bool TryGetEndpoint(StatsStructFields &fields, const string &name, Value &result) {
	Value text;
	if (!fields.TryGet(name, LogicalType::VARCHAR, text)) {
		return false;
	}
	if (!NumericType::TryParseLiteral(StringValue::Get(text), result)) {
		fields.Throw(name, StringUtil::Format("'%s' is not a number", StringValue::Get(text)));
	}
	return true;
}

//! The canonical blob of an endpoint must agree with a bound that is given, and stands in for an absent one
static void CheckBound(BaseStatistics &stats, StatsStructFields &fields, const string &name, const Value &endpoint,
                       bool is_min) {
	auto blob = NumericBlob::Encode(NumericBlob::NormalizeText(endpoint.ToString()));
	if (!(is_min ? StringStats::HasMin(stats) : StringStats::HasMax(stats))) {
		if (is_min) {
			StringStats::SetMin(stats, string_t(blob), StringStatsType::EXACT_STATS);
		} else {
			StringStats::SetMax(stats, string_t(blob), StringStatsType::EXACT_STATS);
		}
		return;
	}
	auto bound = is_min ? StringStats::Min(stats) : StringStats::Max(stats);
	auto bound_type = is_min ? StringStats::GetMinType(stats) : StringStats::GetMaxType(stats);
	auto comparison = StringStats::CompareStringStats(string_t(blob), string_t(bound), bound_type);
	if (is_min ? comparison < 0 : comparison > 0) {
		fields.Throw(name, StringUtil::Format("%s lies %s the bound '%s'", endpoint.ToString(),
		                                      is_min ? "below" : "above", is_min ? "min" : "max"));
	}
}

void NumericRangeStats::SetFromStruct(BaseStatistics &stats, StatsStructFields &fields) {
	Value field;
	LogicalType representation;
	auto has_representation = fields.TryGet("representation", LogicalType::VARCHAR, field);
	if (has_representation && !TryParseRepresentation(StringValue::Get(field), representation)) {
		fields.Throw("representation", StringUtil::Format("'%s' is not a representation of NUMERIC values: expected an "
		                                                  "integer type, a DECIMAL(width, scale) or CANONICAL",
		                                                  StringValue::Get(field)));
	}
	Value min, max;
	auto has_min = TryGetEndpoint(fields, "exact_min", min);
	auto has_max = TryGetEndpoint(fields, "exact_max", max);
	if (has_min != has_max) {
		fields.Throw(has_min ? "exact_max" : "exact_min", "exact_min and exact_max come together");
	}
	if (!has_min) {
		if (!has_representation) {
			// the range of the values stays unknown
			return;
		}
		if (StringStats::HasMin(stats) || StringStats::HasMax(stats)) {
			fields.Throw("representation", "statistics with bounds need exact_min and exact_max");
		}
		// no values: every representation holds them
		Set(stats, NumericRange());
		return;
	}
	if (has_representation && NumericType::IsExact(representation)) {
		// the endpoints take the representation, which must hold them and may carry more fractional places
		for (auto &endpoint : {std::make_pair("exact_min", &min), std::make_pair("exact_max", &max)}) {
			Value typed;
			if (!NumericType::TryExactCast(*endpoint.second, representation, typed)) {
				fields.Throw(endpoint.first,
				             StringUtil::Format("the representation %s does not hold %s", representation.ToString(),
				                                endpoint.second->ToString()));
			}
			*endpoint.second = std::move(typed);
		}
	}
	if (NumericRange::Compare(min, max) > 0) {
		fields.Throw("exact_max", StringUtil::Format("%s lies below exact_min %s", max.ToString(), min.ToString()));
	}
	NumericRange range;
	range.Update(min);
	range.Update(max);
	CheckBound(stats, fields, "exact_min", range.Min(), true);
	CheckBound(stats, fields, "exact_max", range.Max(), false);
	Set(stats, range);
}

} // namespace duckdb
