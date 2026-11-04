#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/column_data_ref.hpp"

#include <algorithm>
#include <utility>

namespace duckdb::capiv2 {
namespace {

struct StatementIteratorWrapperV2 {
	StatementIteratorWrapperV2(shared_ptr<ClientContext> context_p, string query_p, bool apply_invalidation_policy_p)
	    : context(std::move(context_p)), query(std::move(query_p)),
	      apply_invalidation_policy(apply_invalidation_policy_p) {
	}
	shared_ptr<ClientContext> context;
	//! The original query string, kept to render a parse error through ProcessError.
	string query;
	bool apply_invalidation_policy;
	//! Constructed on the first next(): the ParseIterator constructor runs UTF-8
	//! validation and can throw, and that must route through the same parse-error
	//! rendering as a per-statement syntax error.
	unique_ptr<ParseIterator> iterator;
	//! Set once iteration is spent, by clean exhaustion or by a parse error. A spent
	//! iterator yields nothing further: next() returns a NULL statement idempotently.
	bool finished = false;
};

//! Binding-index order, as statement_bind's parameter schema; positional indices may be gapped ($1, $3).
auto ParameterNamesInBindingOrder(const duckdb::SQLStatement &statement)
    -> vector<std::pair<idx_t, duckdb_v2_identifier_t>> {
	vector<std::pair<idx_t, duckdb_v2_identifier_t>> names;
	names.reserve(statement.named_param_map.size());
	for (auto &entry : statement.named_param_map) {
		names.emplace_back(entry.second, Convert(entry.first));
	}
	std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	return names;
}

//! The common table expressions of a statement a collection can be bound to
auto GetCollectionCTEMap(duckdb::SQLStatement &statement) -> CommonTableExpressionMap & {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		return statement.Cast<SelectStatement>().node->cte_map;
	case StatementType::INSERT_STATEMENT:
		return statement.Cast<InsertStatement>().node->cte_map;
	case StatementType::DELETE_STATEMENT:
		return statement.Cast<DeleteStatement>().node->cte_map;
	case StatementType::UPDATE_STATEMENT:
		return statement.Cast<UpdateStatement>().node->cte_map;
	case StatementType::MERGE_INTO_STATEMENT:
		return statement.Cast<MergeIntoStatement>().node->cte_map;
	default:
		throw InvalidInputException("A collection can only be bound to a SELECT, INSERT, UPDATE, DELETE or MERGE INTO "
		                            "statement, not to a %s statement.",
		                            StatementTypeToString(statement.type));
	}
}

} // namespace

