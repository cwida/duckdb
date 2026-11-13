//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/table_filter_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/optional.hpp"

namespace duckdb {

struct SelectionVector;
class Vector;
struct TableFilterState;

struct TableFilterCacheIdentity {
	bool valid = true;
};

class TableFilterCache {
public:
	virtual ~TableFilterCache() = default;

	bool IsValidFor(const TableFilterState &state) const;
	void Filter(const SelectionVector &value_indices, SelectionVector &sel, idx_t &count);
	void FilterConstant(idx_t value_index, idx_t &count);
	//! Selection indexes runs; row_counts supplies the logical row weight of each run.
	void FilterRuns(const SelectionVector &value_indices, array_ptr<const idx_t> row_counts, SelectionVector &sel,
	                idx_t &count);
	bool CanSkipAll() const;
	//! Whether this batch can be filtered without reading value indices. Does not advance adaptive feedback.
	bool CanAcceptAll() const;
	//! Borrow a pure root mask while the cache and its filter state remain alive.
	optional<array_ptr<const bool>> GetPureMask() const;

protected:
	virtual optional<array_ptr<const bool>> GetPureMaskInternal() const {
		return {};
	}
	virtual bool CanAcceptAllInternal() const {
		return false;
	}
	virtual bool CanSkipAllInternal() const {
		return false;
	}
	virtual void FilterInternal(const SelectionVector &value_indices, SelectionVector &sel, idx_t &count) = 0;
	virtual void FilterConstantInternal(idx_t value_index, idx_t &count) = 0;
	virtual void FilterRunsInternal(const SelectionVector &value_indices, array_ptr<const idx_t> row_counts,
	                                SelectionVector &sel, idx_t &count) = 0;

private:
	friend struct TableFilterState;
	shared_ptr<TableFilterCacheIdentity> identity;
};

struct ExpressionFilterExecutor {
	virtual ~ExpressionFilterExecutor() = default;

	virtual idx_t FilterSelection(SelectionVector &sel, Vector &vector, idx_t scan_count,
	                              idx_t &approved_tuple_count) = 0;
	virtual bool CanCache() const {
		return false;
	}
	virtual unique_ptr<TableFilterCache> CreateCache(Vector &values, idx_t count);
};

//! Thread-local state for executing a table filter
struct TableFilterState {
public:
	TableFilterState() = default;
	TableFilterState(const TableFilterState &) = delete;
	TableFilterState &operator=(const TableFilterState &) = delete;
	virtual ~TableFilterState();

public:
	static unique_ptr<TableFilterState> Initialize(ClientContext &context, const TableFilter &filter);
	unique_ptr<TableFilterCache> CreateCache(Vector &values, idx_t count);

	//! Whether a pure cache plan is available for this filter.
	bool can_cache = false;

public:
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

private:
	friend class TableFilterCache;
	shared_ptr<TableFilterCacheIdentity> cache_identity;
};

struct ExpressionFilterState : public TableFilterState {
public:
	ExpressionFilterState(Context &context, const Expression &expression);
	~ExpressionFilterState() override;

	Context &GetContext() {
		D_ASSERT(executor);
		return executor->GetContext();
	}

	unique_ptr<ExpressionExecutor> executor;
	unique_ptr<ExpressionFilterExecutor> fast_executor;
	bool can_cache_result = false;
};

} // namespace duckdb
