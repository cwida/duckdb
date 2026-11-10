#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/stats_struct_fields.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

vector<string> GeometryTypeSet::ToString(bool snake_case) const {
	vector<string> result;
	for (auto d = 0; d < VERT_TYPES; d++) {
		for (auto i = 0; i < PART_TYPES; i++) {
			if (sets[d] & (1 << i)) {
				string str;
				switch (i) {
				case 1:
					str = snake_case ? "point" : "Point";
					break;
				case 2:
					str = snake_case ? "linestring" : "LineString";
					break;
				case 3:
					str = snake_case ? "polygon" : "Polygon";
					break;
				case 4:
					str = snake_case ? "multipoint" : "MultiPoint";
					break;
				case 5:
					str = snake_case ? "multilinestring" : "MultiLineString";
					break;
				case 6:
					str = snake_case ? "multipolygon" : "MultiPolygon";
					break;
				case 7:
					str = snake_case ? "geometrycollection" : "GeometryCollection";
					break;
				default:
					str = snake_case ? "unknown" : "Unknown";
					break;
				}
				switch (d) {
				case 1:
					str += snake_case ? "_z" : " Z";
					break;
				case 2:
					str += snake_case ? "_m" : " M";
					break;
				case 3:
					str += snake_case ? "_zm" : " ZM";
					break;
				default:
					break;
				}

				result.push_back(str);
			}
		}
	}
	return result;
}

bool GeometryTypeSet::TryAdd(const string &type_name) {
	auto names = Unknown().ToString(true);
	const auto idx = StringUtil::CIFind(names, type_name);
	if (idx == DConstants::INVALID_INDEX) {
		return false;
	}
	Add(static_cast<GeometryType>(idx % PART_TYPES), static_cast<VertexType>(idx / PART_TYPES));
	return true;
}

BaseStatistics GeometryStats::CreateUnknown(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	GetDataUnsafe(result).SetUnknown();
	return result;
}

BaseStatistics GeometryStats::CreateEmpty(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	GetDataUnsafe(result).SetEmpty();
	return result;
}

void GeometryStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	// Should we serialize as old extension geometry type for backwards compatibility?
	// (in that case, write unknown string stats)
	if (!serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
		auto string_stats = StringStats::CreateUnknown(LogicalType::VARCHAR);
		StringStats::Serialize(string_stats, serializer);
		return;
	}

	const auto &data = GetDataUnsafe(stats);

	// Write extent
	serializer.WriteObject(300, "extent", [&](Serializer &extent) {
		extent.WriteProperty<double>(101, "x_min", data.extent.x_min);
		extent.WriteProperty<double>(102, "x_max", data.extent.x_max);
		extent.WriteProperty<double>(103, "y_min", data.extent.y_min);
		extent.WriteProperty<double>(104, "y_max", data.extent.y_max);
		extent.WriteProperty<double>(105, "z_min", data.extent.z_min);
		extent.WriteProperty<double>(106, "z_max", data.extent.z_max);
		extent.WriteProperty<double>(107, "m_min", data.extent.m_min);
		extent.WriteProperty<double>(108, "m_max", data.extent.m_max);
	});

	// Write types
	serializer.WriteObject(301, "types", [&](Serializer &types) {
		types.WriteProperty<uint8_t>(101, "types_xy", data.types.sets[0]);
		types.WriteProperty<uint8_t>(102, "types_xyz", data.types.sets[1]);
		types.WriteProperty<uint8_t>(103, "types_xym", data.types.sets[2]);
		types.WriteProperty<uint8_t>(104, "types_xyzm", data.types.sets[3]);
	});

	// Write flags
	serializer.WritePropertyWithDefault(302, "flags", data.flags.flags);
}

void GeometryStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	auto &data = GetDataUnsafe(base);

	// Read old garbage string stats if present, but ignore it since it is not relevant to geometry stats
	if (deserializer.CanDeserializeProperty(200, "min")) {
		auto string_stats = StringStats::CreateEmpty(LogicalType::VARCHAR);
		StringStats::Deserialize(deserializer, string_stats);

		// We don't know how to interpret the old string stats, so we just set the geometry stats to unknown
		data.extent = GeometryExtent::Unknown();
		data.types = GeometryTypeSet::Unknown();
		data.flags = GeometryStatsFlags::Unknown();
		return;
	}

	// Read extent
	deserializer.ReadObject(300, "extent", [&](Deserializer &extent) {
		extent.ReadProperty<double>(101, "x_min", data.extent.x_min);
		extent.ReadProperty<double>(102, "x_max", data.extent.x_max);
		extent.ReadProperty<double>(103, "y_min", data.extent.y_min);
		extent.ReadProperty<double>(104, "y_max", data.extent.y_max);
		extent.ReadProperty<double>(105, "z_min", data.extent.z_min);
		extent.ReadProperty<double>(106, "z_max", data.extent.z_max);
		extent.ReadProperty<double>(107, "m_min", data.extent.m_min);
		extent.ReadProperty<double>(108, "m_max", data.extent.m_max);
	});

	// Read types
	deserializer.ReadObject(301, "types", [&](Deserializer &types) {
		types.ReadProperty<uint8_t>(101, "types_xy", data.types.sets[0]);
		types.ReadProperty<uint8_t>(102, "types_xyz", data.types.sets[1]);
		types.ReadProperty<uint8_t>(103, "types_xym", data.types.sets[2]);
		types.ReadProperty<uint8_t>(104, "types_xyzm", data.types.sets[3]);
	});

	// Read flags
	deserializer.ReadPropertyWithDefault<uint8_t>(302, "flags", data.flags.flags);
}

child_list_t<Value> GeometryStats::ToStruct(const BaseStatistics &stats, bool exact) {
	const auto &data = GetDataUnsafe(stats);
	child_list_t<Value> result;
	child_list_t<Value> extent;

	extent.emplace_back("x_min", Value::DOUBLE(data.extent.x_min));
	extent.emplace_back("x_max", Value::DOUBLE(data.extent.x_max));
	extent.emplace_back("y_min", Value::DOUBLE(data.extent.y_min));
	extent.emplace_back("y_max", Value::DOUBLE(data.extent.y_max));
	if (Value::IsFinite(data.extent.z_min) || Value::IsFinite(data.extent.z_max)) {
		extent.emplace_back("z_min", Value::DOUBLE(data.extent.z_min));
		extent.emplace_back("z_max", Value::DOUBLE(data.extent.z_max));
	}
	if (Value::IsFinite(data.extent.m_min) || Value::IsFinite(data.extent.m_max)) {
		extent.emplace_back("m_min", Value::DOUBLE(data.extent.m_min));
		extent.emplace_back("m_max", Value::DOUBLE(data.extent.m_max));
	}

	result.emplace_back("extent", Value::STRUCT(std::move(extent)));

	// the types the column holds are not part of the rendering stats() shows
	if (exact && !data.types.IsUnknown()) {
		vector<Value> types;
		for (auto &name : data.types.ToString(true)) {
			types.emplace_back(name);
		}
		result.emplace_back("types", Value::LIST(LogicalType::VARCHAR, std::move(types)));
	}

	result.emplace_back("has_empty_geom", Value::BOOLEAN(data.flags.HasEmptyGeometry()));
	result.emplace_back("has_non_empty_geom", Value::BOOLEAN(data.flags.HasNonEmptyGeometry()));
	result.emplace_back("has_empty_part", Value::BOOLEAN(data.flags.HasEmptyPart()));
	result.emplace_back("has_non_empty_part", Value::BOOLEAN(data.flags.HasNonEmptyPart()));
	return result;
}

//! A bound of the extent, by the field name ToStruct gives it
struct GeometryExtentBound {
	const char *name;
	double &bound;
};

//! A flag of the statistics, by the field name ToStruct gives it
struct GeometryStatsFlag {
	const char *name;
	bool &value;
};

