#include "duckdb/function/scalar/crypto_hash.hpp"

#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

namespace duckdb {

namespace {

void SHA256Function(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input = args.data[0];
	auto &local_state = crypto_hash_scalar::GetLocalState(state);
	auto &heap = StringVector::GetStringHeap(result);
	crypto_hash_scalar::StringData data(*local_state.hash_state, heap);

	UnaryExecutor::GenericExecute<string_t, string_t, crypto_hash_scalar::StringOperator<CryptoHashFunction::SHA256>>(
	    input, result, data);
}

} // namespace

ScalarFunctionSet SHA256Fun::GetFunctions() {
	ScalarFunctionSet set("sha256");
	ScalarFunction value_fun({}, LogicalType::VARCHAR, SHA256Function, nullptr, nullptr,
	                         crypto_hash_scalar::InitLocalState<CryptoHashFunction::SHA256>);
	value_fun.GetSignature().AddParameter("value", LogicalType::VARCHAR);
	set.AddFunction(value_fun);
	ScalarFunction blob_fun({}, LogicalType::VARCHAR, SHA256Function, nullptr, nullptr,
	                        crypto_hash_scalar::InitLocalState<CryptoHashFunction::SHA256>);
	blob_fun.GetSignature().AddParameter("blob", LogicalType::BLOB);
	set.AddFunction(blob_fun);
	return set;
}

} // namespace duckdb