auto Convert(duckdb_v2_statement_iterator_handle ptr) -> StatementIteratorWrapperV2 * {
	return reinterpret_cast<StatementIteratorWrapperV2 *>(ptr);
}
auto Convert(StatementIteratorWrapperV2 *ptr) -> duckdb_v2_statement_iterator_handle {
	return reinterpret_cast<duckdb_v2_statement_iterator_handle>(ptr);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_parse_sql(duckdb_v2_connection_handle conn, const char *sql, bool apply_invalidation_policy,
                                    duckdb_v2_statement_iterator_handle *out_iterator,
                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(sql);
	DUCKDB_CHECK_ARG(out_iterator);
	*out_iterator = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *connection = Convert(conn);
		// Set up a lazy iterator over the connection's parser options and
		// extensions, but parse nothing here. Each statement is parsed on demand by
		// statement_iterator_next, so a statement that registers grammar (LOAD an
		// extension) is parsed and executed before a following statement uses it.
		// Statements are raw parser output: statement-level rewrites (pragma
		// reparsing, expansion unpacking, transaction wrapping) happen in
		// statement_execute, so a statement group is never split across the API
		// boundary. A parse error is not raised here; it surfaces from the next()
		// that reaches the failing statement.
		auto wrapper = duckdb::make_uniq<StatementIteratorWrapperV2>(connection->context, std::string(sql),
		                                                             apply_invalidation_policy);
		*out_iterator = Convert(wrapper.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_statement_iterator_next(duckdb_v2_statement_iterator_handle iterator,
                                                  duckdb_v2_sql_statement_handle *out_statement,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(iterator);
	DUCKDB_CHECK_ARG(out_statement);
	*out_statement = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *wrapper = Convert(iterator);
		if (wrapper->finished) {
			// Spent by a prior exhaustion or parse error: *out_statement stays NULL.
			return;
		}
		duckdb::unique_ptr<duckdb::SQLStatement> statement;
		try {
			// Construct the ParseIterator on first use: its constructor runs UTF-8
			// validation and can throw, routed through the same rendering below.
			if (!wrapper->iterator) {
				wrapper->iterator = duckdb::make_uniq<duckdb::ParseIterator>(*wrapper->context, wrapper->query,
				                                                             wrapper->apply_invalidation_policy);
			}
			// Parse the next statement on demand. Peek returns false at exhaustion;
			// *out_statement then stays NULL, idempotently.
			if (!wrapper->iterator->Peek()) {
				wrapper->finished = true;
				return;
			}
			statement = wrapper->iterator->GetStatement();
		} catch (const std::exception &ex) {
			// A parse error surfaces here, when iteration reaches the failing
			// statement, not up front. It terminates iteration (a following next()
			// reports clean exhaustion, not the error again). Route it through the
			// engine's public ProcessError like the eager query path so the rendered
			// shape honors errors_as_json (JSON, else LINE/caret), then re-throw to
			// route back through WithErrorHandler.
			wrapper->finished = true;
			duckdb::ErrorData error(ex);
			wrapper->context->ProcessError(error, wrapper->query);
			error.Throw();
		}
		*out_statement = Convert(statement.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_sql_statement_get_type(duckdb_v2_sql_statement_handle statement,
                                                 DUCKDB_V2_STATEMENT_TYPE *out_type, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(statement);
	DUCKDB_CHECK_ARG(out_type);
	*out_type = DUCKDB_V2_STATEMENT_TYPE_INVALID;
	return WithErrorHandler(err, [&]() { *out_type = Convert(Convert(statement)->type); });
}

DUCKDB_V2_ERROR duckdb_v2_sql_statement_get_text(duckdb_v2_sql_statement_handle statement, duckdb_v2_str *out_text,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(statement);
	DUCKDB_CHECK_ARG(out_text);
	*out_text = duckdb_v2_str {nullptr, 0};
	return WithErrorHandler(err, [&]() {
		// The parse paths already store the statement's own slice in `query`; `stmt_location` covers all of it.
		auto &query = Convert(statement)->query;
		if (!query.empty()) {
			*out_text = Convert(query);
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_sql_statement_get_parameter_count(duckdb_v2_sql_statement_handle statement, idx_t *out_count,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(statement);
	DUCKDB_CHECK_ARG(out_count);
	*out_count = 0;
	return WithErrorHandler(err, [&]() { *out_count = Convert(statement)->named_param_map.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_sql_statement_get_parameter_name(duckdb_v2_sql_statement_handle statement, idx_t index,
                                                           duckdb_v2_identifier_t *out_name,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(statement);
	DUCKDB_CHECK_ARG(out_name);
	*out_name = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() {
		auto names = ParameterNamesInBindingOrder(*Convert(statement));
		if (index >= names.size()) {
			throw duckdb::Exception(duckdb::ExceptionType::OUT_OF_RANGE,
			                        duckdb::StringUtil::Format("Parameter index %llu is out of range for a statement "
			                                                   "with %llu parameters in "
			                                                   "duckdb_v2_sql_statement_get_parameter_name.",
			                                                   static_cast<uint64_t>(index),
			                                                   static_cast<uint64_t>(names.size())));
		}
		*out_name = names[index].second;
	});
}

DUCKDB_V2_ERROR duckdb_v2_statement_add_collection(duckdb_v2_sql_statement_handle statement,
                                                   const duckdb_v2_identifier_t *name,
                                                   duckdb_v2_column_data_collection_handle collection,
                                                   const duckdb_v2_identifier_t *column_names, idx_t column_count,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(statement);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(collection);
	return WithErrorHandler(err, [&]() {
		auto &stmt = *Convert(statement);
		auto &cdc = *Convert(collection);
		duckdb::Identifier alias(ConvertIdentifierName(name));
		if (alias.empty()) {
			throw duckdb::InvalidInputException("The name to bind a collection under cannot be empty.");
		}
		if (column_count > 0 && !column_names) {
			throw duckdb::InvalidInputException("column_names cannot be null when column_count is not 0.");
		}
		if (column_count > 0 && column_count != cdc.ColumnCount()) {
			throw duckdb::InvalidInputException(
			    "column_count (%llu) does not match the collection's column count (%llu).",
			    static_cast<uint64_t>(column_count), static_cast<uint64_t>(cdc.ColumnCount()));
		}
		duckdb::vector<duckdb::Identifier> expected_names;
		for (idx_t i = 0; i < column_count; i++) {
			expected_names.emplace_back(ConvertIdentifierName(column_names[i]));
			if (expected_names.back().empty()) {
				throw duckdb::InvalidInputException("Column name %llu of the collection cannot be empty.",
				                                    static_cast<uint64_t>(i));
			}
		}

		auto &cte_map = GetCollectionCTEMap(stmt);
		if (cte_map.map.find(alias) != cte_map.map.end()) {
			throw duckdb::InvalidInputException("The statement already has a common table expression named %s.",
			                                    duckdb::SQLIdentifier(alias));
		}
		// SELECT * over a borrowed reference to the collection, never materialized
		auto ref = duckdb::make_uniq<duckdb::ColumnDataRef>(cdc, std::move(expected_names));
		ref->alias = alias;
		auto select = duckdb::make_uniq<duckdb::SelectNode>();
		select->select_list.push_back(duckdb::make_uniq<duckdb::StarExpression>());
		select->from_table = std::move(ref);
		auto cte_info = duckdb::make_uniq<duckdb::CommonTableExpressionInfo>();
		cte_info->query_node = std::move(select);
		cte_info->materialized = duckdb::CTEMaterialize::CTE_MATERIALIZE_NEVER;
		cte_map.map.insert(alias, std::move(cte_info));
	});
}

DUCKDB_V2_ERROR duckdb_v2_sql_statement_destroy(duckdb_v2_sql_statement_handle *statement) {
	return WithErrorHandler(nullptr, [&]() {
		if (!statement) {
			return;
		}
		if (*statement) {
			delete Convert(*statement);
			*statement = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_statement_iterator_destroy(duckdb_v2_statement_iterator_handle *iterator) {
	return WithErrorHandler(nullptr, [&]() {
		if (!iterator) {
			return;
		}
		if (*iterator) {
			delete Convert(*iterator);
			*iterator = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_statement_bind(duckdb_v2_connection_handle conn, duckdb_v2_sql_statement_handle statement,
                                         duckdb_v2_schema_handle *out_schema, duckdb_v2_schema_handle *out_parameters,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(statement);
	DUCKDB_CHECK_ARG(out_schema);
	*out_schema = nullptr;
	return WithErrorHandler(err, [&]() {
		if (out_parameters) {
			*out_parameters = nullptr;
		}

		auto *connection = Convert(conn);
		// Borrowed, not consumed: bind a copy. BindStatement, not Prepare: bind phase only (read-only, no optimize /
		// physical plan) and allowed while a result is live (Prepare is refused). It expands the statement as
		// execution would, and throws on a bind error, routed through WithErrorHandler.
		auto signature = connection->context->BindStatement(Convert(statement)->Copy());
		if (signature.metadata_deferred) {
			throw MetadataDeferredException(
			    "the statement expands into several, so its schema is known only once execution binds the one that "
			    "produces the result; execute it instead");
		}
		// Assemble into locals, publish to the out-params only at the end once nothing
		// can throw, so a failure leaves them NULL.
		auto output = duckdb::make_uniq<CV2Schema>();
		for (duckdb::idx_t i = 0; i < signature.types.size(); i++) {
			output->fields.push_back({signature.names[i].GetIdentifierName(), signature.types[i]});
		}

		duckdb::unique_ptr<CV2Schema> params;
		if (out_parameters) {
			params = duckdb::make_uniq<CV2Schema>();
			// BindStatement returns parameters unordered; order by binding index for the
			// positional input-schema contract that pairs with statement_execute's values.
			// Positional params may be gapped ($1, $3 with no $2), so not a dense 1..N; the
			// field name carries the binding index.
			std::sort(signature.parameters.begin(), signature.parameters.end(),
			          [](const auto &a, const auto &b) { return a.index < b.index; });
			for (auto &parameter : signature.parameters) {
				params->fields.push_back({parameter.identifier.GetIdentifierName(), parameter.type});
			}
		}
		*out_schema = Convert(output.release());
		if (out_parameters) {
			*out_parameters = Convert(params.release());
		}
	});
}
