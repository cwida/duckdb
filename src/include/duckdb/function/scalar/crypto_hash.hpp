//===----------------------------------------------------------------------===//
//                         DuckDB
//
// crypto_hash.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/encryption_state.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {
namespace crypto_hash_scalar {

struct LocalState : public FunctionLocalState {
	LocalState(shared_ptr<EncryptionUtil> encryption_util_p, CryptoHashFunction function)
	    : encryption_util(std::move(encryption_util_p)) {
		D_ASSERT(encryption_util);
		hash_state = encryption_util->CreateHashState(function);
		D_ASSERT(hash_state);
	}

	shared_ptr<EncryptionUtil> encryption_util;
	unique_ptr<CryptoHashState> hash_state;
};

template <CryptoHashFunction FUNCTION>
unique_ptr<FunctionLocalState> InitLocalState(ExpressionState &state, const BoundFunctionExpression &, FunctionData *) {
	auto &db = state.GetContext().GetDatabase();
	auto &config = DBConfig::GetConfig(db);
	auto force_mbedtls = Settings::Get<ForceMbedtlsUnsafeSetting>(config);
	if (!force_mbedtls && config.encryption_util && config.encryption_util->SupportsHash(FUNCTION)) {
		return make_uniq<LocalState>(config.encryption_util, FUNCTION);
	}
	return make_uniq<LocalState>(db.GetMbedTLSUtil(force_mbedtls), FUNCTION);
}

struct StringData {
	StringData(CryptoHashState &hash_state, StringHeap &heap) : hash_state(hash_state), heap(heap) {
	}

	CryptoHashState &hash_state;
	StringHeap &heap;
};

struct NumberData {
	explicit NumberData(CryptoHashState &hash_state) : hash_state(hash_state) {
	}

	CryptoHashState &hash_state;
};

template <CryptoHashFunction FUNCTION>
struct StringOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &, idx_t, StringData &data) {
		auto hash = data.heap.EmptyString(CryptoHash::GetHexDigestSize(FUNCTION));
		data.hash_state.HashHex(const_data_ptr_cast(input.GetData()), input.GetSize(), hash.GetDataWriteable());
		hash.Finalize();
		return hash;
	}
};

inline LocalState &GetLocalState(ExpressionState &state) {
	return ExecuteFunctionState::GetFunctionState(state)->Cast<LocalState>();
}

} // namespace crypto_hash_scalar
} // namespace duckdb
