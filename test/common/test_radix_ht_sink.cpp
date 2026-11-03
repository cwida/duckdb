#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/radix_ht_adaptivity.hpp"
#include "duckdb/execution/radix_ht_sink_state.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "test_helpers.hpp"

namespace duckdb {

class RadixSinkFixture {
public:
	RadixSinkFixture(const string &memory_limit, bool aggregate = false,
	                 const LogicalType &group_type = LogicalType::VARCHAR)
	    : db(nullptr), con(db), thread(*con.context), context(*con.context, thread, nullptr) {
		REQUIRE_NO_FAIL(con.Query("SET threads=4; SET memory_limit='" + memory_limit + "';"));
		REQUIRE_NO_FAIL(
		    con.Query("SET temp_directory = " + Value(TestCreatePath("radix_ht_sink_spill")).ToSQLString()));
		vector<unique_ptr<Expression>> groups;
		groups.push_back(make_uniq<BoundReferenceExpression>(group_type, 0));
		vector<unique_ptr<Expression>> aggregates;
		if (aggregate) {
			auto plan = con.ExtractPlan("SELECT i::VARCHAR k, string_agg(i::VARCHAR) FROM range(10) t(i) GROUP BY k");
			optional_ptr<LogicalOperator> aggr = plan.get();
			while (aggr->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
				aggr = aggr->children[0].get();
			}
			aggregates.push_back(aggr->expressions[0]->Copy());
			filter.push_back(0);
			payload.Initialize(Allocator::Get(*con.context), {LogicalType::VARCHAR});
			SetPayload(Value(LogicalType::VARCHAR));
		}
		op.InitializeGroupby(std::move(groups), std::move(aggregates), {});
		grouping_set.insert(ProjectionIndex(0));
		radix = make_uniq<RadixPartitionedHashTable>(grouping_set, op, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
		global = radix->GetGlobalSinkState(*con.context);
		for (idx_t i = 0; i < 4; i++) {
			locals.push_back(radix->GetLocalSinkState(context));
		}
		chunk.Initialize(Allocator::Get(*con.context), {group_type});
	}

	RadixHTGlobalSinkState &Global() {
		return global->Cast<RadixHTGlobalSinkState>();
	}

	RadixHTLocalSinkState &Local(idx_t index = 0) {
		return locals[index]->Cast<RadixHTLocalSinkState>();
	}

	void Keys(idx_t offset, idx_t distinct, idx_t width = 0, bool random = false) {
		chunk.Reset();
		{
			auto writer = FlatVector::Writer<string_t>(chunk.data[0], STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
				const auto row = offset + i;
				const auto key = (random ? Hash(row) : row) % distinct;
				const auto text = string(width, 'x') + std::to_string(key);
				writer.WriteValue(string_t(text));
			}
		}
		chunk.CheckCardinality(STANDARD_VECTOR_SIZE);
	}

	void SetPayload(const Value &value) {
		payload.data[0].Reference(value, count_t(STANDARD_VECTOR_SIZE));
		payload.CheckCardinality(STANDARD_VECTOR_SIZE);
	}

	void Sink(idx_t index = 0) {
		OperatorSinkInput input {*global, *locals[index], interrupt};
		radix->Sink(context, chunk, input, payload, filter);
	}

	void Combine(idx_t index) {
		radix->Combine(context, *global, *locals[index]);
	}

	idx_t ScanGroupCount(optional_ptr<const Value> expected_aggregate = nullptr) {
		radix->Finalize(*con.context, *global);
		radix->MaxThreads(*global);
		auto source = radix->GetGlobalSourceState(*con.context);
		auto local_source = radix->GetLocalSourceState(context);
		OperatorSourceInput input {*source, *local_source, interrupt};
		DataChunk output;
		auto types = chunk.GetTypes();
		types.insert(types.end(), op.aggregate_return_types.begin(), op.aggregate_return_types.end());
		output.Initialize(Allocator::Get(*con.context), types);
		idx_t count = 0;
		while (true) {
			output.Reset();
			const auto result = radix->GetData(context, output, *global, input);
			REQUIRE(result != SourceResultType::BLOCKED);
			if (expected_aggregate) {
				for (idx_t row = 0; row < output.size(); row++) {
					REQUIRE(output.GetValue(1, row) == *expected_aggregate);
				}
			}
			count += output.size();
			if (result == SourceResultType::FINISHED) {
				return count;
			}
		}
	}

public:
	DuckDB db;
	Connection con;
	ThreadContext thread;
	ExecutionContext context;
	InterruptState interrupt;
	GroupedAggregateData op;
	GroupingSet grouping_set;
	unique_ptr<RadixPartitionedHashTable> radix;
	unique_ptr<GlobalSinkState> global;
	vector<unique_ptr<LocalSinkState>> locals;
	DataChunk chunk;
	DataChunk payload;
	unsafe_vector<idx_t> filter;
};

TEST_CASE("Large aggregate tables spill before their occupancy limit", "[radix_ht_sink]") {
	RadixSinkFixture fixture("512MB");
	fixture.Keys(0, 65536);
	for (idx_t worker = 0; worker < 4; worker++) {
		fixture.Sink(worker);
		auto &local = fixture.Local(worker);
		local.ht->Resize(262144);
		local.local_sink_capacity = local.ht->Capacity();
	}
	for (idx_t offset = STANDARD_VECTOR_SIZE; offset < 65536; offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, 65536);
		for (idx_t worker = 0; worker < 4; worker++) {
			fixture.Sink(worker);
		}
	}
	auto &global = fixture.Global();
	REQUIRE_FALSE(global.external);
	for (idx_t worker = 0; worker < 4; worker++) {
		REQUIRE(fixture.Local(worker).ht->Capacity() > global.config.sink_capacity);
	}
	bool spilled_below_threshold = false;
	for (idx_t offset = 0; offset < 262144; offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, 262144, 800);
		for (idx_t worker = 0; worker < 4; worker++) {
			auto &local = fixture.Local(worker);
			const auto old_capacity = local.ht->Capacity();
			const auto old_count = local.ht->Count();
			fixture.Sink(worker);
			if (local.ht->Capacity() < old_capacity) {
				spilled_below_threshold |=
				    old_count + 2 * STANDARD_VECTOR_SIZE < GroupedAggregateHashTable::ResizeThreshold(old_capacity);
			}
		}
	}
	REQUIRE(global.external);
	REQUIRE(spilled_below_threshold);
	for (idx_t worker = 0; worker < 4; worker++) {
		auto &local = fixture.Local(worker);
		REQUIRE_FALSE(local.ht->GetAdaptivityState().HLLEnabled());
		REQUIRE(local.ht->Capacity() == global.config.sink_capacity);
		REQUIRE(local.local_sink_capacity == local.ht->Capacity());
		REQUIRE(local.abandoned_data);
		REQUIRE(local.abandoned_data->Count() > 0);
		fixture.Combine(worker);
	}
	REQUIRE(fixture.ScanGroupCount() == 327680);
}

