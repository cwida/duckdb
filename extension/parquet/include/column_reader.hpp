#pragma once

#include "parquet_types.h"
#include "thrift_tools.hpp"
#include "resizable_buffer.hpp"

#include "parquet_rle_bp_decoder.hpp"
#include "parquet_statistics.hpp"

#include "duckdb/storage/statistics/string_statistics.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/chunk_collection.hpp"

namespace duckdb {

using apache::thrift::protocol::TProtocol;

using parquet::format::ColumnChunk;
using parquet::format::FieldRepetitionType;
using parquet::format::PageHeader;
using parquet::format::SchemaElement;

typedef nullmask_t parquet_filter_t;

class ColumnReader {

public:
	static unique_ptr<ColumnReader> CreateReader(LogicalType type_p, const SchemaElement &schema_p, idx_t schema_idx_p,
	                                             idx_t max_define, idx_t max_repeat);

	ColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t file_idx_p, idx_t max_define_p,
	             idx_t max_repeat_p)
	    : schema(schema_p), file_idx(file_idx_p), max_define(max_define_p), max_repeat(max_repeat_p), type(type_p),
	      page_rows_available(0) {

		// dummies for Skip()
		dummy_result.Initialize(Type());
		none_filter.none();
		dummy_define.resize(STANDARD_VECTOR_SIZE);
		dummy_repeat.resize(STANDARD_VECTOR_SIZE);
	};

	virtual void IntializeRead(const std::vector<ColumnChunk> &columns, TProtocol &protocol_p) {
		D_ASSERT(file_idx < columns.size());
		chunk = &columns[file_idx];
		protocol = &protocol_p;
		D_ASSERT(chunk);
		D_ASSERT(chunk->__isset.meta_data);

		if (chunk->__isset.file_path) {
			throw std::runtime_error("Only inlined data files are supported (no references)");
		}

		// ugh. sometimes there is an extra offset for the dict. sometimes it's wrong.
		chunk_read_offset = chunk->meta_data.data_page_offset;
		if (chunk->meta_data.__isset.dictionary_page_offset && chunk->meta_data.dictionary_page_offset >= 4) {
			// this assumes the data pages follow the dict pages directly.
			chunk_read_offset = chunk->meta_data.dictionary_page_offset;
		}
		group_rows_available = chunk->meta_data.num_values;
	}

	virtual ~ColumnReader();

	virtual idx_t Read(uint64_t num_values, parquet_filter_t &filter, uint8_t *define_out, uint8_t *repeat_out,
	                   Vector &result_out);

	virtual void Skip(idx_t num_values);

	const LogicalType Type() {
		return type;
	}

	const SchemaElement &Schema() {
		return schema;
	}

	virtual idx_t GroupRowsAvailable() {
		return group_rows_available;
	}

	unique_ptr<BaseStatistics> Stats(const std::vector<ColumnChunk> &columns) {
		if (Type().id() == LogicalTypeId::LIST || Type().id() == LogicalTypeId::STRUCT) {
			return nullptr;
		}
		return parquet_transform_column_statistics(Schema(), Type(), columns[file_idx]);
	}

protected:
	// readers that use the default Read() need to implement those
	virtual void Plain(shared_ptr<ByteBuffer> plain_data, uint8_t *defines, idx_t num_values, parquet_filter_t &filter,
	                   idx_t result_offset, Vector &result) {
		throw NotImplementedException("Plain");
	}

	virtual void Dictionary(shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) {
		throw NotImplementedException("Dictionary");
	}

	virtual void Offsets(uint32_t *offsets, uint8_t *defines, idx_t num_values, parquet_filter_t &filter,
	                     idx_t result_offset, Vector &result) {
		throw NotImplementedException("Offsets");
	}

	// these are nops for most types, but not for strings
	virtual void DictReference(Vector &result) {
	}
	virtual void PlainReference(shared_ptr<ByteBuffer>, Vector &result) {
	}

	bool HasDefines() {
		return max_define > 0;
	}

	bool HasRepeats() {
		return max_repeat > 0;
	}

	const SchemaElement &schema;

	idx_t file_idx;
	idx_t max_define;
	idx_t max_repeat;

private:
	void PrepareRead(parquet_filter_t &filter);
	void PreparePage(idx_t compressed_page_size, idx_t uncompressed_page_size);
	void PrepareDataPage(PageHeader &page_hdr);

	LogicalType type;
	const parquet::format::ColumnChunk *chunk;

	apache::thrift::protocol::TProtocol *protocol;
	idx_t page_rows_available;
	idx_t group_rows_available;
	idx_t chunk_read_offset;

	shared_ptr<ResizeableBuffer> block;

	ResizeableBuffer offset_buffer;

	unique_ptr<RleBpDecoder> dict_decoder;
	unique_ptr<RleBpDecoder> defined_decoder;
	unique_ptr<RleBpDecoder> repeated_decoder;

