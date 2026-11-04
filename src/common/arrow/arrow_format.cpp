#include "duckdb/common/arrow/arrow_format.hpp"

#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Shared exports
//===--------------------------------------------------------------------===//
namespace {

//! Also releases a partly built export, whose n_children counts only the children attached so far
void ReleaseSharedArray(ArrowArray *array) {
	if (!array || !array->release) {
		return;
	}
	array->release = nullptr;
	for (int64_t i = 0; i < array->n_children; i++) {
		auto child = array->children[i];
		// A child without a release was moved out and is released by whoever took it
		if (child->release) {
			child->release(child);
		}
		delete child;
	}
	delete[] array->children;
	if (array->dictionary) {
		if (array->dictionary->release) {
			array->dictionary->release(array->dictionary);
		}
		delete array->dictionary;
	}
	delete static_cast<ArrowArrayOwner *>(array->private_data);
	array->children = nullptr;
	array->dictionary = nullptr;
	array->private_data = nullptr;
}

ArrowArray *NewSharedNode() {
	auto node = make_uniq<ArrowArray>();
	node->release = ReleaseSharedArray;
	return node.release();
}

//! The target's release is installed before the call and each allocation is attached as soon as it is made,
//! so a throw leaves nothing that release cannot free
void FillSharedArray(ArrowArray &target, const ArrowArray &source, const ArrowArrayOwner &owner) {
	target.private_data = make_uniq<ArrowArrayOwner>(owner).release();
	target.length = source.length;
	target.null_count = source.null_count;
	target.offset = source.offset;
	target.n_buffers = source.n_buffers;
	target.buffers = source.buffers;
	if (source.n_children > 0) {
		target.children = make_unsafe_uniq_array<ArrowArray *>(NumericCast<idx_t>(source.n_children)).release();
		for (int64_t i = 0; i < source.n_children; i++) {
			target.children[i] = NewSharedNode();
			target.n_children = i + 1;
			FillSharedArray(*target.children[i], *source.children[i], owner);
		}
	}
	if (source.dictionary) {
		target.dictionary = NewSharedNode();
		FillSharedArray(*target.dictionary, *source.dictionary, owner);
	}
}

} // namespace

ArrowFormatGlobalState::ArrowFormatGlobalState(const ResultFormatContext &context)
    : types(context.types), properties(context.client_properties) {
	if (!properties.client_context) {
		throw InternalException("The Arrow format needs the client context of the query that submitted it");
	}
	extension_types = ArrowTypeExtensionData::GetExtensionTypes(*properties.client_context, types);
	ArrowConverter::ToArrowSchema(&schema.arrow_schema, types, IdentifiersToStrings(context.names), properties);
}

ArrowFormatGlobalState::~ArrowFormatGlobalState() {
}

//===--------------------------------------------------------------------===//
// ArrowFormat
//===--------------------------------------------------------------------===//
namespace {

class ArrowUnit : public ResultUnit {
public:
	ArrowUnit(idx_t row_count, idx_t byte_size, unique_ptr<ArrowArrayWrapper> array_p)
	    : ResultUnit(row_count, byte_size), array(std::move(array_p)) {
	}

public:
	unique_ptr<ArrowArrayWrapper> array;
};

class ArrowFormatLocalState : public ResultFormatLocalState {
public:
	//! The array being built. Created at the first row that goes into it
	unique_ptr<ArrowAppender> appender;
	//! Arrays that reached the batch size and have not been handed over yet
	deque<unique_ptr<ArrowUnit>> sealed;
};

unique_ptr<ArrowUnit> SealAppender(ArrowFormatLocalState &lstate) {
	// Finalize hands the buffers to the array, so the size has to be read before it
	auto rows = lstate.appender->RowCount();
	auto bytes = lstate.appender->ByteSize();
	// Allocated before Finalize, so nothing can throw between the array's creation and its adoption
	auto array = make_uniq<ArrowArrayWrapper>();
	array->arrow_array = lstate.appender->Finalize();
	lstate.appender.reset();
	return make_uniq<ArrowUnit>(rows, bytes, std::move(array));
}

} // namespace

