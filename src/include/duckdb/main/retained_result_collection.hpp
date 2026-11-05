//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/retained_result_collection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/enums/query_result_memory_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_unit.hpp"

namespace duckdb {

//! One instance per producer, merged into one global instance under the sink's lock
class RetainedResultCollection {
public:
	DUCKDB_API virtual ~RetainedResultCollection();

public:
	virtual void Append(DataChunk &chunk, idx_t batch_index) = 0;
	//! Flushes the local's partial unit, then merges it in
	virtual void Combine(RetainedResultCollection &local) = 0;
	//! Once, after the last Combine
	virtual void Finalize() = 0;
	//! Rows stored so far; a format's unfinished partial unit counts once Combine flushes it
	virtual idx_t Count() const = 0;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! The chunk format's retained deposit: a ColumnDataCollection (unordered or source ordered) or a
//! BatchedDataCollection (batch ordered), built for the given memory type
class ChunkRetainedCollection : public RetainedResultCollection {
public:
	DUCKDB_API ChunkRetainedCollection(ClientContext &context, const vector<LogicalType> &types,
	                                   QueryResultMemoryType memory_type, bool batch_ordered);
	//! Adopts an already finalized collection, for a detached result built directly over one
	DUCKDB_API explicit ChunkRetainedCollection(unique_ptr<ColumnDataCollection> collection);
	DUCKDB_API ~ChunkRetainedCollection() override;

public:
	DUCKDB_API void Append(DataChunk &chunk, idx_t batch_index) override;
	DUCKDB_API void Combine(RetainedResultCollection &local) override;
	DUCKDB_API void Finalize() override;
	DUCKDB_API idx_t Count() const override;

public:
	DUCKDB_API ColumnDataCollection &Get();
	//! Hands the collection over; the instance holds nothing afterward
	DUCKDB_API unique_ptr<ColumnDataCollection> Take();
	//! The next chunk, scanned with DISALLOW_ZERO_COPY; null at the end and forever after. Never changes
	//! the stored data
	DUCKDB_API unique_ptr<DataChunk> FetchRaw();

private:
	bool batch_ordered;
	unique_ptr<ColumnDataCollection> collection;
	ColumnDataAppendState append_state;
	bool append_initialized = false;
	unique_ptr<BatchedDataCollection> batched;
	ColumnDataScanState scan_state;
	bool scan_initialized = false;
	bool scan_exhausted = false;
};

namespace result_format_detail {

template <class...>
using void_t_helper = void;

//! Defaults to vector<unique_ptr<T>> unless the format declares its own C
template <class FORMAT, class = void>
struct PayloadsOf {
	using type = vector<unique_ptr<typename FORMAT::T>>;
};
template <class FORMAT>
struct PayloadsOf<FORMAT, void_t_helper<typename FORMAT::C>> {
	using type = typename FORMAT::C;
};

} // namespace result_format_detail

//! Only used from inside DefaultRetainedCollection, where FORMAT is always a complete type by the time it
//! is instantiated: a CRTP base like ResultFormatBase<FORMAT> cannot resolve FORMAT::T in its own member
//! typedefs, because FORMAT is still incomplete while its base classes are being instantiated
template <class FORMAT>
using RetainedPayloadsOf = typename result_format_detail::PayloadsOf<FORMAT>::type;

//! The retained store of a format without its own. Units are unpacked as they are stored
template <class FORMAT>
class DefaultRetainedCollection : public RetainedResultCollection {
public:
	using PayloadCollection = RetainedPayloadsOf<FORMAT>;

	DefaultRetainedCollection(ResultFormat &format_p, ResultFormatGlobalState &gstate_p)
	    : format(format_p), gstate(gstate_p) {
	}

public:
	void Append(DataChunk &chunk, idx_t batch_index) override {
		if (has_current_batch && batch_index != current_batch) {
			FlushPartial();
		}
		current_batch = batch_index;
		has_current_batch = true;
		if (!local_state) {
			// On the producer's thread, and only for an instance that receives rows: the global one never does
			local_state = format.InitLocal(gstate);
		}
		format.AppendToUnit(gstate, *local_state, chunk);
		while (format.IsUnitFinished(*local_state)) {
			StoreUnit(format.FinishUnit(gstate, *local_state));
		}
	}

