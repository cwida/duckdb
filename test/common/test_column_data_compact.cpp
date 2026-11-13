#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"
#include "duckdb/common/types/column/column_data_consumer.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

// These tests pin the compact representation contract of ColumnDataCollection at the C++ API level:
// the vector types handed out by zero-copy scans, the per-segment dictionary deduplication, and the
// flat materialization under DISALLOW_ZERO_COPY. Query results are identical by design, so SQL tests
// cannot observe any of these invariants.

namespace duckdb {

static void AppendConstantChunk(ColumnDataCollection &collection, ColumnDataAppendState &state, const Value &value,
                                idx_t count) {
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	chunk.data[0].Reference(value, count_t(count));
	chunk.CheckCardinality(count);
	collection.Append(state, chunk);
}

TEST_CASE("Constant vectors keep their representation through a ColumnDataCollection", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});

	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	const string payload = "a_payload_string_longer_than_twelve_bytes";
	AppendConstantChunk(collection, append_state, Value(payload), STANDARD_VECTOR_SIZE);

	// a zero-copy scan hands the constant back out as a constant vector
	ColumnDataScanState scan_state;
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(scan_state, chunk));
	REQUIRE(chunk.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR);
	REQUIRE(ConstantVector::GetData<string_t>(chunk.data[0])[0].GetString() == payload);

	// re-appending the scanned chunk keeps the form across the collection hop
	ColumnDataCollection second(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState second_append;
	second.InitializeAppend(second_append);
	second.Append(second_append, chunk);
	ColumnDataScanState second_scan;
	DataChunk second_chunk;
	second.InitializeScanChunk(second_chunk);
	second.InitializeScan(second_scan, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(second.Scan(second_scan, second_chunk));
	REQUIRE(second_chunk.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR);

	// DISALLOW_ZERO_COPY keeps handing out self-contained flat vectors
	ColumnDataScanState flat_scan;
	DataChunk flat_chunk;
	collection.InitializeScanChunk(flat_chunk);
	collection.InitializeScan(flat_scan, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(flat_scan, flat_chunk));
	REQUIRE(flat_chunk.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
	auto flat_strings = FlatVector::GetData<string_t>(flat_chunk.data[0]);
	REQUIRE(flat_strings[0].GetString() == payload);
	REQUIRE(flat_strings[STANDARD_VECTOR_SIZE - 1].GetString() == payload);
}

TEST_CASE("Mixed appends materialize constant entries without losing values", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::BIGINT});

	// a partial constant chunk followed by a flat chunk lands in the same entry
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	AppendConstantChunk(collection, append_state, Value::BIGINT(7), 100);
	DataChunk flat_input;
	collection.InitializeScanChunk(flat_input);
	{
		auto writer = FlatVector::Writer<int64_t>(flat_input.data[0], 100);
		for (idx_t i = 0; i < 100; i++) {
			writer.WriteValue(UnsafeNumericCast<int64_t>(i));
		}
	}
	flat_input.CheckCardinality(100);
	collection.Append(append_state, flat_input);
	// a constant with a different value also appends after materialization
	AppendConstantChunk(collection, append_state, Value::BIGINT(-1), 100);

	REQUIRE(collection.Count() == 300);
	for (idx_t i = 0; i < 100; i++) {
		REQUIRE(collection.GetValue(0, i) == Value::BIGINT(7));
		REQUIRE(collection.GetValue(0, 100 + i) == Value::BIGINT(UnsafeNumericCast<int64_t>(i)));
		REQUIRE(collection.GetValue(0, 200 + i) == Value::BIGINT(-1));
	}
}

