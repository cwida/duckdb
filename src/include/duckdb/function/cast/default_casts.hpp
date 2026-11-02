//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/cast/default_casts.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

class CastFunctionSet;
class BaseStatistics;
struct FunctionLocalState;
struct BoundCastInfo;

//! Extra data that can be attached to a bind function of a cast, and is available during binding
struct BindCastInfo {
	DUCKDB_API virtual ~BindCastInfo();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Extra data that can be returned by the bind of a cast, and is available during execution of a cast
struct BoundCastData {
	DUCKDB_API virtual ~BoundCastData();

	DUCKDB_API virtual unique_ptr<BoundCastData> Copy() const = 0;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct CastParameters {
	CastParameters() {
	}
	CastParameters(bool strict, string *error_message) : CastParameters(nullptr, strict, error_message, nullptr) {
	}
	CastParameters(optional_ptr<BoundCastData> cast_data, bool strict, string *error_message,
	               optional_ptr<FunctionLocalState> local_state, bool nullify_parent_p = false)
	    : cast_data(cast_data), strict(strict), error_message(error_message), local_state(local_state),
	      nullify_parent(nullify_parent_p) {
	}
	CastParameters(CastParameters &parent, optional_ptr<BoundCastData> cast_data,
	               optional_ptr<FunctionLocalState> local_state)
	    : cast_data(cast_data), strict(parent.strict), error_message(parent.error_message), local_state(local_state),
	      query_location(parent.query_location) {
	}

	//! The bound cast data (if any)
	optional_ptr<BoundCastData> cast_data;
	//! whether or not to enable strict casting
	bool strict = false;
	// out: error message in case cast has failed
	string *error_message = nullptr;
	//! Source expression
	optional_ptr<const Expression> cast_source;
	//! Target expression
	optional_ptr<const Expression> cast_target;
	//! Local state
	optional_ptr<FunctionLocalState> local_state;
	//! Query location (if any)
	QueryLocation query_location;
	//! In the case of a nested type, when facing a cast error, if we nullify the parent
	bool nullify_parent = false;
};

struct CastLocalStateParameters {
	CastLocalStateParameters(optional_ptr<ClientContext> context_p, optional_ptr<BoundCastData> cast_data_p)
	    : context(context_p), cast_data(cast_data_p) {
	}
	CastLocalStateParameters(ClientContext &context_p, optional_ptr<BoundCastData> cast_data_p)
	    : context(&context_p), cast_data(cast_data_p) {
	}
	CastLocalStateParameters(CastLocalStateParameters &parent, optional_ptr<BoundCastData> cast_data_p)
	    : context(parent.context), cast_data(cast_data_p) {
	}

