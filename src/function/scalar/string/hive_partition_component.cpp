#include "duckdb/function/scalar/string_functions.hpp"

#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

namespace duckdb {

namespace {

void HivePartitionComponentFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// type-erased, as the partition value can have any type
	auto writer = FlatVector::Writer<string_t>(result, args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		auto value = args.data[1].GetValue(row);
		// the name and the value are escaped the same way url_encode does
		auto component = HivePartitioning::Escape(args.data[0].GetValue(row).ToString()) + "=";
		component +=
		    value.IsNull() ? HivePartitioning::DEFAULT_PARTITION_NAME : HivePartitioning::EscapeValue(value.ToString());
		writer.WriteValue(string_t(component.c_str(), UnsafeNumericCast<uint32_t>(component.size())));
	}
}

} // namespace

ScalarFunction HivePartitionComponentFun::GetFunction() {
	ScalarFunction component(HivePartitionComponentFun::Name, {}, LogicalType::VARCHAR, HivePartitionComponentFunction);
	component.GetSignature().AddParameter("name", LogicalType::VARCHAR).AddParameter("value", LogicalType::ANY);
	// a NULL partition value has a directory of its own
	component.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return component;
}

} // namespace duckdb