TEST_CASE("Dictionary vectors deduplicate per segment and survive scans", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});

	// a re-occurring dictionary, as emitted by storage and file scans
	const idx_t dict_size = 100;
	auto dictionary = DictionaryVector::CreateReusableDictionary(LogicalType::VARCHAR, dict_size);
	{
		auto writer = FlatVector::Writer<string_t>(dictionary->data, dict_size);
		for (idx_t i = 0; i < dict_size; i++) {
			if (i == 99) {
				writer.WriteNull();
			} else {
				writer.WriteValue(string_t("dictionary_payload_value_" + to_string(i)));
			}
		}
	}

	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		sel.set_index(i, i % dict_size);
	}
	const idx_t append_rounds = 8;
	for (idx_t round = 0; round < append_rounds; round++) {
		chunk.Reset();
		chunk.data[0].Dictionary(dictionary, sel, STANDARD_VECTOR_SIZE);
		chunk.CheckCardinality(STANDARD_VECTOR_SIZE);
		collection.Append(append_state, chunk);
	}
	REQUIRE(collection.Count() == append_rounds * STANDARD_VECTOR_SIZE);

	// the dictionary values are stored exactly once per segment
	REQUIRE(collection.GetSegments().size() == 1);
	REQUIRE(collection.GetSegments()[0]->dictionaries.size() == 1);
	REQUIRE(collection.SizeInBytes() < append_rounds * STANDARD_VECTOR_SIZE * sizeof(string_t));

	// a zero-copy scan hands dictionary vectors back out, with the dictionary id preserved
	ColumnDataScanState scan_state;
	DataChunk result;
	collection.InitializeScanChunk(result);
	collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(scan_state, result));
	REQUIRE(result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
	REQUIRE(DictionaryVector::DictionaryId(result.data[0]) == dictionary->id);
	REQUIRE(DictionaryVector::DictionarySize(result.data[0]).GetIndex() == dict_size);
	REQUIRE(result.GetValue(0, 1) == Value("dictionary_payload_value_1"));
	REQUIRE(result.GetValue(0, 99).IsNull());

	// re-appending the scanned chunk into another collection keeps the compact form
	ColumnDataCollection second(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState second_append;
	second.InitializeAppend(second_append);
	second.Append(second_append, result);
	ColumnDataScanState second_scan;
	DataChunk second_result;
	second.InitializeScanChunk(second_result);
	second.InitializeScan(second_scan, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(second.Scan(second_scan, second_result));
	REQUIRE(second_result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);

	// DISALLOW_ZERO_COPY materializes flat vectors with the same values
	ColumnDataScanState flat_scan;
	DataChunk flat_result;
	collection.InitializeScanChunk(flat_result);
	collection.InitializeScan(flat_scan, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(flat_scan, flat_result));
	REQUIRE(flat_result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
	REQUIRE(flat_result.GetValue(0, 1) == Value("dictionary_payload_value_1"));
	REQUIRE(flat_result.GetValue(0, 99).IsNull());

	// every chunk shares the same dictionary and resolves its own codes
	ColumnDataScanState full_scan;
	DataChunk full_result;
	collection.InitializeScanChunk(full_result);
	collection.InitializeScan(full_scan, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	idx_t scanned_chunks = 0;
	while (collection.Scan(full_scan, full_result)) {
		REQUIRE(full_result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
		REQUIRE(full_result.GetValue(0, 42) == Value("dictionary_payload_value_42"));
		REQUIRE(full_result.GetValue(0, 99).IsNull());
		scanned_chunks++;
	}
	REQUIRE(scanned_chunks == append_rounds);
}

TEST_CASE("ColumnDataConsumer handles chunks without blocks", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);

	// all-constant chunks store their values in metadata and reference no blocks
	ColumnDataCollection collection(buffer_manager, {LogicalType::BIGINT});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	AppendConstantChunk(collection, append_state, Value::BIGINT(7), STANDARD_VECTOR_SIZE);
	AppendConstantChunk(collection, append_state, Value::BIGINT(7), STANDARD_VECTOR_SIZE);

	ColumnDataConsumer consumer(collection, {0});
	consumer.InitializeScan();
	ColumnDataConsumerScanState state;
	state.current_chunk_state.properties = ColumnDataScanProperties::ALLOW_ZERO_COPY;
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	idx_t total = 0;
	while (consumer.AssignChunk(state)) {
		consumer.ScanChunk(state, chunk);
		REQUIRE(chunk.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR);
		REQUIRE(ConstantVector::GetData<int64_t>(chunk.data[0])[0] == 7);
		total += chunk.size();
		consumer.FinishChunk(state);
	}
	REQUIRE(total == 2 * STANDARD_VECTOR_SIZE);

	// chunks with and without blocks mix in the consumed order
	ColumnDataCollection mixed(buffer_manager, {LogicalType::BIGINT});
	ColumnDataAppendState mixed_append;
	mixed.InitializeAppend(mixed_append);
	AppendConstantChunk(mixed, mixed_append, Value::BIGINT(1), STANDARD_VECTOR_SIZE);
	DataChunk flat_input;
	mixed.InitializeScanChunk(flat_input);
	{
		auto writer = FlatVector::Writer<int64_t>(flat_input.data[0], STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			writer.WriteValue(UnsafeNumericCast<int64_t>(i));
		}
	}
	flat_input.CheckCardinality(STANDARD_VECTOR_SIZE);
	mixed.Append(mixed_append, flat_input);
	AppendConstantChunk(mixed, mixed_append, Value::BIGINT(2), STANDARD_VECTOR_SIZE);

	ColumnDataConsumer mixed_consumer(mixed, {0});
	mixed_consumer.InitializeScan();
	ColumnDataConsumerScanState mixed_state;
	mixed_state.current_chunk_state.properties = ColumnDataScanProperties::ALLOW_ZERO_COPY;
	DataChunk mixed_chunk;
	mixed.InitializeScanChunk(mixed_chunk);
	idx_t rows = 0;
	int64_t sum = 0;
	while (mixed_consumer.AssignChunk(mixed_state)) {
		mixed_consumer.ScanChunk(mixed_state, mixed_chunk);
		auto values = mixed_chunk.data[0].Values<int64_t>();
		for (auto entry : values) {
			sum += entry.GetValue();
		}
		rows += mixed_chunk.size();
		mixed_consumer.FinishChunk(mixed_state);
	}
	REQUIRE(rows == 3 * STANDARD_VECTOR_SIZE);
	const auto n = int64_t(STANDARD_VECTOR_SIZE);
	REQUIRE(sum == n + 2 * n + n * (n - 1) / 2);
}

TEST_CASE("Compact collections account their stored bytes", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::BIGINT});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	for (idx_t i = 0; i < 3; i++) {
		AppendConstantChunk(collection, append_state, Value::BIGINT(7), STANDARD_VECTOR_SIZE);
	}
	REQUIRE(collection.Count() == 3 * STANDARD_VECTOR_SIZE);
	// a nonempty all-constant collection reports its metadata-resident bytes
	REQUIRE(collection.SizeInBytes() > 0);
	REQUIRE(collection.AllocationSize() > 0);
	REQUIRE(collection.AllocationSize() >= collection.SizeInBytes());
	// and stays far below one flat vector
	REQUIRE(collection.SizeInBytes() < 4096);
	// the per-chunk allocation attribution is nonzero as well
	auto &segment = *collection.GetSegments()[0];
	REQUIRE(segment.GetChunkAllocationSize(0) > 0);

	// materialization keeps the constant charge, reported sizes never shrink while appending
	ColumnDataCollection mixed(buffer_manager, {LogicalType::BIGINT});
	ColumnDataAppendState mixed_append;
	mixed.InitializeAppend(mixed_append);
	AppendConstantChunk(mixed, mixed_append, Value::BIGINT(7), 100);
	const auto size_before_materialization = mixed.AllocationSize();
	DataChunk flat_input;
	mixed.InitializeScanChunk(flat_input);
	{
		auto writer = FlatVector::Writer<int64_t>(flat_input.data[0], 100);
		for (idx_t i = 0; i < 100; i++) {
			writer.WriteValue(UnsafeNumericCast<int64_t>(i));
		}
	}
	flat_input.CheckCardinality(100);
	mixed.Append(mixed_append, flat_input);
	auto &mixed_segment = *mixed.GetSegments()[0];
	const idx_t flat_entry_size =
	    ColumnDataCollectionSegment::GetDataSize(sizeof(int64_t)) + ValidityMask::STANDARD_MASK_SIZE;
	// batching consumers rely on reported sizes never shrinking while appending
	REQUIRE(mixed.AllocationSize() >= size_before_materialization);
	// the retired charge stays counted consistently across all size metrics
	REQUIRE(mixed_segment.constant_metadata_size == sizeof(ColumnDataConstantValue));
	REQUIRE(mixed.SizeInBytes() == flat_entry_size + sizeof(ColumnDataConstantValue));
	REQUIRE(mixed_segment.GetChunkAllocationSize(0) == flat_entry_size + sizeof(ColumnDataConstantValue));
}

TEST_CASE("Repeated string bytes stay shared when compact recognition does not apply", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const idx_t payload_size = 131072;
	const string payload(payload_size, 'x');

	// managed result collections keep flat entries but must still share the constant's bytes
	ColumnDataCollection managed(buffer_manager, {LogicalType::VARCHAR},
	                             ColumnDataCollectionLifetime::THROW_ERROR_AFTER_DATABASE_CLOSES);
	ColumnDataAppendState managed_append;
	managed.InitializeAppend(managed_append);
	AppendConstantChunk(managed, managed_append, Value(payload), STANDARD_VECTOR_SIZE);
	ColumnDataScanState scan_state;
	DataChunk chunk;
	managed.InitializeScanChunk(chunk);
	managed.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(managed.Scan(scan_state, chunk));
	REQUIRE(chunk.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
	auto managed_strings = FlatVector::GetData<string_t>(chunk.data[0]);
	REQUIRE(managed_strings[0].GetString() == payload);
	REQUIRE(managed_strings[STANDARD_VECTOR_SIZE - 1].GetString() == payload);
	// one copy of the bytes plus the entry array, not one copy per row
	REQUIRE(managed.SizeInBytes() < 2 * payload_size);

	// an entry that already holds flat rows cannot become compact but still shares the bytes
	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	DataChunk flat_input;
	collection.InitializeScanChunk(flat_input);
	{
		auto writer = FlatVector::Writer<string_t>(flat_input.data[0], 1);
		writer.WriteValue(string_t("short"));
	}
	flat_input.CheckCardinality(1);
	collection.Append(append_state, flat_input);
	AppendConstantChunk(collection, append_state, Value(payload), STANDARD_VECTOR_SIZE - 1);
	REQUIRE(collection.Count() == STANDARD_VECTOR_SIZE);
	REQUIRE(collection.GetValue(0, 0) == Value("short"));
	REQUIRE(collection.GetValue(0, 1) == Value(payload));
	REQUIRE(collection.GetValue(0, STANDARD_VECTOR_SIZE - 1) == Value(payload));
	REQUIRE(collection.SizeInBytes() < 2 * payload_size);
}

static buffer_ptr<DictionaryEntry> MakeStringDictionary(idx_t dict_size, const string &prefix, bool with_null,
                                                        bool with_id = true) {
	auto dictionary = DictionaryVector::CreateReusableDictionary(LogicalType::VARCHAR, dict_size);
	if (!with_id) {
		dictionary->id = string();
	}
	auto writer = FlatVector::Writer<string_t>(dictionary->data, dict_size);
	for (idx_t i = 0; i < dict_size; i++) {
		if (with_null && i + 1 == dict_size) {
			writer.WriteNull();
		} else {
			writer.WriteValue(string_t(prefix + to_string(i)));
		}
	}
	return dictionary;
}

static void AppendDictionaryChunk(ColumnDataCollection &collection, ColumnDataAppendState &state,
                                  buffer_ptr<DictionaryEntry> dictionary, idx_t dict_size, idx_t count) {
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	SelectionVector sel(count);
	for (idx_t i = 0; i < count; i++) {
		sel.set_index(i, i % dict_size);
	}
	chunk.data[0].Dictionary(std::move(dictionary), sel, count);
	chunk.CheckCardinality(count);
	collection.Append(state, chunk);
}

TEST_CASE("Compact string entries survive block relocation", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	auto config = GetTestConfig();
	config->SetOptionByName("temp_directory", TestCreatePath("compact_relocation.tmp"));
	DuckDB db(nullptr, config.get());
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR, LogicalType::VARCHAR});

	const idx_t dict_size = 50;
	const string prefix = "a_non_inlined_dictionary_payload_";
	auto dictionary = MakeStringDictionary(dict_size, prefix, true);
	const string constant_payload = "a_non_inlined_constant_payload_string";

	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	DataChunk input;
	collection.InitializeScanChunk(input);
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		sel.set_index(i, i % dict_size);
	}
	for (idx_t round = 0; round < 2; round++) {
		input.Reset();
		input.data[0].Dictionary(dictionary, sel, STANDARD_VECTOR_SIZE);
		input.data[1].Reference(Value(constant_payload), count_t(STANDARD_VECTOR_SIZE));
		input.CheckCardinality(STANDARD_VECTOR_SIZE);
		collection.Append(append_state, input);
	}

	// release the append pins so the blocks can be unloaded
	vector<shared_ptr<BlockHandle>> handles;
	for (const auto &entry : append_state.current_chunk_state.handles) {
		handles.push_back(entry.second.GetBlockHandle());
	}
	REQUIRE(!handles.empty());
	append_state.current_chunk_state.handles.clear();

	// force every block to a different address, poisoning the old allocations
	auto relocate = [&](vector<shared_ptr<BlockHandle>> &relocate_handles) {
		vector<unique_ptr<FileBuffer>> old_buffers;
		vector<BufferHandle> pins;
		for (auto &handle : relocate_handles) {
			auto lock = handle->GetMemory().GetLock();
			REQUIRE(handle->GetMemory().CanUnload() == CanUnloadResult::CAN_UNLOAD);
			old_buffers.push_back(handle->GetMemory().UnloadAndTakeBlock(lock));
		}
		for (idx_t i = 0; i < relocate_handles.size(); i++) {
			pins.push_back(buffer_manager.Pin(relocate_handles[i]));
			REQUIRE(pins.back().Ptr() != old_buffers[i]->GetData());
			old_buffers[i]->Clear();
		}
	};

	for (idx_t round = 0; round < 2; round++) {
		relocate(handles);

		// the dictionary heap pointers are repaired and the constant pointer reconstructed
		const auto properties =
		    round == 0 ? ColumnDataScanProperties::ALLOW_ZERO_COPY : ColumnDataScanProperties::DISALLOW_ZERO_COPY;
		ColumnDataScanState scan_state;
		DataChunk result;
		collection.InitializeScanChunk(result);
		collection.InitializeScan(scan_state, properties);
		idx_t chunks = 0;
		while (collection.Scan(scan_state, result)) {
			REQUIRE(result.GetValue(0, 1) == Value(prefix + "1"));
			REQUIRE(result.GetValue(0, dict_size - 1).IsNull());
			REQUIRE(result.GetValue(1, 0) == Value(constant_payload));
			REQUIRE(result.GetValue(1, STANDARD_VECTOR_SIZE - 1) == Value(constant_payload));
			chunks++;
		}
		REQUIRE(chunks == 2);
	}

	// a materialized entry carries two swizzle ranges, both repaired after relocation
	const string second_payload = "another_non_inlined_constant_payload";
	ColumnDataCollection mixed(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState mixed_append;
	mixed.InitializeAppend(mixed_append);
	AppendConstantChunk(mixed, mixed_append, Value(constant_payload), 100);
	AppendConstantChunk(mixed, mixed_append, Value(second_payload), 100);
	vector<shared_ptr<BlockHandle>> mixed_handles;
	for (const auto &entry : mixed_append.current_chunk_state.handles) {
		mixed_handles.push_back(entry.second.GetBlockHandle());
	}
	REQUIRE(!mixed_handles.empty());
	mixed_append.current_chunk_state.handles.clear();
	for (idx_t round = 0; round < 2; round++) {
		relocate(mixed_handles);
		const auto properties =
		    round == 0 ? ColumnDataScanProperties::ALLOW_ZERO_COPY : ColumnDataScanProperties::DISALLOW_ZERO_COPY;
		ColumnDataScanState scan_state;
		DataChunk result;
		mixed.InitializeScanChunk(result);
		mixed.InitializeScan(scan_state, properties);
		REQUIRE(mixed.Scan(scan_state, result));
		REQUIRE(result.GetValue(0, 0) == Value(constant_payload));
		REQUIRE(result.GetValue(0, 99) == Value(constant_payload));
		REQUIRE(result.GetValue(0, 100) == Value(second_payload));
		REQUIRE(result.GetValue(0, 199) == Value(second_payload));
	}
}

TEST_CASE("DISALLOW_ZERO_COPY copies outlive the source collection", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const string constant_payload = "a_non_inlined_constant_payload_string";
	const string prefix = "a_non_inlined_dictionary_payload_";
	const idx_t dict_size = 50;

	DataChunk result;
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR, LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		DataChunk input;
		collection.InitializeScanChunk(input);
		SelectionVector sel(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			sel.set_index(i, i % dict_size);
		}
		input.data[0].Dictionary(MakeStringDictionary(dict_size, prefix, false), sel, STANDARD_VECTOR_SIZE);
		input.data[1].Reference(Value(constant_payload), count_t(STANDARD_VECTOR_SIZE));
		input.CheckCardinality(STANDARD_VECTOR_SIZE);
		collection.Append(append_state, input);

		collection.InitializeScanChunk(Allocator::DefaultAllocator(), result);
		ColumnDataScanState scan_state;
		collection.InitializeScan(scan_state, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
		REQUIRE(collection.Scan(scan_state, result));
		// the collection, its blocks, and the scan state are destroyed here
	}
	REQUIRE(result.size() == STANDARD_VECTOR_SIZE);
	REQUIRE(result.GetValue(0, 1) == Value(prefix + "1"));
	REQUIRE(result.GetValue(0, dict_size) == Value(prefix + "0"));
	REQUIRE(result.GetValue(1, 0) == Value(constant_payload));
	REQUIRE(result.GetValue(1, STANDARD_VECTOR_SIZE - 1) == Value(constant_payload));
}

TEST_CASE("Dictionary recognition gate boundaries", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);

	auto scan_first = [&](ColumnDataCollection &collection, DataChunk &result) {
		collection.InitializeScanChunk(result);
		ColumnDataScanState scan_state;
		collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		REQUIRE(collection.Scan(scan_state, result));
	};

	// a dictionary of exactly STANDARD_VECTOR_SIZE values is recognized
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendDictionaryChunk(collection, append_state, MakeStringDictionary(STANDARD_VECTOR_SIZE, "max_size_", false),
		                      STANDARD_VECTOR_SIZE, STANDARD_VECTOR_SIZE);
		DataChunk result;
		scan_first(collection, result);
		REQUIRE(result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
		REQUIRE(result.GetValue(0, 7) == Value("max_size_7"));
	}
	// one value more falls back to flat storage
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendDictionaryChunk(collection, append_state,
		                      MakeStringDictionary(STANDARD_VECTOR_SIZE + 1, "oversized_", false),
		                      STANDARD_VECTOR_SIZE + 1, STANDARD_VECTOR_SIZE);
		DataChunk result;
		scan_first(collection, result);
		REQUIRE(result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		REQUIRE(result.GetValue(0, 7) == Value("oversized_7"));
	}
	// a dictionary without an id is an ad hoc selection and falls back to flat storage
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendDictionaryChunk(collection, append_state, MakeStringDictionary(100, "no_id_", false, false), 100,
		                      STANDARD_VECTOR_SIZE);
		DataChunk result;
		scan_first(collection, result);
		REQUIRE(result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		REQUIRE(result.GetValue(0, 7) == Value("no_id_7"));
	}
	// values exactly as wide as the codes are recognized
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::UINTEGER});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		auto dictionary = DictionaryVector::CreateReusableDictionary(LogicalType::UINTEGER, 100);
		{
			auto writer = FlatVector::Writer<uint32_t>(dictionary->data, 100);
			for (idx_t i = 0; i < 100; i++) {
				writer.WriteValue(UnsafeNumericCast<uint32_t>(i));
			}
		}
		AppendDictionaryChunk(collection, append_state, std::move(dictionary), 100, STANDARD_VECTOR_SIZE);
		DataChunk result;
		scan_first(collection, result);
		REQUIRE(result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
		REQUIRE(result.GetValue(0, 7) == Value::UINTEGER(7));
	}
	// values narrower than the codes fall back to flat storage
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::USMALLINT});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		auto dictionary = DictionaryVector::CreateReusableDictionary(LogicalType::USMALLINT, 100);
		{
			auto writer = FlatVector::Writer<uint16_t>(dictionary->data, 100);
			for (idx_t i = 0; i < 100; i++) {
				writer.WriteValue(UnsafeNumericCast<uint16_t>(i));
			}
		}
		AppendDictionaryChunk(collection, append_state, std::move(dictionary), 100, STANDARD_VECTOR_SIZE);
		DataChunk result;
		scan_first(collection, result);
		REQUIRE(result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		REQUIRE(result.GetValue(0, 7) == Value::USMALLINT(7));
	}
	// dictionary string bytes up to exactly one block are recognized, one byte more falls back
	// to flat storage with shared bytes
	const auto block_size = buffer_manager.GetBlockSize();
	for (idx_t excess = 0; excess < 2; excess++) {
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		const idx_t first_size = block_size / 2;
		const idx_t second_size = block_size - first_size + excess;
		auto dictionary = DictionaryVector::CreateReusableDictionary(LogicalType::VARCHAR, 2);
		{
			auto writer = FlatVector::Writer<string_t>(dictionary->data, 2);
			writer.WriteValue(string_t(string(first_size, 'a')));
			writer.WriteValue(string_t(string(second_size, 'b')));
		}
		AppendDictionaryChunk(collection, append_state, std::move(dictionary), 2, STANDARD_VECTOR_SIZE);
		DataChunk result;
		scan_first(collection, result);
		if (excess == 0) {
			REQUIRE(result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
		} else {
			REQUIRE(result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		}
		REQUIRE(result.GetValue(0, 0) == Value(string(first_size, 'a')));
		REQUIRE(result.GetValue(0, 1) == Value(string(second_size, 'b')));
		// one byte copy per entry at most, never one per row
		REQUIRE(collection.SizeInBytes() < 2 * (first_size + second_size));
	}
}

TEST_CASE("Compact entries extend and materialize across appends", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const string payload = "a_non_inlined_constant_payload_string";
	const string other_payload = "a_different_non_inlined_constant_value";

	// partial appends of the same constant extend one constant entry
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendConstantChunk(collection, append_state, Value(payload), 100);
		AppendConstantChunk(collection, append_state, Value(payload), 100);
		DataChunk result;
		collection.InitializeScanChunk(result);
		ColumnDataScanState scan_state;
		collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		REQUIRE(collection.Scan(scan_state, result));
		REQUIRE(result.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR);
		REQUIRE(result.size() == 200);
		REQUIRE(result.GetValue(0, 199) == Value(payload));
	}
	// a different constant value materializes the entry and continues flat
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendConstantChunk(collection, append_state, Value(payload), 100);
		AppendConstantChunk(collection, append_state, Value(other_payload), 100);
		REQUIRE(collection.GetValue(0, 0) == Value(payload));
		REQUIRE(collection.GetValue(0, 99) == Value(payload));
		REQUIRE(collection.GetValue(0, 100) == Value(other_payload));
		REQUIRE(collection.GetValue(0, 199) == Value(other_payload));
	}
	// NULL and non-NULL constants do not extend each other
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::BIGINT});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendConstantChunk(collection, append_state, Value(LogicalType::BIGINT), 100);
		AppendConstantChunk(collection, append_state, Value::BIGINT(5), 100);
		REQUIRE(collection.GetValue(0, 0).IsNull());
		REQUIRE(collection.GetValue(0, 99).IsNull());
		REQUIRE(collection.GetValue(0, 100) == Value::BIGINT(5));
		REQUIRE(collection.GetValue(0, 199) == Value::BIGINT(5));
	}
	// partial appends from the same dictionary extend one dictionary entry
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		auto dictionary = MakeStringDictionary(50, "extended_dictionary_payload_", false);
		AppendDictionaryChunk(collection, append_state, dictionary, 50, 100);
		AppendDictionaryChunk(collection, append_state, dictionary, 50, 100);
		REQUIRE(collection.GetSegments()[0]->dictionaries.size() == 1);
		DataChunk result;
		collection.InitializeScanChunk(result);
		ColumnDataScanState scan_state;
		collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		REQUIRE(collection.Scan(scan_state, result));
		REQUIRE(result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
		REQUIRE(result.size() == 200);
		REQUIRE(result.GetValue(0, 149) == Value("extended_dictionary_payload_49"));
	}
	// a dictionary with a different id materializes the entry and continues flat
	{
		ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		AppendDictionaryChunk(collection, append_state, MakeStringDictionary(50, "first_dictionary_payload_", true), 50,
		                      100);
		// 49 values for 100 rows so the second append takes the restored byte-sharing fallback
		AppendDictionaryChunk(collection, append_state, MakeStringDictionary(49, "second_dictionary_payload_", false),
		                      49, 100);
		DataChunk result;
		collection.InitializeScanChunk(result);
		ColumnDataScanState scan_state;
		collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		REQUIRE(collection.Scan(scan_state, result));
		REQUIRE(result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		REQUIRE(result.GetValue(0, 1) == Value("first_dictionary_payload_1"));
		REQUIRE(result.GetValue(0, 49).IsNull());
		REQUIRE(result.GetValue(0, 101) == Value("second_dictionary_payload_1"));
	}
}

