//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_functions.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

// LCOV_EXCL_START
unique_ptr<FunctionData> TableFilterFunctions::Bind(BindScalarFunctionInput &input) {
	throw BinderException("Table filter functions are for internal use only!");
}

bool TableFilterFunctions::IsTableFilterFunction(const Identifier &name) {
	static const char *const TABLE_FILTER_FUNCTIONS[] = {BloomFilterScalarFun::NAME, DynamicFilterScalarFun::NAME,
	                                                     OptionalFilterScalarFun::NAME, PrefixRangeScalarFun::NAME,
	                                                     SelectivityOptionalFilterScalarFun::NAME};
	for (auto function_name : TABLE_FILTER_FUNCTIONS) {
		if (name == function_name) {
			return true;
		}
	}
	return false;
}
// LCOV_EXCL_STOP

void GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType type, float &selectivity_threshold,
                                   idx_t &n_vectors_to_check) {
	static constexpr float MIN_MAX_THRESHOLD = 0.9f;
	static constexpr float BF_THRESHOLD = 0.5f;
	static constexpr float PRF_THRESHOLD = 0.5f;

	static constexpr idx_t MIN_MAX_CHECK_N = 6;
	static constexpr idx_t BF_CHECK_N = 6;
	static constexpr idx_t PRF_CHECK_N = 6;

	switch (type) {
	case SelectivityOptionalFilterType::MIN_MAX:
		selectivity_threshold = MIN_MAX_THRESHOLD;
		n_vectors_to_check = MIN_MAX_CHECK_N;
		return;
	case SelectivityOptionalFilterType::BF:
		selectivity_threshold = BF_THRESHOLD;
		n_vectors_to_check = BF_CHECK_N;
		return;
	case SelectivityOptionalFilterType::PRF:
		selectivity_threshold = PRF_THRESHOLD;
		n_vectors_to_check = PRF_CHECK_N;
		return;
	default:
		throw NotImplementedException("GetThresholdAndVectorsToCheck");
	}
}

// The table filter functions are registered for (de)serialization only - TableFilterFunctions::Bind rejects
// ordinary binding - so they are constructed and specialized for the column type here rather than resolved
// through the catalog, and carry no catalog or schema name.
static unique_ptr<Expression> CreateSingleArgumentFunctionExpression(const ScalarFunction &function,
                                                                     const LogicalType &target_type,
                                                                     unique_ptr<FunctionData> bind_data) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(make_uniq<BoundReferenceExpression>(target_type, storage_t(0)));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments),
	                                          std::move(bind_data));
}

unique_ptr<Expression> CreateOptionalFilterExpression(unique_ptr<Expression> child_expr,
                                                      const LogicalType &target_type) {
	auto function = OptionalFilterScalarFun::GetFunction(target_type);
	auto bind_data = make_uniq<OptionalFilterFunctionData>(std::move(child_expr));
	return CreateSingleArgumentFunctionExpression(function, target_type, std::move(bind_data));
}

unique_ptr<Expression> CreateSelectivityOptionalFilterExpression(unique_ptr<Expression> child_expr,
                                                                 const LogicalType &target_type,
                                                                 float selectivity_threshold,
                                                                 idx_t n_vectors_to_check) {
	auto function = SelectivityOptionalFilterScalarFun::GetFunction(target_type);
	auto bind_data = make_uniq<SelectivityOptionalFilterFunctionData>(std::move(child_expr), selectivity_threshold,
	                                                                  n_vectors_to_check);
	return CreateSingleArgumentFunctionExpression(function, target_type, std::move(bind_data));
}

unique_ptr<Expression> CreateDynamicFilterExpression(shared_ptr<DynamicFilterData> filter_data,
                                                     const LogicalType &target_type) {
	return CreateDynamicFilterExpression(std::move(filter_data), target_type, nullptr);
}

unique_ptr<Expression> CreateDynamicFilterExpression(shared_ptr<DynamicFilterData> filter_data,
                                                     const LogicalType &target_type, unique_ptr<Expression> input) {
	auto function = DynamicFilterScalarFun::GetFunction(target_type);
	auto bind_data = make_uniq<DynamicFilterFunctionData>(std::move(filter_data));
	if (!input) {
		input = make_uniq<BoundReferenceExpression>(target_type, storage_t(0));
	} else {
		D_ASSERT(input->GetReturnType() == target_type);
	}
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments),
	                                          std::move(bind_data));
}

void TableFilterFunctionSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                  const BoundScalarFunction &function) {
	// Runtime state cannot be serialized - write nothing
}

unique_ptr<FunctionData> TableFilterFunctionDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	auto key_type = function.GetArguments().empty() ? LogicalType::ANY : function.GetArguments()[0];
	if (function.GetName() == BloomFilterScalarFun::NAME) {
		return make_uniq<BloomFilterFunctionData>(shared_ptr<const BloomFilter>(), false, string(), key_type, 0.0f,
		                                          idx_t(0));
	}
	if (function.GetName() == PrefixRangeScalarFun::NAME) {
		return make_uniq<PrefixRangeFunctionData>(nullptr, true, string(), key_type, 0.0f, idx_t(0));
	}
	if (function.GetName() == DynamicFilterScalarFun::NAME) {
		return make_uniq<DynamicFilterFunctionData>(nullptr);
	}
	throw InternalException("Unsupported table filter function %s during deserialization", function.GetName());
}

} // namespace duckdb