	optional_ptr<ClientContext> context;
	//! The bound cast data (if any)
	optional_ptr<BoundCastData> cast_data;
};

typedef bool (*cast_function_t)(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
typedef unique_ptr<FunctionLocalState> (*init_cast_local_state_t)(CastLocalStateParameters &parameters);

struct CastStatisticsInput {
	DUCKDB_API CastStatisticsInput(BoundCastInfo &bound_cast_p, const LogicalType &source_type_p,
	                               const LogicalType &target_type_p, const BaseStatistics &child_stats_p,
	                               optional_ptr<ClientContext> context_p = nullptr);

	//! Replace the execution function while preserving the callback for later propagation passes
	DUCKDB_API void SetFunction(cast_function_t new_function);

	const LogicalType &source_type;
	const LogicalType &target_type;
	const BaseStatistics &child_stats;
	optional_ptr<ClientContext> context;

private:
	BoundCastInfo &bound_cast;
};

typedef unique_ptr<BaseStatistics> (*cast_statistics_t)(CastStatisticsInput &input);

struct BoundCastInfo {
	friend struct CastStatisticsInput;

	DUCKDB_API
	BoundCastInfo( // NOLINT: allow explicit cast from cast_function_t
	    cast_function_t function, unique_ptr<BoundCastData> cast_data = nullptr,
	    init_cast_local_state_t init_local_state = nullptr);

	bool Cast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) const {
		auto all_ok = function(source, result, count, parameters);
		FlatVector::SetSize(result, count);
		return all_ok;
	}

public:
	BoundCastInfo Copy() const;
	optional_ptr<BoundCastData> GetCastData() const {
		return cast_data;
	}
	//! The stability of the cast and of the casts it applies to child values
	FunctionStability GetStability() const {
		return stability;
	}
	void SetStability(FunctionStability stability_p) {
		stability = stability_p;
	}
	bool HasInitLocalState() const {
		return init_local_state;
	}
	unique_ptr<FunctionLocalState> InitLocalState(CastLocalStateParameters &parameters) const {
		return init_local_state(parameters);
	}
	bool HasFunction() const {
		return function;
	}
	bool IsNopCast() const;
	//! Whether this is the fallback cast used when no cast exists between the types
	//! It throws for any non-NULL value, regardless of the types involved
	bool IsNullCast() const;
	bool Equals(const BoundCastInfo &other) const {
		return function == other.function && statistics == other.statistics;
	}
	bool HasStatisticsCallback() const {
		return statistics;
	}
	void SetStatisticsCallback(cast_statistics_t new_statistics) {
		statistics = new_statistics;
	}
	DUCKDB_API unique_ptr<BaseStatistics> PropagateStatistics(const LogicalType &source_type,
	                                                          const LogicalType &target_type,
	                                                          const BaseStatistics &child_stats,
	                                                          optional_ptr<ClientContext> context = nullptr);
	//! Replace the execution function and clear the statistics callback
	void SetFunction(cast_function_t new_function) {
		function = new_function;
		statistics = nullptr;
	}

private:
	cast_function_t function;
	init_cast_local_state_t init_local_state;
	unique_ptr<BoundCastData> cast_data;
	cast_statistics_t statistics = nullptr;
	FunctionStability stability = FunctionStability::CONSISTENT;
};

struct BindCastInput {
	DUCKDB_API BindCastInput(CastFunctionSet &function_set, optional_ptr<BindCastInfo> info,
	                         optional_ptr<ClientContext> context);

	CastFunctionSet &function_set;
	optional_ptr<BindCastInfo> info;
	optional_ptr<ClientContext> context;
	QueryLocation query_location;
	//! The stability of the bound cast, lowered by a bind that reads settings such as TimeZone and by child casts
	FunctionStability stability = FunctionStability::CONSISTENT;

public:
	DUCKDB_API BoundCastInfo GetCastFunction(const LogicalType &source, const LogicalType &target);
};

struct DefaultCasts {
	DUCKDB_API static BoundCastInfo GetDefaultCastFunction(BindCastInput &input, const LogicalType &source,
	                                                       const LogicalType &target);

	DUCKDB_API static bool NopCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	DUCKDB_API static bool TryVectorNullCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	DUCKDB_API static bool ReinterpretCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);

private:
	static BoundCastInfo BlobCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo BitCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo DateCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo DecimalCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo EnumCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo IntervalCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo ListCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo ArrayCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo NumericCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo MapCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo PointerCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo StringCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo StructCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo TupleCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo TimeCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo TimeNsCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo TimeTzCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo TimestampCastSwitch(BindCastInput &input, const LogicalType &source,
	                                         const LogicalType &target);
	static BoundCastInfo TimestampTzCastSwitch(BindCastInput &input, const LogicalType &source,
	                                           const LogicalType &target);
	static BoundCastInfo TimestampTzNsCastSwitch(BindCastInput &input, const LogicalType &source,
	                                             const LogicalType &target);
	static BoundCastInfo TimestampNsCastSwitch(BindCastInput &input, const LogicalType &source,
	                                           const LogicalType &target);
	static BoundCastInfo TimestampMsCastSwitch(BindCastInput &input, const LogicalType &source,
	                                           const LogicalType &target);
	static BoundCastInfo TimestampSecCastSwitch(BindCastInput &input, const LogicalType &source,
	                                            const LogicalType &target);
	static BoundCastInfo UnionCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo VariantCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo UUIDCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo GeoCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo BlobToGeoCast(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo TypeCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo BignumCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target);
	static BoundCastInfo ImplicitToUnionCast(BindCastInput &input, const LogicalType &source,
	                                         const LogicalType &target);
	static BoundCastInfo ImplicitToVariantCast(BindCastInput &input, const LogicalType &source,
	                                           const LogicalType &target);
};

} // namespace duckdb
