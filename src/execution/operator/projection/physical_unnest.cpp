#include "duckdb/execution/operator/projection/physical_unnest.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"

namespace duckdb {

//! The operator state of the window
class PhysicalUnnestOperatorState : public PhysicalOperatorState {
public:
	PhysicalUnnestOperatorState(PhysicalOperator &op, PhysicalOperator *child)
	    : PhysicalOperatorState(op, child), parent_position(0), list_position(0), list_length(-1) {
	}

	idx_t parent_position;
	idx_t list_position;
	int64_t list_length = -1;

	DataChunk list_data;
	vector<VectorData> list_vector_data;
	vector<VectorData> list_child_data;
};

// this implements a sorted window functions variant
PhysicalUnnest::PhysicalUnnest(vector<LogicalType> types, vector<unique_ptr<Expression>> select_list,
                               idx_t estimated_cardinality, PhysicalOperatorType type)
    : PhysicalOperator(type, move(types), estimated_cardinality), select_list(std::move(select_list)) {

	D_ASSERT(!this->select_list.empty());
}

static void UnnestNull(idx_t start, idx_t end, Vector &result) {
	if (result.GetType().InternalType() == PhysicalType::STRUCT) {
		auto &children = StructVector::GetEntries(result);
		for (auto &child : children) {
			UnnestNull(start, end, *child);
		}
	}
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = start; i < end; i++) {
		validity.SetInvalid(i);
	}
	if (result.GetType().InternalType() == PhysicalType::STRUCT) {
		auto &struct_children = StructVector::GetEntries(result);
		for (auto &child : struct_children) {
			UnnestNull(start, end, *child);
		}
	}
}

template <class T>
static void TemplatedUnnest(VectorData &vdata, idx_t start, idx_t end, Vector &result) {
	auto source_data = (T *)vdata.data;
	auto &source_mask = vdata.validity;
	auto result_data = FlatVector::GetData<T>(result);
	auto &result_mask = FlatVector::Validity(result);

	for (idx_t i = start; i < end; i++) {
		auto source_idx = vdata.sel->get_index(i);
		auto target_idx = i - start;
		if (source_mask.RowIsValid(source_idx)) {
			result_data[target_idx] = source_data[source_idx];
			result_mask.SetValid(target_idx);
		} else {
			result_mask.SetInvalid(target_idx);
		}
	}
}

static void UnnestValidity(VectorData &vdata, idx_t start, idx_t end, Vector &result) {
	auto &source_mask = vdata.validity;
	auto &result_mask = FlatVector::Validity(result);

	for (idx_t i = start; i < end; i++) {
		auto source_idx = vdata.sel->get_index(i);
		auto target_idx = i - start;
		result_mask.Set(target_idx, source_mask.RowIsValid(source_idx));
	}
}

static void UnnestVector(VectorData &vdata, Vector &source, idx_t list_size, idx_t start, idx_t end, Vector &result) {
	switch (result.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedUnnest<int8_t>(vdata, start, end, result);
		break;
	case PhysicalType::INT16:
		TemplatedUnnest<int16_t>(vdata, start, end, result);
		break;
	case PhysicalType::INT32:
		TemplatedUnnest<int32_t>(vdata, start, end, result);
		break;
	case PhysicalType::INT64:
		TemplatedUnnest<int64_t>(vdata, start, end, result);
		break;
	case PhysicalType::INT128:
		TemplatedUnnest<hugeint_t>(vdata, start, end, result);
		break;
	case PhysicalType::UINT8:
		TemplatedUnnest<uint8_t>(vdata, start, end, result);
		break;
	case PhysicalType::UINT16:
		TemplatedUnnest<uint16_t>(vdata, start, end, result);
		break;
	case PhysicalType::UINT32:
		TemplatedUnnest<uint32_t>(vdata, start, end, result);
		break;
	case PhysicalType::UINT64:
		TemplatedUnnest<uint64_t>(vdata, start, end, result);
		break;
	case PhysicalType::FLOAT:
		TemplatedUnnest<float>(vdata, start, end, result);
		break;
	case PhysicalType::DOUBLE:
		TemplatedUnnest<double>(vdata, start, end, result);
		break;
	case PhysicalType::POINTER:
		TemplatedUnnest<uintptr_t>(vdata, start, end, result);
		break;
	case PhysicalType::INTERVAL:
		TemplatedUnnest<interval_t>(vdata, start, end, result);
		break;
	case PhysicalType::HASH:
		TemplatedUnnest<hash_t>(vdata, start, end, result);
		break;
	case PhysicalType::VARCHAR:
		TemplatedUnnest<string_t>(vdata, start, end, result);
		break;
	case PhysicalType::LIST: {
		auto &target = ListVector::GetEntry(result);
		target.Reference(ListVector::GetEntry(source));
		ListVector::SetListSize(result, ListVector::GetListSize(source));
		TemplatedUnnest<list_entry_t>(vdata, start, end, result);
		break;
	}
	case PhysicalType::STRUCT: {
		auto &source_entries = StructVector::GetEntries(source);
		auto &target_entries = StructVector::GetEntries(result);
		UnnestValidity(vdata, start, end, result);
		for (idx_t i = 0; i < source_entries.size(); i++) {
			VectorData sdata;
			source_entries[i]->Orrify(list_size, sdata);
			UnnestVector(sdata, *source_entries[i], list_size, start, end, *target_entries[i]);
		}
		break;
	}
	default:
		throw InternalException("Unimplemented type for UNNEST");
	}
}

