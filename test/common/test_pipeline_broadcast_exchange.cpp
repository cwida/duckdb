#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <thread>

namespace duckdb {

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Completed broadcast spools support concurrent spilling scans", "[broadcast_spool]") {
	DuckDB db;
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='64MB'"));
	REQUIRE_NO_FAIL(con.Query("SET temp_directory=" + Value(TestCreatePath("broadcast_spool_spill")).ToSQLString()));
	auto source =
	    con.Query("SELECT i, CASE WHEN i % 7 != 0 THEN repeat('abcdefghijklmnopqrstuvwxyz', 16) || i::VARCHAR "
	              "END s, [s, NULL, s] l FROM range(1, 998) t(i)");
	REQUIRE_NO_FAIL(*source);
	auto input = source->Fetch();
	REQUIRE(input);
	auto types = input->GetTypes();
	auto &allocator = BufferAllocator::Get(*con.context);
	PipelineBroadcastExchange exchange(*con.context, types, PipelineBroadcastExchangeCompletionMode::RUN_TO_COMPLETION,
	                                   OrderPreservationType::NO_ORDER, false,
	                                   PipelineBroadcastExchangeBufferMode::BUFFER_ALL);
	const idx_t reader_count = 4;
	const idx_t chunk_count = (64000 + input->size() - 1) / input->size();
	for (idx_t i = 0; i < reader_count; i++) {
		exchange.SelectBufferedConsumer(exchange.RegisterConsumer(), PipelineBroadcastExchangeScanMode::CHUNK);
	}

	// Reuse the exchange, retaining each previous scan's pins until its output has been checked.
	for (idx_t epoch = 0; epoch < 2; epoch++) {
		exchange.Reset();
		auto local = exchange.GetLocalState(*con.context, 0);
		SourcePartitionInfo partition;
		InterruptState interrupt;
		for (idx_t i = 0; i < chunk_count; i++) {
			REQUIRE(exchange.Push(*input, *local, partition, interrupt) == SinkResultType::NEED_MORE_INPUT);
		}
		REQUIRE(BufferManager::GetBufferManager(*con.context).GetUsedSwap() > 0);
		exchange.Finish();

		vector<shared_ptr<PipelineBroadcastExchangeScanState>> states;
		vector<unique_ptr<DataChunk>> outputs;
		vector<string> errors(reader_count);
		vector<idx_t> counts(reader_count, 0);
		vector<std::thread> readers;
		for (idx_t i = 0; i < reader_count; i++) {
			states.push_back(exchange.GetScanState());
			auto output = make_uniq<DataChunk>();
			output->Initialize(allocator, types);
			outputs.push_back(std::move(output));
		}
		for (idx_t i = 0; i < reader_count; i++) {
			readers.emplace_back([&, i] {
				try {
					auto &output = *outputs[i];
					optional_idx batch;
					SourceBatchIndexState batch_state;
					while (exchange.Scan(i, output, *states[i], batch, batch_state, interrupt) !=
					       SourceResultType::FINISHED) {
						for (idx_t row = 0; row < output.size(); row++) {
							auto input_row = counts[i]++ % input->size();
							for (idx_t col = 0; col < types.size(); col++) {
								if (!Value::NotDistinctFrom(output.GetValue(col, row),
								                            input->GetValue(col, input_row))) {
									throw InternalException("Incorrect broadcast spool value");
								}
							}
						}
					}
				} catch (std::exception &ex) {
					errors[i] = ex.what();
				}
			});
		}
		for (auto &reader : readers) {
			reader.join();
		}
		for (idx_t i = 0; i < reader_count; i++) {
			INFO(errors[i]);
			REQUIRE(errors[i].empty());
			REQUIRE(counts[i] == chunk_count * input->size());
		}
		// All consumers have retired: returned vectors still borrow their local reader's pins.
		exchange.Cancel();
		for (auto &output : outputs) {
			REQUIRE(output->size() == input->size());
			for (idx_t col = 0; col < types.size(); col++) {
				REQUIRE(Value::NotDistinctFrom(output->GetValue(col, 0), input->GetValue(col, 0)));
				REQUIRE(Value::NotDistinctFrom(output->GetValue(col, input->size() - 1),
				                               input->GetValue(col, input->size() - 1)));
			}
		}
	}
}
#endif

} // namespace duckdb