TEST_CASE("Increasing the reservation preserves an underfull aggregate table", "[radix_ht_sink]") {
	RadixSinkFixture fixture("128MB");
	fixture.Keys(0, 65536);
	fixture.Sink();
	auto &local = fixture.Local();
	auto &global = fixture.Global();
	const auto old_count = local.ht->Count();
	// Simulate a reservation reduction while the pointer table still has spare capacity.
	global.temporary_memory_state->SetRemainingSizeAndUpdateReservation(*fixture.con.context,
	                                                                    local.ht->GetSizeInBytes());
	const auto old_limit = global.GetThreadLimit();
	REQUIRE(local.ht->GetSizeInBytes() > old_limit);
	REQUIRE(old_count + 2 * STANDARD_VECTOR_SIZE < local.ht->ResizeThreshold());
	fixture.Keys(STANDARD_VECTOR_SIZE, 65536);
	fixture.Sink();
	REQUIRE(global.GetThreadLimit() > old_limit);
	REQUIRE(local.ht->Count() == old_count + STANDARD_VECTOR_SIZE);
	REQUIRE_FALSE(local.spilling);
}

TEST_CASE("Arena pressure requests enough reservation to keep aggregate states resident", "[radix_ht_sink]") {
	for (const bool combine_other_worker : {false, true}) {
		CAPTURE(combine_other_worker);
		RadixSinkFixture fixture("1GB", true);
		fixture.Keys(0, STANDARD_VECTOR_SIZE);
		fixture.Sink();
		if (combine_other_worker) {
			fixture.Sink(1);
			fixture.Combine(1);
		}
		auto &local = fixture.Local();
		auto &global = fixture.Global();
		auto &ht = *local.ht;
		const Value payload(string(8 * 1024 * 1024 / STANDARD_VECTOR_SIZE, 'p'));
		fixture.SetPayload(payload);
		// Grow real states on hits, stopping before the sink makes its pressure decision.
		ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
		const auto total_size = ht.GetSizeInBytes();
		const auto arena_size = ht.GetAggregateAllocator()->AllocationSize();
		REQUIRE(arena_size * 5 > total_size * 4);
		REQUIRE(ht.Count() + STANDARD_VECTOR_SIZE < ht.ResizeThreshold());
		global.temporary_memory_state->SetRemainingSizeAndUpdateReservation(
		    *fixture.con.context, 3 * global.number_of_threads * total_size / 2);
		const auto old_limit = global.GetThreadLimit();
		REQUIRE(total_size <= old_limit);
		REQUIRE(global.StatePressureExceeded(ht));

		fixture.SetPayload(Value(LogicalType::VARCHAR));
		fixture.Sink();
		REQUIRE(global.GetThreadLimit() > old_limit);
		REQUIRE_FALSE(global.StatePressureExceeded(ht));
		REQUIRE_FALSE(global.external);
		REQUIRE_FALSE(local.spilling);
		REQUIRE(local.abandoned_exported_data.empty());
		REQUIRE(ht.Count() == STANDARD_VECTOR_SIZE);
		REQUIRE(ht.GetAdaptivityState().HLLEnabled());
		fixture.Combine(0);
		REQUIRE(fixture.ScanGroupCount(payload) == STANDARD_VECTOR_SIZE);
	}
}

