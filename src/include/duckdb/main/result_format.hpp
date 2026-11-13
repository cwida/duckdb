//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/result_format.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/query_result_memory_type.hpp"
#include "duckdb/common/enums/result_ordering.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/main/result_unit.hpp"

namespace duckdb {

class BatchedDataCollection;
class ChunkRetainedCollection;
class ClientContext;
class ColumnDataCollection;
class DataChunk;
class RetainedResultCollection;

class ResultFormatGlobalState {
public:
	DUCKDB_API virtual ~ResultFormatGlobalState();

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
};

class ResultFormatLocalState {
public:
	DUCKDB_API virtual ~ResultFormatLocalState();

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
};

//! Captured on the client thread at submission, because a worker may not read settings while the query runs
struct ResultFormatContext {
	vector<LogicalType> types;
	vector<Identifier> names;
	ClientProperties client_properties;
	ResultOrdering ordering = ResultOrdering::UNORDERED;
};

//! Subclasses declare NAME, GlobalState, the payload T and a static UnpackUnit, which maps null to null;
//! ResultFormatBase<F> supplies the retained store unless the format declares C and Collection.
//! Formats are identified by NAME, never by type
//! Workers call AppendToUnit, IsUnitFinished and FinishUnit concurrently and share the format and global
//! state, so only local state is mutable
//! AppendToUnit must copy out of the chunk: the pipeline reuses it (DataChunk::Reset restores the vector
//! cache's buffers), and this is not enforced structurally
class ResultFormat {
public:
	DUCKDB_API virtual ~ResultFormat();

public:
	virtual const char *Name() const = 0;
	//! Runs once, on the submitting thread, before any worker starts
	virtual unique_ptr<ResultFormatGlobalState> InitGlobal(const ResultFormatContext &context) = 0;
	//! Runs on each producer's own thread, concurrently with other producers' calls, against the shared global state
	virtual unique_ptr<ResultFormatLocalState> InitLocal(ResultFormatGlobalState &gstate) = 0;
	virtual void AppendToUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate, DataChunk &chunk) = 0;
	//! True while a unit that reached the cap is ready to be taken
	virtual bool IsUnitFinished(ResultFormatLocalState &lstate) = 0;
	//! The next finished unit; with none ready, the partial unit under construction; null when empty. Never a
	//! unit without rows: a stream reads only a missing unit as its end
	virtual unique_ptr<ResultUnit> FinishUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate) = 0;
	//! Called for each producer and once for the global instance the producers are merged into
	virtual unique_ptr<RetainedResultCollection> CreateCollection(ClientContext &context,
	                                                              ResultFormatGlobalState &gstate,
	                                                              const ResultFormatContext &format_context) = 0;

public:
	template <class F>
	bool Is() const {
		return NameEquals(F::NAME);
	}

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
	DUCKDB_API bool NameEquals(const char *name) const;
};

class ChunkFormat : public ResultFormat {
public:
	using T = DataChunk;
	using C = ColumnDataCollection;
	using Collection = ChunkRetainedCollection;
	using GlobalState = ResultFormatGlobalState;
	static constexpr const char *NAME = "chunk";

public:
	DUCKDB_API explicit ChunkFormat(QueryResultMemoryType memory_type = QueryResultMemoryType::IN_MEMORY);

public:
	DUCKDB_API static const shared_ptr<ResultFormat> &InMemory();
	//! Rows count against memory_limit and can spill, and the result throws once the database has closed
	DUCKDB_API static const shared_ptr<ResultFormat> &BufferManaged();

	DUCKDB_API QueryResultMemoryType MemoryType() const;

public:
	DUCKDB_API const char *Name() const override;
	DUCKDB_API unique_ptr<ResultFormatGlobalState> InitGlobal(const ResultFormatContext &context) override;
	DUCKDB_API unique_ptr<ResultFormatLocalState> InitLocal(ResultFormatGlobalState &gstate) override;
	DUCKDB_API void AppendToUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate,
	                             DataChunk &chunk) override;
	DUCKDB_API bool IsUnitFinished(ResultFormatLocalState &lstate) override;
	DUCKDB_API unique_ptr<ResultUnit> FinishUnit(ResultFormatGlobalState &gstate,
	                                             ResultFormatLocalState &lstate) override;
	DUCKDB_API unique_ptr<RetainedResultCollection>
	CreateCollection(ClientContext &context, ResultFormatGlobalState &gstate,
	                 const ResultFormatContext &format_context) override;

public:
	DUCKDB_API static unique_ptr<T> UnpackUnit(unique_ptr<ResultUnit> unit);

private:
	QueryResultMemoryType memory_type;
};

} // namespace duckdb
