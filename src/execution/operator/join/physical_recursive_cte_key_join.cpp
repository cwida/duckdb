#include "duckdb/execution/operator/join/physical_recursive_cte_key_join.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_keyed_state.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

RecursiveCTEKeyJoinLayout::RecursiveCTEKeyJoinLayout(
    PhysicalRecursiveCTEStateScan &state_scan_p, PhysicalOperator &probe, bool state_on_left_p,
    vector<idx_t> state_key_indices_p, vector<idx_t> probe_key_indices_p, vector<ExpressionType> key_comparisons_p,
    vector<unique_ptr<Expression>> probe_key_normalizers_p, vector<idx_t> left_projection_map_p,
    vector<idx_t> right_projection_map_p)
    : state_scan(state_scan_p), state_on_left(state_on_left_p), state_key_indices(std::move(state_key_indices_p)),
      probe_key_indices(std::move(probe_key_indices_p)), key_comparisons(std::move(key_comparisons_p)),
      left_projection_map(std::move(left_projection_map_p)), right_projection_map(std::move(right_projection_map_p)),
      probe_key_normalizers(std::move(probe_key_normalizers_p)),
      state_key_map(state_scan.GetTypes().size(), DConstants::INVALID_INDEX),
      state_payload_map(state_scan.GetTypes().size(), DConstants::INVALID_INDEX) {
	if (state_key_indices.empty() || state_key_indices.size() != probe_key_indices.size() ||
	    state_key_indices.size() != key_comparisons.size() ||
	    state_key_indices.size() > state_scan.distinct_idx.size() ||
	    !std::is_sorted(state_key_indices.begin(), state_key_indices.end()) ||
	    std::adjacent_find(state_key_indices.begin(), state_key_indices.end()) != state_key_indices.end()) {
		throw InternalException("Invalid USING KEY join key layout");
	}
	for (idx_t key_idx = 0; key_idx < state_scan.distinct_idx.size(); key_idx++) {
		const auto state_idx = state_scan.distinct_idx[key_idx];
		if (state_idx >= state_scan.GetTypes().size() || key_idx >= state_scan.hash_key_types.size()) {
			throw InternalException("Invalid USING KEY state key ordinal");
		}
		state_key_map[state_idx] = key_idx;
		key_types.push_back(state_scan.hash_key_types[key_idx]);
	}
	for (idx_t join_key_idx = 0; join_key_idx < state_key_indices.size(); join_key_idx++) {
		const auto state_key_idx = state_key_indices[join_key_idx];
		const auto probe_key_idx = probe_key_indices[join_key_idx];
		if (state_key_idx >= key_types.size() || probe_key_idx >= probe.GetTypes().size()) {
			throw InternalException("Invalid USING KEY join key ordinal");
		}
		const auto state_column_idx = state_scan.distinct_idx[state_key_idx];
		const auto &raw_probe_type = state_scan.GetTypes()[state_column_idx];
		if (raw_probe_type != probe.GetTypes()[probe_key_idx]) {
			throw InternalException("Invalid USING KEY join key type");
		}
		if (key_comparisons[join_key_idx] != ExpressionType::COMPARE_EQUAL &&
		    key_comparisons[join_key_idx] != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			throw InternalException("Invalid USING KEY join comparison");
		}
		has_null_safe_comparisons =
		    has_null_safe_comparisons || key_comparisons[join_key_idx] == ExpressionType::COMPARE_NOT_DISTINCT_FROM;
		raw_probe_key_types.push_back(raw_probe_type);
		probe_key_types.push_back(key_types[state_key_idx]);
	}
	if (!probe_key_normalizers.empty()) {
		if (probe_key_normalizers.size() != probe_key_types.size()) {
			throw InternalException("Invalid USING KEY probe key normalizers");
		}
		for (idx_t key_idx = 0; key_idx < probe_key_normalizers.size(); key_idx++) {
			if (probe_key_normalizers[key_idx]->GetReturnType() != probe_key_types[key_idx]) {
				throw InternalException("Invalid USING KEY normalized probe key type");
			}
		}
	}
	for (idx_t payload_idx = 0; payload_idx < state_scan.payload_idx.size(); payload_idx++) {
		const auto state_idx = state_scan.payload_idx[payload_idx];
		if (state_idx >= state_scan.GetTypes().size()) {
			throw InternalException("Invalid USING KEY state payload ordinal");
		}
		state_payload_map[state_idx] = payload_idx;
		payload_types.push_back(state_scan.GetTypes()[state_idx]);
	}
	const auto &left_types = state_on_left ? state_scan.GetTypes() : probe.GetTypes();
	const auto &right_types = state_on_left ? probe.GetTypes() : state_scan.GetTypes();
	for (auto projection_idx : left_projection_map) {
		if (projection_idx >= left_types.size()) {
			throw InternalException("Invalid USING KEY left projection ordinal");
		}
	}
	for (auto projection_idx : right_projection_map) {
		if (projection_idx >= right_types.size()) {
			throw InternalException("Invalid USING KEY right projection ordinal");
		}
	}
}

