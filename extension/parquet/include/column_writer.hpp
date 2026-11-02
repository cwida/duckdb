//===----------------------------------------------------------------------===//
//                         DuckDB
//
// column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "parquet_types.h"
#include "parquet_column_schema.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "parquet_arena.h"

namespace duckdb_parquet {
class RowGroup;
class SchemaElement;
} // namespace duckdb_parquet

namespace duckdb {
class MemoryStream;
class ParquetWriter;
class ColumnWriterPageState;
class PrimitiveColumnWriterState;
struct ChildFieldIDs;
struct ShreddingType;
class ResizeableBuffer;
class ParquetBloomFilter;
class AllocatedData;
class BoundReferenceExpression;
class ClientContext;
class Expression;
class Vector;
struct ValidityMask;

class ColumnWriterState {
public:
	virtual ~ColumnWriterState();

	unsafe_vector<uint16_t> definition_levels;
	unsafe_vector<uint16_t> repetition_levels;
	unsafe_vector<uint8_t> is_empty;
	idx_t parent_null_count = 0;
	idx_t null_count = 0;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class ColumnWriterPageState {
public:
	virtual ~ColumnWriterPageState() {
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct ParquetAnalyzeSchemaState {
public:
	ParquetAnalyzeSchemaState() {
	}
	virtual ~ParquetAnalyzeSchemaState() {
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class ColumnWriter {
protected:
	static constexpr uint16_t PARQUET_DEFINE_VALID = UINT16_C(65535);

public:
	ColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path);
	virtual ~ColumnWriter();

public:
	const LogicalType &Type() const {
		return column_schema.type;
	}
	const ParquetColumnSchema &Schema() const {
		return column_schema;
	}
	ParquetColumnSchema &Schema() {
		return column_schema;
	}
	inline idx_t SchemaIndex() const {
		D_ASSERT(column_schema.schema_index.IsValid());
		return column_schema.schema_index.GetIndex();
	}
	inline idx_t MaxDefine() const {
		return column_schema.max_define;
	}
	idx_t MaxRepeat() const {
		return column_schema.max_repeat;
	}
	virtual bool HasTransform() {
		for (auto &child_writer : child_writers) {
			if (child_writer->HasTransform()) {
				throw NotImplementedException("ColumnWriter of type '%s' requires a transform, but is not a root "
				                              "column, this isn't supported currently",
				                              child_writer->Type());
			}
		}
		return false;
	}
	virtual LogicalType TransformedType() const {
		throw NotImplementedException("Writer does not have a transformed type");
	}
	virtual unique_ptr<Expression> TransformExpression(unique_ptr<BoundReferenceExpression> expr) {
		throw NotImplementedException("Writer does not have a transform expression");
	}

	virtual unique_ptr<ParquetAnalyzeSchemaState> AnalyzeSchemaInit() {
		return nullptr;
	}

	const vector<unique_ptr<ColumnWriter>> &ChildWriters() const {
		return child_writers;
	}

	//! Mark this writer (and descendants) as REQUIRED in the Parquet schema
	void MarkRepetitionRequired();

	virtual void AnalyzeSchema(ParquetAnalyzeSchemaState &state, Vector &input, idx_t count) {
		throw NotImplementedException("Writer doesn't require an AnalyzeSchema pass");
	}

	virtual void AnalyzeSchemaFinalize(const ParquetAnalyzeSchemaState &state) {
		throw NotImplementedException("Writer doesn't require an AnalyzeSchemaFinalize pass");
	}

	virtual bool TryExportPreparedShreddingType(ShreddingType &result) const;

	virtual idx_t FinalizeSchema(duckdb_parquet::SchemaElementList &schemas) = 0;

	//! Create the column writer for a specific type recursively
	static unique_ptr<ColumnWriter> CreateWriterRecursive(ClientContext &context, ParquetWriter &writer,
	                                                      vector<Identifier> path_in_schema, const LogicalType &type,
	                                                      const Identifier &name, bool allow_geometry,
	                                                      optional_ptr<const ChildFieldIDs> field_ids,
	                                                      optional_ptr<const ShreddingType> shredding_types,
	                                                      idx_t max_repeat = 0, idx_t max_define = 1,
	                                                      bool can_have_nulls = true);

	virtual unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) = 0;

	//! indicates whether the write need to analyse the data before preparing it
	virtual bool HasAnalyze() {
		return false;
	}

	virtual void Analyze(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) {
		throw NotImplementedException("Writer does not need analysis");
	}

	//! Called after all data has been passed to Analyze
	virtual void FinalizeAnalyze(ColumnWriterState &state) {
		throw NotImplementedException("Writer does not need analysis");
	}

	virtual void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count,
	                     bool vector_can_span_multiple_pages) = 0;

	virtual void BeginWrite(ColumnWriterState &state) = 0;
	virtual void Write(ColumnWriterState &state, Vector &vector, idx_t count) = 0;
	virtual void PrepareWrite(ColumnWriterState &state) = 0;
	virtual void FinalizeWrite(ColumnWriterState &state) = 0;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	void HandleDefineLevels(ColumnWriterState &state, ColumnWriterState *parent, const ValidityMask &validity,
	                        const idx_t count, const uint16_t define_value, const uint16_t null_value) const;
	void HandleRepeatLevels(ColumnWriterState &state_p, ColumnWriterState *parent, idx_t count) const;

	void CompressPage(MemoryStream &temp_writer, size_t &compressed_size, data_ptr_t &compressed_data,
	                  AllocatedData &compressed_buf);

	void DecrementMaxDefineRecursive();

public:
	ParquetWriter &writer;
	//! The parent writer (if this is a nested field)
	optional_ptr<ColumnWriter> parent;
	ParquetColumnSchema column_schema;
	vector<Identifier> schema_path;
	bool can_have_nulls;

protected:
	vector<unique_ptr<ColumnWriter>> child_writers;
};

} // namespace duckdb
