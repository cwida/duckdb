#include <stdint.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "duckdb/common/vector/struct_vector.hpp"
#include "reader/variant_column_reader.hpp"
#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"
#include "column_reader.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "parquet_column_schema.hpp"

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
class ClientContext;
class ParquetReader;
class ThriftFileTransport;

static vector<VariantPathComponent> GetVariantExtractPath(const ColumnIndex &index) {
	vector<VariantPathComponent> result;
	if (!index.IsPushdownExtract()) {
		return result;
	}
	reference<const ColumnIndex> current(index.GetChildIndex(0));
	while (true) {
		if (current.get().HasPrimaryIndex()) {
			throw InternalException("VARIANT pushdown extract expected a field name path");
		}
		result.emplace_back(current.get().GetFieldName());
		if (!current.get().HasChildren()) {
			break;
		}
		current = current.get().GetChildIndex(0);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Variant Column Reader
//===--------------------------------------------------------------------===//
VariantColumnReader::VariantColumnReader(ClientContext &context, const ParquetReader &reader,
                                         const ParquetColumnSchema &schema,
                                         vector<unique_ptr<ColumnReader>> child_readers_p,
                                         const struct ColumnIndex &index)
    : ColumnReader(reader, schema), context(context), index(index), extract_path(GetVariantExtractPath(index)),
      child_readers(std::move(child_readers_p)) {
	D_ASSERT(Type().InternalType() == PhysicalType::STRUCT);

	for (auto &child : child_readers) {
		if (child) {
			child->SetParent(*this);
		}
	}

	if (child_readers.size() < 2 || !child_readers[0] || !child_readers[1]) {
		throw DataCorruptionException("Failed to read file \"%s\": the Variant column must have 'metadata' and 'value' "
		                              "as the first two columns",
		                              reader.GetFileName());
	}
	if (child_readers[0]->Schema().name == "metadata" && child_readers[1]->Schema().name == "value") {
		metadata_reader_idx = 0;
		value_reader_idx = 1;
	} else if (child_readers[1]->Schema().name == "metadata" && child_readers[0]->Schema().name == "value") {
		metadata_reader_idx = 1;
		value_reader_idx = 0;
	} else {
		throw DataCorruptionException("Failed to read file \"%s\": the Variant column must have 'metadata' and 'value' "
		                              "as the first two columns",
		                              reader.GetFileName());
	}
}

VariantColumnReader::VariantColumnReader(ClientContext &context, const ParquetReader &reader,
                                         const ParquetColumnSchema &schema,
                                         vector<unique_ptr<ColumnReader>> child_readers_p)
    : VariantColumnReader(context, reader, schema, std::move(child_readers_p), {}) {
}

ColumnReader &VariantColumnReader::GetChildReader(idx_t child_idx) {
	if (!child_readers[child_idx]) {
		throw InternalException("VariantColumnReader::GetChildReader(%d) - but this child reader is not set",
		                        child_idx);
	}
	return *child_readers[child_idx].get();
}

void VariantColumnReader::InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows,
                                         const vector<ColumnChunk> &columns, TProtocol &protocol_p) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->InitializeRead(row_group_idx_p, row_group_num_rows, columns, protocol_p);
	}
}

unique_ptr<BaseStatistics> VariantColumnReader::Stats(idx_t row_group_idx_p, const vector<ColumnChunk> &columns) {
	auto result = ColumnReader::Stats(row_group_idx_p, columns);
	if (result && index.IsPushdownExtract()) {
		auto storage_index = StorageIndex::FromColumnIndex(index);
		return result->PushdownExtract(storage_index.GetChildIndexes()[0]);
	}
	return result;
}

static LogicalType GetIntermediateGroupType(optional_ptr<ColumnReader> typed_value) {
	child_list_t<LogicalType> children;
	children.emplace_back("value", LogicalType::BLOB);
	if (typed_value) {
		children.emplace_back("typed_value", typed_value->Type());
	}
	return LogicalType::STRUCT(std::move(children));
}

void VariantColumnReader::PrepareChunk(DataChunk &chunk, idx_t &capacity, const vector<LogicalType> &types,
                                       idx_t count) {
	bool needs_init = chunk.ColumnCount() != types.size() || count > capacity;
	for (idx_t i = 0; !needs_init && i < types.size(); i++) {
		needs_init = chunk.data[i].GetType() != types[i];
	}
	if (needs_init) {
		chunk.Destroy();
		chunk.Initialize(context, types, count);
		capacity = count;
	} else {
		chunk.Reset();
	}
}

