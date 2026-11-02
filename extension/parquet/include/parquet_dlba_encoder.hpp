//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_dlba_encoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "parquet_dbp_encoder.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace duckdb {

class DlbaEncoder {
public:
	DlbaEncoder(const idx_t total_value_count_p, const idx_t string_buffer_size_p)
	    : dbp_encoder(total_value_count_p), string_buffer_size(string_buffer_size_p) {
	}

public:
	template <class T>
	void BeginWrite(Allocator &, WriteStream &, const T &) {
		throw InternalException("DlbaEncoder should only be used with strings");
	}

	template <class T>
	void WriteValue(WriteStream &, const T &) {
		throw InternalException("DlbaEncoder should only be used with strings");
	}

	void FinishWrite(WriteStream &writer) {
		dbp_encoder.FinishWrite(writer);
		writer.WriteData(buffer.get(), stream->GetPosition());
	}

private:
	DbpEncoder dbp_encoder;
	const idx_t string_buffer_size;
	AllocatedData buffer;
	unsafe_unique_ptr<MemoryStream> stream;
};

template <>
inline void DlbaEncoder::BeginWrite(Allocator &allocator, WriteStream &writer, const string_t &first_value) {
	buffer = allocator.Allocate(string_buffer_size + 1);
	stream = make_unsafe_uniq<MemoryStream>(buffer.get(), buffer.GetSize());
	dbp_encoder.BeginWrite(writer, UnsafeNumericCast<int64_t>(first_value.GetSize()));
	stream->WriteData(const_data_ptr_cast(first_value.GetData()), first_value.GetSize());
}

template <>
inline void DlbaEncoder::WriteValue(WriteStream &writer, const string_t &value) {
	dbp_encoder.WriteValue(writer, UnsafeNumericCast<int64_t>(value.GetSize()));
	stream->WriteData(const_data_ptr_cast(value.GetData()), value.GetSize());
}

} // namespace duckdb