bool RecursiveCTEKeyJoinLayout::IsPartial() const {
	return state_key_indices.size() < state_scan.distinct_idx.size();
}

PhysicalRecursiveCTEKeyJoin::PhysicalRecursiveCTEKeyJoin(
    PhysicalPlan &physical_plan, LogicalComparisonJoin &op, PhysicalOperator &probe,
    PhysicalRecursiveCTEStateScan &state_scan_p, bool state_on_left_p, vector<idx_t> state_key_indices_p,
    vector<idx_t> probe_key_indices_p, vector<ExpressionType> key_comparisons_p,
    vector<unique_ptr<Expression>> probe_key_normalizers_p, vector<idx_t> left_projection_map_p,
    vector<idx_t> right_projection_map_p, idx_t estimated_cardinality)
    : CachingPhysicalOperator(physical_plan, PhysicalOperatorType::RECURSIVE_KEY_JOIN, op.types, estimated_cardinality),
      layout(state_scan_p, probe, state_on_left_p, std::move(state_key_indices_p), std::move(probe_key_indices_p),
             std::move(key_comparisons_p), std::move(probe_key_normalizers_p), std::move(left_projection_map_p),
             std::move(right_projection_map_p)) {
	children.push_back(probe);
}

struct RecursiveCTEKeyJoinResult {
	idx_t match_count;
	OperatorResultType result_type;
};

struct RecursiveCTEKeyJoinHashTableState {
	RecursiveCTEKeyJoinHashTableState(GroupedAggregateHashTable &hash_table_p, shared_ptr<TupleDataLayout> layout_p)
	    : hash_table(hash_table_p), layout(std::move(layout_p)) {
	}

	optional_ptr<const GroupedAggregateHashTable> hash_table;
	shared_ptr<TupleDataLayout> layout;
	AggregateHTLookupState lookup_state;
	TupleDataChunkState match_chunk_state;
	RowMatcher partial_matcher;
	bool partial_matcher_initialized = false;
};

class RecursiveCTEKeyJoinState : public CachingOperatorState {
public:
	RecursiveCTEKeyJoinState(ClientContext &context, const PhysicalRecursiveCTEKeyJoin &op)
	    : valid_probe_sel(STANDARD_VECTOR_SIZE), found_key_sel(STANDARD_VECTOR_SIZE),
	      matched_input_sel(STANDARD_VECTOR_SIZE), candidate_input_sel(STANDARD_VECTOR_SIZE),
	      candidate_match_sel(STANDARD_VECTOR_SIZE), candidate_addresses(LogicalType::POINTER),
	      matched_addresses(LogicalType::POINTER), probe_hashes(LogicalType::HASH),
	      key_formats(op.Layout().ProbeKeyTypes().size()), arena(Allocator::Get(context)), row_state(arena) {
		probe_keys.Initialize(Allocator::Get(context), op.Layout().ProbeKeyTypes());
		if (!op.Layout().ProbeKeyNormalizers().empty()) {
			probe_key_executor = make_uniq<ExpressionExecutor>(context);
			raw_probe_keys.Initialize(Allocator::Get(context), op.Layout().RawProbeKeyTypes());
			for (auto &normalizer : op.Layout().ProbeKeyNormalizers()) {
				probe_key_executor->AddExpression(*normalizer);
			}
		}
		lookup_keys.Initialize(Allocator::Get(context), op.Layout().ProbeKeyTypes());
		candidate_keys.Initialize(Allocator::Get(context), op.Layout().KeyTypes());
		state_keys.Initialize(Allocator::Get(context), op.Layout().KeyTypes());
		aggregate_rows.Initialize(Allocator::Get(context), op.Layout().StateScan().aggregate_types);
		state_rows.Initialize(Allocator::Get(context), op.Layout().StateScan().GetTypes());
	}