TEST_CASE("Compact entries move with Combine, Swap, and reuse", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const string payload = "a_non_inlined_constant_payload_string";

	ColumnDataCollection first(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState first_append;
	first.InitializeAppend(first_append);
	AppendConstantChunk(first, first_append, Value(payload), STANDARD_VECTOR_SIZE);

	ColumnDataCollection second(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState second_append;
	second.InitializeAppend(second_append);
	AppendDictionaryChunk(second, second_append, MakeStringDictionary(50, "combined_dictionary_payload_", false), 50,
	                      STANDARD_VECTOR_SIZE);

	const auto combined_size = first.SizeInBytes() + second.SizeInBytes();
	first.Combine(second);
	REQUIRE(first.Count() == 2 * STANDARD_VECTOR_SIZE);
	REQUIRE(first.GetValue(0, 0) == Value(payload));
	REQUIRE(first.GetValue(0, STANDARD_VECTOR_SIZE + 1) == Value("combined_dictionary_payload_1"));
	// the size counters and per-segment dictionaries move with the segments
	REQUIRE(first.SizeInBytes() == combined_size);
	idx_t moved_constant_bytes = 0;
	idx_t moved_dictionaries = 0;
	for (auto &moved_segment : first.GetSegments()) {
		moved_constant_bytes += moved_segment->constant_metadata_size;
		moved_dictionaries += moved_segment->dictionaries.size();
	}
	REQUIRE(moved_constant_bytes == sizeof(ColumnDataConstantValue));
	REQUIRE(moved_dictionaries == 1);

	ColumnDataCollection swapped(buffer_manager, {LogicalType::VARCHAR});
	swapped.Swap(first);
	REQUIRE(swapped.Count() == 2 * STANDARD_VECTOR_SIZE);
	REQUIRE(swapped.GetValue(0, 0) == Value(payload));

	// reuse after a reset starts from clean per-segment dictionary state
	swapped.ResetForReuse();
	REQUIRE(swapped.Count() == 0);
	REQUIRE(swapped.GetSegments()[0]->dictionaries.empty());
	REQUIRE(swapped.GetSegments()[0]->constant_metadata_size == 0);
	ColumnDataAppendState reuse_append;
	swapped.InitializeAppend(reuse_append);
	AppendDictionaryChunk(swapped, reuse_append, MakeStringDictionary(50, "reused_dictionary_payload_", false), 50,
	                      100);
	// the dictionary is materialized again for the reused segment
	REQUIRE(swapped.GetSegments()[0]->dictionaries.size() == 1);
	AppendConstantChunk(swapped, reuse_append, Value(payload), 100);
	REQUIRE(swapped.Count() == 200);
	REQUIRE(swapped.GetValue(0, 1) == Value("reused_dictionary_payload_1"));
	REQUIRE(swapped.GetValue(0, 199) == Value(payload));
}

TEST_CASE("Compact representations with in-memory and hybrid allocators", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	Connection con(db);
	const string payload = "a_non_inlined_constant_payload_string";
	const string prefix = "a_non_inlined_dictionary_payload_";

	auto in_memory = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(),
	                                                 vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR});
	auto hybrid =
	    make_uniq<ColumnDataCollection>(*con.context, vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                    ColumnDataAllocatorType::HYBRID);
	for (auto collection_ptr : {in_memory.get(), hybrid.get()}) {
		auto &collection = *collection_ptr;
		ColumnDataAppendState append_state;
		collection.InitializeAppend(append_state);
		DataChunk input;
		collection.InitializeScanChunk(input);
		SelectionVector sel(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			sel.set_index(i, i % 50);
		}
		input.data[0].Dictionary(MakeStringDictionary(50, prefix, true), sel, STANDARD_VECTOR_SIZE);
		input.data[1].Reference(Value(payload), count_t(STANDARD_VECTOR_SIZE));
		input.CheckCardinality(STANDARD_VECTOR_SIZE);
		collection.Append(append_state, input);

		for (const auto properties :
		     {ColumnDataScanProperties::ALLOW_ZERO_COPY, ColumnDataScanProperties::DISALLOW_ZERO_COPY}) {
			ColumnDataScanState scan_state;
			DataChunk result;
			collection.InitializeScanChunk(result);
			collection.InitializeScan(scan_state, properties);
			REQUIRE(collection.Scan(scan_state, result));
			if (properties == ColumnDataScanProperties::ALLOW_ZERO_COPY) {
				REQUIRE(result.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
				REQUIRE(result.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR);
			} else {
				REQUIRE(result.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
				REQUIRE(result.data[1].GetVectorType() == VectorType::FLAT_VECTOR);
			}
			REQUIRE(result.GetValue(0, 1) == Value(prefix + "1"));
			REQUIRE(result.GetValue(0, 49).IsNull());
			REQUIRE(result.GetValue(1, 0) == Value(payload));
			REQUIRE(result.GetValue(1, STANDARD_VECTOR_SIZE - 1) == Value(payload));
		}
	}
}

TEST_CASE("Collection sizes never shrink while appending", "[column_data_compact]") {
	// no vector-size guard: this mirrors the Vector Sizes CI shape at any STANDARD_VECTOR_SIZE
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::BIGINT});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);

	// a full flat vector allocates a block, the constant then lands in the next chunk
	DataChunk flat_input;
	collection.InitializeScanChunk(flat_input);
	{
		auto writer = FlatVector::Writer<int64_t>(flat_input.data[0], STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			writer.WriteValue(UnsafeNumericCast<int64_t>(i));
		}
	}
	flat_input.CheckCardinality(STANDARD_VECTOR_SIZE);
	collection.Append(append_state, flat_input);
	auto allocation_size = collection.AllocationSize();
	auto size_in_bytes = collection.SizeInBytes();

	// batching consumers require that both sizes never shrink across appends
	auto check_monotonic = [&]() {
		REQUIRE(collection.AllocationSize() >= allocation_size);
		REQUIRE(collection.SizeInBytes() >= size_in_bytes);
		allocation_size = collection.AllocationSize();
		size_in_bytes = collection.SizeInBytes();
	};
	AppendConstantChunk(collection, append_state, Value::BIGINT(7), 1);
	check_monotonic();
	// a different constant materializes the entry inside the already-allocated block
	AppendConstantChunk(collection, append_state, Value::BIGINT(8), 1);
	check_monotonic();

	REQUIRE(collection.Count() == STANDARD_VECTOR_SIZE + 2);
	REQUIRE(collection.GetValue(0, STANDARD_VECTOR_SIZE - 1) ==
	        Value::BIGINT(UnsafeNumericCast<int64_t>(STANDARD_VECTOR_SIZE - 1)));
	REQUIRE(collection.GetValue(0, STANDARD_VECTOR_SIZE) == Value::BIGINT(7));
	REQUIRE(collection.GetValue(0, STANDARD_VECTOR_SIZE + 1) == Value::BIGINT(8));
}