	void Combine(RetainedResultCollection &local_p) override {
		auto &local = local_p.Cast<DefaultRetainedCollection<FORMAT>>();
		local.FlushPartial();
		for (auto &entry : local.entries) {
			entries.push_back(std::move(entry));
		}
		total_rows += local.total_rows;
		local.entries.clear();
		local.total_rows = 0;
	}

	void Finalize() override {
		// The global instance never receives an append; only a local ever builds a format local state
		D_ASSERT(!local_state);
		std::stable_sort(entries.begin(), entries.end(),
		                 [](const Entry &lhs, const Entry &rhs) { return lhs.batch < rhs.batch; });
		for (auto &entry : entries) {
			payloads.push_back(std::move(entry.payload));
		}
		entries.clear();
		finalized = true;
	}

	idx_t Count() const override {
		return total_rows;
	}

public:
	PayloadCollection &Get() {
		D_ASSERT(finalized);
		return payloads;
	}
	unique_ptr<PayloadCollection> Take() {
		D_ASSERT(finalized);
		return make_uniq<PayloadCollection>(std::move(payloads));
	}
	//! Copies the next payload through FORMAT::CopyPayload; null at the end and forever after
	unique_ptr<typename FORMAT::T> Fetch() {
		D_ASSERT(finalized);
		if (fetch_index >= payloads.size()) {
			return nullptr;
		}
		return FORMAT::CopyPayload(*payloads[fetch_index++]);
	}

private:
	struct Entry {
		idx_t batch;
		unique_ptr<typename FORMAT::T> payload;
	};

	void FlushPartial() {
		if (!local_state) {
			return;
		}
		while (auto unit = format.FinishUnit(gstate, *local_state)) {
			StoreUnit(std::move(unit));
		}
	}

	void StoreUnit(unique_ptr<ResultUnit> unit) {
		if (!unit) {
			return;
		}
		total_rows += unit->row_count;
		entries.push_back(Entry {current_batch, FORMAT::UnpackUnit(std::move(unit))});
	}

private:
	ResultFormat &format;
	ResultFormatGlobalState &gstate;
	unique_ptr<ResultFormatLocalState> local_state;
	idx_t current_batch = 0;
	bool has_current_batch = false;
	vector<Entry> entries;
	PayloadCollection payloads;
	idx_t total_rows = 0;
	idx_t fetch_index = 0;
	bool finalized = false;
};

namespace result_format_detail {

//! Defaults to DefaultRetainedCollection<FORMAT> unless the format declares its own concrete Collection
template <class FORMAT, class = void>
struct CollectionOf {
	using type = DefaultRetainedCollection<FORMAT>;
};
template <class FORMAT>
struct CollectionOf<FORMAT, void_t_helper<typename FORMAT::Collection>> {
	using type = typename FORMAT::Collection;
};

} // namespace result_format_detail

//! The concrete RetainedResultCollection a format's retained data is stored in, reached from the
//! handle by RetainedResultCollection::Cast<RetainedCollectionOf<FORMAT>>()
template <class FORMAT>
using RetainedCollectionOf = typename result_format_detail::CollectionOf<FORMAT>::type;

//! Supplies CreateCollection for a format with no custom retained store: derive from ResultFormatBase<F>
//! instead of ResultFormat directly to get a DefaultRetainedCollection<F>
template <class FORMAT>
class ResultFormatBase : public ResultFormat {
public:
	unique_ptr<RetainedResultCollection> CreateCollection(ClientContext &context, ResultFormatGlobalState &gstate,
	                                                      const ResultFormatContext &format_context) override {
		return make_uniq<DefaultRetainedCollection<FORMAT>>(*this, gstate);
	}
};

} // namespace duckdb
