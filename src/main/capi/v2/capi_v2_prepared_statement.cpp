#include "duckdb/main/capi_v2/capi_v2_result_internal.hpp"

namespace duckdb::capiv2 {
namespace {

//! Backing struct for the opaque duckdb_v2_prepared_statement_handle.
struct PreparedStatementWrapperV2 {
	//! Declared before `prepared` so it is destroyed after it: ~PreparedStatement reaches
	//! back into the context to drop the plan it registered there. Held by shared_ptr
	//! rather than left to the engine's weak_ptr so the handle stays usable after the
	//! connection is disconnected, the same guarantee an undrained result carries.
	shared_ptr<ClientContext> context;
	unique_ptr<PreparedStatement> prepared;
};

//! The reuse predicate: the exact negation of the engine's re-bind gate
//! (PreparedStatementData::RequireRebind). A parameter whose type was not resolved at
//! prepare time, or a non-cacheable plan -- which a table scan makes it -- forces a
//! re-bind on every execution.
bool PreparedReusesPlanV2(const StatementProperties &properties) {
	return properties.bound_all_parameters && !properties.always_require_rebind;
}

} // namespace

auto Convert(duckdb_v2_prepared_statement_handle ptr) -> PreparedStatementWrapperV2 * {
	return reinterpret_cast<PreparedStatementWrapperV2 *>(ptr);
}
auto Convert(PreparedStatementWrapperV2 *ptr) -> duckdb_v2_prepared_statement_handle {
	return reinterpret_cast<duckdb_v2_prepared_statement_handle>(ptr);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_prepared_statement_create(duckdb_v2_connection_handle conn,
                                                    duckdb_v2_sql_statement_handle statement, bool require_cacheable,
                                                    duckdb_v2_prepared_statement_handle *out_prepared,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_prepared);
	*out_prepared = nullptr;
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(statement);
	return WithErrorHandler(err, [&]() {
		auto *connection = Convert(conn);
		// Borrowed, not consumed: prepare a copy so the caller keeps the original. The engine refuses a prepare while
		// another statement on the connection is running.
		auto prepared = connection->context->Prepare(Convert(statement)->Copy());
		if (prepared->HasError()) {
			// Prepare reports failure on the returned object rather than throwing; re-throw
			// the typed ErrorData so its ExceptionType routes through
			// GetErrorCodeFromExceptionType instead of collapsing into a generic code.
			prepared->GetErrorObject().Throw();
		}
		if (require_cacheable && !PreparedReusesPlanV2(prepared->GetStatementProperties())) {
			throw duckdb::InvalidInputException(
			    "prepared_statement_create(require_cacheable): this plan is re-bound on every execution (an unresolved "
			    "parameter type, a table scan, or a statement that expands into several), so executing it is no faster "
			    "than statement_execute; prepare without require_cacheable to accept that");
		}
		auto wrapper = duckdb::make_uniq<PreparedStatementWrapperV2>();
		wrapper->context = connection->context;
		wrapper->prepared = std::move(prepared);
		*out_prepared = Convert(wrapper.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_prepared_statement_execute(duckdb_v2_prepared_statement_handle prepared,
                                                     const duckdb_v2_identifier_t *parameter_names,
                                                     const duckdb_v2_value_handle *parameter_values,
                                                     idx_t parameter_count, duckdb_v2_result_handle *out_result,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_result);
	*out_result = nullptr;
	DUCKDB_CHECK_ARG(prepared);
	if (parameter_count > 0 && !parameter_values) {
		return NullArgumentError(err, __func__, "parameter_values");
	}
	return WithErrorHandler(err, [&]() {
		auto *wrapper = Convert(prepared);
		// Fold the parameter values in as constants, keyed by name when parameter_names
		// supplies one and positionally ("1".."N") otherwise. Per execution: nothing is
		// retained on the handle between calls.
		duckdb::identifier_map_t<duckdb::BoundParameterData> values;
		BuildParameterMap(parameter_names, parameter_values, parameter_count, __func__, values);
		*out_result =
		    Convert(ExecutePreparedStatementV2(wrapper->context, *wrapper->prepared, values, nullptr).release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_prepared_statement_execute_arrow(duckdb_v2_prepared_statement_handle prepared,
                                                           const duckdb_v2_identifier_t *parameter_names,
                                                           const duckdb_v2_value_handle *parameter_values,
                                                           idx_t parameter_count, idx_t batch_size,
                                                           duckdb_v2_arrow_result_handle *out_result,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_result);
	*out_result = nullptr;
	DUCKDB_CHECK_ARG(prepared);
	if (parameter_count > 0 && !parameter_values) {
		return NullArgumentError(err, __func__, "parameter_values");
	}
	return WithErrorHandler(err, [&]() {
		auto *wrapper = Convert(prepared);
		duckdb::identifier_map_t<duckdb::BoundParameterData> values;
		BuildParameterMap(parameter_names, parameter_values, parameter_count, __func__, values);
		*out_result = ConvertArrowResult(
		    ExecutePreparedStatementV2(wrapper->context, *wrapper->prepared, values, ArrowResultFormat(batch_size))
		        .release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_prepared_statement_reuses_plan(duckdb_v2_prepared_statement_handle prepared, bool *out_reuses,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(prepared);
	DUCKDB_CHECK_ARG(out_reuses);
	return WithErrorHandler(err, [&]() {
		auto *wrapper = Convert(prepared);
		*out_reuses = PreparedReusesPlanV2(wrapper->prepared->GetStatementProperties());
	});
}

DUCKDB_V2_ERROR duckdb_v2_prepared_statement_destroy(duckdb_v2_prepared_statement_handle *prepared) {
	return WithErrorHandler(nullptr, [&]() {
		if (!prepared) {
			return;
		}
		if (*prepared) {
			delete Convert(*prepared);
			*prepared = nullptr;
		}
	});
}
