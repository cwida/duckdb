//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/arrow/arrow_format.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/retained_result_collection.hpp"

namespace duckdb {

class ArrowRetainedCollection;
class ArrowTypeExtensionData;

//! An appender's finalized array, never mutated once shared because every export borrows its buffers
using ArrowArrayOwner = shared_ptr<const ArrowArrayWrapper>;
using ArrowArrayCollection = vector<ArrowArrayOwner>;

//! Per-query Arrow state, built at submission. The schema and the extension type map are resolved once
//! and shared by every array. Resolving them needs the client context the properties carry
class ArrowFormatGlobalState : public ResultFormatGlobalState {
public:
	DUCKDB_API explicit ArrowFormatGlobalState(const ResultFormatContext &context);
	DUCKDB_API ~ArrowFormatGlobalState() override;

public:
	//! Owned by this state and released with it. Hand consumers a copy
	const ArrowSchema &Schema() const {
		return schema.arrow_schema;
	}
	const vector<LogicalType> &Types() const {
		return types;
	}
	const ClientProperties &Properties() const {
		return properties;
	}
	const unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> &ExtensionTypes() const {
		return extension_types;
	}

private:
	vector<LogicalType> types;
	ClientProperties properties;
	unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_types;
	ArrowSchemaWrapper schema;
};

//! Turns chunks into Arrow arrays of at most batch_size rows, one appender per producer
class ArrowFormat : public ResultFormat {
public:
	using T = ArrowArrayWrapper;
	using C = ArrowArrayCollection;
	using Collection = ArrowRetainedCollection;
	using GlobalState = ArrowFormatGlobalState;
	static constexpr const char *NAME = "arrow";

public:
	DUCKDB_API explicit ArrowFormat(idx_t batch_size);

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
	//! The array the appender finalized, owned by the caller from here on
	DUCKDB_API static unique_ptr<ArrowArrayWrapper> UnpackUnit(unique_ptr<ResultUnit> unit);
	//! An export over the owner's buffers whose every node holds the owner, so a moved-out child outlives the rest
	DUCKDB_API static unique_ptr<ArrowArrayWrapper> ShareArray(const ArrowArrayOwner &owner);
	//! The count in a CHANGED_ROWS result's array, a single BIGINT row; 0 for an empty array
	DUCKDB_API static idx_t ChangedRows(const ArrowArray &array);

private:
	//! The rows an array holds, except at a batch boundary and at a producer's end of input
	idx_t batch_size;
};

//! Fetch exports with ShareArray, so the collection stays whole
class ArrowRetainedCollection : public DefaultRetainedCollection<ArrowFormat> {
public:
	using DefaultRetainedCollection<ArrowFormat>::DefaultRetainedCollection;

public:
	//! An export of the next array; null at the end and forever after
	DUCKDB_API unique_ptr<ArrowArrayWrapper> Fetch();

private:
	idx_t fetch_index = 0;
};

} // namespace duckdb