TEST_CASE("Arena pressure still exports when the reservation cannot grow enough", "[radix_ht_sink]") {
	for (const bool force_external : {false, true}) {
		CAPTURE(force_external);
		RadixSinkFixture fixture("64MB", true);
		if (force_external) {
			REQUIRE_NO_FAIL(fixture.con.Query("SET debug_force_external=true"));
		}
		fixture.Keys(0, STANDARD_VECTOR_SIZE);
		fixture.Sink();
		auto &local = fixture.Local();
		auto &global = fixture.Global();
		auto &ht = *local.ht;
		const Value payload(string(8 * 1024 * 1024 / STANDARD_VECTOR_SIZE, 'p'));
		fixture.SetPayload(payload);
		ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
		REQUIRE(ht.Count() + STANDARD_VECTOR_SIZE < ht.ResizeThreshold());
		REQUIRE(global.StatePressureExceeded(ht));

		fixture.SetPayload(Value(LogicalType::VARCHAR));
		fixture.Sink();
		REQUIRE(global.temporary_memory_state->GetReservation() < global.temporary_memory_state->GetRemainingSize());
		REQUIRE(global.external);
		REQUIRE(local.spilling);
		REQUIRE_FALSE(local.abandoned_exported_data.empty());
		REQUIRE(ht.GetAggregateAllocator()->AllocationSize() == 0);
		fixture.Combine(0);
		REQUIRE(fixture.ScanGroupCount(payload) == STANDARD_VECTOR_SIZE);
	}
}

TEST_CASE("A reduced reservation does not prevent external aggregation", "[radix_ht_sink]") {
	RadixSinkFixture fixture("128MB");
	auto &global = fixture.Global();
	const auto radix_bits = global.config.GetRadixBits();
	const auto minimum_reservation = global.minimum_reservation.load();
	REQUIRE(radix_bits > 1);
	global.temporary_memory_state->SetRemainingSizeAndUpdateReservation(*fixture.con.context, 1);
	global.config.SetRadixBits(1);
	REQUIRE(global.config.GetRadixBits() == radix_bits);
	REQUIRE_FALSE(global.external);
	REQUIRE(global.config.SetRadixBitsToExternal());
	REQUIRE(global.config.GetRadixBits() == radix_bits);
	REQUIRE(global.minimum_reservation == minimum_reservation);
	fixture.Keys(0, STANDARD_VECTOR_SIZE);
	fixture.Sink();
	fixture.Combine(0);
	REQUIRE(fixture.ScanGroupCount() == STANDARD_VECTOR_SIZE);
}

