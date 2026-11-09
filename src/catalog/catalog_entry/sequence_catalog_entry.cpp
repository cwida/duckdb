#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb {

constexpr const char *SequenceCatalogEntry::Name;

SequenceData::SequenceData(CreateSequenceInfo &info)
    : usage_count(info.usage_count), counter(info.start_value), last_value(info.last_value), increment(info.increment),
      start_value(info.start_value), min_value(info.min_value), max_value(info.max_value), cycle(info.cycle) {
}

SequenceCatalogEntry::SequenceCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateSequenceInfo &info)
    : StandardEntry(CatalogType::SEQUENCE_ENTRY, schema, catalog, info.GetSequenceName()),
      state(make_shared_ptr<SequenceState>(info)) {
	this->temporary = info.temporary;
	this->comment = info.comment;
	this->tags = info.tags;
}

unique_ptr<CatalogEntry> SequenceCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateSequenceInfo>();

	auto result = make_uniq<SequenceCatalogEntry>(catalog, schema, cast_info);
	// all versions of the entry share the sequence state
	result->state = state;

	return std::move(result);
}

SequenceData SequenceCatalogEntry::GetData() const {
	lock_guard<mutex> seqlock(state->lock);
	return state->data;
}

int64_t SequenceCatalogEntry::CurrentValue() {
	lock_guard<mutex> seqlock(state->lock);
	int64_t result;
	if (!state->data.last_value) {
		throw SequenceException("currval: sequence is not yet defined in this session");
	}
	result = state->data.last_value.value();
	return result;
}

int64_t SequenceCatalogEntry::NextValue(DuckTransaction &transaction) {
	lock_guard<mutex> seqlock(state->lock);
	int64_t result;
	result = state->data.counter;
	int64_t next_counter;
	bool overflow = !TryAddOperator::Operation(state->data.counter, state->data.increment, next_counter);
	if (state->data.cycle) {
		if (overflow) {
			next_counter = state->data.increment < 0 ? state->data.max_value : state->data.min_value;
		} else if (next_counter < state->data.min_value) {
			next_counter = state->data.max_value;
		} else if (next_counter > state->data.max_value) {
			next_counter = state->data.min_value;
		}
	} else {
		// an exhausted sequence keeps a counter outside of its range
		bool exhausted = result < state->data.min_value || result > state->data.max_value;
		if (!exhausted && overflow) {
			// the next value does not fit in an int64, so the counter moves to an int64 outside of the range
			auto exhausted_counter = GetExhaustedCounter();
			if (exhausted_counter) {
				next_counter = exhausted_counter.value();
			} else {
				exhausted = true;
			}
		}
		if (exhausted && state->data.increment < 0) {
			throw SequenceException("nextval: reached minimum value of sequence %s (%lld)", name,
			                        state->data.min_value);
		}
		if (exhausted) {
			throw SequenceException("nextval: reached maximum value of sequence %s (%lld)", name,
			                        state->data.max_value);
		}
	}
	state->data.counter = next_counter;
	state->data.last_value = result;
	state->data.usage_count++;
	if (!temporary) {
		transaction.PushSequenceUsage(*this, state->data);
	}
	return result;
}

int64_t SequenceCatalogEntry::SetValue(DuckTransaction &transaction, int64_t value, bool is_called) {
	{
		lock_guard<mutex> seqlock(state->lock);
		if (value < state->data.min_value || value > state->data.max_value) {
			throw SequenceException("setval: value %lld is out of bounds for sequence %s (%lld..%lld)", value, name,
			                        state->data.min_value, state->data.max_value);
		}

		state->data.counter = value;
		if (!is_called) {
			state->data.usage_count++;
			if (!temporary) {
				transaction.PushSequenceUsage(*this, state->data);
			}
			return value;
		}
	}

	// is_called: behave as if nextval() was just invoked and returned `value`.
	return NextValue(transaction);
}

void SequenceCatalogEntry::ReplayValue(uint64_t v_usage_count, int64_t v_counter, optional<int64_t> last_value) {
	if (v_usage_count > state->data.usage_count) {
		state->data.usage_count = v_usage_count;
		state->data.counter = v_counter;
		state->data.last_value = last_value;
	}
}

optional<int64_t> SequenceCatalogEntry::GetExhaustedCounter() const {
	if (!temporary) {
		// storage versions prior to v2.0.0 derive the last value from the counter, which this counter does not allow
		auto &storage_manager = StorageManager::Get(catalog);
		if (storage_manager.HasStorageVersion() &&
		    StorageManager::IsPriorToVersion(StorageVersion::V2_0_0, storage_manager.GetStorageVersion())) {
			return nullopt;
		}
	}
	for (auto counter : {NumericLimits<int64_t>::Maximum(), NumericLimits<int64_t>::Minimum()}) {
		if (counter < state->data.min_value || counter > state->data.max_value) {
			return counter;
		}
	}
	return nullopt;
}

void SequenceCatalogEntry::DeriveLastValue() {
	lock_guard<mutex> seqlock(state->lock);
	if (state->data.last_value || state->data.usage_count == 0) {
		return;
	}
	int64_t last_value;
	if (!TrySubtractOperator::Operation(state->data.counter, state->data.increment, last_value)) {
		return;
	}
	// a counter that wrapped around in a cycle does not tell us the last value
	if (last_value < state->data.min_value || last_value > state->data.max_value) {
		return;
	}
	state->data.last_value = last_value;
}

unique_ptr<CreateInfo> SequenceCatalogEntry::GetInfo() const {
	auto seq_data = GetData();

	auto result = make_uniq<CreateSequenceInfo>();
	result->SetQualifiedName(schema.GetQualifiedName(name));
	result->usage_count = seq_data.usage_count;
	result->increment = seq_data.increment;
	result->min_value = seq_data.min_value;
	result->max_value = seq_data.max_value;
	result->start_value = seq_data.counter;
	result->cycle = seq_data.cycle;
	result->last_value = seq_data.last_value;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	return std::move(result);
}

string SequenceCatalogEntry::ToSQL() const {
	auto seq_data = GetData();

	duckdb::stringstream ss;
	ss << "CREATE SEQUENCE ";
	ss << name.GetIdentifierName();
	ss << " INCREMENT BY " << seq_data.increment;
	ss << " MINVALUE " << seq_data.min_value;
	ss << " MAXVALUE " << seq_data.max_value;
	ss << " START " << seq_data.counter;
	ss << " " << (seq_data.cycle ? "CYCLE" : "NO CYCLE") << ";";
	return ss.str();
}
} // namespace duckdb
