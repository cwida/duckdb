#include "duckdb/common/vector/array_vector.hpp"
#include "core_functions/scalar/array_functions.hpp"
#include "core_functions/array_kernels.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"

namespace duckdb {

static void ArrayGenericBinaryResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	const auto &lhs_type = arguments[0]->GetReturnType();
	const auto &rhs_type = arguments[1]->GetReturnType();

	if (lhs_type.IsUnknown() && rhs_type.IsUnknown()) {
		bound_function.GetArguments()[0] = rhs_type;
		bound_function.GetArguments()[1] = lhs_type;
		bound_function.SetReturnType(LogicalType::UNKNOWN);
		return;
	}

	bound_function.GetArguments()[0] = lhs_type.IsUnknown() ? rhs_type : lhs_type;
	bound_function.GetArguments()[1] = rhs_type.IsUnknown() ? lhs_type : rhs_type;

	if (bound_function.GetArguments()[0].id() != LogicalTypeId::ARRAY ||
	    bound_function.GetArguments()[1].id() != LogicalTypeId::ARRAY) {
		throw InvalidInputException(StringUtil::Format("%s: Arguments must be arrays of FLOAT or DOUBLE",
		                                               SQLIdentifier(bound_function.GetName())));
	}

	const auto lhs_size = ArrayType::GetSize(bound_function.GetArguments()[0]);
	const auto rhs_size = ArrayType::GetSize(bound_function.GetArguments()[1]);

	if (lhs_size != rhs_size) {
		throw BinderException("%s: Array arguments must be of the same size", SQLIdentifier(bound_function.GetName()));
	}

	const auto &lhs_element_type = ArrayType::GetChildType(bound_function.GetArguments()[0]);
	const auto &rhs_element_type = ArrayType::GetChildType(bound_function.GetArguments()[1]);

	// Resolve common type
	LogicalType common_type;
	if (!LogicalType::TryGetMaxLogicalType(context, lhs_element_type, rhs_element_type, common_type)) {
		throw BinderException("%s: Cannot infer common element type (left = '%s', right = '%s')",
		                      SQLIdentifier(bound_function.GetName()), lhs_element_type.ToString(),
		                      rhs_element_type.ToString());
	}

	// Ensure it is float or double
	if (common_type.id() != LogicalTypeId::FLOAT && common_type.id() != LogicalTypeId::DOUBLE) {
		throw BinderException("%s: Arguments must be arrays of FLOAT or DOUBLE",
		                      SQLIdentifier(bound_function.GetName()));
	}

	// The important part is just that we resolve the size of the input arrays
	bound_function.GetArguments()[0] = LogicalType::ARRAY(common_type, lhs_size);
	bound_function.GetArguments()[1] = LogicalType::ARRAY(common_type, rhs_size);
}

[[noreturn]] static void ThrowNullArrayElement(const char *side, const Identifier &func_name) {
	throw InvalidInputException(
	    StringUtil::Format("%s: %s argument can not contain NULL values", SQLIdentifier(func_name), side));
}

//! Copies the elements of an array that can not be read in place into the buffer - throws if any of them are NULL
template <class TYPE>
static const TYPE *CopyArrayElements(typename VectorIterator<VectorArrayType<TYPE>>::ValueEntry array, TYPE *buffer,
                                     const char *side, const Identifier &func_name) {
	for (idx_t i = 0; i < array.GetArraySize(); i++) {
		auto entry = array.GetChildValue(i);
		if (!entry.IsValid()) {
			ThrowNullArrayElement(side, func_name);
		}
		buffer[i] = entry.GetValue();
	}
	return buffer;
}

//------------------------------------------------------------------------------
// Element-wise combine functions
//------------------------------------------------------------------------------
// Given two arrays of the same size, combine their elements into a single array
// of the same size as the input arrays.
namespace {
struct CrossProductOp {
	template <class TYPE>
	static void Operation(const TYPE *lhs_data, const TYPE *rhs_data, TYPE *res_data, idx_t size) {
		D_ASSERT(size == 3);

		auto lx = lhs_data[0];
		auto ly = lhs_data[1];
		auto lz = lhs_data[2];

		auto rx = rhs_data[0];
		auto ry = rhs_data[1];
		auto rz = rhs_data[2];

		res_data[0] = ly * rz - lz * ry;
		res_data[1] = lz * rx - lx * rz;
		res_data[2] = lx * ry - ly * rx;
	}
};
} // namespace