TEST_CASE("Aggregate reservations without temporary storage cover the current usage", "[radix_ht_sink]") {
	RadixSinkFixture fixture("128MiB");
	REQUIRE_NO_FAIL(fixture.con.Query("SET temp_directory=''"));
	constexpr idx_t group_count = 16384;
	for (idx_t offset = 0; offset < group_count; offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, group_count, 2048);
		fixture.Sink();
	}
	auto &global = fixture.Global();
	auto &local = fixture.Local();
	REQUIRE_FALSE(global.external);
	REQUIRE_FALSE(local.spilling);
	REQUIRE(local.ht->Count() == group_count);
	fixture.Combine(0);
	REQUIRE(fixture.ScanGroupCount() == group_count);
}

TEST_CASE("Finished aggregate sources release their local hash tables", "[radix_ht_sink]") {
	RadixSinkFixture fixture("128MB");
	fixture.Keys(0, STANDARD_VECTOR_SIZE);
	fixture.Sink(0);
	fixture.Sink(1);
	fixture.Combine(0);
	fixture.Combine(1);
	fixture.radix->SetMultiScan(*fixture.global);
	fixture.radix->Finalize(*fixture.con.context, *fixture.global);
	fixture.radix->MaxThreads(*fixture.global);
	auto source = fixture.radix->GetGlobalSourceState(*fixture.con.context);
	auto scan = [&]() {
		auto local_source = fixture.radix->GetLocalSourceState(fixture.context);
		OperatorSourceInput input {*source, *local_source, fixture.interrupt};
		DataChunk output;
		output.Initialize(Allocator::Get(*fixture.con.context), fixture.chunk.GetTypes());
		idx_t count = 0;
		while (true) {
			output.Reset();
			const auto result = fixture.radix->GetData(fixture.context, output, *fixture.global, input);
			REQUIRE(result != SourceResultType::BLOCKED);
			count += output.size();
			if (result == SourceResultType::FINISHED) {
				break;
			}
		}
		REQUIRE(count == STANDARD_VECTOR_SIZE);
		auto &manager = BufferManager::GetBufferManager(*fixture.con.context);
		const auto used_memory = manager.GetUsedMemory();
		local_source.reset();
		return used_memory - manager.GetUsedMemory();
	};
	const auto first_scan_retained = scan();
	fixture.radix->ResetGlobalSourceState(*fixture.con.context, *source);
	// A rescan needs the same scan buffers but no hash table to finalize the partitions.
	REQUIRE(first_scan_retained == scan());
}

TEST_CASE("State export retires aggregate telemetry even after another worker combines", "[radix_ht_sink]") {
	for (const idx_t input_count : {idx_t(262144), idx_t(1114112)}) {
		CAPTURE(input_count);
		RadixSinkFixture fixture("512MB", true);
		fixture.Keys(0, 65536);
		fixture.Sink();
		fixture.Local().ht->Resize(262144);
		fixture.Local().local_sink_capacity = fixture.Local().ht->Capacity();
		for (idx_t offset = STANDARD_VECTOR_SIZE; offset < input_count; offset += STANDARD_VECTOR_SIZE) {
			fixture.Keys(offset, 65536);
			fixture.Sink();
		}
		auto &local = fixture.Local();
		REQUIRE(local.ht->Capacity() > fixture.Global().config.sink_capacity);
		REQUIRE(local.ht->GetAdaptivityState().HLLEnabled());
		fixture.Sink(1);
		fixture.Combine(1);
		REQUIRE(fixture.Global().any_combined);
		fixture.SetPayload(Value(string(2048, 'p')));
		for (idx_t offset = 0; offset < 65536 && local.abandoned_exported_data.empty();
		     offset += STANDARD_VECTOR_SIZE) {
			fixture.Keys(offset, 65536);
			fixture.Sink();
		}
		REQUIRE_FALSE(local.abandoned_exported_data.empty());
		REQUIRE_FALSE(fixture.Global().external);
		REQUIRE_FALSE(local.ht->GetAdaptivityState().HLLEnabled());
		REQUIRE(local.spilling);
		REQUIRE(local.ht->Capacity() == fixture.Global().config.sink_capacity);
		fixture.SetPayload(Value(LogicalType::VARCHAR));
		for (idx_t offset = 0; offset < 1048576; offset += STANDARD_VECTOR_SIZE) {
			fixture.Keys(offset, 65536);
			fixture.Sink();
		}
		fixture.radix->ResetLocalSinkState(fixture.context, *fixture.global, *fixture.locals[0]);
		REQUIRE(local.ht->GetAdaptivityState().HLLEnabled());
		REQUIRE_FALSE(local.spilling);
		REQUIRE_FALSE(local.ht->GetAdaptivityState().LookupsSkipped());
		REQUIRE(local.ht->GetAdaptivityState().GetSinkCount() == 0);
		REQUIRE(local.ht->GetAdaptivityState().GetCycleInputCount() == 0);
		REQUIRE(local.ht->GetAdaptivityState().GetSkippedInputCount() == 0);
	}
}