ArrowFormat::ArrowFormat(idx_t batch_size_p) : batch_size(batch_size_p) {
	if (batch_size == 0) {
		throw InvalidInputException("The Arrow format needs a batch size of at least one row");
	}
}

const char *ArrowFormat::Name() const {
	return NAME;
}

unique_ptr<ResultFormatGlobalState> ArrowFormat::InitGlobal(const ResultFormatContext &context) {
	return make_uniq<ArrowFormatGlobalState>(context);
}

unique_ptr<ResultFormatLocalState> ArrowFormat::InitLocal(ResultFormatGlobalState &gstate) {
	return make_uniq<ArrowFormatLocalState>();
}

void ArrowFormat::AppendToUnit(ResultFormatGlobalState &gstate_p, ResultFormatLocalState &lstate_p, DataChunk &chunk) {
	auto &gstate = gstate_p.Cast<ArrowFormatGlobalState>();
	auto &lstate = lstate_p.Cast<ArrowFormatLocalState>();
	auto count = chunk.size();
	idx_t processed = 0;
	while (processed < count) {
		if (!lstate.appender) {
			auto capacity = MinValue(batch_size, count - processed);
			lstate.appender =
			    make_uniq<ArrowAppender>(gstate.Types(), capacity, gstate.Properties(), gstate.ExtensionTypes());
		}
		auto to_append = MinValue(batch_size - lstate.appender->RowCount(), count - processed);
		lstate.appender->Append(chunk, processed, processed + to_append, count);
		processed += to_append;
		if (lstate.appender->RowCount() >= batch_size) {
			lstate.sealed.push_back(SealAppender(lstate));
		}
	}
}

bool ArrowFormat::IsUnitFinished(ResultFormatLocalState &lstate) {
	return !lstate.Cast<ArrowFormatLocalState>().sealed.empty();
}

unique_ptr<ResultUnit> ArrowFormat::FinishUnit(ResultFormatGlobalState &gstate, ResultFormatLocalState &lstate_p) {
	auto &lstate = lstate_p.Cast<ArrowFormatLocalState>();
	if (!lstate.sealed.empty()) {
		auto unit = std::move(lstate.sealed.front());
		lstate.sealed.pop_front();
		return std::move(unit);
	}
	if (!lstate.appender || lstate.appender->RowCount() == 0) {
		return nullptr;
	}
	return SealAppender(lstate);
}

unique_ptr<RetainedResultCollection> ArrowFormat::CreateCollection(ClientContext &context,
                                                                   ResultFormatGlobalState &gstate,
                                                                   const ResultFormatContext &format_context) {
	return make_uniq<ArrowRetainedCollection>(*this, gstate);
}

unique_ptr<ArrowArrayWrapper> ArrowFormat::UnpackUnit(unique_ptr<ResultUnit> unit) {
	if (!unit) {
		return nullptr;
	}
	return std::move(unit->Cast<ArrowUnit>().array);
}

unique_ptr<ArrowArrayWrapper> ArrowFormat::ShareArray(const ArrowArrayOwner &owner) {
	D_ASSERT(owner && owner->arrow_array.release);
	auto result = make_uniq<ArrowArrayWrapper>();
	result->arrow_array = ArrowArray {};
	result->arrow_array.release = ReleaseSharedArray;
	FillSharedArray(result->arrow_array, owner->arrow_array, owner);
	return result;
}

idx_t ArrowFormat::ChangedRows(const ArrowArray &array) {
	if (array.length == 0) {
		return 0;
	}
	D_ASSERT(array.length == 1 && array.n_children == 1);
	auto &count = *array.children[0];
	auto values = static_cast<const int64_t *>(count.buffers[1]);
	return NumericCast<idx_t>(values[array.offset + count.offset]);
}

//===--------------------------------------------------------------------===//
// ArrowRetainedCollection
//===--------------------------------------------------------------------===//
unique_ptr<ArrowArrayWrapper> ArrowRetainedCollection::Fetch() {
	auto &arrays = Get();
	if (fetch_index >= arrays.size()) {
		return nullptr;
	}
	auto array = ArrowFormat::ShareArray(arrays[fetch_index]);
	fetch_index++;
	return array;
}

} // namespace duckdb