template <class TYPE, class OP, idx_t N>
static void ArrayFixedCombine(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &lstate = state.Cast<ExecuteFunctionState>();
	const auto &expr = lstate.expr.Cast<BoundFunctionExpression>();
	const auto &func_name = expr.Function().GetName();

	const auto count = args.size();
	auto &res_child = ArrayVector::GetChildMutable(result);

	auto res_data = FlatVector::GetDataMutable<TYPE>(res_child);
	auto lhs_arrays = args.data[0].Values<VectorArrayType<TYPE>>();
	auto rhs_arrays = args.data[1].Values<VectorArrayType<TYPE>>();

	if (lhs_arrays.CanReadAllInPlace() && rhs_arrays.CanReadAllInPlace()) {
		// nothing is NULL - read all arrays in place without checking each row
		for (idx_t i = 0; i < count; i++) {
			OP::Operation(lhs_arrays[i].GetDataUnsafe(), rhs_arrays[i].GetDataUnsafe(), res_data + i * N, N);
		}
	} else {
		TYPE lhs_buffer[N];
		TYPE rhs_buffer[N];
		for (idx_t i = 0; i < count; i++) {
			const auto lhs = lhs_arrays[i];
			const auto rhs = rhs_arrays[i];
			if (!lhs.IsValid() || !rhs.IsValid()) {
				FlatVector::SetNull(result, i, true);
				continue;
			}

			auto lhs_data = lhs.TryGetData();
			if (!lhs_data) {
				lhs_data = CopyArrayElements(lhs, lhs_buffer, "left", func_name);
			}
			auto rhs_data = rhs.TryGetData();
			if (!rhs_data) {
				rhs_data = CopyArrayElements(rhs, rhs_buffer, "right", func_name);
			}
			OP::Operation(lhs_data, rhs_data, res_data + i * N, N);
		}
	}

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//------------------------------------------------------------------------------
// Generic "fold" function
//------------------------------------------------------------------------------
// Given two arrays, combine and reduce their elements into a single scalar value.

template <class TYPE, class OP>
static void ArrayGenericFold(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &lstate = state.Cast<ExecuteFunctionState>();
	const auto &expr = lstate.expr.Cast<BoundFunctionExpression>();
	const auto &func_name = expr.Function().GetName();

	const auto count = args.size();

	auto res_data = FlatVector::GetDataMutable<TYPE>(result);

	const auto array_size = ArrayType::GetSize(args.data[0].GetType());
	D_ASSERT(array_size == ArrayType::GetSize(args.data[1].GetType()));
	auto lhs_arrays = args.data[0].Values<VectorArrayType<TYPE>>();
	auto rhs_arrays = args.data[1].Values<VectorArrayType<TYPE>>();

	if (lhs_arrays.CanReadAllInPlace() && rhs_arrays.CanReadAllInPlace()) {
		// nothing is NULL - read all arrays in place without checking each row
		for (idx_t i = 0; i < count; i++) {
			res_data[i] = OP::Operation(lhs_arrays[i].GetDataUnsafe(), rhs_arrays[i].GetDataUnsafe(), array_size);
		}
	} else {
		vector<TYPE> lhs_buffer(array_size);
		vector<TYPE> rhs_buffer(array_size);
		for (idx_t i = 0; i < count; i++) {
			const auto lhs = lhs_arrays[i];
			const auto rhs = rhs_arrays[i];
			if (!lhs.IsValid() || !rhs.IsValid()) {
				FlatVector::SetNull(result, i, true);
				continue;
			}

			auto lhs_data = lhs.TryGetData();
			if (!lhs_data) {
				lhs_data = CopyArrayElements(lhs, lhs_buffer.data(), "left", func_name);
			}
			auto rhs_data = rhs.TryGetData();
			if (!rhs_data) {
				rhs_data = CopyArrayElements(rhs, rhs_buffer.data(), "right", func_name);
			}
			res_data[i] = OP::Operation(lhs_data, rhs_data, array_size);
		}
	}

	if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static auto ArrayGenericFoldStats(ClientContext &context, FunctionStatisticsInput &input)
    -> unique_ptr<BaseStatistics> {
	// Propagate validity
	const auto &lhs_stats = input.child_stats[0];
	const auto &rhs_stats = input.child_stats[1];
	auto new_stats = NumericStats::CreateUnknown(input.expr.GetReturnType());
	new_stats.CombineValidity(lhs_stats, rhs_stats);
	if (!lhs_stats.CanHaveNoNull() || !rhs_stats.CanHaveNoNull()) {
		new_stats.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	}

	auto &lhs_child_stats = ArrayStats::GetChildStats(lhs_stats);
	auto &rhs_child_stats = ArrayStats::GetChildStats(rhs_stats);

	const auto has_any_nulls = lhs_child_stats.CanHaveNull() || rhs_child_stats.CanHaveNull();

	if (has_any_nulls) {
		// We will throw an error if the child arrays have nulls, so don't propagate any stats
		return new_stats.ToUnique();
	}

	// If the child has no nulls, we won't throw.
	input.expr.FunctionMutable().GetProperties().SetErrorMode(FunctionErrors::CANNOT_ERROR);

	return new_stats.ToUnique();
}

//------------------------------------------------------------------------------
// Function Registration
//------------------------------------------------------------------------------
// Note: In the future we could add a wrapper with a non-type template parameter to specialize for specific array sizes
// e.g. 256, 512, 1024, 2048 etc. which may allow the compiler to vectorize the loop better. Perhaps something for an
// extension.

template <class OP>
static scalar_function_t GetArrayFoldFunction(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
		return ArrayGenericFold<float, OP>;
	case LogicalTypeId::DOUBLE:
		return ArrayGenericFold<double, OP>;
	default:
		throw NotImplementedException("Array function not implemented for type %s", type.ToString());
	}
}

template <class OP>
static void AddArrayFoldFunction(ScalarFunctionSet &set, const LogicalType &type) {
	ScalarFunction func({}, type, GetArrayFoldFunction<OP>(type), nullptr, ArrayGenericFoldStats);
	func.SetResolveTypesCallback(ArrayGenericBinaryResolveTypes);
	auto array = LogicalType::ARRAY(type, optional_idx());

	func.SetFallible();
	func.GetSignature().AddParameter("array1", array).AddParameter("array2", array);

	set.AddFunction(func);
}

ScalarFunctionSet ArrayDistanceFun::GetFunctions() {
	ScalarFunctionSet set("array_distance");
	for (auto &type : LogicalType::Real()) {
		AddArrayFoldFunction<DistanceOp>(set, type);
	}
	return set;
}

ScalarFunctionSet ArrayInnerProductFun::GetFunctions() {
	ScalarFunctionSet set("array_inner_product");
	for (auto &type : LogicalType::Real()) {
		AddArrayFoldFunction<InnerProductOp>(set, type);
	}
	return set;
}

ScalarFunctionSet ArrayNegativeInnerProductFun::GetFunctions() {
	ScalarFunctionSet set("array_negative_inner_product");
	for (auto &type : LogicalType::Real()) {
		AddArrayFoldFunction<NegativeInnerProductOp>(set, type);
	}
	return set;
}

ScalarFunctionSet ArrayCosineSimilarityFun::GetFunctions() {
	ScalarFunctionSet set("array_cosine_similarity");
	for (auto &type : LogicalType::Real()) {
		AddArrayFoldFunction<CosineSimilarityOp>(set, type);
	}
	return set;
}

ScalarFunctionSet ArrayCosineDistanceFun::GetFunctions() {
	ScalarFunctionSet set("array_cosine_distance");
	for (auto &type : LogicalType::Real()) {
		AddArrayFoldFunction<CosineDistanceOp>(set, type);
	}
	return set;
}

ScalarFunctionSet ArrayCrossProductFun::GetFunctions() {
	ScalarFunctionSet set("array_cross_product");

	auto float_array = LogicalType::ARRAY(LogicalType::FLOAT, 3);
	auto double_array = LogicalType::ARRAY(LogicalType::DOUBLE, 3);

	ScalarFunction float_fun({}, float_array, ArrayFixedCombine<float, CrossProductOp, 3>);
	float_fun.GetSignature().AddParameter("array1", float_array).AddParameter("array2", float_array);
	set.AddFunction(float_fun);

	ScalarFunction double_fun({}, double_array, ArrayFixedCombine<double, CrossProductOp, 3>);
	double_fun.GetSignature().AddParameter("array1", double_array).AddParameter("array2", double_array);
	set.AddFunction(double_fun);

	set.SetFallible();

	return set;
}

} // namespace duckdb