TEST_CASE("Aggregate lookup retries recover a small recurring domain after unique input", "[radix_ht_sink]") {
	// Keep chunk metadata at tiny vector sizes from triggering spill before the lookup decision.
	RadixSinkFixture fixture("8GB");
	const auto sample_size = RadixHTAdaptivity::LOOKUP_SAMPLE_SIZE;
	idx_t offset = 0;
	for (; offset < sample_size + 2 * fixture.Global().config.sink_capacity; offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, NumericLimits<idx_t>::Maximum());
		fixture.Sink();
		if (fixture.Local().ht->GetAdaptivityState().LookupsSkipped()) {
			offset += STANDARD_VECTOR_SIZE;
			break;
		}
	}
	auto &ht = *fixture.Local().ht;
	CAPTURE(ht.GetAdaptivityState().GetSinkCount(), ht.GetSizeInBytes(), fixture.Global().GetThreadLimit(),
	        fixture.Global().external.load());
	REQUIRE(ht.GetAdaptivityState().LookupsSkipped());
	REQUIRE(ht.GetAdaptivityState().HLLEnabled());
	REQUIRE(ht.Count() == 0);
	const auto capacity = ht.Capacity();
	REQUIRE(capacity == fixture.Global().config.sink_capacity);
	REQUIRE(ht.GetAdaptivityState().GetSkippedInputCount() == 0);

	// Ordinary abandonment must not reset the skipped-input interval.
	for (; ht.GetAdaptivityState().GetSkippedInputCount() + STANDARD_VECTOR_SIZE < sample_size;
	     offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, NumericLimits<idx_t>::Maximum());
		fixture.Sink();
		REQUIRE(ht.GetAdaptivityState().LookupsSkipped());
	}
	const auto unique_count = offset;
	const auto estimate = ht.GetAdaptivityState().GetHLLUpperBound();
	for (idx_t retry_input = 0; retry_input < 2 * capacity && ht.GetAdaptivityState().LookupsSkipped();
	     retry_input += STANDARD_VECTOR_SIZE) {
		fixture.Keys(retry_input, 64);
		fixture.Sink();
	}
	REQUIRE_FALSE(ht.GetAdaptivityState().LookupsSkipped());
	REQUIRE_FALSE(fixture.Global().external);
	REQUIRE(ht.GetAdaptivityState().HLLEnabled());
	REQUIRE(ht.GetAdaptivityState().GetHLLUpperBound() == estimate);
	REQUIRE(ht.Capacity() == capacity);
	REQUIRE(ht.Count() == 0);
	REQUIRE(ht.GetAdaptivityState().GetCycleInputCount() == 0);
	const auto materialized = ht.GetMaterializedCount();
	for (idx_t hot_input = 0; hot_input < 1024; hot_input += STANDARD_VECTOR_SIZE) {
		fixture.Keys(hot_input, 64);
		fixture.Sink();
	}
	REQUIRE(ht.Count() == 64);
	REQUIRE(ht.GetMaterializedCount() == materialized + 64);
	fixture.Combine(0);
	REQUIRE(fixture.ScanGroupCount() == unique_count);
}

