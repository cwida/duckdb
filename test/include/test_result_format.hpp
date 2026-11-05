//===----------------------------------------------------------------------===//
//                         DuckDB
//
// test_result_format.hpp
//
// A deterministic result format for the format tests: it slices chunks into units capped at a fixed
// row count and records, per unit, the producer that built it.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_unit.hpp"
#include "duckdb/main/retained_result_collection.hpp"

#ifndef DUCKDB_NO_THREADS

#include <thread>

namespace duckdb {

//! What a test unit holds: whole chunks, the producer that built it, and the totals the format tests check
struct TestPayload {
	vector<unique_ptr<DataChunk>> chunks;
	std::thread::id producer;
	idx_t row_count = 0;
	idx_t byte_size = 0;
};

inline unique_ptr<TestPayload> CopyTestPayload(const TestPayload &payload) {
	auto copy = make_uniq<TestPayload>();
	for (auto &chunk : payload.chunks) {
		copy->chunks.push_back(BufferedData::CopyForBuffering(*chunk));
	}
	copy->producer = payload.producer;
	copy->row_count = payload.row_count;
	copy->byte_size = payload.byte_size;
	return copy;
}

class TestUnit : public ResultUnit {
public:
	explicit TestUnit(unique_ptr<TestPayload> payload_p)
	    : ResultUnit(payload_p->row_count, payload_p->byte_size), payload(std::move(payload_p)) {
	}

public:
	unique_ptr<TestPayload> payload;
};

class TestFormatGlobalState : public ResultFormatGlobalState {
public:
	TestFormatGlobalState(vector<LogicalType> types_p, vector<Identifier> names_p, ResultOrdering ordering_p)
	    : types(std::move(types_p)), names(std::move(names_p)), ordering(ordering_p),
	      init_global_thread(std::this_thread::get_id()) {
	}

public:
	vector<LogicalType> types;
	vector<Identifier> names;
	ResultOrdering ordering;
	//! The thread that called InitGlobal, i.e. the thread that submitted the query
	std::thread::id init_global_thread;
	//! How many worker threads built a unit for this query
	atomic<idx_t> local_states {0};
	//! Units finished short of the row cap: one per batch boundary and one per producer at its end
	atomic<idx_t> partial_units {0};
	//! The most finished units one producer ever held undelivered after an append
	atomic<idx_t> max_pending_units {0};
	//! How many times AppendToUnit ran: one per chunk the pipeline handed to the format
	atomic<idx_t> append_calls {0};
};

class TestFormatLocalState : public ResultFormatLocalState {
public:
	//! The partial unit under construction
	vector<unique_ptr<DataChunk>> chunks;
	std::thread::id producer;
	bool producer_set = false;
	idx_t rows = 0;
	idx_t bytes = 0;
	//! Units already sliced off at exactly the cap, waiting to be taken in order
	deque<unique_ptr<TestUnit>> sealed;
};

//! Deterministic: AppendToUnit slices the incoming chunk at the cap, so a unit never exceeds
//! max_unit_rows and one append can finish several units
class TestFormat : public ResultFormatBase<TestFormat> {
public:
	using T = TestPayload;
	using GlobalState = TestFormatGlobalState;
	static constexpr const char *NAME = "test";

public:
	explicit TestFormat(idx_t max_unit_rows_p) : max_unit_rows(max_unit_rows_p) {
	}

public:
	const char *Name() const override {
		return NAME;
	}

	unique_ptr<ResultFormatGlobalState> InitGlobal(const ResultFormatContext &context) override {
		if (throw_in_init_global) {
			throw InvalidInputException("TestFormat::InitGlobal");
		}
		return make_uniq<TestFormatGlobalState>(context.types, context.names, context.ordering);
	}

	unique_ptr<ResultFormatLocalState> InitLocal(ResultFormatGlobalState &gstate) override {
		gstate.Cast<TestFormatGlobalState>().local_states++;
		return make_uniq<TestFormatLocalState>();
	}

	void AppendToUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate_p, DataChunk &chunk) override {
		if (throw_in_append) {
			throw InvalidInputException("TestFormat::AppendToUnit");
		}
		gstate.Cast<TestFormatGlobalState>().append_calls++;
		auto &lstate = lstate_p.Cast<TestFormatLocalState>();
		if (!lstate.producer_set) {
			lstate.producer = std::this_thread::get_id();
			lstate.producer_set = true;
		}
		idx_t offset = 0;
		while (offset < chunk.size()) {
			auto to_take = MinValue<idx_t>(max_unit_rows - lstate.rows, chunk.size() - offset);
			SelectionVector sel(to_take);
			for (idx_t i = 0; i < to_take; i++) {
				sel.set_index(i, offset + i);
			}
			// A view into chunk, materialized into an owned copy below: the pipeline reuses chunk
			DataChunk sliced;
			sliced.InitializeEmpty(chunk.GetTypes());
			sliced.Slice(chunk, sel, to_take);
			auto copy = BufferedData::CopyForBuffering(sliced);
			lstate.rows += copy->size();
			lstate.bytes += copy->GetDataSize();
			lstate.chunks.push_back(std::move(copy));
			offset += to_take;
			if (lstate.rows >= max_unit_rows) {
				lstate.sealed.push_back(Seal(lstate));
			}
		}
		auto &global = gstate.Cast<TestFormatGlobalState>();
		auto pending = lstate.sealed.size();
		auto seen = global.max_pending_units.load();
		while (pending > seen && !global.max_pending_units.compare_exchange_weak(seen, pending)) {
		}
	}

