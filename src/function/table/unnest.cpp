#include "duckdb/function/table/range.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/execution/operator/projection/physical_unnest.hpp"

namespace duckdb {

struct UnnestBindData : public FunctionData {
	explicit UnnestBindData(LogicalType input_type_p) : input_type(std::move(input_type_p)) {
	}

	LogicalType input_type;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<UnnestBindData>(input_type);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<UnnestBindData>();
		return input_type == other.input_type;
	}
};

struct UnnestGlobalState : public GlobalTableFunctionState {
	UnnestGlobalState() {
	}

	vector<unique_ptr<Expression>> select_list;

	idx_t MaxThreads() const override {
		return GlobalTableFunctionState::MAX_THREADS;
	}
};

struct UnnestLocalState : public LocalTableFunctionState {
	UnnestLocalState() {
	}

	unique_ptr<OperatorState> operator_state;
};

static unique_ptr<FunctionData> UnnestBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.input_table_types.size() != 1 || (input.input_table_types[0].id() != LogicalTypeId::LIST &&
	                                            input.input_table_types[0].id() != LogicalTypeId::ARRAY)) {
		throw BinderException("UNNEST requires a single list or array as input");
	}
	auto &input_type = input.input_table_types[0];
	if (input_type.id() == LogicalTypeId::LIST) {
		return_types.push_back(ListType::GetChildType(input_type));
	} else {
		return_types.push_back(ArrayType::GetChildType(input_type));
	}
	names.push_back("unnest");
	return make_uniq<UnnestBindData>(input_type);
}

static unique_ptr<LocalTableFunctionState> UnnestLocalInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<UnnestGlobalState>();

	auto result = make_uniq<UnnestLocalState>();
	result->operator_state = PhysicalUnnest::GetState(context, gstate.select_list);
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> UnnestInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<UnnestBindData>();
	auto result = make_uniq<UnnestGlobalState>();

	unique_ptr<Expression> child = make_uniq<BoundReferenceExpression>(bind_data.input_type, 0U);
	if (child->GetReturnType().id() == LogicalTypeId::ARRAY) {
		child = BoundCastExpression::AddArrayCastToList(context, std::move(child));
	}

	auto bound_unnest = make_uniq<BoundUnnestExpression>(ListType::GetChildType(child->GetReturnType()));
	bound_unnest->ChildMutable() = std::move(child);
	result->select_list.push_back(std::move(bound_unnest));
	return std::move(result);
}

static OperatorResultType UnnestFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                         DataChunk &output) {
	auto &state = data_p.global_state->Cast<UnnestGlobalState>();
	auto &lstate = data_p.local_state->Cast<UnnestLocalState>();
	return PhysicalUnnest::ExecuteInternal(context, input, output, *lstate.operator_state, state.select_list, false);
}

void UnnestTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction unnest_function("unnest", FunctionSignature().AddPositionalOnly("list", LogicalType::ANY), nullptr,
	                              UnnestBind, UnnestInit, UnnestLocalInit);
	unnest_function.in_out_function = UnnestFunction;
	unnest_function.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;
	set.AddFunction(unnest_function);
}

} // namespace duckdb