void PhysicalUnnest::GetChunkInternal(ExecutionContext &context, DataChunk &chunk,
                                      PhysicalOperatorState *state_p) const {
	auto state = reinterpret_cast<PhysicalUnnestOperatorState *>(state_p);
	while (true) { // repeat until we actually have produced some rows
		if (state->child_chunk.size() == 0 || state->parent_position >= state->child_chunk.size()) {
			// get the child data
			children[0]->GetChunk(context, state->child_chunk, state->child_state.get());
			if (state->child_chunk.size() == 0) {
				return;
			}
			state->parent_position = 0;
			state->list_position = 0;
			state->list_length = -1;

			// get the list data to unnest
			ExpressionExecutor executor;
			vector<LogicalType> list_data_types;
			for (auto &exp : select_list) {
				D_ASSERT(exp->type == ExpressionType::BOUND_UNNEST);
				auto bue = (BoundUnnestExpression *)exp.get();
				list_data_types.push_back(bue->child->return_type);
				executor.AddExpression(*bue->child.get());
			}
			state->list_data.Destroy();
			state->list_data.Initialize(list_data_types);
			executor.Execute(state->child_chunk, state->list_data);

			// paranoia aplenty
			state->child_chunk.Verify();
			state->list_data.Verify();
			D_ASSERT(state->child_chunk.size() == state->list_data.size());
			D_ASSERT(state->list_data.ColumnCount() == select_list.size());

			// initialize VectorData object so the nullmask can accessed
			state->list_vector_data.resize(state->list_data.ColumnCount());
			state->list_child_data.resize(state->list_data.ColumnCount());
			for (idx_t col_idx = 0; col_idx < state->list_data.ColumnCount(); col_idx++) {
				auto &list_vector = state->list_data.data[col_idx];
				list_vector.Orrify(state->list_data.size(), state->list_vector_data[col_idx]);

				auto &child_vector = ListVector::GetEntry(list_vector);
				auto list_size = ListVector::GetListSize(list_vector);
				child_vector.Orrify(list_size, state->list_child_data[col_idx]);
			}
		}

		// need to figure out how many times we need to repeat for current row
		if (state->list_length < 0) {
			for (idx_t col_idx = 0; col_idx < state->list_data.ColumnCount(); col_idx++) {
				auto &vdata = state->list_vector_data[col_idx];
				auto current_idx = vdata.sel->get_index(state->parent_position);

				int64_t list_length;
				// deal with NULL values
				if (!vdata.validity.RowIsValid(current_idx)) {
					list_length = 1;
				} else {
					auto list_data = (list_entry_t *)vdata.data;
					auto list_entry = list_data[current_idx];
					list_length = (int64_t)list_entry.length;
				}

				if (list_length > state->list_length) {
					state->list_length = list_length;
				}
			}
		}

		D_ASSERT(state->list_length >= 0);

		auto this_chunk_len = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state->list_length - state->list_position);

		// first cols are from child, last n cols from unnest
		chunk.SetCardinality(this_chunk_len);

		for (idx_t col_idx = 0; col_idx < state->child_chunk.ColumnCount(); col_idx++) {
			ConstantVector::Reference(chunk.data[col_idx], state->child_chunk.data[col_idx], state->parent_position,
			                          state->child_chunk.size());
		}

		for (idx_t col_idx = 0; col_idx < state->list_data.ColumnCount(); col_idx++) {
			auto &result_vector = chunk.data[col_idx + state->child_chunk.ColumnCount()];

			auto &vdata = state->list_vector_data[col_idx];
			auto &child_data = state->list_child_data[col_idx];
			auto current_idx = vdata.sel->get_index(state->parent_position);

			auto list_data = (list_entry_t *)vdata.data;
			auto list_entry = list_data[current_idx];

			idx_t list_count;
			if (state->list_position >= list_entry.length) {
				list_count = 0;
			} else {
				list_count = MinValue<idx_t>(this_chunk_len, list_entry.length - state->list_position);
			}

			if (list_entry.length > state->list_position) {
				if (!vdata.validity.RowIsValid(current_idx)) {
					UnnestNull(0, list_count, result_vector);
				} else {
					auto &list_vector = state->list_data.data[col_idx];
					auto &child_vector = ListVector::GetEntry(list_vector);
					auto list_size = ListVector::GetListSize(list_vector);

					auto base_offset = list_entry.offset + state->list_position;
					UnnestVector(child_data, child_vector, list_size, base_offset, base_offset + list_count,
					             result_vector);
				}
			}
			UnnestNull(list_count, this_chunk_len, result_vector);
		}

		state->list_position += this_chunk_len;
		if ((int64_t)state->list_position == state->list_length) {
			state->parent_position++;
			state->list_length = -1;
			state->list_position = 0;
		}

		chunk.Verify();
		if (chunk.size() > 0) {
			return;
		}
	}
}

unique_ptr<PhysicalOperatorState> PhysicalUnnest::GetOperatorState() {
	return make_unique<PhysicalUnnestOperatorState>(*this, children[0].get());
}

} // namespace duckdb
