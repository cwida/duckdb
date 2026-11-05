#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/execution/operator/helper/physical_verify_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"

namespace duckdb {

PhysicalVerifyVector::PhysicalVerifyVector(PhysicalPlan &physical_plan, PhysicalOperator &child,
                                           DebugVectorVerification verification)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::VERIFY_VECTOR, child.GetTypes(),
                       child.estimated_cardinality),
      verification(verification) {
	children.push_back(child);
}

class VerifyVectorState : public OperatorState {
public:
	explicit VerifyVectorState() : const_idx(0) {
	}

	idx_t const_idx;
};

OperatorResultType VerifyEmitConstantVectors(const DataChunk &input, DataChunk &chunk, OperatorState &state_p) {
	auto &state = state_p.Cast<VerifyVectorState>();
	D_ASSERT(state.const_idx < input.size());

	// Ensure that we don't alter the input data while another thread is still using it.
	DataChunk copied_input;
	copied_input.Initialize(Allocator::DefaultAllocator(), input.GetTypes());
	input.Copy(copied_input);

	// emit constant vectors at the current index
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		ConstantVector::Reference(chunk.data[c], count_t(1), copied_input.data[c], state.const_idx, 1);
	}
	state.const_idx++;
	if (state.const_idx >= copied_input.size()) {
		state.const_idx = 0;
		return OperatorResultType::NEED_MORE_INPUT;
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorResultType VerifyEmitDictionaryVectors(const DataChunk &input, DataChunk &chunk, OperatorState &state) {
	input.Copy(chunk);
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		Vector::DebugTransformToDictionary(chunk.data[c]);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

struct ConstantOrSequenceInfo {
	vector<Value> values;
	bool is_constant = true;
};

static const vector<Value> &GetNestedValueChildren(const Value &value) {
	switch (value.type().InternalType()) {
	case PhysicalType::STRUCT:
		return StructValue::GetChildren(value);
	case PhysicalType::LIST:
		return ListValue::GetChildren(value);
	case PhysicalType::ARRAY:
		return ArrayValue::GetChildren(value);
	default:
		throw InternalException("Expected a nested value");
	}
}

static bool CanShareConstantValue(const Value &left, const Value &right) {
	if (left.type() != right.type()) {
		return false;
	}
	if (left.IsNull() || right.IsNull()) {
		return left.IsNull() == right.IsNull();
	}
	switch (left.type().InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY: {
		// Compare VARIANT storage children directly to preserve payload types and object fields.
		auto &left_children = GetNestedValueChildren(left);
		auto &right_children = GetNestedValueChildren(right);
		if (left_children.size() != right_children.size()) {
			return false;
		}
		for (idx_t i = 0; i < left_children.size(); i++) {
			if (!CanShareConstantValue(left_children[i], right_children[i])) {
				return false;
			}
		}
		return true;
	}
	default:
		return ValueOperations::NotDistinctFrom(left, right);
	}
}

OperatorResultType VerifyEmitSequenceVector(const DataChunk &input_p, DataChunk &chunk, OperatorState &state_p) {
	auto &state = state_p.Cast<VerifyVectorState>();
	D_ASSERT(state.const_idx < input_p.size());

	// FIXME: work-around for variant bug...
	DataChunk input;
	input.Initialize(Allocator::DefaultAllocator(), input_p.GetTypes());
	input_p.Copy(input);

	// find the longest length sequence or constant vector to emit
	vector<ConstantOrSequenceInfo> infos;
	idx_t max_length = 0;
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		bool can_be_sequence = false;
		switch (chunk.data[c].GetType().id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT: {
			can_be_sequence = true;
			break;
		}
		default: {
			break;
		}
		}
		bool can_be_constant = true;
		switch (chunk.data[c].GetType().id()) {
		case LogicalTypeId::INTERVAL:
			can_be_constant = false;
			break;
		default:
			break;
		}
		ConstantOrSequenceInfo info;
		info.is_constant = true;
		for (idx_t k = state.const_idx; k < input.size(); k++) {
			auto val = input.data[c].GetValue(k);
			if (info.values.empty()) {
				info.values.push_back(std::move(val));
			} else if (info.is_constant) {
				if (can_be_constant && CanShareConstantValue(val, info.values[0])) {
					// found the same value! continue
					info.values.push_back(std::move(val));
					continue;
				}
				// not the same value - can we convert this into a sequence vector?
				if (!can_be_sequence) {
					break;
				}
				// we can only convert to a sequence if we have only gathered one value
				// otherwise we would have multiple identical values here already
				if (info.values.size() > 1) {
					break;
				}
				// cannot create a sequence with null values
				if (val.IsNull() || info.values[0].IsNull()) {
					break;
				}
				// check if the increment fits in the target type
				// i.e. we cannot have a sequence vector with an increment of 200 in `int8_t`
				auto increment = hugeint_t(val.GetValue<int64_t>()) - hugeint_t(info.values[0].GetValue<int64_t>());
				bool increment_fits = true;
				switch (chunk.data[c].GetType().id()) {
				case LogicalTypeId::TINYINT: {
					int8_t result;
					if (!Hugeint::TryCast<int8_t>(increment, result)) {
						increment_fits = false;
					}
					break;
				}
				case LogicalTypeId::SMALLINT: {
					int16_t result;
					if (!Hugeint::TryCast<int16_t>(increment, result)) {
						increment_fits = false;
					}
					break;
				}
				case LogicalTypeId::INTEGER: {
					int32_t result;
					if (!Hugeint::TryCast<int32_t>(increment, result)) {
						increment_fits = false;
					}
					break;
				}
				case LogicalTypeId::BIGINT: {
					int64_t result;
					if (!Hugeint::TryCast<int64_t>(increment, result)) {
						increment_fits = false;
					}
					break;
				}
				default:
					throw InternalException("Unsupported sequence type");
				}
				if (!increment_fits) {
					break;
				}
				info.values.push_back(std::move(val));
				info.is_constant = false;
				continue;
			} else {
				D_ASSERT(info.values.size() >= 2);
				// sequence vector - check if this value is on the trajectory
				if (val.IsNull()) {
					// not on trajectory - this value is null
					break;
				}
				int64_t start = info.values[0].GetValue<int64_t>();
				int64_t increment = info.values[1].GetValue<int64_t>() - start;
				int64_t last_value = info.values.back().GetValue<int64_t>();
				if (hugeint_t(val.GetValue<int64_t>()) == hugeint_t(last_value) + hugeint_t(increment)) {
					// value still fits in the sequence
					info.values.push_back(std::move(val));
					continue;
				}
				// value no longer fits into the sequence - break
				break;
			}
		}
		if (info.values.size() > max_length) {
			max_length = info.values.size();
		}
		infos.push_back(std::move(info));
	}
	// go over each of the columns again and construct either (1) a dictionary vector, or (2) a constant/sequence vector
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		auto &info = infos[c];
		if (info.values.size() != max_length) {
			// dictionary vector
			SelectionVector sel(max_length);
			for (idx_t k = 0; k < max_length; k++) {
				sel.set_index(k, state.const_idx + k);
			}
			chunk.data[c].Slice(input.data[c], sel, max_length);
		} else if (info.is_constant) {
			// constant vector
			chunk.data[c].Reference(info.values[0], count_t(max_length));
		} else {
			// sequence vector
			int64_t start = info.values[0].GetValue<int64_t>();
			int64_t increment = info.values[1].GetValue<int64_t>() - start;
			chunk.data[c].Sequence(start, increment, max_length);
		}
	}
	state.const_idx += max_length;
	if (state.const_idx >= input.size()) {
		state.const_idx = 0;
		return OperatorResultType::NEED_MORE_INPUT;
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorResultType VerifyEmitNestedShuffleVector(const DataChunk &input, DataChunk &chunk, OperatorState &state) {
	input.Copy(chunk);
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		Vector::DebugShuffleNestedVector(chunk.data[c]);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

unique_ptr<OperatorState> PhysicalVerifyVector::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<VerifyVectorState>();
}

OperatorResultType PhysicalVerifyVector::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                 GlobalOperatorState &gstate, OperatorState &state) const {
	switch (verification) {
	case DebugVectorVerification::DICTIONARY_OPERATOR:
		return VerifyEmitDictionaryVectors(input, chunk, state);
	case DebugVectorVerification::CONSTANT_OPERATOR:
		return VerifyEmitConstantVectors(input, chunk, state);
	case DebugVectorVerification::SEQUENCE_OPERATOR:
		return VerifyEmitSequenceVector(input, chunk, state);
	case DebugVectorVerification::NESTED_SHUFFLE:
		return VerifyEmitNestedShuffleVector(input, chunk, state);
	default:
		throw InternalException("PhysicalVerifyVector created but no verification code present");
	}
}

} // namespace duckdb