static VectorType FirstScannedVectorType(ColumnDataCollection &collection, DataChunk &chunk) {
	ColumnDataScanState scan_state;
	collection.InitializeScanChunk(chunk);
	collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(scan_state, chunk));
	return chunk.data[0].GetVectorType();
}

TEST_CASE("Sparse appends over fresh dictionaries stay flat", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const string prefix = "a_sparse_dictionary_payload_string_value_";

	// 500 appends of 3 rows, each over a fresh dictionary covering most of a vector: copying
	// a dictionary for 3 codes is a net loss, the rows must be stored flat instead
	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	// the same logical rows appended flat give the exact storage the fallback must match
	ColumnDataCollection flat_equivalent(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState flat_append;
	flat_equivalent.InitializeAppend(flat_append);
	DataChunk flat_rows;
	flat_equivalent.InitializeScanChunk(flat_rows);
	const idx_t dict_size = (STANDARD_VECTOR_SIZE / 4) * 3;
	for (idx_t round = 0; round < 500; round++) {
		AppendDictionaryChunk(collection, append_state, MakeStringDictionary(dict_size, prefix, false), dict_size, 3);
		flat_rows.Reset();
		{
			auto writer = FlatVector::Writer<string_t>(flat_rows.data[0], 3);
			for (idx_t i = 0; i < 3; i++) {
				writer.WriteValue(string_t(prefix + to_string(i % dict_size)));
			}
		}
		flat_rows.CheckCardinality(3);
		flat_equivalent.Append(flat_append, flat_rows);
	}
	REQUIRE(collection.Count() == 1500);
	REQUIRE(collection.GetValue(0, 0) == Value(prefix + "0"));
	REQUIRE(collection.GetValue(0, 4) == Value(prefix + "1"));

	DataChunk chunk;
	REQUIRE(FirstScannedVectorType(collection, chunk) == VectorType::FLAT_VECTOR);
	// the sparse appends must cost exactly the flat rows, not a dictionary copy per segment
	REQUIRE(collection.SizeInBytes() == flat_equivalent.SizeInBytes());

	// worst case: a single row over a full-size dictionary
	ColumnDataCollection single(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState single_append;
	single.InitializeAppend(single_append);
	AppendDictionaryChunk(single, single_append, MakeStringDictionary(STANDARD_VECTOR_SIZE, prefix, false),
	                      STANDARD_VECTOR_SIZE, 1);
	REQUIRE(single.Count() == 1);
	REQUIRE(single.GetValue(0, 0) == Value(prefix + "0"));
	DataChunk single_chunk;
	REQUIRE(FirstScannedVectorType(single, single_chunk) == VectorType::FLAT_VECTOR);

	// the single dictionary row must cost the same as the same row appended flat
	ColumnDataCollection flat_single(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState flat_single_append;
	flat_single.InitializeAppend(flat_single_append);
	DataChunk flat_input;
	flat_single.InitializeScanChunk(flat_input);
	{
		auto writer = FlatVector::Writer<string_t>(flat_input.data[0], 1);
		writer.WriteValue(string_t(prefix + "0"));
	}
	flat_input.CheckCardinality(1);
	flat_single.Append(flat_single_append, flat_input);
	REQUIRE(single.SizeInBytes() == flat_single.SizeInBytes());
}

TEST_CASE("Dictionary reuse bypasses the fresh-dictionary size gate", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const string prefix = "a_reused_dictionary_payload_string_value_";
	const idx_t dict_size = (STANDARD_VECTOR_SIZE / 4) * 3;
	auto dictionary = MakeStringDictionary(dict_size, prefix, false);

	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	// a full-vector append qualifies and materializes the segment dictionary
	AppendDictionaryChunk(collection, append_state, dictionary, dict_size, STANDARD_VECTOR_SIZE);
	const auto size_after_first = collection.SizeInBytes();

	// a sparse append over the same dictionary reuses it for free: only the codes are added
	AppendDictionaryChunk(collection, append_state, dictionary, dict_size, 3);
	REQUIRE(collection.Count() == STANDARD_VECTOR_SIZE + 3);
	const auto sparse_delta = collection.SizeInBytes() - size_after_first;
	REQUIRE(sparse_delta <= AlignValue(STANDARD_VECTOR_SIZE * sizeof(sel_t)) + 64);

	// both chunks scan as dictionary vectors
	ColumnDataScanState scan_state;
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(scan_state, chunk));
	REQUIRE(chunk.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
	REQUIRE(collection.Scan(scan_state, chunk));
	REQUIRE(chunk.data[0].GetVectorType() == VectorType::DICTIONARY_VECTOR);
	REQUIRE(chunk.size() == 3);
	REQUIRE(chunk.GetValue(0, 2) == Value(prefix + "2"));
}

TEST_CASE("The fresh-dictionary gate sits at twice the appended rows", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const string prefix = "a_boundary_dictionary_payload_string_value_";

	// copy_count * 2 == dict_size still takes the dictionary path
	ColumnDataCollection at_boundary(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState at_boundary_append;
	at_boundary.InitializeAppend(at_boundary_append);
	AppendDictionaryChunk(at_boundary, at_boundary_append, MakeStringDictionary(100, prefix, false), 100, 50);
	DataChunk at_chunk;
	REQUIRE(FirstScannedVectorType(at_boundary, at_chunk) == VectorType::DICTIONARY_VECTOR);
	REQUIRE(at_chunk.size() == 50);

	// one dictionary entry more falls through to the flat copy
	ColumnDataCollection past_boundary(buffer_manager, {LogicalType::VARCHAR});
	ColumnDataAppendState past_boundary_append;
	past_boundary.InitializeAppend(past_boundary_append);
	AppendDictionaryChunk(past_boundary, past_boundary_append, MakeStringDictionary(101, prefix, false), 101, 50);
	DataChunk past_chunk;
	REQUIRE(FirstScannedVectorType(past_boundary, past_chunk) == VectorType::FLAT_VECTOR);
	REQUIRE(past_chunk.size() == 50);
	REQUIRE(past_chunk.GetValue(0, 49) == Value(prefix + "49"));
}

//! Fill 'chunk' with 'count' BIGINT[] rows: row zero holds 'length' values starting at 'base', the rest are empty
static void FillLargeListChunk(DataChunk &chunk, idx_t count, idx_t length, int64_t base) {
	chunk.Reset();
	auto &list = chunk.data[0];
	ListVector::Reserve(list, length);
	auto entries = FlatVector::GetDataMutable<list_entry_t>(list);
	entries[0] = list_entry_t {0, length};
	for (idx_t i = 1; i < count; i++) {
		entries[i] = list_entry_t {length, 0};
	}
	auto child = FlatVector::GetDataMutable<int64_t>(ListVector::GetChildMutable(list));
	for (idx_t i = 0; i < length; i++) {
		child[i] = base + int64_t(i);
	}
	ListVector::SetListSize(list, length);
	FlatVector::SetSize(list, count_t(count));
	chunk.CheckCardinality(count);
}

static void RequireLargeList(DataChunk &chunk, idx_t length, int64_t base) {
	auto &list = chunk.data[0];
	auto entries = FlatVector::GetData<list_entry_t>(list);
	REQUIRE(entries[0].length == length);
	auto child = FlatVector::GetData<int64_t>(ListVector::GetChild(list));
	REQUIRE(child[entries[0].offset] == base);
	REQUIRE(child[entries[0].offset + length - 1] == base + int64_t(length) - 1);
}

TEST_CASE("Shared list children live in evictable blocks", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		// the shapes below assume full-size vectors
		return;
	}
	// each chunk carries one 8 MiB list child, 16 chunks exceed the limit unless finished chunks spill
	DBConfig config;
	config.SetOptionByName("memory_limit", Value("64MB"));
	config.SetOptionByName("temp_directory", Value(TestCreatePath("shared_list_spill")));
	DuckDB db(nullptr, &config);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const idx_t length = 1024 * 1024;
	const idx_t chunks = 16;
	ColumnDataCollection collection(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	for (idx_t c = 0; c < chunks; c++) {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), collection.Types());
		FillLargeListChunk(input, STANDARD_VECTOR_SIZE, length, int64_t(c) * 10);
		collection.Append(append_state, input);
	}
	ColumnDataScanState scan_state;
	DataChunk chunk;
	collection.InitializeScanChunk(chunk);
	collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	for (idx_t c = 0; c < chunks; c++) {
		REQUIRE(collection.Scan(scan_state, chunk));
		auto &child = ListVector::GetChild(chunk.data[0]);
		REQUIRE(child.GetBufferRef()->GetBufferType() == VectorBufferType::SHARED_DATA_BUFFER);
		RequireLargeList(chunk, length, int64_t(c) * 10);
	}
}

TEST_CASE("Shared list views detach on mutation", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const idx_t length = 3000;
	ColumnDataCollection source(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
	ColumnDataAppendState append_state;
	source.InitializeAppend(append_state);
	DataChunk input;
	input.Initialize(Allocator::DefaultAllocator(), source.Types());
	FillLargeListChunk(input, 1, length, 1);
	source.Append(append_state, input);

	auto scan_view = [&](ColumnDataScanState &scan_state, DataChunk &chunk) {
		source.InitializeScanChunk(chunk);
		source.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		REQUIRE(source.Scan(scan_state, chunk));
		REQUIRE(ListVector::GetChild(chunk.data[0]).GetBufferRef()->GetBufferType() ==
		        VectorBufferType::SHARED_DATA_BUFFER);
	};
	auto copy_first = [&](DataChunk &chunk) {
		ColumnDataCollection target(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
		ColumnDataAppendState target_append;
		target.InitializeAppend(target_append);
		target.Append(target_append, chunk);
		DataChunk result;
		target.InitializeScanChunk(result);
		ColumnDataScanState target_scan;
		target.InitializeScan(target_scan, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
		REQUIRE(target.Scan(target_scan, result));
		return result.GetValue(0, 0);
	};

	// a NULL set on the view must reach the appended copy, not be replaced by the unchanged shared storage
	DataChunk null_chunk;
	ColumnDataScanState null_chunk_scan;
	scan_view(null_chunk_scan, null_chunk);
	FlatVector::ValidityMutable(ListVector::GetChildMutable(null_chunk.data[0])).SetInvalid(0);
	REQUIRE(ListValue::GetChildren(copy_first(null_chunk))[0].IsNull());

	// a data write lands in a private copy: the appended copy sees it, the shared storage does not
	DataChunk write_chunk;
	ColumnDataScanState write_chunk_scan;
	scan_view(write_chunk_scan, write_chunk);
	FlatVector::GetDataMutable<int64_t>(ListVector::GetChildMutable(write_chunk.data[0]))[0] = 99;
	REQUIRE(ListValue::GetChildren(copy_first(write_chunk))[0] == Value::BIGINT(99));
	DataChunk fresh;
	ColumnDataScanState fresh_scan;
	scan_view(fresh_scan, fresh);
	REQUIRE(fresh.GetValue(0, 0) == input.GetValue(0, 0));
}

TEST_CASE("In-memory collections copy shared list children", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	const idx_t length = 3000;
	auto in_memory = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(),
	                                                 vector<LogicalType> {LogicalType::LIST(LogicalType::BIGINT)});
	{
		DuckDB db(":memory:");
		auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
		ColumnDataCollection source(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
		ColumnDataAppendState append_state;
		source.InitializeAppend(append_state);
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), source.Types());
		FillLargeListChunk(input, 1, length, 7);
		source.Append(append_state, input);

		DataChunk chunk;
		ColumnDataScanState scan_state;
		source.InitializeScanChunk(chunk);
		source.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		REQUIRE(source.Scan(scan_state, chunk));
		ColumnDataAppendState in_memory_append;
		in_memory->InitializeAppend(in_memory_append);
		in_memory->Append(in_memory_append, chunk);
	}
	// the database and its buffer manager are gone, the in-memory collection owns its data
	DataChunk result;
	in_memory->InitializeScanChunk(result);
	ColumnDataScanState scan_state;
	in_memory->InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(in_memory->Scan(scan_state, result));
	REQUIRE(ListVector::GetChild(result.data[0]).GetBufferRef()->GetBufferType() !=
	        VectorBufferType::SHARED_DATA_BUFFER);
	RequireLargeList(result, length, 7);
	in_memory.reset();
}

TEST_CASE("Chunk allocation estimates charge shared list children", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const idx_t length = 100000;
	ColumnDataCollection collection(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	for (idx_t c = 0; c < 2; c++) {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), collection.Types());
		FillLargeListChunk(input, STANDARD_VECTOR_SIZE, length, 0);
		collection.Append(append_state, input);
	}
	auto &segment = *collection.GetSegments()[0];
	REQUIRE(segment.ChunkCount() == 2);
	// the estimate covers the child values a chunk holds, as block storage of the same child would
	REQUIRE(segment.GetChunkAllocationSize(0) >= length * sizeof(int64_t));
	REQUIRE(segment.GetChunkAllocationSize(1) >= length * sizeof(int64_t));
	REQUIRE(collection.AllocationSize() >= 2 * length * sizeof(int64_t));
}

//! A buffer-managed collection holding one large BIGINT[] list per chunk, row zero of each chunk
static unique_ptr<ColumnDataCollection> MakeLargeListCollection(BufferManager &buffer_manager, idx_t chunks,
                                                                idx_t count, idx_t length) {
	auto collection =
	    make_uniq<ColumnDataCollection>(buffer_manager, vector<LogicalType> {LogicalType::LIST(LogicalType::BIGINT)});
	ColumnDataAppendState append_state;
	collection->InitializeAppend(append_state);
	for (idx_t c = 0; c < chunks; c++) {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), collection->Types());
		FillLargeListChunk(input, count, length, int64_t(c) * 10 + 1);
		collection->Append(append_state, input);
	}
	return collection;
}