	// dummies for Skip()
	Vector dummy_result;
	parquet_filter_t none_filter;
	ResizeableBuffer dummy_define;
	ResizeableBuffer dummy_repeat;
};

template <class T> static bool IsValid(T value) {
    return true;
}

// TODO this throws warnings :/
template <> bool IsValid(float value) {
    return Value::FloatIsValid(value);
}

template <> bool IsValid(double value) {
    return Value::DoubleIsValid(value);
}

template <class VALUE_TYPE> class TemplatedColumnReader : public ColumnReader {

public:
	TemplatedColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t schema_idx_p, idx_t max_define_p,
	                      idx_t max_repeat_p)
	    : ColumnReader(type_p, schema_p, schema_idx_p, max_define_p, max_repeat_p), dict_size(0){};

	void Dictionary(shared_ptr<ByteBuffer> data, idx_t num_entries) override {
		dict = move(data);
		dict_size = num_entries;
	}

	void Offsets(uint32_t *offsets, uint8_t *defines, uint64_t num_values, parquet_filter_t &filter,
	             idx_t result_offset, Vector &result) override {
		auto result_ptr = FlatVector::GetData<VALUE_TYPE>(result);

		idx_t offset_idx = 0;
		for (idx_t row_idx = 0; row_idx < num_values; row_idx++) {
			if (HasDefines() && defines[row_idx + result_offset] != max_define) {
				FlatVector::SetNull(result, row_idx + result_offset, true);
				continue;
			}
			if (filter[row_idx + result_offset]) {
				VALUE_TYPE val = DictRead(offsets[offset_idx++]);
				if (!IsValid(val)) {
					FlatVector::SetNull(result, row_idx + result_offset, true);
					continue;
				}
				result_ptr[row_idx + result_offset] = val;
			} else {
				offset_idx++;
			}
		}
	}

	void Plain(shared_ptr<ByteBuffer> plain_data, uint8_t *defines, uint64_t num_values, parquet_filter_t &filter,
	           idx_t result_offset, Vector &result) override {
		auto result_ptr = FlatVector::GetData<VALUE_TYPE>(result);
		for (idx_t row_idx = 0; row_idx < num_values; row_idx++) {
			if (HasDefines() && defines[row_idx + result_offset] != max_define) {
				FlatVector::SetNull(result, row_idx + result_offset, true);
				continue;
			}
			if (filter[row_idx + result_offset]) {
				VALUE_TYPE val = PlainRead(*plain_data);
				if (!IsValid(val)) {
					FlatVector::SetNull(result, row_idx + result_offset, true);
					continue;
				}
				result_ptr[row_idx + result_offset] = val;
			} else { // there is still some data there that we have to skip over
				PlainSkip(*plain_data);
			}
		}
	}

protected:
	virtual VALUE_TYPE DictRead(uint32_t &offset) {
		D_ASSERT(offset < dict_size);
		return ((VALUE_TYPE *)dict->ptr)[offset];
	}

	virtual VALUE_TYPE PlainRead(ByteBuffer &plain_data) {
		return plain_data.read<VALUE_TYPE>();
	}

	virtual void PlainSkip(ByteBuffer &plain_data) {
		plain_data.inc(sizeof(VALUE_TYPE));
	}

	shared_ptr<ByteBuffer> dict;
	idx_t dict_size;

};

class StringColumnReader : public TemplatedColumnReader<string_t> {

public:
	StringColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t schema_idx_p, idx_t max_define_p,
	                   idx_t max_repeat_p)
	    : TemplatedColumnReader<string_t>(type_p, schema_p, schema_idx_p, max_define_p, max_repeat_p){};

	void Dictionary(shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) override;

protected:
	string_t PlainRead(ByteBuffer &plain_data) override;
	void PlainSkip(ByteBuffer &plain_data) override;
	string_t DictRead(uint32_t &offset) override;

	void DictReference(Vector &result) override;
	void PlainReference(shared_ptr<ByteBuffer> plain_data, Vector &result) override;

private:
	unique_ptr<string_t[]> dict_strings;
	void VerifyString(LogicalTypeId id, const char *str_data, idx_t str_len);
};

template <class PARQUET_PHYSICAL_TYPE, timestamp_t (*FUNC)(const PARQUET_PHYSICAL_TYPE &input)>
class TimestampColumnReader : public TemplatedColumnReader<timestamp_t> {

public:
	TimestampColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t file_idx_p, idx_t max_define_p,
	                      idx_t max_repeat_p)
	    : TemplatedColumnReader<timestamp_t>(type_p, schema_p, file_idx_p, max_define_p, max_repeat_p){};

