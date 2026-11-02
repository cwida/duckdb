#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "core_functions/aggregate/histogram_helpers.hpp"
#include "core_functions/aggregate/holistic_functions.hpp"

namespace duckdb {

namespace {

static constexpr int64_t MAX_APPROX_K = 1000000;

struct ApproxTopKString {
	ApproxTopKString() : str(UINT32_C(0)), hash(0) {
	}
	ApproxTopKString(string_t str_p, hash_t hash_p) : str(str_p), hash(hash_p) {
	}

	string_t str;
	hash_t hash;
};

struct ApproxTopKHash {
	std::size_t operator()(const ApproxTopKString &k) const {
		return k.hash;
	}
};

struct ApproxTopKEquality {
	bool operator()(const ApproxTopKString &a, const ApproxTopKString &b) const {
		return Equals::Operation(a.str, b.str);
	}
};

template <typename T>
using approx_topk_map_t = unordered_map<ApproxTopKString, T, ApproxTopKHash, ApproxTopKEquality>;

// approx top k algorithm based on "A parallel space saving algorithm for frequent items and the Hurwitz zeta
// distribution" arxiv link -  https://arxiv.org/pdf/1401.0702
// together with the filter extension (Filtered Space-Saving) from "Estimating Top-k Destinations in Data Streams"
struct ApproxTopKValue {
	//! The counter
	idx_t count = 0;
	//! Index in the values array
	idx_t index = 0;
	//! The string value
	ApproxTopKString str_val;
	//! Allocated data
	char *dataptr = nullptr;
	uint32_t size = 0;
	idx_t capacity = 0;
};

struct InternalApproxTopKState {
	// the top-k data structure has two components
	// a list of k values sorted on "count" (i.e. values[0] has the lowest count)
	// a lookup map: string_t -> idx in "values" array
	unsafe_unique_array<ApproxTopKValue> stored_values;
	unsafe_vector<reference<ApproxTopKValue>> values;
	approx_topk_map_t<reference<ApproxTopKValue>> lookup_map;
	unsafe_vector<idx_t> filter;
	idx_t k = 0;
	idx_t capacity = 0;
	idx_t filter_mask;

	void Initialize(idx_t kval) {
		static constexpr idx_t MONITORED_VALUES_RATIO = 3;
		static constexpr idx_t FILTER_RATIO = 8;

		D_ASSERT(values.empty());
		D_ASSERT(lookup_map.empty());
		k = kval;
		if (k > MAX_APPROX_K) {
			throw InvalidInputException("Requested 'k' (%d) is bigger than accepted max (%d)", kval, MAX_APPROX_K);
		}
		capacity = kval * MONITORED_VALUES_RATIO;
		stored_values = make_unsafe_uniq_array_uninitialized<ApproxTopKValue>(capacity);
		values.reserve(capacity);

		// we scale the filter based on the amount of values we are monitoring
		idx_t filter_size = NextPowerOfTwo(capacity * FILTER_RATIO);
		filter_mask = filter_size - 1;
		filter.resize(filter_size);
	}

	static void CopyValue(ApproxTopKValue &value, const ApproxTopKString &input, ArenaAllocator &allocator) {
		value.str_val.hash = input.hash;
		if (input.str.IsInlined()) {
			// no need to copy
			value.str_val = input;
			return;
		}
		value.size = UnsafeNumericCast<uint32_t>(input.str.GetSize());
		if (value.size > value.capacity) {
			// need to re-allocate for this value
			value.capacity = NextPowerOfTwo(value.size);
			value.dataptr = char_ptr_cast(allocator.Allocate(value.capacity));
		}
		// copy over the data
		memcpy(value.dataptr, input.str.GetData(), value.size);
		value.str_val.str = string_t(value.dataptr, value.size);
	}

