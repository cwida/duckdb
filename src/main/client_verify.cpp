#include "duckdb/common/error_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/enums/debug_statement_verification.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"

namespace duckdb {

struct PreparedStatementVerification {
	PreparedStatementVerification() {
	}

	void ConvertConstants(QueryNode &node);
	void ConvertConstants(unique_ptr<ParsedExpression> &child);

	identifier_map_t<unique_ptr<ParsedExpression>> values;
};

void PreparedStatementVerification::ConvertConstants(QueryNode &node) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { ConvertConstants(child); });
}

void PreparedStatementVerification::ConvertConstants(unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		// constant: extract the constant value
		auto alias = expr->GetAlias();
		expr->ClearAlias();
		// check if the value already exists
		idx_t index = values.size();
		auto identifier = Identifier(std::to_string(index + 1));
		const auto predicate = [&](const std::pair<const Identifier, unique_ptr<ParsedExpression>> &pair) {
			return pair.second->Equals(*expr.get());
		};
		auto result = std::find_if(values.begin(), values.end(), predicate);
		if (result == values.end()) {
			// If it doesn't exist yet, add it
			values[identifier] = std::move(expr);
		} else {
			identifier = result->first;
		}

		// replace it with an expression
		auto parameter = make_uniq<ParameterExpression>();
		parameter->IdentifierMutable() = identifier;
		parameter->SetAlias(alias);
		expr = std::move(parameter);
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery = expr->Cast<SubqueryExpression>().Subquery();
		ConvertConstants(*subquery->node);
	}
	ParsedExpressionIterator::EnumerateChildren(*expr,
	                                            [&](unique_ptr<ParsedExpression> &child) { ConvertConstants(child); });
}

//! Swap in a statement produced by verification, carrying over the source text of the statement the user
//! issued - the rewrite is an internal detail, so errors and logging must keep reporting the original query
static void ReplaceStatement(unique_ptr<SQLStatement> &statement, unique_ptr<SQLStatement> replacement) {
	replacement->query = statement->query;
	statement = std::move(replacement);
}