	bool IsUnitFinished(ResultFormatLocalState &lstate_p) override {
		if (throw_in_is_finished) {
			throw InvalidInputException("TestFormat::IsUnitFinished");
		}
		auto &lstate = lstate_p.Cast<TestFormatLocalState>();
		return !lstate.sealed.empty();
	}

	unique_ptr<ResultUnit> FinishUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate_p) override {
		auto &lstate = lstate_p.Cast<TestFormatLocalState>();
		if (lstate.sealed.empty() && lstate.chunks.empty()) {
			return nullptr;
		}
		if (throw_in_finish) {
			throw InvalidInputException("TestFormat::FinishUnit");
		}
		if (!lstate.sealed.empty()) {
			auto unit = std::move(lstate.sealed.front());
			lstate.sealed.pop_front();
			return std::move(unit);
		}
		if (lstate.rows < max_unit_rows) {
			if (throw_in_partial_finish) {
				throw InvalidInputException("TestFormat::FinishUnit of a partial unit");
			}
			gstate.Cast<TestFormatGlobalState>().partial_units++;
		}
		return Seal(lstate);
	}

public:
	static unique_ptr<TestPayload> UnpackUnit(unique_ptr<ResultUnit> unit) {
		if (!unit) {
			return nullptr;
		}
		return std::move(unit->Cast<TestUnit>().payload);
	}
	static unique_ptr<TestPayload> CopyPayload(const TestPayload &payload) {
		return CopyTestPayload(payload);
	}

public:
	idx_t max_unit_rows;
	atomic<bool> throw_in_init_global {false};
	atomic<bool> throw_in_append {false};
	atomic<bool> throw_in_is_finished {false};
	atomic<bool> throw_in_finish {false};
	//! Throws only when a unit is finished short of the cap, at a batch boundary or a producer's end
	atomic<bool> throw_in_partial_finish {false};

private:
	//! Takes whatever is currently accumulated, whether it reached the cap or not
	unique_ptr<TestUnit> Seal(TestFormatLocalState &lstate) const {
		auto payload = make_uniq<TestPayload>();
		payload->chunks = std::move(lstate.chunks);
		payload->producer = lstate.producer;
		payload->row_count = lstate.rows;
		payload->byte_size = lstate.bytes;
		lstate.chunks.clear();
		lstate.rows = 0;
		lstate.bytes = 0;
		return make_uniq<TestUnit>(std::move(payload));
	}
};

//! Declares the same payload type as TestFormat, so only the name check can refuse the mismatch
class OtherTestFormat : public ResultFormatBase<OtherTestFormat> {
public:
	using T = TestPayload;
	using GlobalState = TestFormatGlobalState;
	static constexpr const char *NAME = "other";

public:
	const char *Name() const override {
		return NAME;
	}

	unique_ptr<ResultFormatGlobalState> InitGlobal(const ResultFormatContext &context) override {
		return make_uniq<TestFormatGlobalState>(context.types, context.names, context.ordering);
	}

	unique_ptr<ResultFormatLocalState> InitLocal(ResultFormatGlobalState &gstate) override {
		return make_uniq<TestFormatLocalState>();
	}

	void AppendToUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate, DataChunk &chunk) override {
	}

	bool IsUnitFinished(ResultFormatLocalState &lstate) override {
		return false;
	}

	unique_ptr<ResultUnit> FinishUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate) override {
		return nullptr;
	}

public:
	static unique_ptr<TestPayload> UnpackUnit(unique_ptr<ResultUnit> unit) {
		return TestFormat::UnpackUnit(std::move(unit));
	}
	static unique_ptr<TestPayload> CopyPayload(const TestPayload &payload) {
		return CopyTestPayload(payload);
	}
};

inline vector<Value> UnitValues(const TestPayload &payload, idx_t column) {
	vector<Value> values;
	for (auto &chunk : payload.chunks) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			values.push_back(chunk->GetValue(column, row));
		}
	}
	return values;
}

inline vector<Value> UnitValues(const ResultUnit &unit, idx_t column) {
	return UnitValues(*unit.Cast<TestUnit>().payload, column);
}

inline unique_ptr<QueryResult> SubmitFormatted(Connection &con, const string &query, idx_t max_unit_rows) {
	auto handle = con.Submit(query, make_shared_ptr<TestFormat>(max_unit_rows));
	REQUIRE(!handle->HasError());
	return handle;
}

inline void RequireAscending(const vector<int64_t> &rows, idx_t expected_count) {
	REQUIRE(rows.size() == expected_count);
	for (idx_t i = 0; i < rows.size(); i++) {
		if (rows[i] != NumericCast<int64_t>(i)) {
			FAIL(StringUtil::Format("Out-of-order row %llu: expected %llu, got %lld", i, i, rows[i]));
		}
	}
}

} // namespace duckdb

#endif
