#include <stdint.h>
#include <utility>

#include "duckdb/common/vector/list_vector.hpp"
#include "reader/list_column_reader.hpp"
#include "parquet_reader.hpp"
#include "column_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {
struct ParquetColumnSchema;

struct ListReaderData {
	ListReaderData(list_entry_t *result_ptr, ValidityMask &result_mask)
	    : result_ptr(result_ptr), result_mask(result_mask) {
	}

	list_entry_t *result_ptr;
	ValidityMask &result_mask;
};

struct TemplatedListReader {
	using DATA = ListReaderData;

	static DATA Initialize(optional_ptr<Vector> result_out) {
		D_ASSERT(ListVector::GetListSize(*result_out) == 0);

		auto result_ptr = FlatVector::GetDataMutable<list_entry_t>(*result_out);
		auto &result_mask = FlatVector::ValidityMutable(*result_out);
		return ListReaderData(result_ptr, result_mask);
	}

	static idx_t GetOffset(optional_ptr<Vector> result_out) {
		return ListVector::GetListSize(*result_out);
	}

	static void HandleRepeat(DATA &data, idx_t offset) {
		data.result_ptr[offset].length++;
	}

	static void HandleListStart(DATA &data, idx_t offset, idx_t offset_in_child, idx_t length) {
		data.result_ptr[offset].offset = offset_in_child;
		data.result_ptr[offset].length = length;
	}

	static void HandleNull(DATA &data, idx_t offset) {
		data.result_mask.SetInvalid(offset);
		data.result_ptr[offset].offset = 0;
		data.result_ptr[offset].length = 0;
	}

	static void AppendVector(optional_ptr<Vector> result_out, const Vector &read_vector, idx_t child_idx) {
		ListVector::Append(*result_out, read_vector, child_idx);
	}
};

struct TemplatedListSkipper {
	using DATA = bool;

	static DATA Initialize(optional_ptr<Vector>) {
		return false;
	}

	static idx_t GetOffset(optional_ptr<Vector>) {
		return 0;
	}

	static void HandleRepeat(DATA &, idx_t) {
	}

	static void HandleListStart(DATA &, idx_t, idx_t, idx_t) {
	}

	static void HandleNull(DATA &, idx_t) {
	}

	static void AppendVector(optional_ptr<Vector>, Vector &, idx_t) {
	}
};

