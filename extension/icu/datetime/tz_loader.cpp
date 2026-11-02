#include "tz_data.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "zstd.h"

namespace duckdb {
namespace datetime {

//! The counts the block starts with, which say how long each of the sections after it is
struct TZBlockHeader {
	uint32_t zone_count;
	uint32_t zone_data_count;
	uint32_t rule_count;
	uint32_t windows_zone_count;
	uint32_t pool_size;
	//! How long the zone data section is, which is what the offsets after it are relative to
	uint32_t zone_region_size;
};

//! How many values a rule is stored as, which is what the generator writes them out as
static constexpr idx_t RULE_FIELD_COUNT = 11;

//! The scalars a zone starts with, followed by its arrays
struct TZZoneHeader {
	uint32_t transition_count;
	uint32_t type_count;
	uint32_t link_count;
	int32_t final_raw;
	int32_t final_year;
	int32_t final_rule;
};

//! Walks the block, checking that nothing reaches past the end of it
class BlockReader {
public:
	BlockReader(const uint8_t *data, idx_t size) : position(data), end(data + size) {
	}

	template <class T>
	const T *Read(idx_t count) {
		const auto result = reinterpret_cast<const T *>(position);
		Advance(count * sizeof(T));
		return result;
	}

	void Advance(idx_t bytes) {
		if (bytes > idx_t(end - position)) {
			throw InternalException("The time zone data is corrupt");
		}
		position += bytes;
	}

	const uint8_t *Position() const {
		return position;
	}
	//! Rounds the position up, so that what is read next is aligned for its element type
	void Align(idx_t alignment) {
		Advance((alignment - (uintptr_t(position) % alignment)) % alignment);
	}

private:
	const uint8_t *position;
	const uint8_t *end;
};

//! The decompressed block together with the tables that point into it
struct TZBlock {
	unique_ptr<uint8_t[]> bytes;
	vector<TZZone> zones;
	vector<TZZoneData> zone_data;
	vector<TZRule> rules;
	vector<TZWindowsZone> windows_zones;
	TZData data;
};

static void ReadZone(const uint8_t *base, TZZoneData &zone) {
	const auto header = reinterpret_cast<const TZZoneHeader *>(base);
	auto position = base + sizeof(TZZoneHeader);

	zone.transition_count = header->transition_count;
	zone.type_count = uint16_t(header->type_count);
	zone.link_count = uint16_t(header->link_count);
	zone.final_raw = header->final_raw;
	zone.final_year = header->final_year;
	zone.final_rule = int16_t(header->final_rule);

	zone.transitions = reinterpret_cast<const int64_t *>(position);
	position += header->transition_count * sizeof(int64_t);
	zone.type_offsets = reinterpret_cast<const TZTypeOffset *>(position);
	position += header->type_count * sizeof(TZTypeOffset);
	zone.links = reinterpret_cast<const uint16_t *>(position);
	position += header->link_count * sizeof(uint16_t);
	zone.type_map = position;
}

static unique_ptr<TZBlock> Decompress() {
	auto block = make_uniq<TZBlock>();
	block->bytes = unique_ptr<uint8_t[]>(new uint8_t[TZ_UNCOMPRESSED_SIZE]);
	const auto size =
	    duckdb_zstd::ZSTD_decompress(block->bytes.get(), TZ_UNCOMPRESSED_SIZE, TZ_COMPRESSED, TZ_COMPRESSED_SIZE);
	if (duckdb_zstd::ZSTD_isError(size) || size != TZ_UNCOMPRESSED_SIZE) {
		throw InternalException("Failed to decompress the time zone data");
	}

	BlockReader reader(block->bytes.get(), TZ_UNCOMPRESSED_SIZE);
	const auto header = reader.Read<TZBlockHeader>(1);

	// the zone data comes first, while the block is still aligned for the 64 bit transitions
	const auto zone_region = reader.Position();
	reader.Advance(header->zone_region_size);
	const auto data_offsets = reader.Read<uint32_t>(header->zone_data_count);
	const auto name_offsets = reader.Read<uint32_t>(header->zone_count);
	const auto rule_values = reader.Read<int32_t>(header->rule_count * RULE_FIELD_COUNT);
	const auto windows_entries = reader.Read<uint32_t>(static_cast<idx_t>(header->windows_zone_count) * 3);
	reader.Align(sizeof(uint16_t));
	const auto data_indexes = reader.Read<uint16_t>(header->zone_count);
	const auto pool = reinterpret_cast<const char *>(reader.Read<uint8_t>(header->pool_size));

	block->zone_data.resize(header->zone_data_count);
	for (idx_t i = 0; i < header->zone_data_count; i++) {
		if (data_offsets[i] >= header->zone_region_size) {
			throw InternalException("The time zone data is corrupt");
		}
		ReadZone(zone_region + data_offsets[i], block->zone_data[i]);
	}

	block->zones.resize(header->zone_count);
	for (idx_t i = 0; i < header->zone_count; i++) {
		if (name_offsets[i] >= header->pool_size || data_indexes[i] >= header->zone_data_count) {
			throw InternalException("The time zone data is corrupt");
		}
		block->zones[i].name = pool + name_offsets[i];
		block->zones[i].data_index = data_indexes[i];
	}

	block->rules.resize(header->rule_count);
	for (idx_t i = 0; i < header->rule_count; i++) {
		const auto values = rule_values + i * RULE_FIELD_COUNT;
		auto &rule = block->rules[i];
		rule.start_month = int8_t(values[0]);
		rule.start_day = int8_t(values[1]);
		rule.start_day_of_week = int8_t(values[2]);
		rule.start_time = values[3];
		rule.start_time_mode = int8_t(values[4]);
		rule.end_month = int8_t(values[5]);
		rule.end_day = int8_t(values[6]);
		rule.end_day_of_week = int8_t(values[7]);
		rule.end_time = values[8];
		rule.end_time_mode = int8_t(values[9]);
		rule.dst_savings = values[10];
	}

	block->windows_zones.resize(header->windows_zone_count);
	for (idx_t i = 0; i < header->windows_zone_count; i++) {
		const auto entry = windows_entries + i * 3;
		if (entry[0] >= header->pool_size || entry[1] >= header->pool_size || entry[2] >= header->pool_size) {
			throw InternalException("The time zone data is corrupt");
		}
		block->windows_zones[i].windows_name = pool + entry[0];
		block->windows_zones[i].region = pool + entry[1];
		block->windows_zones[i].zone = pool + entry[2];
	}

	block->data.zones = block->zones.data();
	block->data.zone_count = header->zone_count;
	block->data.zone_data = block->zone_data.data();
	block->data.zone_data_count = header->zone_data_count;
	block->data.rules = block->rules.data();
	block->data.rule_count = header->rule_count;
	block->data.windows_zones = block->windows_zones.data();
	block->data.windows_zone_count = header->windows_zone_count;
	return block;
}

const TZData &GetTZData() {
	// the initialization of a local static is thread safe, and the block is kept afterwards
	static const unique_ptr<TZBlock> block = Decompress();
	return block->data;
}

} // namespace datetime
} // namespace duckdb
