#include "duckdb/common/vector/list_vector.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "core_functions/array_kernels.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//------------------------------------------------------------------------------
// Generic "fold" function
//------------------------------------------------------------------------------
// Given two lists of the same size, combine and reduce their elements into a
// single scalar value.

template <class TYPE, class OP>
static void ListGenericFold(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &lstate = state.Cast<ExecuteFunctionState>();
	const auto &expr = lstate.expr.Cast<BoundFunctionExpression>();
	const auto &func_name = expr.Function().GetName();

	auto &lhs_vec = args.data[0];
	auto &rhs_vec = args.data[1];

	const auto lhs_count = ListVector::GetListSize(lhs_vec);
	const auto rhs_count = ListVector::GetListSize(rhs_vec);

	auto &lhs_child = ListVector::GetChildMutable(lhs_vec);
	auto &rhs_child = ListVector::GetChildMutable(rhs_vec);

	lhs_child.Flatten();
	rhs_child.Flatten();

	D_ASSERT(lhs_child.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(rhs_child.GetVectorType() == VectorType::FLAT_VECTOR);

	if (!FlatVector::ValidityMutable(lhs_child).CheckAllValid(lhs_count)) {
		throw InvalidInputException("%s: left argument can not contain NULL values", SQLIdentifier(func_name));
	}

	if (!FlatVector::ValidityMutable(rhs_child).CheckAllValid(rhs_count)) {
		throw InvalidInputException("%s: right argument can not contain NULL values", SQLIdentifier(func_name));
	}

	auto lhs_data = FlatVector::GetData<TYPE>(lhs_child);
	auto rhs_data = FlatVector::GetData<TYPE>(rhs_child);

	BinaryExecutor::Execute<list_entry_t, list_entry_t, TYPE>(
	    lhs_vec, rhs_vec, result, [&](const list_entry_t &left, const list_entry_t &right) -> optional<TYPE> {
		    if (left.length != right.length) {
			    throw InvalidInputException(
			        "%s: list dimensions must be equal, got left length '%d' and right length '%d'",
			        SQLIdentifier(func_name), left.length, right.length);
		    }

		    if (!OP::ALLOW_EMPTY && left.length == 0) {
			    return nullopt;
		    }

		    return OP::Operation(lhs_data + left.offset, rhs_data + right.offset, left.length);
	    });
}

//-------------------------------------------------------------------------
// Function Registration
//-------------------------------------------------------------------------

template <class OP>
static void AddListFoldFunction(ScalarFunctionSet &set, const LogicalType &type) {
	const auto list = LogicalType::LIST(type);
	scalar_function_t function;
	if (type.id() == LogicalTypeId::FLOAT) {
		function = ListGenericFold<float, OP>;
	} else if (type.id() == LogicalTypeId::DOUBLE) {
		function = ListGenericFold<double, OP>;
	} else {
		throw NotImplementedException("List function not implemented for type %s", type.ToString());
	}
	ScalarFunction fun({}, type, function);
	fun.GetSignature().AddParameter("list1", list).AddParameter("list2", list);
	set.AddFunction(fun);
}

ScalarFunctionSet ListDistanceFun::GetFunctions() {
	ScalarFunctionSet set("list_distance");
	for (auto &type : LogicalType::Real()) {
		AddListFoldFunction<DistanceOp>(set, type);
	}
	set.SetFallible();
	return set;
}

ScalarFunctionSet ListInnerProductFun::GetFunctions() {
	ScalarFunctionSet set("list_inner_product");
	for (auto &type : LogicalType::Real()) {
		AddListFoldFunction<InnerProductOp>(set, type);
	}
	set.SetFallible();
	return set;
}

ScalarFunctionSet ListNegativeInnerProductFun::GetFunctions() {
	ScalarFunctionSet set("list_negative_inner_product");
	for (auto &type : LogicalType::Real()) {
		AddListFoldFunction<NegativeInnerProductOp>(set, type);
	}
	set.SetFallible();
	return set;
}

ScalarFunctionSet ListCosineSimilarityFun::GetFunctions() {
	ScalarFunctionSet set("list_cosine_similarity");
	for (auto &type : LogicalType::Real()) {
		AddListFoldFunction<CosineSimilarityOp>(set, type);
	}
	set.SetFallible();
	return set;
}

ScalarFunctionSet ListCosineDistanceFun::GetFunctions() {
	ScalarFunctionSet set("list_cosine_distance");
	for (auto &type : LogicalType::Real()) {
		AddListFoldFunction<CosineDistanceOp>(set, type);
	}
	set.SetFallible();
	return set;
}

} // namespace duckdb