void GeometryStats::SetFromStruct(BaseStatistics &stats, StatsStructFields &fields) {
	auto &data = GetDataUnsafe(stats);
	Value field;

	auto extent = fields.Get("extent");
	if (extent) {
		StatsStructFields bounds(*extent, fields.FieldPath("extent"));
		const GeometryExtentBound axes[] = {{"x_min", data.extent.x_min}, {"x_max", data.extent.x_max},
		                                    {"y_min", data.extent.y_min}, {"y_max", data.extent.y_max},
		                                    {"z_min", data.extent.z_min}, {"z_max", data.extent.z_max},
		                                    {"m_min", data.extent.m_min}, {"m_max", data.extent.m_max}};
		for (auto &axis : axes) {
			if (bounds.TryGet(axis.name, LogicalType::DOUBLE, field)) {
				axis.bound = DoubleValue::Get(field);
			}
		}
		bounds.VerifyAllRead(stats.GetType());
	}

	auto types = fields.Get("types");
	if (types) {
		if (types->type().id() != LogicalTypeId::LIST) {
			fields.Throw("types", "expected a list of geometry type names, e.g. ['point', 'polygon_z']");
		}
		auto type_set = GeometryTypeSet::Empty();
		for (auto &entry : ListValue::GetChildren(*types)) {
			auto name = StatsStructFields::Plain(entry);
			if (name.IsNull() || name.type().id() != LogicalTypeId::VARCHAR ||
			    !type_set.TryAdd(StringValue::Get(name))) {
				fields.Throw("types",
				             StringUtil::Format("'%s' is not a geometry type name, e.g. 'point' or 'polygon_z'",
				                                name.ToString()));
			}
		}
		data.types = type_set;
	}

	// a flag that is not given keeps its value
	bool has_empty_geom = data.flags.HasEmptyGeometry();
	bool has_non_empty_geom = data.flags.HasNonEmptyGeometry();
	bool has_empty_part = data.flags.HasEmptyPart();
	bool has_non_empty_part = data.flags.HasNonEmptyPart();
	const GeometryStatsFlag flags[] = {{"has_empty_geom", has_empty_geom},
	                                   {"has_non_empty_geom", has_non_empty_geom},
	                                   {"has_empty_part", has_empty_part},
	                                   {"has_non_empty_part", has_non_empty_part}};
	for (auto &flag : flags) {
		if (fields.TryGet(flag.name, LogicalType::BOOLEAN, field)) {
			flag.value = BooleanValue::Get(field);
		}
	}
	auto new_flags = GeometryStatsFlags::Empty();
	if (has_empty_geom) {
		new_flags.SetHasEmptyGeometry();
	}
	if (has_non_empty_geom) {
		new_flags.SetHasNonEmptyGeometry();
	}
	if (has_empty_part) {
		new_flags.SetHasEmptyPart();
	}
	if (has_non_empty_part) {
		new_flags.SetHasNonEmptyPart();
	}
	data.flags = new_flags;
}

void GeometryStats::Update(BaseStatistics &stats, const string_t &value) {
	auto &data = GetDataUnsafe(stats);
	data.Update(value);
}

void GeometryStats::Merge(BaseStatistics &stats, const BaseStatistics &other) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}
	if (other.GetType().id() == LogicalTypeId::SQLNULL) {
		return;
	}

	auto &target = GetDataUnsafe(stats);
	auto &source = GetDataUnsafe(other);
	target.Merge(source);
}

void GeometryStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	// TODO: Verify stats
}

const GeometryStatsData &GeometryStats::GetDataUnsafe(const BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::GEOMETRY_STATS);
	return stats.stats_union.geometry_data;
}

GeometryStatsData &GeometryStats::GetDataUnsafe(BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::GEOMETRY_STATS);
	return stats.stats_union.geometry_data;
}

GeometryExtent &GeometryStats::GetExtent(BaseStatistics &stats) {
	return GetDataUnsafe(stats).extent;
}

GeometryTypeSet &GeometryStats::GetTypes(BaseStatistics &stats) {
	return GetDataUnsafe(stats).types;
}

GeometryStatsFlags &GeometryStats::GetFlags(BaseStatistics &stats) {
	return GetDataUnsafe(stats).flags;
}

const GeometryExtent &GeometryStats::GetExtent(const BaseStatistics &stats) {
	return GetDataUnsafe(stats).extent;
}

const GeometryTypeSet &GeometryStats::GetTypes(const BaseStatistics &stats) {
	return GetDataUnsafe(stats).types;
}

const GeometryStatsFlags &GeometryStats::GetFlags(const BaseStatistics &stats) {
	return GetDataUnsafe(stats).flags;
}

} // namespace duckdb