	OperatorResultType Execute(DataChunk &input, DataChunk &output, const RecursiveCTEKeyJoinLayout &layout,
	                           RecursiveCTEKeyedState &recursive_state);

	bool SupportsReuse() const override {
		return true;
	}

	void Reset() override {
		ResetCachingState();
		partial_input_initialized = false;
		active_probe = false;
	}

private:
	template <bool COLLECT_METRICS>
	OperatorResultType ExecuteInternal(DataChunk &input, DataChunk &output, const RecursiveCTEKeyJoinLayout &layout,
	                                   RecursiveCTEKeyedState &recursive_state);
	void BindHashTable(GroupedAggregateHashTable &hash_table);
	RecursiveCTEKeyJoinResult ProbeCompleteKey(DataChunk &input, const RecursiveCTEKeyJoinLayout &layout,
	                                           RecursiveCTEKeyedState &recursive_state);
	RecursiveCTEKeyJoinResult ProbePartialKey(DataChunk &input, const RecursiveCTEKeyJoinLayout &layout,
	                                          RecursiveCTEKeyedState &recursive_state);
	void ExtractProbeKeys(DataChunk &input, const RecursiveCTEKeyJoinLayout &layout);
	void FinalizeStateRows(RecursiveCTEKeyedState &recursive_state, idx_t match_count);
	void EmitResult(DataChunk &input, DataChunk &output, const RecursiveCTEKeyJoinLayout &layout, idx_t match_count);

private:
	DataChunk probe_keys;
	DataChunk raw_probe_keys;
	DataChunk lookup_keys;
	DataChunk candidate_keys;
	DataChunk state_keys;
	DataChunk aggregate_rows;
	DataChunk state_rows;
	SelectionVector valid_probe_sel;
	SelectionVector found_key_sel;
	SelectionVector matched_input_sel;
	SelectionVector candidate_input_sel;
	SelectionVector candidate_match_sel;
	Vector candidate_addresses;
	Vector matched_addresses;
	Vector probe_hashes;
	vector<UnifiedVectorFormat> key_formats;
	unique_ptr<ExpressionExecutor> probe_key_executor;
	unique_ptr<RecursiveCTEKeyJoinHashTableState> hash_table_state;
	ArenaAllocator arena;
	RowOperationsState row_state;
	idx_t valid_probe_position = 0;
	idx_t valid_probe_count = 0;
	idx_t current_probe_input = 0;
	idx_t current_entry = DConstants::INVALID_INDEX;
	bool partial_input_initialized = false;
	bool active_probe = false;
};

unique_ptr<OperatorState> PhysicalRecursiveCTEKeyJoin::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<RecursiveCTEKeyJoinState>(context.client, *this);
}

static idx_t SelectNonNullKeys(DataChunk &keys, vector<UnifiedVectorFormat> &formats, SelectionVector &result) {
	for (idx_t key_idx = 0; key_idx < keys.ColumnCount(); key_idx++) {
		keys.data[key_idx].ToUnifiedFormat(formats[key_idx]);
	}
	idx_t result_count = 0;
	for (idx_t row_idx = 0; row_idx < keys.size(); row_idx++) {
		bool valid = true;
		for (auto &format : formats) {
			valid = valid && format.validity.RowIsValid(format.sel->get_index(row_idx));
		}
		if (valid) {
			result.set_index(result_count++, row_idx);
		}
	}
	return result_count;
}