	void InsertOrReplaceEntry(const ApproxTopKString &input, AggregateInputData &aggr_input, idx_t increment = 1) {
		if (values.size() < capacity) {
			D_ASSERT(increment > 0);
			// we can always add this entry
			auto &val = stored_values[values.size()];
			val.index = values.size();
			values.push_back(val);
		}
		auto &value = values.back().get();
		if (value.count > 0) {
			// the capacity is reached - we need to replace an entry

			// we use the filter as an early out
			// based on the hash - we find a slot in the filter
			// instead of monitoring the value immediately, we add to the slot in the filter
			// ONLY when the value in the filter exceeds the current min value, we start monitoring the value
			// this speeds up the algorithm as switching monitor values means we need to erase/insert in the hash table
			auto &filter_value = filter[input.hash & filter_mask];
			if (filter_value + increment < value.count) {
				// if the filter has a lower count than the current min count
				// we can skip adding this entry (for now)
				filter_value += increment;
				return;
			}
			// the filter exceeds the min value - start monitoring this value
			// erase the existing entry from the map
			// and set the filter for the minimum value back to the current minimum value
			filter[value.str_val.hash & filter_mask] = value.count;
			lookup_map.erase(value.str_val);
		}
		CopyValue(value, input, aggr_input.allocator);
		lookup_map.insert(make_pair(value.str_val, reference<ApproxTopKValue>(value)));
		IncrementCount(value, increment);
	}

	void IncrementCount(ApproxTopKValue &value, idx_t increment = 1) {
		value.count += increment;
		// maintain sortedness of "values"
		// swap while we have a higher count than the next entry
		while (value.index > 0 && values[value.index].get().count > values[value.index - 1].get().count) {
			// swap the elements around
			auto &left = values[value.index];
			auto &right = values[value.index - 1];
			std::swap(left.get().index, right.get().index);
			std::swap(left, right);
		}
	}

	void Verify() const {
#ifdef DEBUG
		if (values.empty()) {
			D_ASSERT(lookup_map.empty());
			return;
		}
		D_ASSERT(values.size() <= capacity);
		for (idx_t k = 0; k < values.size(); k++) {
			auto &val = values[k].get();
			D_ASSERT(val.count > 0);
			// verify map exists
			auto entry = lookup_map.find(val.str_val);
			D_ASSERT(entry != lookup_map.end());
			// verify the index is correct
			D_ASSERT(val.index == k);
			if (k > 0) {
				// sortedness
				D_ASSERT(val.count <= values[k - 1].get().count);
			}
		}
		D_ASSERT(lookup_map.size() == values.size());
#endif
	}
};

struct ApproxTopKState {
	InternalApproxTopKState *state;

	InternalApproxTopKState &GetState() {
		if (!state) {
			state = new InternalApproxTopKState();
		}
		return *state;
	}

