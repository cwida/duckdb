//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/quantile_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/quantile_enum.hpp"
#include "core_functions/aggregate/holistic_functions.hpp"

namespace duckdb {

//	Avoid using naked Values in inner loops...
struct QuantileValue {
	explicit QuantileValue(const Value &v) : val(v), dbl(v.GetValue<double>()) {
		const auto &type = val.type();
		switch (type.id()) {
		case LogicalTypeId::DECIMAL: {
			if (type.InternalType() == PhysicalType::VARCHAR) {
				// a wide decimal quantile has no HUGEINT representation, it is interpolated as DOUBLE
				val = Value::DOUBLE(dbl);
				break;
			}
			integral = IntegralValue::Get(v);
			scaling = Hugeint::POWERS_OF_TEN[DecimalType::GetScale(type)];
			break;
		}
		default:
			break;
		}
	}

	Value val;

	//	DOUBLE
	double dbl;

	//	DECIMAL
	hugeint_t integral;
	hugeint_t scaling;

	inline bool operator==(const QuantileValue &other) const {
		return val == other.val;
	}
};

struct QuantileBindData : public FunctionData {
	QuantileBindData();
	explicit QuantileBindData(const Value &quantile_p);
	explicit QuantileBindData(const vector<Value> &quantiles_p);
	QuantileBindData(const QuantileBindData &other);

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;

	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
	                      const BoundAggregateFunction &function);

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function);

	vector<QuantileValue> quantiles;
	vector<idx_t> order;
	bool desc;
};

} // namespace duckdb