void ClientContext::StatementVerification(ClientContextLock &lock, unique_ptr<SQLStatement> &statement,
                                          QueryParameters query_parameters, bool allow_nested_statements) {
	auto verification = Settings::Get<DebugVerifyStatementSetting>(*this);
	if (verification == DebugStatementVerification::COPY_STATEMENT) {
		if (statement->type == StatementType::LOGICAL_PLAN_STATEMENT) {
			// COPY verification not supported for plan statements
			return;
		}
		ReplaceStatement(statement, statement->Copy());
	} else if (verification == DebugStatementVerification::REPARSE_STATEMENT) {
		if (statement->type == StatementType::RELATION_STATEMENT) {
			// reparsing not supported for relation statements
			return;
		}
		// ToString() writes identifiers as they were folded when the statement was first parsed, so folding
		// them a second time would corrupt any identifier that was quoted in the original statement
		Parser parser(*this, IdentifierCaseMode::PRESERVE_CASE);
		ErrorData error;
		parser.ParseQuery(statement->ToString());
		// FIXME: these properties don't round-trip in ToString(), so we overwrite them manually
		if (statement->type == StatementType::UPDATE_STATEMENT) {
			// re-apply `prioritize_table_when_binding` (which is normally set during transform)
			parser.statements[0]->Cast<UpdateStatement>().node->prioritize_table_when_binding =
			    statement->Cast<UpdateStatement>().node->prioritize_table_when_binding;
		} else if (statement->type == StatementType::TRANSACTION_STATEMENT) {
			// re-apply invalidation policy
			auto &reparsed_transaction_stmt = parser.statements[0]->Cast<TransactionStatement>();
			auto &previous_transaction_stmt = statement->Cast<TransactionStatement>();
			reparsed_transaction_stmt.info->invalidation_policy = previous_transaction_stmt.info->invalidation_policy;
			// re-apply auto rollback
			reparsed_transaction_stmt.info->auto_rollback = statement->Cast<TransactionStatement>().info->auto_rollback;
		}
		ReplaceStatement(statement, std::move(parser.statements[0]));
	} else if (verification == DebugStatementVerification::SERIALIZE_STATEMENT) {
		switch (statement->type) {
		case StatementType::SELECT_STATEMENT:
		case StatementType::INSERT_STATEMENT:
		case StatementType::DELETE_STATEMENT:
		case StatementType::UPDATE_STATEMENT:
			break;
		default:
			// unsupported statement for serialization
			return;
		}
		Allocator allocator;
		MemoryStream stream(allocator);
		SerializationOptions options;
		options.storage_compatibility = StorageCompatibility::FromString("latest");
		optional_ptr<SelectStatement> to_serialize_stmt;
		optional_ptr<QueryNode> to_serialize_node;
		switch (statement->type) {
		case StatementType::SELECT_STATEMENT:
			to_serialize_stmt = statement->Cast<SelectStatement>();
			break;
		case StatementType::INSERT_STATEMENT:
			to_serialize_node = (QueryNode &)*statement->Cast<InsertStatement>().node;
			break;
		case StatementType::DELETE_STATEMENT:
			to_serialize_node = (QueryNode &)*statement->Cast<DeleteStatement>().node;
			break;
		case StatementType::UPDATE_STATEMENT:
			to_serialize_node = (QueryNode &)*statement->Cast<UpdateStatement>().node;
			break;
		default:
			throw InternalException("Unsupported type for serialization verification");
		}
		// do the round-trip
		unique_ptr<SelectStatement> deserialized_stmt;
		unique_ptr<QueryNode> deserialized_node;
		if (to_serialize_stmt) {
			BinarySerializer::Serialize(*to_serialize_stmt, stream, options);
			stream.Rewind();
			deserialized_stmt = BinaryDeserializer::Deserialize<SelectStatement>(stream);
		} else {
			BinarySerializer::Serialize(*to_serialize_node, stream, options);
			stream.Rewind();
			deserialized_node = BinaryDeserializer::Deserialize<QueryNode>(stream);
		}

		switch (statement->type) {
		case StatementType::SELECT_STATEMENT:
			ReplaceStatement(statement, std::move(deserialized_stmt));
			break;
		case StatementType::INSERT_STATEMENT: {
			auto result = make_uniq<InsertStatement>();
			result->node = unique_ptr_cast<QueryNode, InsertQueryNode>(std::move(deserialized_node));
			ReplaceStatement(statement, std::move(result));
			break;
		}
		case StatementType::DELETE_STATEMENT: {
			auto result = make_uniq<DeleteStatement>();
			result->node = unique_ptr_cast<QueryNode, DeleteQueryNode>(std::move(deserialized_node));
			ReplaceStatement(statement, std::move(result));
			break;
		}
		case StatementType::UPDATE_STATEMENT: {
			auto result = make_uniq<UpdateStatement>();
			result->node = unique_ptr_cast<QueryNode, UpdateQueryNode>(std::move(deserialized_node));
			ReplaceStatement(statement, std::move(result));
			break;
		}
		default:
			throw InternalException("Unsupported type for serialization verification");
		}
	} else if (verification == DebugStatementVerification::PREPARED_STATEMENT) {
		if (!allow_nested_statements || statement->type != StatementType::SELECT_STATEMENT) {
			// only supported for SELECT (for now?)
			return;
		}
		if (!statement->named_param_map.empty()) {
			// not supported for statements that already take parameters
			return;
		}
		if (query_parameters.statement_args && !query_parameters.statement_args->empty()) {
			// not supported for statements that already have parameters
			return;
		}
		auto &select_stmt = statement->Cast<SelectStatement>();
		auto node = select_stmt.node->Copy();

		// convert constants in the query node to parameters
		// i.e. turn SELECT 'hello', 42 into SELECT ?, ?
		PreparedStatementVerification prep_verifier;
		prep_verifier.ConvertConstants(*node);

		// create the prepared select
		// i.e. PREPARE p AS SELECT ?, ?
		auto prepare_base = make_uniq<SelectStatement>();
		prepare_base->node = std::move(node);
		for (auto &kv : prep_verifier.values) {
			prepare_base->named_param_map[kv.first] = 0;
		}

		// create the PREPARE and EXECUTE statements
		string name = "__duckdb_verification_prepared_statement_" + UUID::ToString(UUID::GenerateRandomUUID());
		auto prepare = make_uniq<PrepareStatement>();
		prepare->name = Identifier(name);
		prepare->statement = std::move(prepare_base);

		// execute the PREPARE
		ErrorData error;
		try {
			auto prepare_result = RunStatementInternal(lock, std::move(prepare), query_parameters);
			if (prepare_result->HasError()) {
				error = prepare_result->GetErrorObject();
			}
		} catch (std::exception &ex) {
			error = ErrorData(ex);
		}
		if (error.HasError()) {
			// FIXME: this is extremely lenient...
			if (error.Type() == ExceptionType::INTERNAL) {
				error.Throw("Failed prepare during verify: ");
			}
			return;
		}

		// create and return the EXECUTE statement
		// i.e. EXECUTE p('hello', 42)
		auto execute = make_uniq<ExecuteStatement>();
		execute->name = Identifier(name);
		execute->named_values = std::move(prep_verifier.values);

		ReplaceStatement(statement, std::move(execute));
	} else if (verification == DebugStatementVerification::EXPLAIN_STATEMENT ||
	           verification == DebugStatementVerification::EXPLAIN_SQL ||
	           verification == DebugStatementVerification::EXPLAIN_SQL_STRICT) {
		const bool export_sql = verification != DebugStatementVerification::EXPLAIN_STATEMENT;
		if (!allow_nested_statements || (export_sql && statement->type != StatementType::SELECT_STATEMENT)) {
			return;
		}
		if (statement->type == StatementType::EXPLAIN_STATEMENT) {
			// don't explain explain...
			return;
		}
		if (!statement->named_param_map.empty()) {
			// not supported for statements that already take parameters
			return;
		}
		if (query_parameters.statement_args && !query_parameters.statement_args->empty()) {
			// not supported for statements that already have parameters
			return;
		}
		auto explain_and_replace = [&]() {
			// deliberately left without source text: the EXPLAIN runs as a nested statement, and giving it a
			// query would let it consume the error location that belongs to the statement we are verifying
			auto explain_stmt = make_uniq<ExplainStatement>(
			    statement->Copy(), export_sql ? ExplainType::EXPLAIN_SQL : ExplainType::EXPLAIN_STANDARD);
			explain_stmt->allow_unsupported_sql = verification == DebugStatementVerification::EXPLAIN_SQL;
			// Disable the profiler during the verification EXPLAIN to prevent it from consuming the profiler context
			// (which would lose parser timing captured before StatementVerification was called) and from
			// overwriting the profiling output file with the EXPLAIN's profiling data.
			auto &client_config = ClientConfig::GetConfig(*this);
			bool saved_profiler = client_config.enable_profiler;
			ScopedConfigSetting suppress_profiling(
			    client_config, [](ClientConfig &config) { config.enable_profiler = false; },
			    [saved_profiler](ClientConfig &config) { config.enable_profiler = saved_profiler; });
			auto explain_result = RunStatementInternal(lock, std::move(explain_stmt), query_parameters, false);
			explain_result->ThrowIfError();
			if (!export_sql) {
				return;
			}
			auto chunk = explain_result->Fetch();
			if (!chunk || chunk->size() == 0) {
				D_ASSERT(verification == DebugStatementVerification::EXPLAIN_SQL);
				return;
			}
			D_ASSERT(chunk && chunk->size() == 1 && chunk->ColumnCount() == 2);
			auto sql = chunk->GetValue(1, 0).GetValue<string>();
			auto parser_options = GetParserOptions();
			parser_options.identifier_case_mode = IdentifierCaseMode::PRESERVE_CASE;
			Parser parser(parser_options);
			parser.ParseQuery(sql);
			if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT ||
			    !parser.statements[0]->named_param_map.empty()) {
				throw InternalException("SQL export verification did not produce one parameter-free SELECT");
			}
			ReplaceStatement(statement, std::move(parser.statements[0]));
		};
		// The generated SQL encodes what the optimizer derived under the planning snapshot (statistics, exact
		// cardinalities), so it must execute in the transaction the EXPLAIN planned it in
		const bool shared_transaction = export_sql && transaction.IsAutoCommit();
		if (shared_transaction) {
			transaction.SetAutoCommit(false);
		}
		try {
			explain_and_replace();
		} catch (...) {
			if (shared_transaction) {
				if (transaction.HasActiveTransaction()) {
					transaction.Rollback(nullptr);
				}
				transaction.SetAutoCommit(true);
			}
			throw;
		}
		if (shared_transaction) {
			// the transaction stays open and the verified statement commits it
			transaction.SetAutoCommit(true);
		}
	}
}

} // namespace duckdb