	const InternalApproxTopKState &GetState() const {
		if (!state) {
			throw InternalException("No state available");
		}
		return *state;
	}
};

struct ApproxTopKOperation {
	template <class TYPE, class STATE>
	static void Operation(STATE &aggr_state, const TYPE &input, AggregateInputData &aggr_input,
	                      const Vector &top_k_vector, idx_t offset, idx_t count) {
		auto &state = aggr_state.GetState();
		if (state.values.empty()) {
			static constexpr int64_t MAX_APPROX_K = 1000000;
			// not initialized yet - initialize the K value and set all counters to 0
			auto top_k_format = top_k_vector.Values<int64_t>();
			auto top_k_entry = top_k_format[offset];
			if (!top_k_entry.IsValid()) {
				throw InvalidInputException("Invalid input for approx_top_k: k value cannot be NULL");
			}
			auto kval = top_k_entry.GetValue();
			if (kval <= 0) {
				throw InvalidInputException("Invalid input for approx_top_k: k value must be > 0");
			}
			if (kval >= MAX_APPROX_K) {
				throw InvalidInputException("Invalid input for approx_top_k: k value must be < %d", MAX_APPROX_K);
			}
			state.Initialize(UnsafeNumericCast<idx_t>(kval));
		}
		ApproxTopKString topk_string(input, Hash(input));
		auto entry = state.lookup_map.find(topk_string);
		if (entry != state.lookup_map.end()) {
			// the input is monitored - increment the count
			state.IncrementCount(entry->second.get());
		} else {
			// the input is not monitored - replace the first entry with the current entry and increment
			state.InsertOrReplaceEntry(topk_string, aggr_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &aggr_source, STATE &aggr_target, AggregateInputData &aggr_input) {
		if (!aggr_source.state) {
			// source state is empty
			return;
		}
		auto &source = aggr_source.GetState();
		auto &target = aggr_target.GetState();
		if (source.values.empty()) {
			// source is empty
			return;
		}
		source.Verify();
		auto min_source = source.values.back().get().count;
		idx_t min_target;
		if (target.values.empty()) {
			min_target = 0;
			target.Initialize(source.k);
		} else {
			if (source.k != target.k) {
				throw NotImplementedException("Approx Top K - cannot combine approx_top_K with different k values. "
				                              "K values must be the same for all entries within the same group");
			}
			min_target = target.values.back().get().count;
		}
		// for all entries in target
		// check if they are tracked in source
		//     if they do - add the tracked count
		//     if they do not - add the minimum count
		for (idx_t target_idx = 0; target_idx < target.values.size(); target_idx++) {
			auto &val = target.values[target_idx].get();
			auto source_entry = source.lookup_map.find(val.str_val);
			idx_t increment = min_source;
			if (source_entry != source.lookup_map.end()) {
				increment = source_entry->second.get().count;
			}
			if (increment == 0) {
				continue;
			}
			target.IncrementCount(val, increment);
		}
		// now for each entry in source, if it is not tracked by the target, at the target minimum
		for (auto &source_entry : source.values) {
			auto &source_val = source_entry.get();
			auto target_entry = target.lookup_map.find(source_val.str_val);
			if (target_entry != target.lookup_map.end()) {
				// already tracked - no need to add anything
				continue;
			}
			auto new_count = source_val.count + min_target;
			idx_t increment;
			if (target.values.size() >= target.capacity) {
				idx_t current_min = target.values.empty() ? 0 : target.values.back().get().count;
				D_ASSERT(target.values.size() == target.capacity);
				// target already has capacity values
				// check if we should insert this entry
				if (new_count <= current_min) {
					// if we do not we can skip this entry
					continue;
				}
				increment = new_count - current_min;
			} else {
				// target does not have capacity entries yet
				// just add this entry with the full count
				increment = new_count;
			}
			target.InsertOrReplaceEntry(source_val.str_val, aggr_input, increment);
		}
		// copy over the filter
		D_ASSERT(source.filter.size() == target.filter.size());
		for (idx_t filter_idx = 0; filter_idx < source.filter.size(); filter_idx++) {
			target.filter[filter_idx] += source.filter[filter_idx];
		}
		target.Verify();
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &aggr_input_data) {
		delete state.state;
	}

	static bool IgnoreNull() {
		return true;
	}
};

template <class T = string_t, class OP = HistogramGenericFunctor>
void ApproxTopKUpdate(Vector inputs[], AggregateInputData &aggr_input, idx_t input_count, Vector &state_vector,
                      idx_t count) {
	using STATE = ApproxTopKState;
	auto &input = inputs[0];

	const auto &top_k_vector = inputs[1];

	auto extra_state = OP::CreateExtraState();
	UnifiedVectorFormat input_data;
	OP::PrepareData(input, extra_state, input_data);

	auto states = state_vector.Values<STATE *>();
	auto data = UnifiedVectorFormat::GetData<T>(input_data);
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			continue;
		}
		auto &state = *states[i].GetValue();
		ApproxTopKOperation::Operation<T, STATE>(state, data[idx], aggr_input, top_k_vector, i, count);
	}
}

template <class OP = HistogramGenericFunctor>
void ApproxTopKFinalize(Vector &state_vector, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
	auto states = state_vector.Values<ApproxTopKState *>();

	auto old_len = ListVector::GetListSize(result);
	idx_t new_entries = 0;
	// figure out how much space we need
	for (idx_t i = 0; i < count; i++) {
		auto &state = states[i].GetValue()->GetState();
		if (state.values.empty()) {
			continue;
		}
		// get up to k values for each state
		// this can be less of fewer unique values were found
		new_entries += MinValue<idx_t>(state.values.size(), state.k);
	}
	// reserve space in the list vector
	ListVector::Reserve(result, old_len + new_entries);
	auto list_entries = FlatVector::Writer<list_entry_t>(result, count, offset);
	auto &child_data = ListVector::GetChildMutable(result);

	idx_t current_offset = old_len;
	for (idx_t i = 0; i < count; i++) {
		auto &state = states[i].GetValue()->GetState();
		if (state.values.empty()) {
			list_entries.WriteNull();
			continue;
		}
		list_entry_t list_entry;
		list_entry.offset = current_offset;
		for (idx_t val_idx = 0; val_idx < MinValue<idx_t>(state.values.size(), state.k); val_idx++) {
			auto &val = state.values[val_idx].get();
			D_ASSERT(val.count > 0);
			OP::template HistogramFinalize<string_t>(val.str_val.str, child_data, current_offset);
			current_offset++;
		}
		list_entry.length = current_offset - list_entry.offset;
		list_entries.WriteValue(list_entry);
	}
	D_ASSERT(current_offset == old_len + new_entries);
	ListVector::SetListSize(result, current_offset);
	result.Verify();
}

//===--------------------------------------------------------------------===//
// State Export
//===--------------------------------------------------------------------===//
//! Exported state: STRUCT(k, values LIST(STRUCT(value, count)), filter) - the monitored values (descending count)
//! and the Filtered Space-Saving counters. Values are decoded to the input type on export, re-encoded on import.
AggregateStateLayout ApproxTopKGetStateType(AggregateLayoutInput &input) {
	child_list_t<LogicalType> value_children;
	value_children.emplace_back("value", input.function.GetArguments()[0]);
	value_children.emplace_back("count", LogicalType::UBIGINT);

	child_list_t<LogicalType> children;
	children.emplace_back("k", LogicalType::UBIGINT);
	children.emplace_back("values", LogicalType::LIST(LogicalType::STRUCT(std::move(value_children))));
	children.emplace_back("filter", LogicalType::LIST(LogicalType::UBIGINT));

	AggregateStateLayout layout;
	layout.type = LogicalType::STRUCT(std::move(children));
	layout.total_state_size = AlignValue<idx_t>(sizeof(ApproxTopKState));
	return layout;
}

template <class OP = HistogramGenericFunctor>
void ApproxTopKExportState(Vector &state_vector, AggregateFinalizeInputData &aggr_input_data, Vector &result,
                           idx_t count, idx_t offset) {
	auto states = state_vector.Values<ApproxTopKState *>();

	auto &mask = FlatVector::ValidityMutable(result);
	auto &fields = StructVector::GetEntries(result);
	auto k_data = FlatVector::GetDataMutable<uint64_t>(fields[0]);
	auto &value_lists = fields[1];
	auto &filter_lists = fields[2];
	auto &k_validity = FlatVector::ValidityMutable(fields[0]);
	auto &value_validity = FlatVector::ValidityMutable(value_lists);
	auto &filter_validity = FlatVector::ValidityMutable(filter_lists);
	auto value_entries = FlatVector::ScatterWriter<list_entry_t>(value_lists);
	auto filter_entries = FlatVector::ScatterWriter<list_entry_t>(filter_lists);
	idx_t total_values = ListVector::GetListSize(value_lists);
	idx_t total_filters = ListVector::GetListSize(filter_lists);
	for (idx_t i = 0; i < count; i++) {
		const idx_t row = offset + i;
		auto state_ptr = states[i].GetValue()->state;
		value_entries[row].offset = total_values;
		filter_entries[row].offset = total_filters;
		if (!state_ptr || state_ptr->values.empty()) {
			// no values have been added to this state - export NULL (children of a NULL struct must also be NULL)
			mask.SetInvalid(row);
			k_validity.SetInvalid(row);
			value_validity.SetInvalid(row);
			filter_validity.SetInvalid(row);
			value_entries[row].length = 0;
			filter_entries[row].length = 0;
			k_data[row] = 0;
			continue;
		}
		k_data[row] = state_ptr->k;
		value_entries[row].length = state_ptr->values.size();
		filter_entries[row].length = state_ptr->filter.size();
		total_values += state_ptr->values.size();
		total_filters += state_ptr->filter.size();
	}

	ListVector::Reserve(value_lists, total_values);
	ListVector::Reserve(filter_lists, total_filters);
	auto &value_structs = ListVector::GetChildMutable(value_lists);
	auto &value_fields = StructVector::GetEntries(value_structs);
	auto &value_child = value_fields[0];
	auto count_data = FlatVector::GetDataMutable<uint64_t>(value_fields[1]);
	auto filter_data = FlatVector::GetDataMutable<uint64_t>(ListVector::GetChildMutable(filter_lists));
	for (idx_t i = 0; i < count; i++) {
		const idx_t row = offset + i;
		auto state_ptr = states[i].GetValue()->state;
		if (!state_ptr || state_ptr->values.empty()) {
			continue;
		}
		auto &state = *state_ptr;
		// write the values (in descending count order) - decoding them back to the input type
		idx_t value_offset = value_entries[row].offset;
		for (auto &val_ref : state.values) {
			auto &val = val_ref.get();
			OP::template HistogramFinalize<string_t>(val.str_val.str, value_child, value_offset);
			count_data[value_offset] = val.count;
			value_offset++;
		}
		for (idx_t filter_idx = 0; filter_idx < state.filter.size(); filter_idx++) {
			filter_data[filter_entries[row].offset + filter_idx] = state.filter[filter_idx];
		}
	}
	ListVector::SetListSize(value_lists, total_values);
	ListVector::SetListSize(filter_lists, total_filters);
	FlatVector::SetSize(fields[0], offset + count);
	FlatVector::SetSize(value_lists, offset + count);
	FlatVector::SetSize(filter_lists, offset + count);
	FlatVector::SetSize(result, offset + count);
}

template <class OP = HistogramGenericFunctor>
void ApproxTopKImportState(AggregateImportInputData &input) {
	const auto &layout = input.layout;
	const auto count = input.input_vec.size();
	// the input can be any vector type (e.g. dictionary-encoded from a compressed scan) - flatten it so the
	// struct/list children can be read by position
	Vector input_vec(input.input_vec, 0, count);
	input_vec.Flatten();
	const auto dest_buffer = input.dest_buffer;
	auto &allocator = input.allocator;
	const auto validity = input_vec.Validity();
	const auto &fields = StructVector::GetEntries(input_vec);
	auto k_data = FlatVector::GetData<uint64_t>(fields[0]);
	auto &value_lists = fields[1];
	auto &filter_lists = fields[2];
	auto value_entries = FlatVector::GetData<list_entry_t>(value_lists);
	auto filter_entries = FlatVector::GetData<list_entry_t>(filter_lists);
	auto &value_structs = ListVector::GetChild(value_lists);
	const auto &value_fields = StructVector::GetEntries(value_structs);

	// encode the values - this maps them to the same representation the state stores (e.g. as sort keys)
	auto extra_state = OP::CreateExtraState();
	UnifiedVectorFormat value_data;
	OP::PrepareData(value_fields[0], extra_state, value_data);
	auto value_strings = UnifiedVectorFormat::GetData<string_t>(value_data);
	auto count_data = FlatVector::GetData<uint64_t>(value_fields[1]);
	auto filter_data = FlatVector::GetData<uint64_t>(ListVector::GetChild(filter_lists));

	for (idx_t i = 0; i < count; i++) {
		auto &state = *reinterpret_cast<ApproxTopKState *>(dest_buffer + i * layout.total_state_size);
		state.state = nullptr;
		if (!validity.IsValid(i)) {
			// NULL input - leave the state empty
			continue;
		}
		if (FlatVector::IsNull(fields[0], i) || FlatVector::IsNull(value_lists, i) ||
		    FlatVector::IsNull(filter_lists, i)) {
			throw InvalidInputException("Invalid approx_top_k state - the k, values and filter cannot be NULL");
		}
		auto internal_state = make_uniq<InternalApproxTopKState>();
		auto &target = *internal_state;
		target.Initialize(k_data[i]);
		if (value_entries[i].length > target.capacity || filter_entries[i].length != target.filter.size()) {
			throw InvalidInputException("Invalid approx_top_k state - "
			                            "the values/filter sizes do not match the k value");
		}
		// insert the values - these are ordered by descending count, keeping "values" sorted
		for (idx_t value_idx = 0; value_idx < value_entries[i].length; value_idx++) {
			const auto idx = value_entries[i].offset + value_idx;
			const auto sel_idx = value_data.sel->get_index(idx);
			if (!value_data.validity.RowIsValid(sel_idx)) {
				throw InvalidInputException("Invalid approx_top_k state - the state values cannot be NULL");
			}

			auto &val = target.stored_values[target.values.size()];
			val.index = target.values.size();
			target.values.push_back(val);

			const auto &str_val = value_strings[sel_idx];
			ApproxTopKString topk_string(str_val, Hash(str_val));
			InternalApproxTopKState::CopyValue(val, topk_string, allocator);
			const bool inserted =
			    target.lookup_map.insert(make_pair(val.str_val, reference<ApproxTopKValue>(val))).second;
			if (!inserted) {
				throw InvalidInputException("Invalid approx_top_k state - the state values must be unique");
			}
			if (FlatVector::IsNull(value_fields[1], idx)) {
				throw InvalidInputException("Invalid approx_top_k state - the state counts cannot be NULL");
			}
			val.count = count_data[idx];
			if (val.count == 0) {
				throw InvalidInputException("Invalid approx_top_k state - the state values must have a count > 0");
			}
			if (value_idx > 0 && val.count > target.values[value_idx - 1].get().count) {
				throw InvalidInputException(
				    "Invalid approx_top_k state - the state values must be ordered by descending count");
			}
		}
		for (idx_t filter_idx = 0; filter_idx < filter_entries[i].length; filter_idx++) {
			const auto filter_entry_idx = filter_entries[i].offset + filter_idx;
			if (FlatVector::IsNull(ListVector::GetChild(filter_lists), filter_entry_idx)) {
				throw InvalidInputException("Invalid approx_top_k state - the filter values cannot be NULL");
			}
			target.filter[filter_idx] = filter_data[filter_entry_idx];
		}
		target.Verify();
		state.state = internal_state.release();
	}
}

unique_ptr<FunctionData> ApproxTopKBind(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	for (auto &arg : arguments) {
		if (arg->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}
	//	k must be constant
	auto k = input.GetConstant(1);
	if (arguments[0]->GetReturnType().id() == LogicalTypeId::VARCHAR) {
		function.SetStateUpdateCallback(ApproxTopKUpdate<string_t, HistogramStringFunctor>);
		function.SetStateFinalizeCallback(ApproxTopKFinalize<HistogramStringFunctor>);
		function.SetExportAggregateStateCallback(ApproxTopKExportState<HistogramStringFunctor>);
		function.SetImportAggregateStateCallback(ApproxTopKImportState<HistogramStringFunctor>);
	}
	// resolve the (originally ANY) value argument type so the exported state layout uses the actual input type
	function.GetArguments()[0] = arguments[0]->GetReturnType();
	function.SetReturnType(LogicalType::LIST(arguments[0]->GetReturnType()));
	return nullptr;
}

} // namespace

AggregateFunction ApproxTopKFun::GetFunction() {
	using STATE = ApproxTopKState;
	using OP = ApproxTopKOperation;
	auto fun = AggregateFunction("approx_top_k", {}, LogicalType::LIST(LogicalType::ANY),
	                             AggregateFunction::StateSize<STATE>, AggregateFunction::StateInitialize<STATE, OP>,
	                             ApproxTopKUpdate, AggregateFunction::StateCombine<STATE, OP>, ApproxTopKFinalize,
	                             nullptr, ApproxTopKBind, AggregateFunction::StateDestroy<STATE, OP>);
	fun.GetSignature().AddParameter("val", LogicalTypeId::ANY).AddParameter("k", LogicalType::BIGINT);
	fun.SetStateExportCallbacks(ApproxTopKGetStateType, ApproxTopKExportState<HistogramGenericFunctor>,
	                            ApproxTopKImportState<HistogramGenericFunctor>);
	fun.SetFallible(); // the update callback rejects NULL and out-of-range k values at runtime
	return fun;
}

} // namespace duckdb