static idx_t SelectValidProbeKeys(DataChunk &keys, const RecursiveCTEKeyJoinLayout &layout,
                                  vector<UnifiedVectorFormat> &formats, SelectionVector &result) {
	const auto &comparisons = layout.KeyComparisons();
	D_ASSERT(keys.ColumnCount() == comparisons.size());
	if (!layout.HasNullSafeComparisons()) {
		return SelectNonNullKeys(keys, formats, result);
	}
	for (idx_t key_idx = 0; key_idx < keys.ColumnCount(); key_idx++) {
		if (comparisons[key_idx] == ExpressionType::COMPARE_EQUAL) {
			keys.data[key_idx].ToUnifiedFormat(formats[key_idx]);
		}
	}
	idx_t result_count = 0;
	for (idx_t row_idx = 0; row_idx < keys.size(); row_idx++) {
		bool valid = true;
		for (idx_t key_idx = 0; key_idx < comparisons.size(); key_idx++) {
			if (comparisons[key_idx] == ExpressionType::COMPARE_EQUAL) {
				auto &format = formats[key_idx];
				valid = valid && format.validity.RowIsValid(format.sel->get_index(row_idx));
			}
		}
		if (valid) {
			result.set_index(result_count++, row_idx);
		}
	}
	return result_count;
}

void RecursiveCTEKeyJoinState::BindHashTable(GroupedAggregateHashTable &hash_table) {
	if (hash_table_state && hash_table_state->hash_table.get() == &hash_table &&
	    hash_table_state->layout.get() == &hash_table.GetLayout()) {
		return;
	}
	if (partial_input_initialized) {
		throw InternalException("USING KEY direct probe changed hash tables while producing output");
	}
	hash_table_state = make_uniq<RecursiveCTEKeyJoinHashTableState>(hash_table, hash_table.GetLayoutPtr());
}

RecursiveCTEKeyJoinResult RecursiveCTEKeyJoinState::ProbeCompleteKey(DataChunk &input,
                                                                     const RecursiveCTEKeyJoinLayout &layout,
                                                                     RecursiveCTEKeyedState &recursive_state) {
	ExtractProbeKeys(input, layout);
	const auto current_probe_count = SelectValidProbeKeys(probe_keys, layout, key_formats, valid_probe_sel);
	if (current_probe_count == 0) {
		return {0, OperatorResultType::NEED_MORE_INPUT};
	}

	lookup_keys.Reset();
	if (current_probe_count == input.size()) {
		lookup_keys.Reference(probe_keys);
	} else {
		lookup_keys.Slice(probe_keys, valid_probe_sel, current_probe_count);
	}
	auto &lookup_state = hash_table_state->lookup_state;
	auto &hash_table = recursive_state.GetHashTable();
	const auto match_count = hash_table.LookupGroups(lookup_keys, lookup_state, found_key_sel);
	if (recursive_state.GetMetrics().Enabled()) {
		recursive_state.GetMetrics().RecordDirectProbeRows(current_probe_count);
		recursive_state.GetMetrics().RecordDirectProbeMatches(match_count);
	}
	if (match_count == 0) {
		return {0, OperatorResultType::NEED_MORE_INPUT};
	}

	matched_addresses.SetVectorType(VectorType::FLAT_VECTOR);
	auto matched_address_data = FlatVector::GetDataMutable<data_ptr_t>(matched_addresses);
	auto lookup_addresses = FlatVector::GetData<data_ptr_t>(lookup_state.addresses);
	for (idx_t match_idx = 0; match_idx < match_count; match_idx++) {
		const auto lookup_idx = found_key_sel.get_index_unsafe(match_idx);
		const auto input_idx =
		    current_probe_count == input.size() ? lookup_idx : valid_probe_sel.get_index_unsafe(lookup_idx);
		matched_input_sel.set_index(match_idx, input_idx);
		matched_address_data[match_idx] = lookup_addresses[lookup_idx];
	}
	FlatVector::SetSize(matched_addresses, match_count);
	return {match_count, OperatorResultType::NEED_MORE_INPUT};
}