template <class OP>
idx_t ListColumnReader::ReadInternal(ColumnReaderInput &input, optional_ptr<Vector> result_out) {
	idx_t result_offset = 0;
	auto data = OP::Initialize(result_out);

	auto &num_values = input.num_values;
	auto &define_out = input.define_out;
	auto &repeat_out = input.repeat_out;

	// if an individual list is longer than STANDARD_VECTOR_SIZE we actually have to loop the child read to fill it
	bool finished = false;
	while (!finished) {
		idx_t child_actual_num_values = 0;

		// check if we have any overflow from a previous read
		if (overflow_child_count == 0) {
			// we don't: read elements from the child reader
			child_defines.zero();
			child_repeats.zero();
			// we don't know in advance how many values to read because of the beautiful repetition/definition setup
			// we just read (up to) a vector from the child column, and see if we have read enough
			// if we have not read enough, we read another vector
			// if we have read enough, we leave any unhandled elements in the overflow vector for a subsequent read
			auto child_req_num_values =
			    MinValue<idx_t>(STANDARD_VECTOR_SIZE, child_column_reader->GroupRowsAvailable());
			read_vector.ResetFromCache(read_cache);

			ColumnReaderInput child_input(child_req_num_values, child_defines_ptr, child_repeats_ptr);
			child_actual_num_values = child_column_reader->Read(child_input, read_vector);
		} else {
			// we do: use the overflow values
			child_actual_num_values = overflow_child_count;
			overflow_child_count = 0;
		}

		if (child_actual_num_values == 0) {
			// no more elements available: we are done
			break;
		}
		read_vector.Verify();
		idx_t current_chunk_offset = OP::GetOffset(result_out);

		// hard-won piece of code this, modify at your own risk
		// the intuition is that we have to only collapse values into lists that are repeated *on this level*
		// the rest is pretty much handed up as-is as a single-valued list or NULL
		idx_t child_idx;
		for (child_idx = 0; child_idx < child_actual_num_values; child_idx++) {
			if (child_repeats_ptr[child_idx] == MaxRepeat()) {
				// value repeats on this level, append
				if (result_offset == 0) {
					throw DataCorruptionException(
					    "Corrupt Parquet file: a value in list column \"%s\" repeats a list before any list started",
					    Schema().name);
				}
				OP::HandleRepeat(data, result_offset - 1);
				continue;
			}

			if (result_offset >= num_values) {
				// we ran out of output space
				finished = true;
				break;
			}
			if (child_defines_ptr[child_idx] >= MaxDefine()) {
				// value has been defined down the stack, hence its NOT NULL
				OP::HandleListStart(data, result_offset, child_idx + current_chunk_offset, 1);
			} else if (child_defines_ptr[child_idx] == MaxDefine() - 1) {
				// empty list
				OP::HandleListStart(data, result_offset, child_idx + current_chunk_offset, 0);
			} else {
				// value is NULL somewhere up the stack
				OP::HandleNull(data, result_offset);
			}

			if (repeat_out) {
				repeat_out[result_offset] = child_repeats_ptr[child_idx];
			}
			if (define_out) {
				define_out[result_offset] = child_defines_ptr[child_idx];
			}

			result_offset++;
		}
		// actually append the required elements to the child list
		OP::AppendVector(result_out, read_vector, child_idx);

		// we have read more values from the child reader than we can fit into the result for this read
		// we have to pass everything from child_idx to child_actual_num_values into the next call
		if (child_idx < child_actual_num_values && result_offset == num_values) {
			read_vector.Slice(read_vector, child_idx, child_actual_num_values);
			overflow_child_count = child_actual_num_values - child_idx;
			read_vector.Verify();

			// move values in the child repeats and defines *backward* by child_idx
			for (idx_t repdef_idx = 0; repdef_idx < overflow_child_count; repdef_idx++) {
				child_defines_ptr[repdef_idx] = child_defines_ptr[child_idx + repdef_idx];
				child_repeats_ptr[repdef_idx] = child_repeats_ptr[child_idx + repdef_idx];
			}
		}
	}
	return result_offset;
}

idx_t ListColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	ApplyPendingSkips(input.define_out, input.repeat_out);
	return ReadInternal<TemplatedListReader>(input, result);
}

ListColumnReader::ListColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema,
                                   unique_ptr<ColumnReader> child_column_reader_p)
    : ColumnReader(reader, schema), child_column_reader(std::move(child_column_reader_p)),
      read_cache(reader.allocator, ListType::GetChildType(Type())), read_vector(read_cache), overflow_child_count(0) {
	child_defines.resize(reader.allocator, STANDARD_VECTOR_SIZE);
	child_repeats.resize(reader.allocator, STANDARD_VECTOR_SIZE);
	child_defines_ptr = (uint8_t *)child_defines.ptr;
	child_repeats_ptr = (uint8_t *)child_repeats.ptr;
	if (child_column_reader) {
		child_column_reader->SetParent(*this);
	}
}

ColumnReader &ListColumnReader::GetChildReader() {
	return *child_column_reader;
}

void ListColumnReader::ApplyPendingSkips(data_ptr_t define_out, data_ptr_t repeat_out) {
	if (pending_skips == 0) {
		return;
	}
	ColumnReaderInput empty_input(pending_skips, nullptr, nullptr);
	ReadInternal<TemplatedListSkipper>(empty_input, nullptr);
	pending_skips = 0;
}

} // namespace duckdb
