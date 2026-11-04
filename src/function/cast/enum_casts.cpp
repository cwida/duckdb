#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"

namespace duckdb {

template <class SRC_TYPE, class RES_TYPE>
static bool EnumEnumCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &enum_dictionary = EnumType::GetValuesInsertOrder(source.GetType());
	auto dictionary_data = FlatVector::GetData<string_t>(enum_dictionary);
	auto res_enum_type = result.GetType();

	VectorTryCastData vector_cast_data(result, parameters);
	UnaryExecutor::Execute<SRC_TYPE, RES_TYPE>(source, result, count, [&](SRC_TYPE value) -> optional<RES_TYPE> {
		auto key = EnumType::GetPos(res_enum_type, dictionary_data[value]);
		if (key == -1) {
			HandleCastError::AssignError(
			    CastExceptionText<string_t>(dictionary_data[value], source.GetType(), res_enum_type),
			    vector_cast_data.parameters);
			vector_cast_data.all_converted = false;
			return nullopt;
		} else {
			return UnsafeNumericCast<RES_TYPE>(key);
		}
	});
	return vector_cast_data.all_converted;
}

template <class SRC_TYPE>
static BoundCastInfo EnumEnumCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	switch (target.InternalType()) {
	case PhysicalType::UINT8:
		return EnumEnumCast<SRC_TYPE, uint8_t>;
	case PhysicalType::UINT16:
		return EnumEnumCast<SRC_TYPE, uint16_t>;
	case PhysicalType::UINT32:
		return EnumEnumCast<SRC_TYPE, uint32_t>;
	default:
		throw InternalException("ENUM can only have unsigned integers (except UINT64) as physical types");
	}
}

template <class SRC>
static bool EnumToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &enum_dictionary = EnumType::GetValuesInsertOrder(source.GetType());
	auto dictionary_data = FlatVector::GetData<string_t>(enum_dictionary);

	UnaryExecutor::Execute<SRC, string_t>(source, result, count,
	                                      [&](SRC enum_idx) { return dictionary_data[enum_idx]; });
	return true;
}

struct EnumBoundCastData : public BoundCastData {
	EnumBoundCastData(BoundCastInfo to_varchar_cast, BoundCastInfo from_varchar_cast)
	    : to_varchar_cast(std::move(to_varchar_cast)), from_varchar_cast(std::move(from_varchar_cast)) {
	}

	BoundCastInfo to_varchar_cast;
	BoundCastInfo from_varchar_cast;

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<EnumBoundCastData>(to_varchar_cast.Copy(), from_varchar_cast.Copy());
	}
};

static unique_ptr<BoundCastData> BindEnumCast(BindCastInput &input, const LogicalType &source,
                                              const LogicalType &target) {
	auto to_varchar_cast = input.GetCastFunction(source, LogicalType::VARCHAR);
	auto from_varchar_cast = input.GetCastFunction(LogicalType::VARCHAR, target);
	return make_uniq<EnumBoundCastData>(std::move(to_varchar_cast), std::move(from_varchar_cast));
}

struct EnumCastLocalState : public FunctionLocalState {
public:
	unique_ptr<FunctionLocalState> to_varchar_local;
	unique_ptr<FunctionLocalState> from_varchar_local;
};

static unique_ptr<FunctionLocalState> InitEnumCastLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<EnumBoundCastData>();
	auto result = make_uniq<EnumCastLocalState>();

	if (cast_data.from_varchar_cast.HasInitLocalState()) {
		CastLocalStateParameters from_varchar_params(parameters, cast_data.from_varchar_cast.GetCastData());
		result->from_varchar_local = cast_data.from_varchar_cast.InitLocalState(from_varchar_params);
	}
	if (cast_data.to_varchar_cast.HasInitLocalState()) {
		CastLocalStateParameters to_varchar_params(parameters, cast_data.to_varchar_cast.GetCastData());
		result->to_varchar_local = cast_data.to_varchar_cast.InitLocalState(to_varchar_params);
	}
	return std::move(result);
}

static bool EnumToAnyCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<EnumBoundCastData>();
	auto &lstate = parameters.local_state->Cast<EnumCastLocalState>();

	Vector varchar_cast(LogicalType::VARCHAR, count);

	// cast to varchar
	CastParameters to_varchar_params(parameters, cast_data.to_varchar_cast.GetCastData(), lstate.to_varchar_local);
	if (!cast_data.to_varchar_cast.Cast(source, varchar_cast, count, to_varchar_params)) {
		return false;
	}

	// cast from varchar to the target
	CastParameters from_varchar_params(parameters, cast_data.from_varchar_cast.GetCastData(),
	                                   lstate.from_varchar_local);
	return cast_data.from_varchar_cast.Cast(varchar_cast, result, count, from_varchar_params);
}

BoundCastInfo DefaultCasts::EnumCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	auto enum_physical_type = source.InternalType();

	if (target.id() == LogicalTypeId::ENUM) {
		// This means they are both ENUMs, but of different types.
		switch (enum_physical_type) {
		case PhysicalType::UINT8:
			return EnumEnumCastSwitch<uint8_t>(input, source, target);
		case PhysicalType::UINT16:
			return EnumEnumCastSwitch<uint16_t>(input, source, target);
		case PhysicalType::UINT32:
			return EnumEnumCastSwitch<uint32_t>(input, source, target);
		default:
			throw InternalException("ENUM can only have unsigned integers (except UINT64) as physical types");
		}
	}
	if (target == LogicalType::VARCHAR) {
		switch (enum_physical_type) {
		case PhysicalType::UINT8:
			return EnumToVarcharCast<uint8_t>;
		case PhysicalType::UINT16:
			return EnumToVarcharCast<uint16_t>;
		case PhysicalType::UINT32:
			return EnumToVarcharCast<uint32_t>;
		default:
			throw InternalException("ENUM can only have unsigned integers (except UINT64) as physical types");
		}
	}
	// Otherwise, fall back to ANY cast
	return BoundCastInfo(EnumToAnyCast, BindEnumCast(input, source, target), InitEnumCastLocalState);
}

} // namespace duckdb