TEST_CASE("Aggregate growth converges when cardinality is just above an occupancy limit", "[radix_ht_sink]") {
	RadixSinkFixture fixture("2GB", false, LogicalType::BIGINT);
	idx_t previous_capacity = fixture.Global().config.sink_capacity;
	idx_t growth_count = 0;
	for (idx_t offset = 0; offset < 800000; offset += STANDARD_VECTOR_SIZE) {
		fixture.chunk.Reset();
		{
			auto writer = FlatVector::Writer<int64_t>(fixture.chunk.data[0], STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
				writer.WriteValue(132000 + (offset + i) % 44000);
			}
		}
		fixture.chunk.CheckCardinality(STANDARD_VECTOR_SIZE);
		fixture.Sink();
		const auto capacity = fixture.Local().ht->Capacity();
		if (capacity != previous_capacity) {
			REQUIRE_FALSE(fixture.Global().external);
			REQUIRE(capacity > previous_capacity);
			previous_capacity = capacity;
			growth_count++;
		}
	}
	REQUIRE(growth_count > 0);
	REQUIRE(fixture.Local().ht->Count() == 44000);
	REQUIRE_FALSE(fixture.Local().ht->GetAdaptivityState().LookupsSkipped());
	fixture.Combine(0);
	REQUIRE(fixture.ScanGroupCount() == 44000);
}

TEST_CASE("Spilling tolerates memory pressure while shrinking retained pointer allocations", "[radix_ht_sink]") {
	for (const bool memory_pressure : {false, true}) {
		CAPTURE(memory_pressure);
		RadixSinkFixture fixture("128MB");
		fixture.Keys(0, STANDARD_VECTOR_SIZE);
		fixture.Sink();
		auto &local = fixture.Local();
		local.ht->Resize(1048576);
		fixture.radix->ResetLocalSinkState(fixture.context, *fixture.global, *fixture.locals[0]);
		fixture.Sink();
		const auto old_capacity = local.ht->Capacity();
		REQUIRE(old_capacity < fixture.Global().config.sink_capacity);
		const auto retained_size = local.ht->GetSizeInBytes();
		const auto old_materialized = local.ht->GetMaterializedCount();
		REQUIRE(retained_size >= 1048576 * sizeof(ht_entry_t));
		{
			AllocatedData competing_allocation;
			if (memory_pressure) {
				auto &manager = BufferManager::GetBufferManager(*fixture.con.context);
				competing_allocation =
				    manager.GetBufferAllocator().Allocate(manager.GetMaxMemory() - manager.GetUsedMemory() - 65536);
			}
			REQUIRE(fixture.Global().config.SetRadixBitsToExternal());
			REQUIRE_NOTHROW(fixture.Sink());
			REQUIRE(local.spilling);
			REQUIRE_FALSE(local.ht->GetAdaptivityState().HLLEnabled());
			REQUIRE(local.ht->GetMaterializedCount() == old_materialized + STANDARD_VECTOR_SIZE);
			REQUIRE(local.local_sink_capacity == local.ht->Capacity());
			if (memory_pressure) {
				REQUIRE(local.ht->Capacity() == old_capacity);
				REQUIRE(local.ht->GetSizeInBytes() >= retained_size);
			} else {
				REQUIRE(local.ht->Capacity() == fixture.Global().config.sink_capacity);
				REQUIRE(local.ht->GetSizeInBytes() < retained_size);
			}
		}
		fixture.Sink();
		fixture.Combine(0);
		REQUIRE(fixture.ScanGroupCount() == STANDARD_VECTOR_SIZE);
	}
}

