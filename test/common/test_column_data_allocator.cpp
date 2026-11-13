#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/common/types/column/column_data_allocator.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

namespace duckdb {

TEST_CASE("ColumnDataAllocator copy constructor preserves managed_result_set", "[column_data_allocator]") {
	DuckDB db(":memory:");
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);

	ColumnDataAllocator original(buffer_manager, ColumnDataCollectionLifetime::THROW_ERROR_AFTER_DATABASE_CLOSES);
	REQUIRE(original.GetDatabase() != nullptr);

	ColumnDataAllocator copied(original);
	REQUIRE(copied.GetDatabase() != nullptr);
	REQUIRE(copied.GetType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Concurrent column scans repair relocated string heaps", "[column_data_allocator]") {
	auto config = GetTestConfig();
	config->SetOptionByName("temp_directory", TestCreatePath("column_data_relocation.tmp"));
	DuckDB db(nullptr, config.get());
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	ColumnDataCollection collection(buffer_manager, {LogicalType::VARCHAR});
	DataChunk input;
	collection.InitializeScanChunk(input);
	const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, 1024);
	vector<Value> expected;
	{
		auto writer = FlatVector::Writer<string_t>(input.data[0], count);
		for (idx_t i = 0; i < count; i++) {
			if (i % 13 == 0) {
				writer.WriteNull();
				expected.emplace_back(LogicalType::VARCHAR);
			} else {
				const auto value = i % 7 == 0 ? "short" : "long string payload for relocation " + to_string(i);
				writer.WriteValue(string_t(value));
				expected.emplace_back(value);
			}
		}
	}
	input.CheckCardinality(count);
	ColumnDataAppendState append_state;
	collection.InitializeAppend(append_state);
	collection.Append(append_state, input);
	vector<shared_ptr<BlockHandle>> handles;
	for (const auto &entry : append_state.current_chunk_state.handles) {
		handles.push_back(entry.second.GetBlockHandle());
	}
	REQUIRE(!handles.empty());
	append_state.current_chunk_state.handles.clear();

	for (idx_t round = 0; round < 3; round++) {
		vector<unique_ptr<FileBuffer>> old_buffers;
		vector<BufferHandle> pins;
		for (auto &handle : handles) {
			auto lock = handle->GetMemory().GetLock();
			REQUIRE(handle->GetMemory().CanUnload() == CanUnloadResult::CAN_UNLOAD);
			old_buffers.push_back(handle->GetMemory().UnloadAndTakeBlock(lock));
		}
		// Keep old allocations alive to force different addresses, and poison stale string pointers.
		for (idx_t i = 0; i < handles.size(); i++) {
			pins.push_back(buffer_manager.Pin(handles[i]));
			REQUIRE(pins.back().Ptr() != old_buffers[i]->GetData());
			old_buffers[i]->Clear();
		}

		// Copy first, repair shared data, then read it again without relocating.
		for (const auto properties :
		     {ColumnDataScanProperties::DISALLOW_ZERO_COPY, ColumnDataScanProperties::ALLOW_ZERO_COPY,
		      ColumnDataScanProperties::ALLOW_ZERO_COPY}) {
			atomic<idx_t> ready {0};
			atomic<bool> start {false};
			vector<string> errors(4);
			vector<thread> workers;
			for (idx_t worker = 0; worker < errors.size(); worker++) {
				workers.emplace_back([&, worker] {
					ready++;
					while (!start.load()) {
						std::this_thread::yield();
					}
					try {
						ColumnDataScanState scan;
						collection.InitializeScan(scan, properties);
						DataChunk result;
						collection.InitializeScanChunk(result);
						if (!collection.Scan(scan, result) || result.size() != expected.size()) {
							errors[worker] = "incorrect row count";
							return;
						}
						for (idx_t i = 0; i < expected.size(); i++) {
							const auto actual = result.GetValue(0, i);
							if (actual.IsNull() != expected[i].IsNull() ||
							    (!actual.IsNull() && !Value::DefaultValuesAreEqual(actual, expected[i]))) {
								errors[worker] = "incorrect string at row " + to_string(i);
								return;
							}
						}
					} catch (std::exception &ex) {
						errors[worker] = ex.what();
					}
				});
			}
			while (ready.load() != errors.size()) {
				std::this_thread::yield();
			}
			start = true;
			for (auto &worker : workers) {
				worker.join();
			}
			for (const auto &error : errors) {
				INFO(error);
				REQUIRE(error.empty());
			}
		}
	}
}
#endif

} // namespace duckdb
