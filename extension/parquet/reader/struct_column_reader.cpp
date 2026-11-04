#include <stdint.h>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "duckdb/common/vector/struct_vector.hpp"
#include "reader/struct_column_reader.hpp"
#include "column_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb_apache {
namespace thrift {
namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache
namespace duckdb_parquet {
class ColumnChunk;
} // namespace duckdb_parquet

namespace duckdb {
class ParquetReader;
class ThriftFileTransport;
struct ParquetColumnSchema;

//===--------------------------------------------------------------------===//
// Struct Column Reader
//===--------------------------------------------------------------------===//
StructColumnReader::StructColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema,
                                       vector<unique_ptr<ColumnReader>> child_readers_p)
    : ColumnReader(reader, schema), child_readers(std::move(child_readers_p)) {
	D_ASSERT(Type().InternalType() == PhysicalType::STRUCT);
	for (auto &child : child_readers) {
		if (child) {
			child->SetParent(*this);
		}
	}
}

ColumnReader &StructColumnReader::GetChildReader(idx_t child_idx) {
	if (!child_readers[child_idx]) {
		throw InternalException("StructColumnReader::GetChildReader(%d) - but this child reader is not set", child_idx);
	}
	return *child_readers[child_idx].get();
}

void StructColumnReader::InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows,
                                        const duckdb_parquet::ColumnChunkList &columns, TProtocol &protocol_p) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->InitializeRead(row_group_idx_p, row_group_num_rows, columns, protocol_p);
	}
}

idx_t StructColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	auto &struct_entries = StructVector::GetEntries(result);
	D_ASSERT(StructType::GetChildTypes(Type()).size() == struct_entries.size());

	if (pending_skips > 0) {
		throw InternalException("StructColumnReader cannot have pending skips");
	}
	auto &num_values = input.num_values;
	auto &define_out = input.define_out;
	auto &repeat_out = input.repeat_out;

	// If the child reader values are all valid, "define_out" may not be initialized at all
	// So, we just initialize them to all be valid beforehand
	std::fill_n(define_out, num_values, MaxDefine());

	optional_idx read_count;
	for (idx_t i = 0; i < child_readers.size(); i++) {
		auto &child = child_readers[i];
		auto &target_vector = struct_entries[i];
		if (!child) {
			// if we are not scanning this vector - set it to NULL
			ConstantVector::SetNull(target_vector, count_t(num_values));
			continue;
		}
		ColumnReaderInput child_input(num_values, define_out, repeat_out);
		auto child_num_values = child->Read(child_input, target_vector);
		if (!read_count.IsValid()) {
			read_count = child_num_values;
		} else if (read_count.GetIndex() != child_num_values) {
			throw std::runtime_error("Struct child row count mismatch");
		}
	}
	if (!read_count.IsValid()) {
		read_count = num_values;
	}
	// set the validity mask for this level
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < read_count.GetIndex(); i++) {
		if (define_out[i] < MaxDefine()) {
			validity.SetInvalid(i);
		}
	}
	// set the parent struct's size so it matches its children (each child reader sets its own size)
	FlatVector::SetSize(result, count_t(read_count.GetIndex()));

	return read_count.GetIndex();
}

void StructColumnReader::Skip(idx_t num_values) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->Skip(num_values);
	}
}

void StructColumnReader::RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->RegisterPrefetch(transport, allow_merge);
	}
}

void StructColumnReader::InitializePageIndex(const ParquetRowGroupPageIndex &page_index) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->InitializePageIndex(page_index);
	}
}

void StructColumnReader::GatherColumnChunks(unordered_set<idx_t> &column_chunks) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->GatherColumnChunks(column_chunks);
	}
}

static bool TypeHasExactRowCount(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return false;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		for (auto &kv : StructType::GetChildTypes(type)) {
			if (TypeHasExactRowCount(kv.second)) {
				return true;
			}
		}
		return false;
	default:
		return true;
	}
}

idx_t StructColumnReader::GroupRowsAvailable() {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		if (TypeHasExactRowCount(child->Type())) {
			return child->GroupRowsAvailable();
		}
	}
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		return child->GroupRowsAvailable();
	}
	throw InternalException("No projected columns in struct?");
}

} // namespace duckdb
