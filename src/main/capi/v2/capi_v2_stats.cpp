#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"

#include "duckdb/common/error_data.hpp"

namespace duckdb::capiv2 {

BaseStatistics &CV2Stats::GetMutableStats(const char *function_name) {
	if (!mutable_stats) {
		throw InvalidInputException("%s: the statistics are read-only", function_name);
	}
	return *mutable_stats;
}

//! The statistics value as a VARIANT: a VARCHAR holds JSON text, any other value is cast with the built-in casts
static Value StatsValueAsVariant(const Value &input) {
	if (input.type().id() == LogicalTypeId::VARIANT) {
		return input;
	}
	auto value = input;
	if (value.type().id() == LogicalTypeId::VARCHAR && !value.type().IsJSONType()) {
		value = value.WithType(LogicalType::JSON());
	}
	try {
		return value.DefaultCastAs(LogicalType::VARIANT());
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw InvalidInputException(
		    "duckdb_v2_stats_set_variant: the value of type %s does not convert to a VARIANT: %s",
		    input.type().ToString(), error.RawMessage());
	}
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

using duckdb::BaseStatistics;
using duckdb::InvalidInputException;
using duckdb::LogicalType;
using duckdb::StatsInfo;
using duckdb::Value;

//----------------------------------------------------------------------------------------------------------------------
// Validity
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_can_have_null(duckdb_v2_stats_handle stats, bool *out,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = Convert(stats)->GetStats().CanHaveNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_can_have_valid(duckdb_v2_stats_handle stats, bool *out,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = Convert(stats)->GetStats().CanHaveNoNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_can_have_null(duckdb_v2_stats_handle stats, bool value,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_can_have_null");
		result.Set(value ? StatsInfo::CAN_HAVE_NULL_VALUES : StatsInfo::CANNOT_HAVE_NULL_VALUES);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_can_have_valid(duckdb_v2_stats_handle stats, bool value,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_can_have_valid");
		result.Set(value ? StatsInfo::CAN_HAVE_VALID_VALUES : StatsInfo::CANNOT_HAVE_VALID_VALUES);
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Statistics as a VARIANT
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_get_variant(duckdb_v2_stats_handle stats, duckdb_v2_value_handle *variant,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(variant);
	*variant = nullptr;
	return WithErrorHandler(err, [&]() {
		auto value = Convert(stats)->GetStats().ToStruct().DefaultCastAs(LogicalType::VARIANT());
		*variant = Convert(new Value(std::move(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_variant(duckdb_v2_stats_handle stats, duckdb_v2_value_handle variant,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(variant);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_variant");
		auto value = StatsValueAsVariant(*Convert(variant));
		// set a copy so that a refused value leaves the statistics as they were
		auto updated = result.Copy();
		updated.SetFromStruct(value);
		result = std::move(updated);
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Construction
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_create(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle type,
                                       duckdb_v2_stats_handle *stats, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(stats);
	*stats = nullptr;
	return WithErrorHandler(err, [&]() {
		auto type_ref = Convert(type);
		const auto &logical_type = *type_ref;
		if (logical_type.id() == duckdb::LogicalTypeId::INVALID) {
			throw InvalidInputException("duckdb_v2_stats_create: cannot create statistics of an INVALID type");
		}
		*stats = Convert(CV2Stats::Owned(BaseStatistics::CreateUnknown(logical_type)).release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_destroy(duckdb_v2_stats_handle *stats) {
	return WithErrorHandler(nullptr, [&]() {
		if (!stats || !*stats) {
			return;
		}
		auto handle = Convert(*stats);
		if (!handle->IsOwned()) {
			throw InvalidInputException("duckdb_v2_stats_destroy: the statistics are borrowed, only statistics from "
			                            "duckdb_v2_stats_create can be destroyed");
		}
		delete handle;
		*stats = nullptr;
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Combining statistics
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_merge(duckdb_v2_stats_handle stats, duckdb_v2_stats_handle other,
                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(other);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_merge");
		auto &other_stats = Convert(other)->GetStats();
		if (result.GetType() != other_stats.GetType()) {
			throw InvalidInputException(
			    "duckdb_v2_stats_merge: cannot merge statistics of type %s into statistics of type %s",
			    other_stats.GetType().ToString(), result.GetType().ToString());
		}
		result.Merge(other_stats);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_assign(duckdb_v2_stats_handle stats, duckdb_v2_stats_handle other,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(other);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_assign");
		auto &other_stats = Convert(other)->GetStats();
		if (result.GetType() != other_stats.GetType()) {
			throw InvalidInputException(
			    "duckdb_v2_stats_assign: cannot assign statistics of type %s to statistics of type %s",
			    other_stats.GetType().ToString(), result.GetType().ToString());
		}
		result = other_stats.Copy();
	});
}
