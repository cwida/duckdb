#include "catch.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

namespace {
struct LazyAllocationState : PrivateAllocatorData {
	atomic<idx_t> allocations {0};
	atomic<idx_t> live_bytes {0};
	atomic<bool> fail {false};

	static data_ptr_t Allocate(PrivateAllocatorData *data, idx_t size) {
		auto &state = data->Cast<LazyAllocationState>();
		if (state.fail) {
			throw OutOfMemoryException("lazy vector test allocation failure");
		}
		auto result = Allocator::DefaultAllocator().AllocateData(size);
		state.allocations++;
		state.live_bytes += size;
		return result;
	}
	static void Free(PrivateAllocatorData *data, data_ptr_t pointer, idx_t size) {
		data->Cast<LazyAllocationState>().live_bytes -= size;
		Allocator::DefaultAllocator().FreeData(pointer, size);
	}
	static data_ptr_t Reallocate(PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
		auto result = Allocate(data, size);
		memcpy(result, pointer, MinValue(old_size, size));
		Free(data, pointer, old_size);
		return result;
	}
};

void RequireLazyNestedStorage(const Vector &vec, Allocator &allocator, idx_t capacity) {
	REQUIRE(vec.size() == 0);
	REQUIRE(vec.Buffer().Capacity() == capacity);
	switch (vec.GetType().InternalType()) {
	case PhysicalType::STRUCT:
		for (auto &child : StructVector::GetEntries(vec)) {
			RequireLazyNestedStorage(child, allocator, capacity);
		}
		return;
	case PhysicalType::ARRAY:
		RequireLazyNestedStorage(ArrayVector::GetChild(vec), allocator, capacity * ArrayType::GetSize(vec.GetType()));
		return;
	case PhysicalType::LIST:
		RequireLazyNestedStorage(ListVector::GetChild(vec), allocator, capacity);
		break;
	default:
		break;
	}
	REQUIRE(FlatVector::GetData(vec) == nullptr);
	REQUIRE(vec.Buffer().GetAllocator().get() == &allocator);
}

void CheckLazyCAPIInput(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto &allocations = *static_cast<LazyAllocationState *>(duckdb_scalar_function_get_extra_info(info));
	auto child = duckdb_list_vector_get_child(duckdb_data_chunk_get_vector(input, 0));
	allocations.fail = true;
	try {
		if (!duckdb_vector_get_data(child)) {
			throw InternalException("C API input child has no payload");
		}
	} catch (std::exception &ex) {
		allocations.fail = false;
		duckdb_scalar_function_set_error(info, ex.what());
		return;
	}
	allocations.fail = false;
	auto count = duckdb_data_chunk_get_size(input);
	auto writer = FlatVector::Writer<int64_t>(*reinterpret_cast<Vector *>(output), count);
	for (idx_t i = 0; i < count; i++) {
		writer.WriteValue(1);
	}
}
} // namespace

TEST_CASE("Chunk payload allocation is lazy by default", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	DataChunk chunk;
	chunk.Initialize(allocator, {LogicalType::BIGINT, LogicalType::VARCHAR}, 4096);
	REQUIRE(chunk.GetCapacity() == 4096);
	for (idx_t iteration = 0; iteration < 3; iteration++) {
		REQUIRE(chunk.size() == 0);
		REQUIRE(FlatVector::GetData(chunk.data[0]) == nullptr);
		REQUIRE(FlatVector::GetData(chunk.data[1]) == nullptr);
		UnifiedVectorFormat format;
		chunk.data[0].ToUnifiedFormat(format);
		REQUIRE(chunk.data[0].Values<int64_t>().size() == 0);
		chunk.Reset();
	}
	REQUIRE(allocations.allocations == 0);
	{
		auto writer = FlatVector::Writer<int64_t>(chunk.data[0], 2);
		writer.WriteNull();
		writer.WriteValue(12);
	}
	auto data = FlatVector::GetData(chunk.data[0]);
	REQUIRE(allocations.allocations == 1);
	for (idx_t iteration = 0; iteration < 3; iteration++) {
		chunk.Reset();
		auto writer = FlatVector::Writer<int64_t>(chunk.data[0], 2);
		writer.WriteValue(41);
		writer.WriteValue(42);
		REQUIRE(FlatVector::GetData(chunk.data[0]) == data);
		REQUIRE(chunk.data[0].GetValue(0) == Value::BIGINT(41));
	}
	REQUIRE(allocations.allocations == 1);
	REQUIRE(FlatVector::GetData(chunk.data[1]) == nullptr);
	chunk.Destroy();
	REQUIRE(allocations.live_bytes == 0);
}

