//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/nested_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

struct ListArgFunctor {
	static Vector &GetList(Vector &list) {
		return list;
	}
	static idx_t GetListSize(Vector &list) {
		return ListVector::GetListSize(list);
	}
	static Vector &GetEntry(Vector &list) {
		return ListVector::GetChildMutable(list);
	}
};

struct ContainsFunctor {
	static inline bool Initialize() {
		return false;
	}
	static inline bool UpdateResultEntries(idx_t child_idx) {
		return true;
	}
};

struct PositionFunctor {
	static inline int32_t Initialize() {
		return 0;
	}
	static inline int32_t UpdateResultEntries(idx_t child_idx) {
		return UnsafeNumericCast<int32_t>(child_idx + 1);
	}
};

struct MapUtil {
	static void ReinterpretMap(Vector &target, const Vector &other);
};

struct VariableReturnBindData : public FunctionData {
	LogicalType stype;

	explicit VariableReturnBindData(LogicalType stype_p) : stype(std::move(stype_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<VariableReturnBindData>(stype);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<VariableReturnBindData>();
		return stype == other.stype;
	}
	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const BoundScalarFunction &function) {
		auto &info = bind_data->Cast<VariableReturnBindData>();
		serializer.WriteProperty(100, "variable_return_type", info.stype);
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &bound_function) {
		auto stype = deserializer.ReadProperty<LogicalType>(100, "variable_return_type");
		return make_uniq<VariableReturnBindData>(std::move(stype));
	}
	//! Bind callback that stores the (resolved) return type of the function
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		return make_uniq<VariableReturnBindData>(input.GetBoundFunction().GetReturnType());
	}
};

template <class T, class MAP_TYPE = map<T, idx_t>>
struct HistogramAggState {
	MAP_TYPE *hist;
};

ScalarFunction GetKeyExtractFunction();
ScalarFunction GetIndexExtractFunction();
ScalarFunction GetExtractAtFunction();

} // namespace duckdb