TEST_CASE("Aggregate memory accounting tracks hits and replacement tuple storage", "[radix_ht_sink]") {
	RadixSinkFixture fixture("128MB", true);
	fixture.Keys(0, STANDARD_VECTOR_SIZE, 800);
	fixture.Sink();
	auto &ht = *fixture.Local().ht;
	const auto wide_size = ht.GetSizeInBytes();
	const auto materialized = ht.GetMaterializedCount();
	const auto old_arena_size = ht.GetAggregateAllocator()->AllocationSize();
	fixture.SetPayload(Value(string(2048, 'p')));
	// Updating existing groups can grow aggregate states without allocating tuple blocks.
	ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
	REQUIRE(ht.GetMaterializedCount() == materialized);
	const auto arena_growth = ht.GetAggregateAllocator()->AllocationSize() - old_arena_size;
	REQUIRE(arena_growth > 0);
	REQUIRE(ht.GetSizeInBytes() == wide_size + arena_growth);

	fixture.SetPayload(Value(LogicalType::VARCHAR));
	ht.ResetForNewIteration(ht.GetRadixBits());
	fixture.Keys(0, STANDARD_VECTOR_SIZE);
	ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
	REQUIRE(ht.GetMaterializedCount() == materialized);
	const auto narrow_size = ht.GetSizeInBytes();
	REQUIRE(narrow_size < wide_size);

	ht.Abandon();
	ht.SetRadixBits(ht.GetRadixBits() + 1);
	ht.Repartition();
	REQUIRE(ht.GetMaterializedCount() == materialized);
	const auto repartitioned_size = ht.GetSizeInBytes();
	auto old_data = ht.AcquirePartitionedData();
	REQUIRE(ht.GetSizeInBytes() < repartitioned_size);
	const auto empty_size = ht.GetSizeInBytes();
	ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
	REQUIRE(ht.GetMaterializedCount() == materialized);
	REQUIRE(ht.GetSizeInBytes() > empty_size);
}

TEST_CASE("Failed adaptive growth preserves rows and falls back to the existing table", "[radix_ht_sink]") {
	RadixSinkFixture fixture("512MB");
	fixture.Keys(0, 65536);
	fixture.Sink();
	auto &local = fixture.Local();
	auto &global = fixture.Global();
	// Materialize repeated groups without attempting the optional growth yet.
	for (idx_t offset = STANDARD_VECTOR_SIZE; offset < 6 * 65536; offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, 65536);
		if (local.ht->Count() + STANDARD_VECTOR_SIZE >= local.ht->ResizeThreshold()) {
			local.ht->Abandon();
		}
		local.ht->AddChunk(fixture.chunk, fixture.payload, fixture.filter);
	}
	// Leave policy headroom even when tiny vectors require more tuple metadata.
	const auto reservation = 5 * (local.ht->GetSizeInBytes() + 4 * 1024 * 1024);
	global.temporary_memory_state->SetMinimumReservation(reservation);
	global.temporary_memory_state->SetRemainingSizeAndUpdateReservation(*fixture.con.context, reservation);
	REQUIRE(local.ht->GetAdaptivityState().HLLEnabled());
	const auto old_capacity = local.ht->Capacity();
	const auto old_materialized = local.ht->GetMaterializedCount();
	auto &manager = BufferManager::GetBufferManager(*fixture.con.context);
	{
		auto competing_allocation =
		    manager.GetBufferAllocator().Allocate(manager.GetMaxMemory() - manager.GetUsedMemory() - 65536);
		REQUIRE_FALSE(RadixHTAdaptivity::TryGrow(global, local));
		REQUIRE_FALSE(local.ht->GetAdaptivityState().HLLEnabled());
		REQUIRE(local.ht->Capacity() == old_capacity);
		REQUIRE(local.local_sink_capacity == old_capacity);
		REQUIRE(local.ht->GetMaterializedCount() == old_materialized);
		REQUIRE(local.ht->Count() == 0);
	}
	for (idx_t offset = 6 * 65536; offset < 7 * 65536; offset += STANDARD_VECTOR_SIZE) {
		fixture.Keys(offset, 65536);
		fixture.Sink();
	}
	fixture.Combine(0);
	REQUIRE(fixture.ScanGroupCount() == 65536);
}