//! Zero-copy scan of the first chunk, valid while 'scan_state' lives
static void ScanFirstChunk(ColumnDataCollection &collection, ColumnDataScanState &scan_state, DataChunk &chunk) {
	collection.InitializeScanChunk(chunk);
	collection.InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
	REQUIRE(collection.Scan(scan_state, chunk));
	REQUIRE(ListVector::GetChild(chunk.data[0]).GetBufferRef()->GetBufferType() ==
	        VectorBufferType::SHARED_DATA_BUFFER);
}

static Value AppendAndReadFirst(BufferManager &buffer_manager, DataChunk &chunk) {
	ColumnDataCollection target(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
	ColumnDataAppendState target_append;
	target.InitializeAppend(target_append);
	target.Append(target_append, chunk);
	DataChunk result;
	target.InitializeScanChunk(result);
	ColumnDataScanState target_scan;
	target.InitializeScan(target_scan, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
	REQUIRE(target.Scan(target_scan, result));
	return result.GetValue(0, 0);
}

TEST_CASE("Slices of a shared list view survive the view detaching", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	auto collection = MakeLargeListCollection(buffer_manager, 1, 1, 3000);
	DataChunk chunk;
	ColumnDataScanState chunk_scan;
	ScanFirstChunk(*collection, chunk_scan, chunk);
	auto &child = ListVector::GetChildMutable(chunk.data[0]);
	Vector slice(child, 1, 11);
	// detaching the parent must keep the storage the slice points into pinned
	FlatVector::GetDataMutable<int64_t>(child)[0] = 99;
	REQUIRE(slice.GetValue(0) == Value::BIGINT(2));
	REQUIRE(slice.GetValue(9) == Value::BIGINT(11));
	REQUIRE(child.GetValue(0) == Value::BIGINT(99));
}

TEST_CASE("Shared list views that diverge from their storage are copied", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	auto collection = MakeLargeListCollection(buffer_manager, 1, 1, 3000);

	// a NULL assigned through SetValue allocates a mask on the all-valid view alone
	DataChunk null_chunk;
	ColumnDataScanState null_chunk_scan;
	ScanFirstChunk(*collection, null_chunk_scan, null_chunk);
	ListVector::GetChildMutable(null_chunk.data[0]).SetValue(0, Value(LogicalType::BIGINT));
	REQUIRE(ListValue::GetChildren(AppendAndReadFirst(buffer_manager, null_chunk))[0].IsNull());

	// a view reinterpreted as a constant no longer presents the stored entries
	DataChunk constant_chunk;
	ColumnDataScanState constant_chunk_scan;
	ScanFirstChunk(*collection, constant_chunk_scan, constant_chunk);
	ListVector::GetChildMutable(constant_chunk.data[0]).SetVectorType(VectorType::CONSTANT_VECTOR);
	auto constant_children = ListValue::GetChildren(AppendAndReadFirst(buffer_manager, constant_chunk));
	REQUIRE(constant_children.size() == 3000);
	REQUIRE(constant_children[2999] == Value::BIGINT(1));
}

TEST_CASE("Repeated references to shared list storage are accounted once", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	const idx_t length = 100000;
	auto source = MakeLargeListCollection(buffer_manager, 1, STANDARD_VECTOR_SIZE, length);
	DataChunk chunk;
	ColumnDataScanState chunk_scan;
	ScanFirstChunk(*source, chunk_scan, chunk);
	ColumnDataCollection target(buffer_manager, {LogicalType::LIST(LogicalType::BIGINT)});
	ColumnDataAppendState target_append;
	target.InitializeAppend(target_append);
	for (idx_t i = 0; i < 4; i++) {
		target.Append(target_append, chunk);
	}
	auto payload = length * sizeof(int64_t);
	REQUIRE(target.AllocationSize() >= payload);
	REQUIRE(target.AllocationSize() < 2 * payload);
	// the chunk estimates still charge every reference
	auto &segment = *target.GetSegments()[0];
	idx_t estimated = 0;
	for (idx_t chunk_idx = 0; chunk_idx < segment.ChunkCount(); chunk_idx++) {
		estimated += segment.GetChunkAllocationSize(chunk_idx);
	}
	REQUIRE(estimated >= 4 * payload);
}

TEST_CASE("Consuming a collection releases its shared list children", "[column_data_compact]") {
	if (STANDARD_VECTOR_SIZE < 1024) {
		return;
	}
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	for (bool reverse : {false, true}) {
		auto collection = MakeLargeListCollection(buffer_manager, 3, STANDARD_VECTOR_SIZE, 3000);
		vector<weak_ptr<ColumnDataSharedListChild>> owners;
		{
			DataChunk chunk;
			ColumnDataScanState scan_state;
			collection->InitializeScanChunk(chunk);
			collection->InitializeScan(scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
			while (collection->Scan(scan_state, chunk)) {
				auto &view = ListVector::GetChild(chunk.data[0]).GetBufferRef()->Cast<ColumnDataListChildBuffer>();
				owners.push_back(view.GetOwner());
			}
		}
		REQUIRE(owners.size() == 3);

		ColumnDataConsumer consumer(*collection, {0});
		consumer.InitializeScan();
		vector<unique_ptr<ColumnDataConsumerScanState>> states;
		for (idx_t i = 0; i < 3; i++) {
			auto state = make_uniq<ColumnDataConsumerScanState>();
			state->current_chunk_state.properties = ColumnDataScanProperties::ALLOW_ZERO_COPY;
			REQUIRE(consumer.AssignChunk(*state));
			DataChunk chunk;
			collection->InitializeScanChunk(chunk);
			consumer.ScanChunk(*state, chunk);
			REQUIRE(ListValue::GetChildren(chunk.GetValue(0, 0)).size() == 3000);
			states.push_back(std::move(state));
		}
		// finishing in either order releases every consumed chunk, including the last one
		for (idx_t i = 0; i < 3; i++) {
			consumer.FinishChunk(*states[reverse ? 2 - i : i]);
		}
		// the collection is still alive, but nothing references the consumed storage anymore
		for (auto &owner : owners) {
			REQUIRE(owner.expired());
		}
	}
}

} // namespace duckdb