idx_t VariantColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	if (pending_skips > 0) {
		throw InternalException("VariantColumnReader cannot have pending skips");
	}
	optional_ptr<ColumnReader> typed_value_reader = child_readers.size() == 3 ? child_readers[2].get() : nullptr;

	auto &num_values = input.num_values;
	auto &define_out = input.define_out;
	auto &repeat_out = input.repeat_out;

	// If the child reader values are all valid, "define_out" may not be initialized at all
	// So, we just initialize them to all be valid beforehand
	std::fill_n(define_out, num_values, MaxDefine());

	auto group_type = GetIntermediateGroupType(typed_value_reader);
	PrepareChunk(intermediate_chunk, intermediate_capacity, {LogicalType::BLOB, group_type}, num_values);
	auto &metadata_intermediate = intermediate_chunk.data[0];
	auto &intermediate_group = intermediate_chunk.data[1];
	auto &group_entries = StructVector::GetEntries(intermediate_group);
	auto &value_intermediate = group_entries[0];

	ColumnReaderInput metadata_reader_input(num_values, define_out, repeat_out);
	auto metadata_values = child_readers[metadata_reader_idx]->Read(metadata_reader_input, metadata_intermediate);

	ColumnReaderInput value_reader_input(num_values, define_out, repeat_out);
	auto value_values = child_readers[value_reader_idx]->Read(value_reader_input, value_intermediate);

	D_ASSERT(child_readers[metadata_reader_idx]->Schema().name == "metadata");
	D_ASSERT(child_readers[value_reader_idx]->Schema().name == "value");

	if (metadata_values != value_values) {
		throw InvalidInputException(
		    "The Variant column did not contain the same amount of values for 'metadata' and 'value'");
	}

	if (typed_value_reader) {
		ColumnReaderInput child_input(num_values, define_out, repeat_out);
		auto typed_values = typed_value_reader->Read(child_input, group_entries[1]);
		if (typed_values != value_values) {
			throw InvalidInputException(
			    "The shredded Variant column did not contain the same amount of values for 'typed_value' and 'value'");
		}
	}
	// convert the actual columns
	Convert(metadata_intermediate, intermediate_group, result, num_values);
	if (index.IsPushdownExtract()) {
		D_ASSERT(!extract_path.empty());
		Vector extract_result(LogicalType::VARIANT(), num_values);
		VariantUtils::VariantExtract(result, extract_path, extract_result, num_values);
		result.Reference(extract_result);
	}

	return value_values;
}

void VariantColumnReader::Skip(idx_t num_values) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->Skip(num_values);
	}
}

void VariantColumnReader::RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->RegisterPrefetch(transport, allow_merge);
	}
}

void VariantColumnReader::InitializePageIndex(const ParquetRowGroupPageIndex &page_index) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->InitializePageIndex(page_index);
	}
}

void VariantColumnReader::GatherColumnChunks(unordered_set<idx_t> &column_chunks) {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		child->GatherColumnChunks(column_chunks);
	}
}

idx_t VariantColumnReader::GroupRowsAvailable() {
	for (auto &child : child_readers) {
		if (!child) {
			continue;
		}
		return child->GroupRowsAvailable();
	}
	throw InternalException("No projected columns in struct?");
}

bool VariantColumnReader::TypedValueLayoutToType(const LogicalType &typed_value, LogicalType &output) {
	if (!typed_value.IsNested()) {
		output = typed_value;
		return true;
	}
	auto type_id = typed_value.id();
	if (StructType::IsStruct(type_id)) {
		//! OBJECT (...)
		auto &object_fields = StructType::GetChildTypes(typed_value);
		child_list_t<LogicalType> children;
		for (auto &object_field : object_fields) {
			auto &name = object_field.first;
			auto &field = object_field.second;
			if (name.empty()) {
				//! An empty key can't be a STRUCT member name
				return false;
			}
			//! <name>: {
			//! 	value: BLOB,
			//! 	typed_value: <type>
			//! }
			auto &field_children = StructType::GetChildTypes(field);
			idx_t index = DConstants::INVALID_INDEX;
			for (idx_t i = 0; i < field_children.size(); i++) {
				if (field_children[i].first == "typed_value") {
					index = i;
					break;
				}
			}
			if (index == DConstants::INVALID_INDEX) {
				//! FIXME: we might be able to just omit this field from the OBJECT, instead of flat-out failing the
				//! conversion No 'typed_value' field, so we can't assign a structured type to this field at all
				return false;
			}
			LogicalType child_type;
			if (!TypedValueLayoutToType(field_children[index].second, child_type)) {
				return false;
			}
			children.emplace_back(name, child_type);
		}
		output = LogicalType::STRUCT(std::move(children));
		return true;
	}
	if (type_id == LogicalTypeId::LIST) {
		//! ARRAY
		auto &element = ListType::GetChildType(typed_value);
		//! element: {
		//! 	value: BLOB,
		//! 	typed_value: <type>
		//! }
		auto &element_children = StructType::GetChildTypes(element);
		idx_t index = DConstants::INVALID_INDEX;
		for (idx_t i = 0; i < element_children.size(); i++) {
			if (element_children[i].first == "typed_value") {
				index = i;
				break;
			}
		}
		if (index == DConstants::INVALID_INDEX) {
			//! This *might* be allowed by the spec, it's hard to reason about..
			return false;
		}
		LogicalType child_type;
		if (!TypedValueLayoutToType(element_children[index].second, child_type)) {
			return false;
		}
		output = LogicalType::LIST(child_type);
		return true;
	}
	throw InvalidInputException("VARIANT typed value has to be a primitive/struct/list, not %s",
	                            typed_value.ToString());
}

} // namespace duckdb