TEST_CASE("Aggregate append pins are released before spilling", "[radix_ht_sink]") {
	const auto group_count = MinValue<idx_t>(16, STANDARD_VECTOR_SIZE);
	for (const bool acquire : {false, true}) {
		CAPTURE(acquire);
		RadixSinkFixture fixture("128MB");
		fixture.Global().temporary_memory_state->SetMinimumReservation(64 * 1024 * 1024);
		fixture.Global().temporary_memory_state->SetRemainingSizeAndUpdateReservation(*fixture.con.context,
		                                                                              64 * 1024 * 1024);
		fixture.Global().config.SetRadixBits(3);
		fixture.Keys(0, group_count, 192000 / group_count);
		fixture.Sink();
		auto &ht = *fixture.Local().ht;
		REQUIRE(ht.GetRadixBits() >= 3);
		REQUIRE(ht.Count() == group_count);
		REQUIRE(ht.GetPartitionedData().Count() == 0);
		fixture.Global().any_abandoned = true;
		// Leave completed blocks available for eviction alongside the current append blocks.
		for (idx_t i = 0; i < 7; i++) {
			ht.Abandon();
			ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
		}
		const auto materialized = ht.GetMaterializedCount();
		REQUIRE(materialized == 8 * group_count);
		auto &manager = BufferManager::GetBufferManager(*fixture.con.context);
		{
			auto competing_allocation =
			    manager.GetBufferAllocator().Allocate(manager.GetMaxMemory() - manager.GetUsedMemory() - 65536);
			if (acquire) {
				auto data = ht.AcquirePartitionedData();
				REQUIRE(data->Count() == materialized);
				REQUIRE(ht.GetMaterializedCount() == 0);
				continue;
			}
			REQUIRE_NOTHROW(ht.Abandon());
			REQUIRE(ht.GetMaterializedCount() == materialized);
		}
		// A pressure episode may be followed by another resident append to a partial block.
		for (idx_t i = 0; i < 3; i++) {
			ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter);
			ht.Abandon();
		}
		REQUIRE(ht.GetMaterializedCount() == materialized + 3 * group_count);
		fixture.Combine(0);
		REQUIRE(fixture.ScanGroupCount() == group_count);
	}
}

TEST_CASE("Abandoned aggregate blocks can be evicted before the next sink call", "[radix_ht_sink]") {
	for (const bool direct : {false, true}) {
		if (direct && STANDARD_VECTOR_SIZE < 64) {
			continue;
		}
		CAPTURE(direct);
		const auto group_count = MinValue<idx_t>(direct ? 64 : 16, STANDARD_VECTOR_SIZE);
		RadixSinkFixture fixture("128MB");
		fixture.Global().temporary_memory_state->SetMinimumReservation(64 * 1024 * 1024);
		fixture.Global().temporary_memory_state->SetRemainingSizeAndUpdateReservation(*fixture.con.context,
		                                                                              64 * 1024 * 1024);
		fixture.Global().config.SetRadixBits(3);
		fixture.Keys(0, group_count, 192000 / group_count);
		fixture.Sink();
		auto &ht = *fixture.Local().ht;
		REQUIRE(ht.GetRadixBits() == 3);
		REQUIRE(ht.Count() == group_count);
		REQUIRE(ht.GetPartitionedData().Count() == (direct ? group_count : 0));
		ht.EnableHLL(false);
		// Exercise the sink's occupancy-triggered abandon without growing a large key domain.
		fixture.Local().local_sink_capacity = STANDARD_VECTOR_SIZE;
		for (idx_t i = 0; i < 32; i++) {
			fixture.Sink();
			REQUIRE(ht.Count() == 0);
		}
		fixture.Local().local_sink_capacity = ht.Capacity();
		fixture.Sink();
		REQUIRE_FALSE(fixture.Global().external);
		REQUIRE(ht.Count() == group_count);
		REQUIRE(ht.GetMaterializedCount() == 33 * group_count);
		auto &manager = BufferManager::GetBufferManager(*fixture.con.context);
		const auto used = manager.GetUsedMemory();
		const idx_t extra = 2 * 1024 * 1024;
		REQUIRE(used > extra);
		REQUIRE(used < manager.GetMaxMemory());
		{
			// Reclaim completed blocks while the current groups are still referenced by the hash table.
			auto allocation = manager.GetBufferAllocator().Allocate(manager.GetMaxMemory() - used + extra);
			allocation.get()[0] = 1;
			REQUIRE(ht.AddChunk(fixture.chunk, fixture.payload, fixture.filter) == 0);
			REQUIRE(ht.Count() == group_count);
			REQUIRE(ht.GetMaterializedCount() == 33 * group_count);
		}
		fixture.Combine(0);
		REQUIRE(fixture.ScanGroupCount() == group_count);
	}
}

} // namespace duckdb