RecursiveCTEKeyJoinResult RecursiveCTEKeyJoinState::ProbePartialKey(DataChunk &input,
                                                                    const RecursiveCTEKeyJoinLayout &layout,
                                                                    RecursiveCTEKeyedState &recursive_state) {
	const auto &state_key_indices = layout.StateKeyIndices();
	auto &index = recursive_state.GetPartialKeyIndex(state_key_indices);
	auto &match_chunk_state = hash_table_state->match_chunk_state;
	auto &partial_matcher = hash_table_state->partial_matcher;
	const auto collect_metrics = recursive_state.GetMetrics().Enabled();
	if (!partial_input_initialized) {
		ExtractProbeKeys(input, layout);
		valid_probe_count = SelectValidProbeKeys(probe_keys, layout, key_formats, valid_probe_sel);
		if (valid_probe_count == 0) {
			return {0, OperatorResultType::NEED_MORE_INPUT};
		}
		probe_keys.Hash(probe_hashes);
		valid_probe_position = 0;
		active_probe = false;
		partial_input_initialized = true;
		if (collect_metrics) {
			recursive_state.GetMetrics().RecordDirectProbeRows(valid_probe_count);
		}
	}
	if (!hash_table_state->partial_matcher_initialized) {
		const auto &predicates = layout.KeyComparisons();
		vector<column_t> columns;
		for (auto key_idx : state_key_indices) {
			columns.push_back(key_idx);
		}
		TupleDataCollection::InitializeChunkState(match_chunk_state, layout.KeyTypes(), columns);
		partial_matcher.Initialize(false, recursive_state.GetHashTable().GetLayout(), predicates, std::move(columns));
		hash_table_state->partial_matcher_initialized = true;
	}

	idx_t chain_visits = 0;
	while (true) {
		idx_t candidate_count = 0;
		candidate_addresses.SetVectorType(VectorType::FLAT_VECTOR);
		auto candidate_address_data = FlatVector::GetDataMutable<data_ptr_t>(candidate_addresses);
		const auto hash_data = probe_hashes.Values<hash_t>();
		while (candidate_count < STANDARD_VECTOR_SIZE) {
			if (!active_probe) {
				if (valid_probe_position >= valid_probe_count) {
					break;
				}
				current_probe_input = valid_probe_sel.get_index_unsafe(valid_probe_position++);
				current_entry = index.GetHead(hash_data[current_probe_input].GetValue());
				active_probe = true;
			}
			if (current_entry == DConstants::INVALID_INDEX) {
				active_probe = false;
				continue;
			}
			const auto &entry = index.GetEntry(current_entry);
			current_entry = entry.next;
			if (collect_metrics) {
				chain_visits++;
			}
			if (entry.hash != hash_data[current_probe_input].GetValue()) {
				continue;
			}
			candidate_input_sel.set_index(candidate_count, current_probe_input);
			candidate_address_data[candidate_count++] = entry.address;
		}
		if (active_probe && current_entry == DConstants::INVALID_INDEX) {
			active_probe = false;
		}
		const bool has_more = active_probe || valid_probe_position < valid_probe_count;
		if (candidate_count == 0) {
			if (collect_metrics) {
				recursive_state.GetMetrics().RecordPartialProbeChainVisits(chain_visits);
			}
			partial_input_initialized = false;
			return {0, OperatorResultType::NEED_MORE_INPUT};
		}
		FlatVector::SetSize(candidate_addresses, candidate_count);
		candidate_keys.Reset();
		for (idx_t partial_idx = 0; partial_idx < state_key_indices.size(); partial_idx++) {
			const auto state_key_idx = state_key_indices[partial_idx];
			candidate_keys.data[state_key_idx].Slice(probe_keys.data[partial_idx], candidate_input_sel,
			                                         candidate_count);
		}
		candidate_keys.SetChildCardinality(candidate_count);
		TupleDataCollection::ToUnifiedFormat(match_chunk_state, candidate_keys);
		for (idx_t candidate_idx = 0; candidate_idx < candidate_count; candidate_idx++) {
			candidate_match_sel.set_index(candidate_idx, candidate_idx);
		}
		idx_t no_match_count = 0;
		const auto match_count =
		    partial_matcher.Match(candidate_keys, match_chunk_state.vector_data, candidate_match_sel, candidate_count,
		                          candidate_addresses, nullptr, no_match_count);
		if (match_count == 0 && has_more) {
			continue;
		}
		if (match_count == 0) {
			if (collect_metrics) {
				recursive_state.GetMetrics().RecordPartialProbeChainVisits(chain_visits);
			}
			partial_input_initialized = false;
			return {0, OperatorResultType::NEED_MORE_INPUT};
		}

		matched_addresses.SetVectorType(VectorType::FLAT_VECTOR);
		auto matched_address_data = FlatVector::GetDataMutable<data_ptr_t>(matched_addresses);
		for (idx_t match_idx = 0; match_idx < match_count; match_idx++) {
			const auto candidate_idx = candidate_match_sel.get_index_unsafe(match_idx);
			matched_input_sel.set_index(match_idx, candidate_input_sel.get_index_unsafe(candidate_idx));
			matched_address_data[match_idx] = candidate_address_data[candidate_idx];
		}
		FlatVector::SetSize(matched_addresses, match_count);
		if (collect_metrics) {
			recursive_state.GetMetrics().RecordPartialProbeChainVisits(chain_visits);
			recursive_state.GetMetrics().RecordDirectProbeMatches(match_count);
		}
		if (!has_more) {
			partial_input_initialized = false;
		}
		return {match_count, has_more ? OperatorResultType::HAVE_MORE_OUTPUT : OperatorResultType::NEED_MORE_INPUT};
	}
}

