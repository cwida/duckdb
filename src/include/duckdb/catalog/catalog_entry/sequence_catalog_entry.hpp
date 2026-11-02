//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/common/optional.hpp"

namespace duckdb {
class DuckTransaction;
class SequenceCatalogEntry;

struct SequenceValue {
	SequenceCatalogEntry *entry;
	uint64_t usage_count;
	int64_t counter;
};

struct SequenceData {
	explicit SequenceData(CreateSequenceInfo &info);

	//! The amount of times the sequence has been used
	uint64_t usage_count;
	//! The sequence counter
	int64_t counter;
	//! The most recently returned value
	optional<int64_t> last_value;
	//! The increment value
	int64_t increment;
	//! The start_value of the sequence
	int64_t start_value;
	//! The minimum value of the sequence
	int64_t min_value;
	//! The maximum value of the sequence
	int64_t max_value;
	//! Whether or not the sequence cycles
	bool cycle;
};

//! The state of a sequence: shared by all versions of the catalog entry, so that altering the entry (e.g. setting a
//! comment) does not fork the counter
struct SequenceState {
	explicit SequenceState(CreateSequenceInfo &info) : data(info) {
	}

	//! Lock for getting a value on the sequence
	mutable mutex lock;
	//! Sequence data
	SequenceData data;
};

//! A sequence catalog entry
class SequenceCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SEQUENCE_ENTRY;
	static constexpr const char *Name = "sequence";

public:
	//! Create a real TableCatalogEntry and initialize storage for it
	SequenceCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateSequenceInfo &info);

public:
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	unique_ptr<CreateInfo> GetInfo() const override;

	SequenceData GetData() const;
	int64_t CurrentValue();
	int64_t NextValue(DuckTransaction &transaction);
	int64_t SetValue(DuckTransaction &transaction, int64_t value, bool is_called);
	void ReplayValue(uint64_t usage_count, int64_t counter, optional<int64_t> last_value);
	//! Derive the last value from the counter - storage versions prior to v2.0.0 do not store it
	void DeriveLastValue();

	string ToSQL() const override;

private:
	//! An exhaustion counter outside of the range, if the range and the storage format permit one
	optional<int64_t> GetExhaustedCounter() const;

	//! The sequence state (shared with the other versions of this entry)
	shared_ptr<SequenceState> state;
};
} // namespace duckdb