TEST_CASE("Chunk reset preserves borrowed buffers and wrappers", "[lazy_vector]") {
	SelectionVector sel(2);
	sel.set_index(0, 1);
	sel.set_index(1, 0);
	for (auto &value : {Value::BIGINT(7), Value::LIST(LogicalType::BIGINT, {Value::BIGINT(7)})}) {
		INFO(value.type().ToString());
		Vector input(value.type(), 2);
		input.Append(value);
		input.Append(Value(value.type()));
		Vector dictionary(input, sel, 2);
		DataChunk output;
		output.Initialize(Allocator::DefaultAllocator(), {value.type()});
		for (auto &source : {&input, &dictionary}) {
			output.data[0].Reference(*source);
			output.Reset();
			output.data[0].Append(Value(value.type()));
			REQUIRE(source->size() == 2);
			REQUIRE(input.GetValue(0) == value);
			REQUIRE(dictionary.GetValue(0).IsNull());
			REQUIRE(dictionary.GetValue(1) == value);
			REQUIRE(output.data[0].GetBufferRef() != source->GetBufferRef());
		}
		output.data[0].Slice(input, 0, 1);
		output.Reset();
		REQUIRE(input.size() == 2);
		REQUIRE(input.GetValue(0) == value);
	}
}

TEST_CASE("Dictionary composition preserves shared selections and global entries", "[lazy_vector]") {
	auto entry = DictionaryVector::CreateReusableGlobalDictionary(LogicalType::BIGINT, 3);
	{
		auto writer = FlatVector::Writer<int64_t>(entry->data, 3);
		writer.WriteValue(10);
		writer.WriteValue(20);
		writer.WriteValue(30);
	}
	DataChunk input, output;
	input.InitializeEmpty({LogicalType::BIGINT, LogicalType::BIGINT});
	output.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT, LogicalType::BIGINT}, 3);
	SelectionVector original(3), select(2);
	original.set_index(0, 2);
	original.set_index(1, 0);
	original.set_index(2, 1);
	select.set_index(0, 2);
	select.set_index(1, 0);
	for (auto &column : input.data) {
		column.Dictionary(entry, original, 3);
	}
	output.Slice(input, select, 2);
	duckdb::weak_ptr<VectorBuffer> retained = output.data[0].GetBufferRef();
	for (idx_t iteration = 0; iteration < 3; iteration++) {
		retained = output.data[0].GetBufferRef();
		output.Reset();
		REQUIRE(retained.expired());
		REQUIRE(output.size() == 0);
		REQUIRE(output.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		REQUIRE(output.GetAllocationSize() == 0);
		REQUIRE(output.data[0].Values<int64_t>().size() == 0);
		output.data[0].Verify();
		output.Slice(input, select, 2);
		REQUIRE(DictionaryVector::SelVector(output.data[0]).data() ==
		        DictionaryVector::SelVector(output.data[1]).data());
		REQUIRE(DictionaryVector::IsGlobalDictionary(output.data[0]));
		REQUIRE(DictionaryVector::DictionaryId(output.data[0]) == entry->id);
		REQUIRE(output.GetValue(0, 0) == Value::BIGINT(20));
		REQUIRE(output.GetValue(1, 1) == Value::BIGINT(30));
		REQUIRE(input.data[0].size() == 3);
		REQUIRE(original.get_index(0) == 2);
	}
	output.Reset();
	REQUIRE(output.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
	SECTION("Append uses the cached capacity") {
		output.Append(input, VectorAppendMode::ERROR_ON_NO_SPACE);
		REQUIRE(output.GetValue(0, 2) == Value::BIGINT(20));
	}
	SECTION("Copy writes to the cached storage") {
		input.Copy(output);
		REQUIRE(output.GetValue(0, 2) == Value::BIGINT(20));
	}
	SECTION("Publishing cardinality allows writing") {
		output.SetChildCardinality(1);
		for (auto &column : output.data) {
			auto writer = FlatVector::Writer<int64_t>(column, 1);
			writer.WriteValue(9);
		}
		REQUIRE(output.GetValue(0, 0) == Value::BIGINT(9));
	}
	REQUIRE(retained.expired());
}

TEST_CASE("Empty lazy buffers preserve slicing ownership", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	DataChunk chunk;
	chunk.Initialize(allocator, {LogicalType::BIGINT}, 8);
	Vector input(Value::BIGINT(7), count_t(1));
	input.Flatten();
	SelectionVector sel(1);
	sel.set_index(0, 0);
	auto &source = chunk.data[0];
	source.Slice(input, sel, 1);
	chunk.Reset();
	auto owned_buffer = source.GetBufferRef();
	REQUIRE(source.GetVectorType() == VectorType::FLAT_VECTOR);
	REQUIRE(allocations.allocations == 0);

	SECTION("Range slicing creates a separate zero-capacity view") {
		Vector result(source.GetType(), nullptr);
		result.Slice(source, 0, 0);
		REQUIRE(result.GetBufferRef() != owned_buffer);
		REQUIRE(FlatVector::GetCapacity(result) == 0);
		result.Append(Value::BIGINT(42), VectorAppendMode::ALLOW_RESIZE);
		REQUIRE(result.GetValue(0) == Value::BIGINT(42));
		REQUIRE(owned_buffer->Size() == 0);
	}
	SECTION("Selected slicing creates an ordinary dictionary") {
		auto result = Vector::Ref(source);
		result.Slice(sel, 0);
		REQUIRE(result.GetVectorType() == VectorType::DICTIONARY_VECTOR);
		REQUIRE(result.GetBufferRef() != source.GetBufferRef());
		REQUIRE(result.size() == 0);
		REQUIRE(allocations.allocations == 0);
	}
	SECTION("Selected flatten produces independent writable storage") {
		auto result = Vector::Ref(source);
		result.Flatten(sel, 0);
		REQUIRE(result.GetBufferRef() != owned_buffer);
		result.Append(Value::BIGINT(42), VectorAppendMode::ALLOW_RESIZE);
		REQUIRE(result.GetValue(0) == Value::BIGINT(42));
		REQUIRE(owned_buffer->Size() == 0);
	}
	SECTION("Empty serialization needs no payload") {
		MemoryStream stream(Allocator::DefaultAllocator());
		BinarySerializer serializer(stream);
		serializer.Begin();
		source.Serialize(serializer);
		serializer.End();
		REQUIRE(allocations.allocations == 0);
		stream.Rewind();
		BinaryDeserializer deserializer(stream);
		deserializer.Begin();
		Vector result(source.GetType());
		result.Deserialize(deserializer, 0);
		deserializer.End();
		REQUIRE(result.size() == 0);
		REQUIRE(result.GetType() == source.GetType());
	}
	REQUIRE(source.size() == 0);
	REQUIRE(owned_buffer->Size() == 0);
}

TEST_CASE("Nested payloads remain lazy through empty reads and recover from allocation failure", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	auto string_array = Value::ARRAY(LogicalType::VARCHAR, {Value(string(64, 'x')), Value(LogicalType::VARCHAR)});
	auto structure = Value::STRUCT({{"number", Value::BIGINT(7)}, {"strings", string_array}});
	auto value = Value::LIST(structure.type(), {structure, Value(structure.type())});
	for (idx_t capacity : {idx_t(0), idx_t(2)}) {
		DataChunk chunk;
		auto initial_allocations = allocations.allocations.load();
		chunk.Initialize(allocator, {value.type()}, capacity);
		auto &column = chunk.data[0];
		RequireLazyNestedStorage(column, allocator, capacity);
		RecursiveUnifiedVectorFormat format;
		Vector::RecursiveToUnifiedFormat(column, format);
		{
			Vector empty(column, 0, 0);
			REQUIRE(empty.size() == 0);
		}
		chunk.Reset();
		REQUIRE(allocations.allocations == initial_allocations);
		allocations.fail = true;
		REQUIRE_THROWS_AS(column.Append(value, VectorAppendMode::ALLOW_RESIZE), OutOfMemoryException);
		chunk.Reset();
		allocations.fail = false;
		column.Append(value, VectorAppendMode::ALLOW_RESIZE);
		REQUIRE(column.GetValue(0) == value);
		column.Buffer().Verify(column.GetType());
		chunk.Destroy();
		REQUIRE(allocations.live_bytes == 0);
	}
}

TEST_CASE("Recursive reset releases nested dictionary inputs", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	auto list_type = LogicalType::LIST(LogicalType::BIGINT);
	auto value = Value::LIST(LogicalType::BIGINT, {Value::BIGINT(7), Value(LogicalType::BIGINT)});
	Vector input(list_type, 2);
	input.Append(value);
	input.Append(Value::LIST(LogicalType::BIGINT, {}));
	SelectionVector sel(2);
	sel.set_index(0, 1);
	sel.set_index(1, 0);
	Vector dictionary(input, sel, 2);
	for (auto &type : {list_type, LogicalType::STRUCT({{"items", list_type}}), LogicalType::ARRAY(list_type, 2)}) {
		INFO(type.ToString());
		DataChunk chunk;
		chunk.Initialize(allocator, {type}, 2);
		auto &column = chunk.data[0];
		auto &lists = type.InternalType() == PhysicalType::STRUCT  ? StructVector::GetEntries(column)[0]
		              : type.InternalType() == PhysicalType::ARRAY ? ArrayVector::GetChildMutable(column)
		                                                           : column;
		auto &elements = ListVector::GetChildMutable(lists);
		auto capacity = FlatVector::GetCapacity(elements);
		auto before = allocations.allocations.load();
		elements.Slice(ListVector::GetChild(input), sel, 2);
		lists.Slice(input, sel, 2);
		for (idx_t iteration = 0; iteration < 3; iteration++) {
			FlatVector::SetSize(column, type.InternalType() == PhysicalType::ARRAY ? 1 : 2);
			column.Buffer().Verify(type);
			duckdb::weak_ptr<DictionaryEntry> entry = lists.BufferMutable().Cast<DictionaryBuffer>().GetEntryPtr();
			duckdb::weak_ptr<DictionaryEntry> child_entry =
			    elements.BufferMutable().Cast<DictionaryBuffer>().GetEntryPtr();
			auto entry_references = entry.use_count();
			chunk.Reset();
			REQUIRE(entry.use_count() == entry_references - 1);
			REQUIRE(child_entry.expired());
			REQUIRE(lists.GetVectorType() == VectorType::FLAT_VECTOR);
			REQUIRE(elements.GetVectorType() == VectorType::FLAT_VECTOR);
			REQUIRE(column.size() == 0);
			REQUIRE(lists.size() == 0);
			REQUIRE(elements.size() == 0);
			REQUIRE(ListVector::GetListSize(lists) == 0);
			REQUIRE(ListVector::GetListCapacity(lists) == capacity);
			RecursiveUnifiedVectorFormat format;
			Vector::RecursiveToUnifiedFormat(column, format);
			column.Buffer().Verify(type);
			REQUIRE(allocations.allocations == before);
			elements.Slice(ListVector::GetChild(input), sel, 2);
			// Exercise both flat inputs and dictionary composition.
			lists.Slice(iteration % 2 ? dictionary : input, sel, 2);
			REQUIRE(lists.GetValue(0) == (iteration % 2 ? value : Value::LIST(LogicalType::BIGINT, {})));
		}
		chunk.Reset();
		lists.Append(value);
		lists.Append(value);
		FlatVector::SetSize(column, type.InternalType() == PhysicalType::ARRAY ? 1 : 2);
		column.Buffer().Verify(type);
		REQUIRE(lists.GetValue(0) == value);
		REQUIRE(lists.GetValue(1) == value);
		REQUIRE(input.GetValue(0) == value);
		REQUIRE(dictionary.GetValue(1) == value);
		chunk.Destroy();
		REQUIRE(allocations.live_bytes == 0);
	}
}

TEST_CASE("Nested reset buffers support list mutations and preserve grown capacity", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	auto inner = LogicalType::LIST(LogicalType::BIGINT);
	auto value = Value::LIST(LogicalType::BIGINT, {Value::BIGINT(9)});
	DataChunk chunk;
	chunk.Initialize(allocator, {LogicalType::LIST(inner)}, 2);
	auto &lists = chunk.data[0];
	auto &children = ListVector::GetChildMutable(lists);
	Vector input(inner, 2);
	input.Append(value);
	input.Append(Value(inner));
	SelectionVector sel(2);
	sel.set_index(0, 1);
	sel.set_index(1, 0);
	children.Slice(input, sel, 2);
	chunk.Reset();
	REQUIRE(ListVector::GetListCapacity(lists) == 2);

	SECTION("Reserve grows a cached child without changing its size") {
		ListVector::Reserve(lists, 6);
		REQUIRE(children.size() == 0);
		REQUIRE(ListVector::GetListCapacity(lists) >= 6);
	}
	SECTION("Publishing a child size grows cached storage") {
		ListVector::SetListSize(lists, 6);
		REQUIRE(children.size() == 6);
		REQUIRE(ListVector::GetListCapacity(lists) >= 6);
	}
	SECTION("Appending through the cached LIST parent grows its child") {
		ListVector::Append(children, ListVector::GetChild(input), 1);
		ListVector::Append(children, ListVector::GetChild(input), *FlatVector::IncrementalSelectionVector(), 1);
		ListVector::PushBack(children, Value::BIGINT(9));
		REQUIRE(ListVector::GetListSize(children) == 3);
	}
	SECTION("Typed writers prepare nested parents and children") {
		auto writer = FlatVector::Writer<VectorListType<VectorListType<int64_t>>>(lists, 1);
		for (auto &child : writer.WriteList(1)) {
			for (auto &element : child.WriteList(1)) {
				element.WriteValue(9);
			}
		}
	}
	SECTION("Failed allocation remains resettable and can be retried") {
		allocations.fail = true;
		REQUIRE_THROWS_AS(ListVector::Reserve(lists, 6), OutOfMemoryException);
		chunk.Reset();
		allocations.fail = false;
		ListVector::Reserve(lists, 6);
	}
	chunk.Reset();
	ListVector::Reserve(lists, 6);
	ListVector::Reserve(children, 12);
	auto capacity = ListVector::GetListCapacity(lists);
	auto child_capacity = ListVector::GetListCapacity(children);
	auto before = allocations.allocations.load();
	for (idx_t iteration = 0; iteration < 2; iteration++) {
		chunk.Reset();
		REQUIRE(chunk.GetCapacity() == 2);
		REQUIRE(ListVector::GetListCapacity(lists) == capacity);
		REQUIRE(ListVector::GetListCapacity(children) == child_capacity);
		REQUIRE(children.size() == 0);
		REQUIRE(ListVector::GetListSize(children) == 0);
		ListVector::Reserve(lists, 6);
		ListVector::Reserve(children, 12);
		REQUIRE(allocations.allocations == before);
	}
	chunk.Destroy();
	REQUIRE(allocations.live_bytes == 0);
}

TEST_CASE("Reset retains grown STRUCT, ARRAY and LIST storage", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	auto numbers = Value::ARRAY(LogicalType::BIGINT, {Value::BIGINT(11), Value(LogicalType::BIGINT)});
	auto value = Value::STRUCT({{"array", numbers}, {"list", Value::LIST(numbers.type(), {numbers, numbers})}});
	DataChunk chunk;
	chunk.Initialize(allocator, {value.type()}, 0);
	auto &column = chunk.data[0];
	column.Append(value, VectorAppendMode::ALLOW_RESIZE);
	column.Reserve(8);
	auto before = allocations.allocations.load();
	for (idx_t iteration = 0; iteration < 3; iteration++) {
		chunk.Reset();
		REQUIRE(chunk.GetCapacity() == 0);
		REQUIRE(FlatVector::GetCapacity(column) >= 8);
		column.Append(value, VectorAppendMode::ALLOW_RESIZE);
		REQUIRE(column.GetValue(0) == value);
		column.Buffer().Verify(value.type());
		REQUIRE(allocations.allocations == before);
	}
	chunk.Destroy();
	REQUIRE(allocations.live_bytes == 0);
}

TEST_CASE("Nested C API outputs allocate before raw pointer access", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	auto structure =
	    LogicalType::STRUCT({{"numbers", LogicalType::ARRAY(LogicalType::BIGINT, 2)}, {"text", LogicalType::VARCHAR}});
	DataChunk chunk;
	chunk.Initialize(allocator, {LogicalType::LIST(structure)}, 2);
	allocations.fail = true;
	REQUIRE_THROWS_AS(PrepareCAPIVector(chunk.data[0]), OutOfMemoryException);
	allocations.fail = false;
	PrepareCAPIVector(chunk.data[0]);
	REQUIRE(allocations.allocations == 3);

	allocations.fail = true;
	for (idx_t iteration = 0; iteration < 2; iteration++) {
		chunk.Reset();
		PrepareCAPIVector(chunk.data[0]);
		auto lists = reinterpret_cast<duckdb_vector>(&chunk.data[0]);
		auto structs = duckdb_list_vector_get_child(lists);
		auto arrays = duckdb_struct_vector_get_child(structs, 0);
		auto numbers = duckdb_array_vector_get_child(arrays);
		auto strings = duckdb_struct_vector_get_child(structs, 1);
		for (auto vec : {lists, numbers, strings}) {
			REQUIRE(duckdb_vector_get_data(vec) != nullptr);
			REQUIRE_NOTHROW(duckdb_vector_get_validity(vec));
		}
		REQUIRE(allocations.allocations == 3);
	}
	chunk.Destroy();
	REQUIRE(allocations.live_bytes == 0);
}

TEST_CASE("Nested C API inputs and fetched chunks prepare empty children", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	DBConfig config;
	config.allocator = make_uniq<Allocator>(LazyAllocationState::Allocate, LazyAllocationState::Free,
	                                        LazyAllocationState::Reallocate, std::move(state));
	DuckDB db(nullptr, &config);
	Connection connection(db);
	auto c_connection = reinterpret_cast<duckdb_connection>(&connection);
	auto function = duckdb_create_scalar_function();
	auto bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	auto list = duckdb_create_list_type(bigint);
	duckdb_scalar_function_set_name(function, "check_lazy_input");
	duckdb_scalar_function_add_parameter(function, list);
	duckdb_scalar_function_set_return_type(function, bigint);
	duckdb_scalar_function_set_extra_info(function, &allocations, nullptr);
	duckdb_scalar_function_set_function(function, CheckLazyCAPIInput);
	REQUIRE(duckdb_register_scalar_function(c_connection, function) == DuckDBSuccess);
	duckdb_destroy_scalar_function(&function);
	duckdb_destroy_logical_type(&list);
	duckdb_destroy_logical_type(&bigint);

	auto checked =
	    connection.Query("SELECT sum(check_lazy_input(list_filter([i], lambda x: false))) FROM range(40) t(i)");
	REQUIRE_NO_FAIL(*checked);
	REQUIRE(checked->Collection().GetValue(0, 0).GetValue<int64_t>() == 40);

	const auto query = "SELECT list_filter([i], lambda x: false) FROM range(40) t(i)";
	for (const string mode : {"indexed", "fetch", "streaming"}) {
		INFO(mode);
		duckdb_result result {};
		duckdb_prepared_statement prepared = nullptr;
		if (mode == "streaming") {
			REQUIRE(duckdb_prepare(c_connection, query, &prepared) == DuckDBSuccess);
			REQUIRE(duckdb_execute_prepared_streaming(prepared, &result) == DuckDBSuccess);
		} else {
			REQUIRE(duckdb_query(c_connection, query, &result) == DuckDBSuccess);
		}
		auto chunk = mode == "indexed" ? duckdb_result_get_chunk(result, 0) : duckdb_fetch_chunk(result);
		REQUIRE(chunk != nullptr);
		auto child = duckdb_list_vector_get_child(duckdb_data_chunk_get_vector(chunk, 0));
		auto &vector = *reinterpret_cast<Vector *>(child);
		REQUIRE(vector.size() == 0);
		auto data = FlatVector::GetData(vector);
		REQUIRE(data != nullptr);
		auto allocation_size = vector.GetAllocationSize();
		allocations.fail = true;
		REQUIRE(duckdb_vector_get_data(child) == data);
		allocations.fail = false;
		REQUIRE(vector.GetAllocationSize() == allocation_size);
		duckdb_destroy_data_chunk(&chunk);
		duckdb_destroy_result(&result);
		duckdb_destroy_prepare(&prepared);
	}
}

TEST_CASE("Cached buffers retain the destination allocator and survive chunk destruction", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	SelectionVector sel(1);
	sel.set_index(0, 0);
	for (auto &value : {Value::BIGINT(7), Value::LIST(LogicalType::BIGINT, {Value::BIGINT(7)})}) {
		INFO(value.type().ToString());
		DataChunk chunk;
		chunk.Initialize(allocator, {value.type()}, 16);
		Vector input(value, count_t(1));
		input.Flatten();
		auto before = allocations.allocations.load();
		chunk.data[0].Slice(input, sel, 1);
		chunk.Reset();
		REQUIRE(allocations.allocations == before);
		auto output = Vector::Ref(chunk.data[0]);
		chunk.Destroy();
		REQUIRE(FlatVector::GetCapacity(output) == 16);
		output.Append(value);
		REQUIRE(output.Buffer().GetAllocator().get() == &allocator);
		if (value.type().InternalType() == PhysicalType::LIST) {
			REQUIRE(ListVector::GetChild(output).Buffer().GetAllocator().get() == &allocator);
		}
		REQUIRE(output.GetValue(0) == value);
		output.SetBuffer(nullptr);
		REQUIRE(allocations.live_bytes == 0);
	}
}

TEST_CASE("Lazy scalar buffers handle failed allocation and capacity growth", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	DataChunk chunk;
	chunk.Initialize(allocator, {LogicalType::BIGINT}, 16);
	allocations.fail = true;
	REQUIRE_THROWS_AS(FlatVector::Writer<int64_t>(chunk.data[0], 1), OutOfMemoryException);
	REQUIRE(chunk.size() == 0);
	REQUIRE(FlatVector::GetData(chunk.data[0]) == nullptr);
	chunk.Reset();
	allocations.fail = false;
	chunk.data[0].Reserve(64);
	{
		auto writer = FlatVector::Writer<int64_t>(chunk.data[0], 1);
		writer.WriteValue(42);
	}
	REQUIRE(allocations.allocations == 1);
	chunk.Reset();
	REQUIRE(FlatVector::GetCapacity(chunk.data[0]) == 64);
	REQUIRE(chunk.GetCapacity() == 16);
	DataChunk moved;
	moved.Move(chunk);
	moved.Reset();
	REQUIRE(moved.data[0].Buffer().GetAllocator().get() == &allocator);
	moved.Destroy();
	REQUIRE(allocations.live_bytes == 0);

	DataChunk zero;
	zero.Initialize(allocator, {LogicalType::BIGINT}, 0);
	REQUIRE(allocations.allocations == 1);
	zero.data[0].Append(Value::BIGINT(55), VectorAppendMode::ALLOW_RESIZE);
	REQUIRE(zero.data[0].GetValue(0) == Value::BIGINT(55));
	REQUIRE(zero.data[0].Buffer().GetAllocator().get() == &allocator);
}

TEST_CASE("Lazy buffers support flat mutation and empty reinterpretation", "[lazy_vector]") {
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	Vector input(Value::BIGINT(7), count_t(1));
	input.Flatten();
	SelectionVector sel(1);
	sel.set_index(0, 0);
	chunk.data[0].Slice(input, sel, 1);
	chunk.Reset();
	Vector reinterpreted(LogicalType::UBIGINT, nullptr);
	reinterpreted.Reinterpret(chunk.data[0]);
	REQUIRE(reinterpreted.size() == 0);
	REQUIRE(chunk.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
	FlatVector::SetNull(chunk.data[0], 0, true);
	FlatVector::SetSize(chunk.data[0], 1);
	REQUIRE(chunk.data[0].GetValue(0).IsNull());
}

TEST_CASE("Zero-copy scalar data does not allocate a discarded payload", "[lazy_vector]") {
	auto state = make_uniq<LazyAllocationState>();
	auto &allocations = *state;
	Allocator allocator(LazyAllocationState::Allocate, LazyAllocationState::Free, LazyAllocationState::Reallocate,
	                    std::move(state));
	DataChunk chunk;
	chunk.Initialize(allocator, {LogicalType::BIGINT});
	Vector input(Value::BIGINT(7), count_t(1));
	input.Flatten();
	SelectionVector sel(1);
	sel.set_index(0, 0);
	int64_t external[] = {42, 43};
	for (idx_t iteration = 0; iteration < 3; iteration++) {
		chunk.Reset();
		FlatVector::SetData(chunk.data[0], data_ptr_cast(external), count_t(2));
		REQUIRE(chunk.data[0].GetValue(1) == Value::BIGINT(43));
		REQUIRE(allocations.allocations == 0);
		chunk.data[0].Slice(input, sel, 1);
	}
}
namespace {
struct LazySourceState : GlobalTableFunctionState {
	static constexpr idx_t BATCH_SIZE = STANDARD_VECTOR_SIZE < 32 ? STANDARD_VECTOR_SIZE : 32;
	idx_t batch = 0;
	buffer_ptr<VectorBuffer> previous;
};

void CheckLazyOutput(const Vector &result) {
	if (result.size() != 0) {
		throw InternalException("Execution output was not reset lazily");
	}
}

void LazySource(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<LazySourceState>();
	auto &result = output.data[0];
	CheckLazyOutput(result);
	if (state.batch == 0 && FlatVector::GetData(result)) {
		throw InternalException("Pipeline output allocated its payload before writing");
	}
	if (state.batch == 3) {
		return;
	}
	// Exercise the mutable-pointer boundary used by non-expression producers.
	auto data = FlatVector::GetDataMutable<int64_t>(result);
	std::fill(data, data + LazySourceState::BATCH_SIZE, NumericCast<int64_t>(state.batch));
	if (state.batch % 2 == 0) {
		FlatVector::ValidityMutable(result).SetInvalid(0);
	}
	FlatVector::SetSize(result, LazySourceState::BATCH_SIZE);
	if (result.Buffer().GetAllocator().get() != &BufferAllocator::Get(context) ||
	    (state.previous && state.previous != result.GetBufferRef())) {
		throw InternalException("Pipeline output lost its initialized resource or reusable storage");
	}
	state.previous = result.GetBufferRef();
	state.batch++;
}

void LazyReference(DataChunk &input, ExpressionState &, Vector &result) {
	CheckLazyOutput(result);
	if (FlatVector::GetData(result)) {
		throw InternalException("Reference-only scalar output allocated a payload");
	}
	result.Reference(input.data[0]);
}

void LazyComputed(DataChunk &input, ExpressionState &state, Vector &result) {
	CheckLazyOutput(result);
	UnaryExecutor::Execute<int64_t, int64_t>(input.data[0], result, input.size(),
	                                         [](int64_t value) { return value + 1; });
	if (result.Buffer().GetAllocator().get() != &BufferAllocator::Get(state.GetContext())) {
		throw InternalException("Expression output lost its initialized resource");
	}
}
} // namespace

TEST_CASE("Pipeline and expression outputs initialize and reset lazily", "[lazy_vector]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "lazy_output_test", ""};
	ExtensionLoader loader {load_info};
	TableFunction source(
	    "lazy_source", {}, LazySource,
	    [](ClientContext &, TableFunctionBindInput &, duckdb::vector<LogicalType> &types,
	       duckdb::vector<Identifier> &names) -> duckdb::unique_ptr<FunctionData> {
		    types.push_back(LogicalType::BIGINT);
		    names.push_back("i");
		    return nullptr;
	    },
	    [](ClientContext &, TableFunctionInitInput &) -> duckdb::unique_ptr<GlobalTableFunctionState> {
		    return make_uniq<LazySourceState>();
	    });
	loader.RegisterFunction(source);
	ScalarFunction reference("lazy_ref", {LogicalType::BIGINT}, LogicalType::BIGINT, LazyReference);
	reference.SetVolatile();
	loader.RegisterFunction(reference);
	ScalarFunction computed("lazy_computed", {LogicalType::BIGINT}, LogicalType::BIGINT, LazyComputed);
	computed.SetVolatile();
	loader.RegisterFunction(computed);

	for (bool expressions : {false, true}) {
		auto query = expressions ? "SELECT lazy_ref(lazy_computed(lazy_ref(i))) FROM lazy_source()"
		                         : "SELECT * FROM lazy_source()";
		auto result = connection.Query(query);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 3 * LazySourceState::BATCH_SIZE);
	}
}

TEST_CASE("Caching operators retain lazy output storage after slicing", "[lazy_vector]") {
	class SliceOperator : public CachingPhysicalOperator {
	public:
		explicit SliceOperator(PhysicalPlan &plan)
		    : CachingPhysicalOperator(plan, PhysicalOperatorType::FILTER, {LogicalType::BIGINT}, 1) {
		}

	protected:
		OperatorResultType ExecuteInternal(ExecutionContext &, DataChunk &input, DataChunk &output,
		                                   GlobalOperatorState &, OperatorState &) const override {
			SelectionVector selection(1);
			selection.set_index(0, 0);
			output.Slice(input, selection, 1);
			return OperatorResultType::NEED_MORE_INPUT;
		}
	};

	DuckDB db(nullptr);
	Connection connection(db);
	ThreadContext thread(*connection.context);
	ExecutionContext context(*connection.context, thread, nullptr);
	PhysicalPlan plan(Allocator::DefaultAllocator());
	SliceOperator op(plan);
	GlobalOperatorState global;
	CachingOperatorState state;
	state.initialized = true;
	state.can_cache_chunk = OperatorCachingMode::UNORDERED;
	DataChunk input, output;
	input.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	{
		auto writer = FlatVector::Writer<int64_t>(input.data[0], 1);
		writer.WriteValue(7);
	}
	auto &allocator = BufferAllocator::Get(*connection.context);
	output.Initialize(allocator, {LogicalType::BIGINT});
	auto owned_buffer = output.data[0].GetBufferRef();
	const auto batch_count = MinValue<idx_t>(3, STANDARD_VECTOR_SIZE);
	for (idx_t iteration = 0; iteration < batch_count; iteration++) {
		REQUIRE(op.Execute(context, input, output, global, state) == OperatorResultType::NEED_MORE_INPUT);
		REQUIRE(output.size() == 0);
		REQUIRE(output.data[0].GetVectorType() == VectorType::FLAT_VECTOR);
		REQUIRE(output.data[0].GetBufferRef() == owned_buffer);
		REQUIRE(output.data[0].Buffer().GetAllocator().get() == &allocator);
		REQUIRE(FlatVector::GetData(output.data[0]) == nullptr);
		REQUIRE(state.cached_chunk->size() == iteration + 1);
		REQUIRE(state.cached_chunk->GetValue(0, iteration) == Value::BIGINT(7));
		REQUIRE(input.GetValue(0, 0) == Value::BIGINT(7));
	}
	REQUIRE(op.FinalExecute(context, output, global, state) == OperatorFinalizeResultType::FINISHED);
	REQUIRE(output.size() == batch_count);
	for (idx_t row = 0; row < output.size(); row++) {
		REQUIRE(output.GetValue(0, row) == Value::BIGINT(7));
	}
}

TEST_CASE("Heap references added after a dictionary reset survive the next writer", "[lazy_vector]") {
	struct LifetimeHolder : AuxiliaryDataHolder {
		explicit LifetimeHolder(bool &destroyed) : destroyed(destroyed) {
		}
		~LifetimeHolder() override {
			destroyed = true;
		}
		bool &destroyed;
	};
	bool destroyed = false;
	Vector input(Value(string(64, 'x')), count_t(1));
	input.Flatten();
	input.AddAuxiliaryData(make_uniq<LifetimeHolder>(destroyed));
	DataChunk output;
	output.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
	SelectionVector sel(1);
	sel.set_index(0, 0);
	output.data[0].Slice(input, sel, 1);
	output.Reset();
	output.data[0].AddHeapReference(input);
	{
		auto writer = FlatVector::Writer<string_t>(output.data[0], 1);
		writer.WriteStringRef(FlatVector::GetData<string_t>(input)[0]);
	}
	input.SetBuffer(nullptr);
	REQUIRE_FALSE(destroyed);
	REQUIRE(output.data[0].GetValue(0) == Value(string(64, 'x')));
	output.Destroy();
	REQUIRE(destroyed);
}

TEST_CASE("Null-only scalar producers have physical storage", "[lazy_vector]") {
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	FlatVector::ValidityMutable(chunk.data[0]).SetAllInvalid(2);
	FlatVector::SetSize(chunk.data[0], 2);
	REQUIRE(FlatVector::GetData(chunk.data[0]) != nullptr);
	REQUIRE(chunk.data[0].GetValue(0).IsNull());
	REQUIRE(chunk.data[0].GetValue(1).IsNull());
}