void RecursiveCTEKeyJoinState::ExtractProbeKeys(DataChunk &input, const RecursiveCTEKeyJoinLayout &layout) {
	probe_keys.Reset();
	const auto &probe_key_indices = layout.ProbeKeyIndices();
	if (layout.ProbeKeyNormalizers().empty()) {
		for (idx_t key_idx = 0; key_idx < probe_key_indices.size(); key_idx++) {
			probe_keys.data[key_idx].Reference(input.data[probe_key_indices[key_idx]]);
		}
		probe_keys.CheckCardinality(input.size());
		return;
	}
	raw_probe_keys.Reset();
	for (idx_t key_idx = 0; key_idx < probe_key_indices.size(); key_idx++) {
		raw_probe_keys.data[key_idx].Reference(input.data[probe_key_indices[key_idx]]);
	}
	raw_probe_keys.CheckCardinality(input.size());
	D_ASSERT(probe_key_executor);
	probe_key_executor->Execute(raw_probe_keys, probe_keys);
}

void RecursiveCTEKeyJoinState::FinalizeStateRows(RecursiveCTEKeyedState &recursive_state, idx_t match_count) {
	state_keys.CheckCardinality(match_count);
	recursive_state.FinalizeStateRows(row_state, matched_addresses, state_keys, aggregate_rows, state_rows);
}

void RecursiveCTEKeyJoinState::EmitResult(DataChunk &input, DataChunk &output, const RecursiveCTEKeyJoinLayout &layout,
                                          idx_t match_count) {
	idx_t output_idx = 0;
	auto emit_probe = [&](const vector<idx_t> &projection_map) {
		for (auto probe_idx : projection_map) {
			output.data[output_idx++].Slice(input.data[probe_idx], matched_input_sel, match_count);
		}
	};
	auto emit_state = [&](const vector<idx_t> &projection_map) {
		for (auto state_idx : projection_map) {
			output.data[output_idx++].Reference(state_rows.data[state_idx]);
		}
	};
	if (layout.StateOnLeft()) {
		emit_state(layout.LeftProjectionMap());
		emit_probe(layout.RightProjectionMap());
	} else {
		emit_probe(layout.LeftProjectionMap());
		emit_state(layout.RightProjectionMap());
	}
	if (output_idx != output.ColumnCount()) {
		throw InternalException("USING KEY direct probe produced %d columns, expected %d", output_idx,
		                        output.ColumnCount());
	}
	output.CheckCardinality(match_count);
}

