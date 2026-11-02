#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "core_functions/scalar/struct_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct StructKeysBindData : public FunctionData {
	const LogicalType type;
	Vector keys_vector;

	explicit StructKeysBindData(const LogicalType &type_p)
	    : type(type_p), keys_vector(LogicalType::LIST(LogicalType::VARCHAR), 2) {
		const auto &child_types = StructType::GetChildTypes(type);
		const auto count = child_types.size();

		auto list_writer = FlatVector::Writer<VectorListType<string_t>>(keys_vector, 2);
		idx_t idx = 0;
		for (auto &child_writer : list_writer.WriteList(count)) {
			child_writer.WriteValue(string_t(child_types[idx++].first.GetIdentifierName()));
		}
		list_writer.WriteNull();
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<StructKeysBindData>();
		// Compare type and flag (content is derived from them)
		return type == o.type;
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StructKeysBindData>(type);
	}
};

static void StructKeysFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input = args.data[0];
	const idx_t count = args.size();

	auto &data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<StructKeysBindData>();
	auto &keys_vector = data.keys_vector;

	// If the input is a constant, we must return a CONSTANT_VECTOR
	if (args.AllConstant()) {
		ConstantVector::Reference(result, count_t(count), keys_vector, 0, 1);
		return;
	}

	// Non-constant input: return a DICTIONARY_VECTOR over two entries (keys list and NULL) to preserve per-row NULLs
	// Build the dictionary selection: 0 for non-null input, 1 for null input
	SelectionVector sel(count);
	auto validity_entries = input.Validity();
	for (idx_t i = 0; i < count; i++) {
		sel.set_index(i, !validity_entries.IsValid(i));
	}

	result.Slice(keys_vector, sel, count);
}

static unique_ptr<FunctionData> StructKeysBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	const auto &return_type = arguments[0]->GetReturnType();
	if (return_type.id() != LogicalTypeId::STRUCT) {
		throw InvalidInputException("struct_keys() expects a STRUCT argument");
	}
	return make_uniq<StructKeysBindData>(arguments[0]->GetReturnType());
}

ScalarFunction StructKeysFun::GetFunction() {
	ScalarFunction func({}, LogicalType::LIST(LogicalType::VARCHAR), StructKeysFunction, StructKeysBind);
	func.GetSignature().AddParameter("struct", LogicalTypeId::ANY);
	return func;
}

} // namespace duckdb