protected:
	void Dictionary(shared_ptr<ByteBuffer> dictionary_data, idx_t num_entries) {
		dict = make_shared<ResizeableBuffer>(num_entries * sizeof(timestamp_t));
		auto dict_ptr = (timestamp_t *)dict->ptr;
		for (idx_t i = 0; i < num_entries; i++) {
			dict_ptr[i] = FUNC(dictionary_data->read<PARQUET_PHYSICAL_TYPE>());
		}
		dict_size = num_entries;
	}

	void Skip(idx_t num_values) {
		D_ASSERT(0);
	}

	timestamp_t PlainRead(ByteBuffer &plain_data) {
		return FUNC(plain_data.read<PARQUET_PHYSICAL_TYPE>());
	}

	void PlainSkip(ByteBuffer &plain_data) {
		plain_data.inc(sizeof(PARQUET_PHYSICAL_TYPE));
	}
};

class BooleanColumnReader : public TemplatedColumnReader<bool> {
public:
	BooleanColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t schema_idx_p, idx_t max_define_p,
	                    idx_t max_repeat_p)
	    : TemplatedColumnReader<bool>(type_p, schema_p, schema_idx_p, max_define_p, max_repeat_p), byte_pos(0){};
	bool PlainRead(ByteBuffer &plain_data) override {
		plain_data.available(1);
		bool ret = (*plain_data.ptr >> byte_pos) & 1;
		byte_pos++;
		if (byte_pos == 8) {
			byte_pos = 0;
			plain_data.inc(1);
		}
		return ret;
	}
	void PlainSkip(ByteBuffer &plain_data) override {
		PlainRead(plain_data);
	}

private:
	uint8_t byte_pos;
};

class StructColumnReader : public ColumnReader {

public:
	StructColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t schema_idx_p, idx_t max_define_p,
	                   idx_t max_repeat_p, vector<unique_ptr<ColumnReader>> child_readers_p)
	    : ColumnReader(type_p, schema_p, schema_idx_p, max_define_p, max_repeat_p),
	      child_readers(move(child_readers_p)) {
		D_ASSERT(type_p.id() == LogicalTypeId::STRUCT);
		D_ASSERT(!type_p.child_types().empty());
	};

	ColumnReader *GetChildReader(idx_t child_idx) {
		return child_readers[child_idx].get();
	}

	void IntializeRead(const std::vector<ColumnChunk> &columns, TProtocol &protocol_p) override {
		for (auto &child : child_readers) {
			child->IntializeRead(columns, protocol_p);
		}
	}

	idx_t Read(uint64_t num_values, parquet_filter_t &filter, uint8_t *define_out, uint8_t *repeat_out,
	           Vector &result) override {
		result.Initialize(Type());

		for (idx_t i = 0; i < Type().child_types().size(); i++) {
			auto child_read = make_unique<Vector>();
			child_read->Initialize(Type().child_types()[i].second);
			auto child_num_values = child_readers[i]->Read(num_values, filter, define_out, repeat_out, *child_read);
			D_ASSERT(child_num_values == num_values);
			StructVector::AddEntry(result, Type().child_types()[i].first, move(child_read));
		}

		return num_values;
	}

	virtual void Skip(idx_t num_values) override {
		D_ASSERT(0);
	}

	idx_t GroupRowsAvailable() override {
		return child_readers[0]->GroupRowsAvailable();
	}

	vector<unique_ptr<ColumnReader>> child_readers;
};

class ListColumnReader : public ColumnReader {
public:
	ListColumnReader(LogicalType type_p, const SchemaElement &schema_p, idx_t schema_idx_p, idx_t max_define_p,
	                 idx_t max_repeat_p, unique_ptr<ColumnReader> child_column_reader_p)
	    : ColumnReader(type_p, schema_p, schema_idx_p, max_define_p, max_repeat_p),
	      child_column_reader(move(child_column_reader_p)), overflow_child_count(0) {

		child_defines.resize(STANDARD_VECTOR_SIZE);
		child_repeats.resize(STANDARD_VECTOR_SIZE);
		child_defines_ptr = (uint8_t *)child_defines.ptr;
		child_repeats_ptr = (uint8_t *)child_repeats.ptr;

		auto child_type = Type().child_types()[0].second;
		child_result.Initialize(child_type);

		vector<LogicalType> append_chunk_types;
		append_chunk_types.push_back(child_type);
		append_chunk.Initialize(append_chunk_types);

		child_filter.set();
	};

	idx_t Read(uint64_t num_values, parquet_filter_t &filter, uint8_t *define_out, uint8_t *repeat_out,
	           Vector &result_out) override;

	virtual void Skip(idx_t num_values) override {
		D_ASSERT(0);
	}

	void IntializeRead(const std::vector<ColumnChunk> &columns, TProtocol &protocol_p) override {
		child_column_reader->IntializeRead(columns, protocol_p);
	}

	idx_t GroupRowsAvailable() override {
		return child_column_reader->GroupRowsAvailable();
	}

private:
	unique_ptr<ColumnReader> child_column_reader;
	ResizeableBuffer child_defines;
	ResizeableBuffer child_repeats;
	uint8_t *child_defines_ptr;
	uint8_t *child_repeats_ptr;

	Vector child_result;
	parquet_filter_t child_filter;
	DataChunk append_chunk;

	Vector overflow_child_vector;
	idx_t overflow_child_count;
};

} // namespace duckdb