template <bool COLLECT_METRICS>
OperatorResultType RecursiveCTEKeyJoinState::ExecuteInternal(DataChunk &input, DataChunk &output,
                                                             const RecursiveCTEKeyJoinLayout &layout,
                                                             RecursiveCTEKeyedState &recursive_state) {
	const auto lookup_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	BindHashTable(recursive_state.GetHashTable());
	auto result = layout.IsPartial() ? ProbePartialKey(input, layout, recursive_state)
	                                 : ProbeCompleteKey(input, layout, recursive_state);
	if constexpr (COLLECT_METRICS) {
		const auto lookup_end = std::chrono::steady_clock::now();
		recursive_state.GetEpochMetrics().RecordDirectProbeLookup(NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(lookup_end - lookup_start).count()));
	}
	if (result.match_count == 0) {
		return result.result_type;
	}
	const auto gather_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	if (layout.IsPartial()) {
		recursive_state.GetHashTable().GatherGroups(hash_table_state->lookup_state, matched_addresses,
		                                            *FlatVector::IncrementalSelectionVector(), result.match_count,
		                                            state_keys);
	} else {
		recursive_state.GetHashTable().GatherGroups(hash_table_state->lookup_state, found_key_sel, result.match_count,
		                                            state_keys);
	}
	if constexpr (COLLECT_METRICS) {
		const auto gather_end = std::chrono::steady_clock::now();
		recursive_state.GetEpochMetrics().RecordDirectProbeKeyGather(NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(gather_end - gather_start).count()));
	}
	if constexpr (COLLECT_METRICS) {
		if (!layout.StateScan().aggregate_types.empty()) {
			const auto finalize_start = std::chrono::steady_clock::now();
			FinalizeStateRows(recursive_state, result.match_count);
			const auto finalize_end = std::chrono::steady_clock::now();
			recursive_state.GetEpochMetrics().RecordDirectProbePayloadFinalize(NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count()));
		} else {
			FinalizeStateRows(recursive_state, result.match_count);
		}
	} else {
		FinalizeStateRows(recursive_state, result.match_count);
	}
	EmitResult(input, output, layout, result.match_count);
	return result.result_type;
}

OperatorResultType RecursiveCTEKeyJoinState::Execute(DataChunk &input, DataChunk &output,
                                                     const RecursiveCTEKeyJoinLayout &layout,
                                                     RecursiveCTEKeyedState &recursive_state) {
	if (recursive_state.GetMetrics().Enabled()) {
		return ExecuteInternal<true>(input, output, layout, recursive_state);
	}
	return ExecuteInternal<false>(input, output, layout, recursive_state);
}

OperatorResultType PhysicalRecursiveCTEKeyJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input,
                                                                DataChunk &chunk, GlobalOperatorState &gstate,
                                                                OperatorState &state_p) const {
	auto &state_scan = layout.StateScan();
	if (!state_scan.recursive_cte || !state_scan.recursive_cte->sink_state) {
		throw InternalException("USING KEY direct probe has no recursive state");
	}
	auto &recursive_state = state_scan.recursive_cte->sink_state->Cast<RecursiveCTEKeyedState>();
	return state_p.Cast<RecursiveCTEKeyJoinState>().Execute(input, chunk, layout, recursive_state);
}

string PhysicalRecursiveCTEKeyJoin::GetName() const {
	return layout.IsPartial() ? "RECURSIVE_PARTIAL_KEY_JOIN" : "RECURSIVE_KEY_JOIN";
}

InsertionOrderPreservingMap<string> PhysicalRecursiveCTEKeyJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Join Type"] = "INNER";
	result["Key Columns"] = to_string(layout.ProbeKeyIndices().size());
	result["Key Mode"] = layout.IsPartial() ? "PARTIAL" : "COMPLETE";
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
