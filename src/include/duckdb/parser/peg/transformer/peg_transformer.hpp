#pragma once

#include "duckdb/common/deque.hpp"
#include "duckdb/parser/peg/ast/unpivot_name_values.hpp"
#include "duckdb/parser/qualified_name_set.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/transformer/transform_result.hpp"
#include "duckdb/parser/peg/ast/add_column_entry.hpp"
#include "duckdb/parser/peg/ast/column_constraint_entry.hpp"
#include "duckdb/parser/peg/ast/analyze_target.hpp"
#include "duckdb/parser/peg/ast/column_elements.hpp"
#include "duckdb/parser/peg/ast/create_table_column_element.hpp"
#include "duckdb/parser/peg/ast/create_table_definition.hpp"
#include "duckdb/parser/peg/ast/partition_sorted_options.hpp"
#include "duckdb/parser/peg/ast/distinct_clause.hpp"
#include "duckdb/parser/peg/ast/describe_target.hpp"
#include "duckdb/parser/peg/ast/extension_repository_info.hpp"
#include "duckdb/parser/peg/ast/generated_column_definition.hpp"
#include "duckdb/parser/peg/ast/generic_copy_option.hpp"
#include "duckdb/parser/peg/ast/generic_copy_option_value.hpp"
#include "duckdb/parser/peg/ast/insert_values.hpp"
#include "duckdb/parser/peg/ast/create_pivot_entry.hpp"
#include "duckdb/parser/peg/ast/join_prefix.hpp"
#include "duckdb/parser/peg/ast/join_qualifier.hpp"
#include "duckdb/parser/peg/ast/key_actions.hpp"
#include "duckdb/parser/peg/ast/limit_percent_result.hpp"
#include "duckdb/parser/peg/ast/macro_parameter.hpp"
#include "duckdb/parser/peg/ast/on_conflict_expression_target.hpp"
#include "duckdb/parser/peg/ast/sequence_option.hpp"
#include "duckdb/parser/peg/ast/setting_info.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/peg/ast/cast_arguments.hpp"
#include "duckdb/parser/peg/ast/expression_chain.hpp"
#include "duckdb/parser/peg/ast/method_arguments.hpp"
#include "duckdb/parser/peg/ast/trim_arguments.hpp"
#include "duckdb/parser/peg/ast/trigger_event_info.hpp"
#include "duckdb/parser/peg/ast/trigger_table_referencing_info.hpp"
#include "duckdb/parser/peg/ast/window_frame.hpp"
#include "duckdb/function/macro_function.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/common/stack_checker.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_data/connect_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"
#include "duckdb/parser/parsed_data/vacuum_info.hpp"
#include "duckdb/parser/statement/copy_database_statement.hpp"
#include "duckdb/parser/statement/set_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"
#include "duckdb/common/arena_containers/arena_ptr.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"

namespace duckdb {

// Forward declare
struct QualifiedName;
struct CompiledGrammar;
struct MatcherToken;
struct GroupingExpressionMap;
class Matcher;
class TokenIterator;

enum class GroupByExpressionInfoType : uint8_t { EXPRESSION, EMPTY, CUBE, ROLLUP, GROUPING_SETS };

struct GroupByExpressionInfo {
	GroupByExpressionInfoType type = GroupByExpressionInfoType::EMPTY;
	unique_ptr<ParsedExpression> expression;
	vector<unique_ptr<ParsedExpression>> expressions;
	vector<GroupByExpressionInfo> children;
};

//===--------------------------------------------------------------------===//
// START GENERATED TRANSFORM RESULT TYPES
//===--------------------------------------------------------------------===//
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.AddColumnEntry", AddColumnEntry);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.AnalyzeTarget", AnalyzeTarget);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.BetweenInLikeOperator", BetweenInLikeOperator);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.BinaryExpressionTail", BinaryExpressionTail);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CaseCheck", CaseCheck);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CastArguments", CastArguments);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CatalogType", CatalogType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ColumnConstraintEntry", ColumnConstraintEntry);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ColumnElements", ColumnElements);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ColumnList", ColumnList);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CommonTableExpressionMap", CommonTableExpressionMap);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ComparisonExpressionTail", ComparisonExpressionTail);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ConstraintCheckMode", ConstraintCheckMode);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ConstraintColumnDefinition", ConstraintColumnDefinition);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CopyDatabaseType", CopyDatabaseType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CreateTableColumnElement", CreateTableColumnElement);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.CreateTableDefinition", CreateTableDefinition);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.DatePartSpecifier", DatePartSpecifier);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.DescribeTarget", DescribeTarget);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.DistinctClause", DistinctClause);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ExpressionType", ExpressionType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ExtensionRepositoryInfo", ExtensionRepositoryInfo);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.FunctionArgument", FunctionArgument);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.GeneratedColumnDefinition", GeneratedColumnDefinition);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.GenericCopyOption", GenericCopyOption);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.GenericCopyOptionValue", GenericCopyOptionValue);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.GroupByExpressionInfo", GroupByExpressionInfo);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.GroupByNode", GroupByNode);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.Identifier", Identifier);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.InsertColumnOrder", InsertColumnOrder);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.InsertValues", InsertValues);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.IsDistinctFromTail", IsDistinctFromTail);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.IsExpressionTail", IsExpressionTail);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.JoinPrefix", JoinPrefix);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.JoinQualifier", JoinQualifier);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.JoinType", JoinType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.KeyActions", KeyActions);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.LimitPercentResult", LimitPercentResult);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.LogicalType", LogicalType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.LogicalTypeId", LogicalTypeId);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MacroParameter", MacroParameter);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MatchRecognizeAfterMatchClause",
                                      MatchRecognizeAfterMatchClause);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MatchRecognizeClause", MatchRecognizeClause);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MatchRecognizeQuantifier", MatchRecognizeQuantifier);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MatchRecognizeRows", MatchRecognizeRows);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MatchRecognizeSubset", MatchRecognizeSubset);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MergeActionCondition", MergeActionCondition);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.MethodArguments", MethodArguments);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.OnConflictAction", OnConflictAction);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.OnConflictExpressionTarget", OnConflictExpressionTarget);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.OrderByNode", OrderByNode);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.OrderByNullType", OrderByNullType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.OrderType", OrderType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.OtherOperatorTail", OtherOperatorTail);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ParsedOperator", ParsedOperator);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.PartitionSortedOptions", PartitionSortedOptions);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.PivotColumn", PivotColumn);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.QualifiedColumnName", QualifiedColumnName);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.QualifiedName", QualifiedName);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.SampleMethod", SampleMethod);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.SecretPersistType", SecretPersistType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.SetOperationType", SetOperationType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.SetScope", SetScope);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.SettingInfo", SettingInfo);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.ShowType", ShowType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TableAlias", TableAlias);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TransactionModifierType", TransactionModifierType);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TriggerEventInfo", TriggerEventInfo);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TriggerForEach", TriggerForEach);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TriggerTableReferencingInfo",
                                      TriggerTableReferencingInfo);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TriggerTiming", TriggerTiming);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.TrimArguments", TrimArguments);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.UnpivotNameValues", UnpivotNameValues);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.VacuumOptions", VacuumOptions);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.Value", Value);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.WindowBoundaryExpression", WindowBoundaryExpression);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.WindowExcludeMode", WindowExcludeMode);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.WindowFrame", WindowFrame);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.arena_ptr<TransformResultValue>",
                                      arena_ptr<TransformResultValue>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.bool", bool);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.case_insensitive_map_t<unique_ptr<ParsedExpression>>",
                                      case_insensitive_map_t<unique_ptr<ParsedExpression>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.int64_t", int64_t);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.optional_idx", optional_idx);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<Identifier, unique_ptr<CommonTableExpressionInfo>>",
                                      pair<Identifier, unique_ptr<CommonTableExpressionInfo>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<Identifier, unique_ptr<ParsedExpression>>",
                                      pair<Identifier, unique_ptr<ParsedExpression>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<MergeActionCondition, unique_ptr<MergeIntoAction>>",
                                      pair<MergeActionCondition, unique_ptr<MergeIntoAction>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<QualifiedColumnName, string>",
                                      pair<QualifiedColumnName, string>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<SampleMethod, optional_idx>",
                                      pair<SampleMethod, optional_idx>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<string, bool>", pair<string, bool>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<string, unique_ptr<ParsedExpression>>",
                                      pair<string, unique_ptr<ParsedExpression>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.pair<string, unique_ptr<SequenceOption>>",
                                      pair<string, unique_ptr<SequenceOption>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE(
    "duckdb.transform_result.pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>",
    pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.qualified_column_map_t<string>",
                                      qualified_column_map_t<string>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.qualified_column_set_t", qualified_column_set_t);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.string", string);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<AlterInfo>", unique_ptr<AlterInfo>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<AlterTableInfo>", unique_ptr<AlterTableInfo>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<AtClause>", unique_ptr<AtClause>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<BaseTableRef>", unique_ptr<BaseTableRef>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<ColumnRefExpression>",
                                      unique_ptr<ColumnRefExpression>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<ConnectInfo>", unique_ptr<ConnectInfo>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<Constraint>", unique_ptr<Constraint>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<CreateStatement>",
                                      unique_ptr<CreateStatement>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<CreateTypeInfo>", unique_ptr<CreateTypeInfo>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<DropStatement>", unique_ptr<DropStatement>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<ExternalResourceOptions>",
                                      unique_ptr<ExternalResourceOptions>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<MacroFunction>", unique_ptr<MacroFunction>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<MergeIntoAction>",
                                      unique_ptr<MergeIntoAction>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<OnConflictInfo>", unique_ptr<OnConflictInfo>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<ParsedExpression>",
                                      unique_ptr<ParsedExpression>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<QueryNode>", unique_ptr<QueryNode>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<ResultModifier>", unique_ptr<ResultModifier>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<SQLStatement>", unique_ptr<SQLStatement>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<SampleOptions>", unique_ptr<SampleOptions>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<SelectNode>", unique_ptr<SelectNode>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<SelectStatement>",
                                      unique_ptr<SelectStatement>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<SetOperationNode>",
                                      unique_ptr<SetOperationNode>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<SetStatement>", unique_ptr<SetStatement>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<TableRef>", unique_ptr<TableRef>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<UpdateSetInfo>", unique_ptr<UpdateSetInfo>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.unique_ptr<WindowExpression>",
                                      unique_ptr<WindowExpression>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<FunctionArgument>", vector<FunctionArgument>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<GenericCopyOption>", vector<GenericCopyOption>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<Identifier>", vector<Identifier>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<IsExpressionTail>", vector<IsExpressionTail>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<LogicalType>", vector<LogicalType>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<MacroParameter>", vector<MacroParameter>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<MatchRecognizeSubset>",
                                      vector<MatchRecognizeSubset>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<OrderByNode>", vector<OrderByNode>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<PivotColumn>", vector<PivotColumn>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<PivotColumnEntry>", vector<PivotColumnEntry>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<WindowBoundaryExpression>",
                                      vector<WindowBoundaryExpression>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<bool>", vector<bool>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<pair<Identifier, unique_ptr<ParsedExpression>>>",
                                      vector<pair<Identifier, unique_ptr<ParsedExpression>>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<string>", vector<string>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<unique_ptr<ParsedExpression>>",
                                      vector<unique_ptr<ParsedExpression>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<unique_ptr<ResultModifier>>",
                                      vector<unique_ptr<ResultModifier>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<unique_ptr<SQLStatement>>",
                                      vector<unique_ptr<SQLStatement>>);
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.transform_result.vector<unique_ptr<TableRef>>",
                                      vector<unique_ptr<TableRef>>);
//===--------------------------------------------------------------------===//
// END GENERATED TRANSFORM RESULT TYPES
//===--------------------------------------------------------------------===//

class PEGTransformer;
class TransformStack;
class TransformProcess;
class GeneratedTransformProcess;

using transform_process_initialize_t = void (*)(PEGTransformer &transformer, GeneratedTransformProcess &process);
using transform_process_finalize_t = arena_ptr<TransformResultValue> (*)(PEGTransformer &transformer,
                                                                         GeneratedTransformProcess &process);

struct TransformFrameOps {
	const char *name;
	transform_process_initialize_t initialize;
	transform_process_finalize_t finalize;
};

template <typename T>
unique_ptr<TypedTransformResult<T>> TryBridgeTransformResultValue(TransformResultValue &base_result);

//! Input to start a transformer execution. The rule can be supplied explicitly for transparent parse nodes.
struct TransformInput {
	TransformInput(ParseResult &parse_result_p) // NOLINT(google-explicit-constructor): enables braced PushChild calls.
	    : parse_result(parse_result_p) {
	}
	TransformInput(const CompiledGrammarRule &rule_p, ParseResult &parse_result_p)
	    : rule(rule_p), parse_result(parse_result_p) {
	}

	optional_ptr<const CompiledGrammarRule> GetRule() const {
		// A collapsed result stands in for a rule that would have returned it unchanged; transform it as itself
		if (parse_result.collapsed) {
			return parse_result.GetRule();
		}
		return rule ? rule : parse_result.GetRule();
	}

	optional_ptr<const CompiledGrammarRule> rule;
	ParseResult &parse_result;
};

//! Essentially a std::variant<TransformInput, arena_ptr<TransformResultValue>>.
//! Produced by TransformProcess::Resume to control the next execution step.
class TransformStep {
public:
	static TransformStep Child(TransformInput input);
	static TransformStep Complete(arena_ptr<TransformResultValue> result);

	optional<TransformInput> GetChild();
	arena_ptr<TransformResultValue> TakeResult();

private:
	TransformStep(optional<TransformInput> child_p, arena_ptr<TransformResultValue> result_p)
	    : child(std::move(child_p)), result(std::move(result_p)) {
	}

private:
	optional<TransformInput> child;
	arena_ptr<TransformResultValue> result;
};

class TransformProcess {
public:
	virtual ~TransformProcess() = default;

	//! Resume transforming, optionally with the result of the previously requested child.
	virtual TransformStep Resume(arena_ptr<TransformResultValue> child_result) = 0;
};

class GeneratedTransformProcess final : public TransformProcess {
public:
	GeneratedTransformProcess(PEGTransformer &transformer, TransformInput input, const TransformFrameOps &info);

	void ReserveChildSlots(idx_t count);
	void SetChildResult(idx_t slot, arena_ptr<TransformResultValue> result);
	void PushChild(TransformInput input, idx_t slot);
	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override;

	template <class T>
	T TakeResult(idx_t slot) {
		if (slot >= child_results.size() || !child_results[slot]) {
			ThrowMissingResult(slot);
		}
		auto *result_value = TryGetTransformResult<T>(*child_results[slot]);
		if (!result_value) {
			auto bridged = TryBridgeTransformResultValue<T>(*child_results[slot]);
			if (bridged) {
				auto bridged_result = std::move(bridged->value);
				child_results[slot].reset();
				return bridged_result;
			}
			ThrowUnexpectedResultType(slot);
		}
		auto result = std::move(*result_value);
		child_results[slot].reset();
		return result;
	}

	template <class T>
	T &GetResult(idx_t slot) {
		if (slot >= child_results.size() || !child_results[slot]) {
			ThrowMissingResult(slot);
		}
		auto *result_value = TryGetTransformResult<T>(*child_results[slot]);
		if (!result_value) {
			ThrowUnexpectedResultType(slot);
		}
		return *result_value;
	}

	[[noreturn]] void ThrowMissingResult(idx_t slot) const;
	[[noreturn]] void ThrowUnexpectedResultType(idx_t slot) const;

	ParseResult &parse_result;
	const TransformFrameOps &info;
	idx_t manual_state = 0;
	arena_vector<arena_ptr<TransformResultValue>> child_results;

private:
	struct PendingChild {
		TransformInput input;
		idx_t slot;
	};

	TransformStep NextStep();

private:
	PEGTransformer &transformer;
	arena_vector<PendingChild> pending_children;
	optional_idx child_result_slot;
	bool completed = false;
};

using transform_finalize_function_t =
    std::function<arena_ptr<TransformResultValue>(PEGTransformer &transformer, ParseResult &parse_result)>;

class FinalizeTransformProcess final : public TransformProcess {
public:
	FinalizeTransformProcess(PEGTransformer &transformer, ParseResult &parse_result,
	                         transform_finalize_function_t finalize);
	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override;

private:
	PEGTransformer &transformer;
	ParseResult &parse_result;
	transform_finalize_function_t finalize;
	bool completed = false;
};

struct TransformStackFrame {
	explicit TransformStackFrame(TransformInput input);

	optional_ptr<const CompiledGrammarRule> rule;
	ParseResult &parse_result;
	arena_ptr<TransformProcess> process;
	arena_ptr<TransformResultValue> child_result;
	//! How much this frame adds to the expression depth while it is on the stack
	idx_t expression_depth_weight = 0;
	//! Rewind process storage to this point after destroying the frame.
	ArenaAllocator::Position process_arena_position;
};

class TransformStack {
public:
	explicit TransformStack(PEGTransformer &transformer);
	~TransformStack();
	arena_ptr<TransformResultValue> Execute(TransformInput input);

	template <class T>
	T Execute(TransformInput input) {
		auto base_result = Execute(input);
		auto *result_value = TryGetTransformResult<T>(*base_result);
		if (!result_value) {
			throw InternalException("Unexpected transformer result type for root rule '%s'", input.parse_result.Name());
		}
		return std::move(*result_value);
	}

#ifdef DEBUG
	string FormatStack() const;
#endif

private:
	void PushFrame(TransformInput input);
	void PopFrame();
	void InitializeFrame(TransformStackFrame &frame);
	arena_ptr<TransformResultValue> ExecuteFrame(TransformStackFrame &frame);

private:
	PEGTransformer &transformer;
	vector<TransformStackFrame> frames;
};

class PEGTransformer {
public:
	PEGTransformer(ArenaAllocator &allocator, TokenIterator &token_iterator, ParserOptions &options_p,
	               const CompiledGrammar &grammar_p)
	    : allocator(allocator), token_iterator(token_iterator), options(options_p), grammar(grammar_p),
	      process_allocator(allocator.GetUnderlyingAllocator()) {
	}

	const CompiledGrammarRule &GetRule(const string &rule_name) const;
	const CompiledGrammarRule &GetRule(PEGRuleId rule_id) const;

public:
	template <typename T>
	T Transform(ParseResult &parse_result) {
		auto base_result = TransformInternal(parse_result);

		auto *result_value = TryGetTransformResult<T>(*base_result);
		if (!result_value) {
			// allow transparent bridging between string-typed and Identifier-typed rules
			auto bridged = TryBridgeTransformResult<T>(*base_result);
			if (bridged) {
				auto bridged_result = std::move(bridged->value);
				SetResultLocation(bridged_result, parse_result.GetLocation());
				return bridged_result;
			}
			throw InternalException("Transformer for rule '" + parse_result.Name() + "' returned an unexpected type.");
		}

		auto result = std::move(*result_value);
		SetResultLocation(result, parse_result.GetLocation());
		return result;
	}

	//! Bridge between string-typed and Identifier-typed rule results (and their vector forms).
	//! The generic form performs no bridging; the specializations below convert transparently.
	template <typename T>
	static unique_ptr<TypedTransformResult<T>> TryBridgeTransformResult(TransformResultValue &base_result) {
		return TryBridgeTransformResultValue<T>(base_result);
	}

	template <typename T>
	T Transform(ListParseResult &parse_result, idx_t child_index) {
		auto &child_parse_result = parse_result.GetChild(child_index);
		return Transform<T>(child_parse_result);
	}

	template <typename T>
	void TransformOptional(ListParseResult &list_pr, idx_t child_idx, T &target) {
		auto &opt = list_pr.Child<OptionalParseResult>(child_idx);
		if (opt.HasResult()) {
			target = Transform<T>(opt.GetResult());
		}
	}

	// Make overloads return raw pointers, as ownership is handled by the ArenaAllocator.
	template <class T, typename... Args>
	T *Make(Args &&...args) {
		return allocator.Make<T>(std::forward<Args>(args)...);
	}

	//! Process storage is reclaimed after the transform frame completes.
	template <class T, typename... Args>
	arena_ptr<TransformProcess> MakeProcess(Args &&...args) {
		return arena_ptr<TransformProcess>(process_allocator.Make<T>(std::forward<Args>(args)...));
	}

	//! The result a transformer hands back, carved out of the same arena
	template <class T, typename... Args>
	arena_ptr<TransformResultValue> MakeResult(Args &&...args) {
		return arena_ptr<TransformResultValue>(allocator.Make<TypedTransformResult<T>>(std::forward<Args>(args)...));
	}

	ArenaAllocator &GetAllocator() {
		return allocator;
	}

	void Clear();
	void ClearParameters();
	static void ParamTypeCheck(PreparedParamType last_type, PreparedParamType new_type);
	void SetParam(const Identifier &name, idx_t index, PreparedParamType type);
	bool GetParam(const Identifier &name, idx_t &index, PreparedParamType type);
	void SetParamCount(idx_t new_count);
	idx_t ParamCount() const;
	unique_ptr<SQLStatement> CreatePivotStatement(unique_ptr<SQLStatement> statement);
	unique_ptr<SQLStatement> GenerateCreateEnumStmt(unique_ptr<CreatePivotEntry> entry);
	void PivotEntryCheck(const string &type);
	void ExtractCTEsRecursive(CommonTableExpressionMap &cte_map);
	bool IsWindowFrameDefault(WindowBoundary start, WindowBoundary end);
	unique_ptr<WindowExpression> GetWindowClause(const Identifier &window_name);
	void SetQueryLocation(ParsedExpression &expr, QueryLocation query_location);
	void SetQueryLocation(TableRef &ref, QueryLocation query_location);

private:
	arena_ptr<TransformResultValue> TransformInternal(ParseResult &parse_result);
	void SetResultLocation(ParseResult &parse_result, TransformResultValue &result);

	template <typename T>
	void SetResultLocation(T &, QueryLocation) {
	}
	void SetResultLocation(unique_ptr<ParsedExpression> &expr, QueryLocation location) {
		if (!expr) {
			return;
		}
		if (location.IsValid() && !expr->HasQueryLocation()) {
			SetQueryLocation(*expr, location);
		}
	}
	void SetResultLocation(unique_ptr<TableRef> &ref, QueryLocation location) {
		if (!ref) {
			return;
		}
		if (location.IsValid() && !ref->query_location.IsValid()) {
			SetQueryLocation(*ref, location);
		}
	}

public:
	ArenaAllocator &allocator;
	TokenIterator &token_iterator;
	identifier_map_t<idx_t> named_parameter_map;
	idx_t prepared_statement_parameter_index = 0;
	PreparedParamType last_param_type = PreparedParamType::INVALID;

	deque<identifier_map_t<unique_ptr<WindowExpression>>> window_clauses;

	vector<unique_ptr<CreatePivotEntry>> pivot_entries;
	vector<reference<CommonTableExpressionMap>> stored_cte_map;

	bool in_window_definition = false;
	bool has_anonymous_parameters = false;

	friend class StackChecker<PEGTransformer>;
	idx_t stack_depth = 0;
	//! An upper bound of the depth of the expression trees currently being transformed - tracked by the transform
	//! stacks: every nested expression, nested type and link of an operator chain counts while it is transformed
	idx_t expression_depth = 0;
	//! The depth a frame of this rule adds while it is on the stack (see ParsedGrammar::SetExpressionDepth)
	static idx_t ExpressionDepthWeight(optional_ptr<const CompiledGrammarRule> rule, ParseResult &parse_result);
	void EnterExpression(idx_t weight);
	void ExitExpression(idx_t weight);

	StackChecker<PEGTransformer> StackCheck(idx_t extra_stack = 1) {
		if (stack_depth + extra_stack >= options.max_expression_depth) {
			throw ParserException(
			    "Max expression depth limit of %lld exceeded. Use \"SET max_expression_depth TO x\" to "
			    "increase the maximum expression depth.",
			    options.max_expression_depth);
		}
		return StackChecker<PEGTransformer>(*this, extra_stack);
	}

	ParserOptions options;
	const CompiledGrammar &grammar;

private:
	ArenaAllocator process_allocator;

private:
	friend class GeneratedTransformProcess;
	friend class FinalizeTransformProcess;
	friend class TransformStack;
};

template <typename T>
inline unique_ptr<TypedTransformResult<T>> TryBridgeTransformResultValue(TransformResultValue &base_result) {
	return nullptr;
}

//! Transparent bridging between string-typed and Identifier-typed transform results.
template <>
inline unique_ptr<TypedTransformResult<string>>
TryBridgeTransformResultValue<string>(TransformResultValue &base_result) {
	if (auto *ident = TryGetTransformResult<Identifier>(base_result)) {
		return make_uniq<TypedTransformResult<string>>(ident->GetIdentifierName());
	}
	return nullptr;
}

template <>
inline unique_ptr<TypedTransformResult<Identifier>>
TryBridgeTransformResultValue<Identifier>(TransformResultValue &base_result) {
	if (auto *str = TryGetTransformResult<string>(base_result)) {
		return make_uniq<TypedTransformResult<Identifier>>(Identifier(*str));
	}
	return nullptr;
}

template <>
inline unique_ptr<TypedTransformResult<vector<string>>>
TryBridgeTransformResultValue<vector<string>>(TransformResultValue &base_result) {
	if (auto *idents = TryGetTransformResult<vector<Identifier>>(base_result)) {
		return make_uniq<TypedTransformResult<vector<string>>>(IdentifiersToStrings(*idents));
	}
	return nullptr;
}

template <>
inline unique_ptr<TypedTransformResult<vector<Identifier>>>
TryBridgeTransformResultValue<vector<Identifier>>(TransformResultValue &base_result) {
	if (auto *strs = TryGetTransformResult<vector<string>>(base_result)) {
		return make_uniq<TypedTransformResult<vector<Identifier>>>(StringsToIdentifiers(*strs));
	}
	return nullptr;
}

class PEGTransformerFactory {
public:
	static void RegisterDefaultTransforms(ParsedGrammar &grammar);

	//! Match a single TopLevelStatement from `token_iterator` and transform it into a SQLStatement.
	//! Returns nullptr if the matched TLS was separator-only (no statement). On success the iterator
	//! is advanced past the consumed tokens. A syntax error sets `failure_index` to the furthest token
	//! reached and returns nullptr; the caller builds the error with PEGSyntaxError if it still needs
	//! one, since composing the message re-parses the statement several times.
	static unique_ptr<SQLStatement> TransformTopLevelStatement(TokenIterator &token_iterator, ParserOptions &options,
	                                                           const CompiledGrammar &grammar,
	                                                           optional_idx &failure_index);
	static ParseResult &ExtractResultFromParens(ParseResult &parse_result);
	static vector<reference<ParseResult>> ExtractParseResultsFromList(ParseResult &parse_result);
	static bool ExpressionIsEmptyStar(const ParsedExpression &expr);
	static QualifiedName StringToQualifiedName(vector<string> input);
	static QualifiedColumnName StringToQualifiedColumnName(const vector<string> &input);
	static LogicalType GetIntervalTargetType(DatePartSpecifier date_part);
	static LogicalType ApplyColumnCollation(const optional<LogicalType> &type, unique_ptr<ParsedExpression> collation);
	static void AddGroupByExpression(unique_ptr<ParsedExpression> expression, GroupingExpressionMap &map,
	                                 GroupByNode &result, vector<ProjectionIndex> &result_set);
	static vector<GroupingSet> GroupByExpressionUnfolding(GroupByExpressionInfo &group_by_expr,
	                                                      GroupingExpressionMap &map, GroupByNode &result);
	static unique_ptr<ResultModifier> VerifyLimitOffset(LimitPercentResult &limit, LimitPercentResult &offset);
	static unique_ptr<QueryNode> ToRecursiveCTE(unique_ptr<QueryNode> node, const Identifier &name,
	                                            vector<Identifier> &aliases,
	                                            vector<unique_ptr<ParsedExpression>> &key_targets);
	static void WrapRecursiveView(unique_ptr<CreateViewInfo> &info, unique_ptr<QueryNode> inner_node);
	static void ConvertToRecursiveView(unique_ptr<CreateViewInfo> &info, unique_ptr<QueryNode> &node);
	static void VerifyColumnRefs(const ParsedExpression &expr);
	static void RemoveOrderQualificationRecursive(unique_ptr<ParsedExpression> &root_expr);
	static void GetValueFromExpression(unique_ptr<ParsedExpression> &expr, vector<Value> &result);
	static bool TransformPivotInList(unique_ptr<ParsedExpression> &expr, PivotColumnEntry &entry);
	static void AddPivotEntry(PEGTransformer &transformer, string enum_name, unique_ptr<SelectNode> base,
	                          unique_ptr<ParsedExpression> column, unique_ptr<QueryNode> subquery, bool has_parameters);
	static Value GetConstantExpressionValue(unique_ptr<ParsedExpression> &expr);
	static void SplitGenericOptions(const vector<GenericCopyOption> &options_in,
	                                case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_options,
	                                unordered_map<string, Value> &options, const char *statement_name);
	//! Fold `(k v, ...)` into unbound expressions, for statements that resolve every option at bind time
	//! instead of splitting literals out at parse time. Validates like SplitGenericOptions.
	static void CollectGenericOptions(const vector<GenericCopyOption> &options_in,
	                                  case_insensitive_map_t<unique_ptr<ParsedExpression>> &options,
	                                  const char *statement_name);
	static void AddToMultiStatement(const unique_ptr<MultiStatement> &multi_statement,
	                                unique_ptr<AlterInfo> alter_info);
	static void AddUpdateToMultiStatement(const unique_ptr<MultiStatement> &multi_statement, const string &column_name,
	                                      const AlterEntryData &table_data,
	                                      const unique_ptr<ParsedExpression> &original_expression);
	static unique_ptr<MultiStatement> TransformAndMaterializeAlter(AlterEntryData &data,
	                                                               unique_ptr<AlterInfo> info_with_null_placeholder,
	                                                               const string &column_name,
	                                                               unique_ptr<ParsedExpression> expression);

	static void InitializePivotStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotStatementTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeUnpivotStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpivotStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeLiteralExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLiteralExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializePrefixExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePrefixExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOverClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOverClauseTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSelectStatementInternalTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSelectStatementInternalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSimpleSelectTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSimpleSelectTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTableRefTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableRefTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeWithClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithClauseTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeWindowDefinitionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowDefinitionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);

	//===--------------------------------------------------------------------===//
	// START GENERATED TRAMPOLINE RULES
	//===--------------------------------------------------------------------===//
	static void InitializeStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStatementTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeIdentifierOrKeywordTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIdentifierOrKeywordTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeAlterStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterStatementTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAlterOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterOptionsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeAlterTableStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterTableStmtTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAlterSchemaStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterSchemaStmtTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAlterSchemaOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterSchemaOptionsTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeAlterTableOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterTableOptionsTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeAddConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAddConstraintTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDropConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropConstraintTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAddColumnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAddColumnTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeAddColumnEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAddColumnEntryTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeDropColumnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropColumnTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeAlterColumnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterColumnTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeRenameColumnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRenameColumnTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeNestedColumnNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNestedColumnNameTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeIdentifierDotTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIdentifierDotTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeRenameAlterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRenameAlterTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSetPartitionedByTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetPartitionedByTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeResetPartitionedByTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetPartitionedByTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSetSortedByTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetSortedByTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeResetSortedByTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetSortedByTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSetOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetOptionsTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeResetOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetOptionsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeAlterColumnEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterColumnEntryTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeAddOrDropDefaultTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAddOrDropDefaultTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeAddDefaultTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAddDefaultTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeDropDefaultTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropDefaultTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeChangeNullabilityTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeChangeNullabilityTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDropOrSetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropOrSetTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeDropNullabilityTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropNullabilityTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSetNullabilityTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetNullabilityTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAlterTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeUsingExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUsingExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAlterViewStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterViewStmtTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeAlterSequenceStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterSequenceStmtTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeQualifiedSequenceNameTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedSequenceNameTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeAlterSequenceOptionsTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterSequenceOptionsTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeRenameAlterSequenceOptionsTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeRenameAlterSequenceOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSetSequenceOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetSequenceOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeAlterDatabaseStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAlterDatabaseStmtTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeAnalyzeStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnalyzeStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeAnalyzeTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnalyzeTargetTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeAnalyzeVerboseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnalyzeVerboseTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAttachStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAttachStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeDatabasePathTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDatabasePathTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeAttachAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAttachAliasTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeAttachOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAttachOptionsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCallStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCallStatementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCheckpointStatementTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCheckpointStatementTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCheckpointForceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCheckpointForceTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCommentStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeCommentTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentTargetTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCommentStringLiteralIdentifierTrampoline(PEGTransformer &transformer,
	                                                               GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCommentStringLiteralIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCommentOnTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentOnTypeTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCommentTableTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentTableTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCommentSequenceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentSequenceTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCommentFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentFunctionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCommentMacroTableTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentMacroTableTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeCommentMacroTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentMacroTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCommentViewTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentViewTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeCommentDatabaseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentDatabaseTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCommentIndexTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentIndexTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCommentSchemaTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentSchemaTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCommentTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeCommentColumnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentColumnTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCommentValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommentValueTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeStringLiteralValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStringLiteralValueTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeAnalyzeKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnalyzeKeywordTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeIndexNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIndexNameTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeConstraintNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeConstraintNameTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSequenceNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSequenceNameTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCollationNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCollationNameTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeNumberLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNumberLiteralTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeStringLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStringLiteralTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static void InitializeTypeVariationsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeVariationsTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSimpleTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSimpleTypeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCharacterSimpleTypeTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCharacterSimpleTypeTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeQualifiedSimpleTypeTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedSimpleTypeTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeIntervalTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalTypeTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeIntervalIntervalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalIntervalTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeIntervalWithSpecifierTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalWithSpecifierTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeIntervalWithRangeSpecifierTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIntervalWithRangeSpecifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIntervalWithSimpleSpecifierTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIntervalWithSimpleSpecifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIntervalWithoutSpecifierTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIntervalWithoutSpecifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIntervalToIntervalAsTypeTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIntervalToIntervalAsTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeYearKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeYearKeywordTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMonthKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMonthKeywordTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDayKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDayKeywordTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeHourKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeHourKeywordTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMinuteKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMinuteKeywordTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSecondKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSecondKeywordTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeMillisecondKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMillisecondKeywordTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeMicrosecondKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMicrosecondKeywordTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeWeekKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWeekKeywordTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeQuarterKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuarterKeywordTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeDecadeKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDecadeKeywordTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCenturyKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCenturyKeywordTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeMillenniumKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMillenniumKeywordTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeIntervalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeIntervalToIntervalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalToIntervalTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeYearToMonthTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeYearToMonthTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDayToHourTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDayToHourTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeDayToMinuteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDayToMinuteTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDayToSecondTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDayToSecondTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeHourToMinuteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeHourToMinuteTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeHourToSecondTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeHourToSecondTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeMinuteToSecondTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMinuteToSecondTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeBitTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBitTypeTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeGeometryTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGeometryTypeTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeVariantTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVariantTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeNumericTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNumericTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSimpleNumericTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSimpleNumericTypeTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDecimalNumericTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDecimalNumericTypeTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeIntTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntTypeTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeIntegerTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntegerTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSmallintTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSmallintTypeTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeBigintTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBigintTypeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeRealTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRealTypeTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeBooleanTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBooleanTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDoubleTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDoubleTypeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeFloatTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFloatTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeDecimalTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDecimalTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDecTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDecTypeTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeNumericModTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNumericModTypeTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeQualifiedTypeNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedTypeNameTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeTypeNameAsQualifiedNameTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTypeNameAsQualifiedNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCatalogReservedSchemaTypeNameTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCatalogReservedSchemaTypeNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSchemaReservedTypeNameTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSchemaReservedTypeNameTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeTypeModifiersTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeModifiersTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeNestedTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNestedTypeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeRowTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowTypeTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeSetofTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetofTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeUnionTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnionTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeColIdTypeListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColIdTypeListTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeMapTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMapTypeTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeTupleTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTupleTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeColIdTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColIdTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeArrayBoundsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeArrayBoundsTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeArrayKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeArrayKeywordTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeArrayKeywordWithBoundsTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeArrayKeywordWithBoundsTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeSquareBracketsArrayTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSquareBracketsArrayTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeTimeTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTimeTypeTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeTimeOrTimestampTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTimeOrTimestampTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeTimeTypeIdTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTimeTypeIdTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeTimestampTypeIdTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTimestampTypeIdTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeTimeZoneTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTimeZoneTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeWithOrWithoutTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithOrWithoutTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeWithRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithRuleTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeWithoutRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithoutRuleTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeConnectStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeConnectStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeDisconnectStatementTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDisconnectStatementTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeSessionTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSessionTargetTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeLocalSessionTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLocalSessionTargetTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeStringSessionTargetTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStringSessionTargetTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCatalogSessionTargetTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCatalogSessionTargetTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeCopyStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyStatementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCopyVariationsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyVariationsTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCopyTableTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyTableTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeFromOrToTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromOrToTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeCopyFromTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyFromTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeCopyToTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyToTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeCopySelectTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopySelectTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCopyFileNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyFileNameTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCopyFileNameExpressionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyFileNameExpressionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeCopyFileNameStringLiteralTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCopyFileNameStringLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCopyFileNameIdentifierTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyFileNameIdentifierTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeCopyFileNameIdentifierColIdTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCopyFileNameIdentifierColIdTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIdentifierColIdTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIdentifierColIdTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCopyFileNameSuffixTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyFileNameSuffixTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeCopyLegacyOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyLegacyOptionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeCopyOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyOptionsTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeCopyOptionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyOptionListTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSpecializedOptionListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSpecializedOptionListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeSpecializedOptionTailTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSpecializedOptionTailTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeSpecializedOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSpecializedOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeSingleOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSingleOptionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeBinaryOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBinaryOptionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeFreezeOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFreezeOptionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeOidsOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOidsOptionTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCsvOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCsvOptionTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeHeaderOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeHeaderOptionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeNullAsOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullAsOptionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDelimiterAsOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDelimiterAsOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeQuoteAsOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuoteAsOptionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeEscapeAsOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEscapeAsOptionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeEncodingOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEncodingOptionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeForceQuoteOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForceQuoteOptionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeStarSymbolColumnListTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStarSymbolColumnListTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeForceQuoteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForceQuoteTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializePartitionByOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePartitionByOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializePartitionByColumnListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePartitionByColumnListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeStarPartitionByColumnListTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeStarPartitionByColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeParenthesizedPartitionByColumnListTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeParenthesizedPartitionByColumnListTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static void InitializeSinglePartitionByColumnListTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSinglePartitionByColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeForceNullOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForceNullOptionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeForceNotNullTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForceNotNullTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCopyGenericOptionListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyGenericOptionListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeCopyGenericOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyGenericOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeOrderByCopyOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrderByCopyOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializePartitionedByCopyOptionTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizePartitionedByCopyOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeGenericCopyOptionListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGenericCopyOptionListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeGenericCopyOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGenericCopyOptionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeGenericCopyOptionValueTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGenericCopyOptionValueTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeGenericCopyOptionOrderListTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeGenericCopyOptionOrderListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeGenericCopyOptionExpressionTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeGenericCopyOptionExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeGenericCopyOptionParenthesizedExpressionListTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeGenericCopyOptionParenthesizedExpressionListTrampoline(PEGTransformer &transformer,
	                                                               GeneratedTransformProcess &process);
	static void InitializeCopyFromDatabaseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyFromDatabaseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeCopyFromDatabaseWithFlagTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCopyFromDatabaseWithFlagTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCopyFromDatabaseWithoutFlagTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCopyFromDatabaseWithoutFlagTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCopyDatabaseFlagTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyDatabaseFlagTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSchemaOrDataTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSchemaOrDataTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCopySchemaTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopySchemaTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCopyDataTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCopyDataTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeCreateIndexStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateIndexStmtTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeWithListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithListTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeRelOptionOrOidsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRelOptionOrOidsTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeRelOptionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRelOptionListTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOidsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOidsTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static void InitializeWithOrWithoutOidsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithOrWithoutOidsTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeWithOidsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithOidsTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeWithoutOidsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithoutOidsTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeIndexElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIndexElementTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeUniqueIndexTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUniqueIndexTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeIndexTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIndexTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeRelOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRelOptionTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeRelOptionNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRelOptionNameTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDottedColLabelTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDottedColLabelTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeRelOptionArgumentOptTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRelOptionArgumentOptTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeDefArgTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefArgTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeDefArgNullTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefArgNullTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeDefArgKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefArgKeywordTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDefArgStringLiteralTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefArgStringLiteralTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeNoneLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNoneLiteralTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeCreateMacroStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateMacroStmtTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeReturnsTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReturnsTypeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMacroOrFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMacroOrFunctionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeMacroKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMacroKeywordTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeFunctionKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionKeywordTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeMacroDefinitionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMacroDefinitionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeMacroDefinitionBodyTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMacroDefinitionBodyTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeMacroParametersTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMacroParametersTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeMacroParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMacroParameterTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSimpleParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSimpleParameterTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeScalarMacroDefinitionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeScalarMacroDefinitionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeTableMacroDefinitionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableMacroDefinitionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeCreateSchemaStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateSchemaStmtTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeWithOptionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithOptionListTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCreateSecretStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateSecretStmtTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSecretStorageSpecifierTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSecretStorageSpecifierTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeSecretNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSecretNameTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCreateSequenceStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateSequenceStmtTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSequenceOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSequenceOptionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSeqSetCycleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqSetCycleTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSeqCycleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqCycleTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeSeqNoCycleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqNoCycleTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSeqSetIncrementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqSetIncrementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSeqSetMinMaxTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqSetMinMaxTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSeqNoMinMaxTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqNoMinMaxTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSeqStartWithTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqStartWithTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSeqOwnedByTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqOwnedByTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSeqMinOrMaxTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSeqMinOrMaxTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMinValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMinValueTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeMaxValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMaxValueTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeCreateStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCreateStatementVariationTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCreateStatementVariationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeOrReplaceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrReplaceTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeTemporaryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTemporaryTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializePersistentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePersistentTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeTempPersistentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTempPersistentTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeTemporaryPersistentTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTemporaryPersistentTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCreateTableStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTableStmtTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCreateTableDefinitionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTableDefinitionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeCreateTableAsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTableAsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializePartitionSortedOptionsTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePartitionSortedOptionsTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializePartitionOptSortedOptionsTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizePartitionOptSortedOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSortedOptPartitionOptionsTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSortedOptPartitionOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializePartitionOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePartitionOptionsTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSortedOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSortedOptionsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeWithDataTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithDataTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeWithDataOnlyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithDataOnlyTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeWithNoDataTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithNoDataTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeIdentifierListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIdentifierListTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCreateColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateColumnListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeIfNotExistsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIfNotExistsTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeQualifiedNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedNameTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSchemaReservedIdentifierOrStringLiteralTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSchemaReservedIdentifierOrStringLiteralTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static void InitializeCatalogReservedSchemaIdentifierTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCatalogReservedSchemaIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIdentifierOrStringLiteralTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIdentifierOrStringLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeReservedIdentifierOrStringLiteralTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeReservedIdentifierOrStringLiteralTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static void InitializeCatalogQualificationTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCatalogQualificationTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeSchemaQualificationTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSchemaQualificationTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeReservedSchemaQualificationTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeReservedSchemaQualificationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeTableQualificationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableQualificationTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeReservedTableQualificationTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeReservedTableQualificationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCreateTableColumnListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTableColumnListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeCreateTableColumnElementTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCreateTableColumnElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCreateTableColumnDefinitionTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCreateTableColumnDefinitionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCreateTableConstraintTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTableConstraintTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeColumnDefinitionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnDefinitionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeColumnConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnConstraintTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeNameableColumnConstraintTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeNameableColumnConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeColumnConstraintElementTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeColumnConstraintElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeNotNullConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotNullConstraintTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeNullConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullConstraintTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeNotNullColumnConstraintTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeNotNullColumnConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeUniqueConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUniqueConstraintTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePrimaryKeyConstraintTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePrimaryKeyConstraintTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeConstraintTimingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeConstraintTimingTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeImmediateConstraintTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeImmediateConstraintTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeDeferredConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeferredConstraintTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeDefaultValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefaultValueTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCheckConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCheckConstraintTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeForeignKeyConstraintTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForeignKeyConstraintTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeColumnCollationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnCollationTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeColumnCompressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnCompressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeKeyActionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeKeyActionsTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeUpdateFirstKeyActionsTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateFirstKeyActionsTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeDeleteFirstKeyActionsTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeleteFirstKeyActionsTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeUpdateActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateActionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDeleteActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeleteActionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeKeyActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeKeyActionTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeNoKeyActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNoKeyActionTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeRestrictKeyActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRestrictKeyActionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeCascadeKeyActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCascadeKeyActionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSetNullKeyActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetNullKeyActionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSetDefaultKeyActionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetDefaultKeyActionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeTopLevelConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTopLevelConstraintTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTopLevelConstraintListTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTopLevelConstraintListTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeTopCheckConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTopCheckConstraintTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTopPrimaryKeyConstraintTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTopPrimaryKeyConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeTopUniqueConstraintTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTopUniqueConstraintTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeTopForeignKeyConstraintTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTopForeignKeyConstraintTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeColumnIdListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnIdListTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDottedIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDottedIdentifierTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeDotColLabelTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDotColLabelTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIdentifierTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeColIdTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColIdTrampoline(PEGTransformer &transformer,
	                                                               GeneratedTransformProcess &process);
	static void InitializeColIdOrStringTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColIdOrStringTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTypeFuncNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeFuncNameTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTypeFuncKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeFuncKeywordTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeColLabelTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColLabelTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeColLabelOrStringTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColLabelOrStringTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeColLabelIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColLabelIdentifierTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeStringLiteralIdentifierTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeStringLiteralIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeGeneratedColumnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGeneratedColumnTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeGeneratedTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGeneratedTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeGeneratedAlwaysTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGeneratedAlwaysTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeGeneratedByDefaultTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGeneratedByDefaultTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeGeneratedColumnTypeTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGeneratedColumnTypeTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCommitActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommitActionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializePreserveOrDeleteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePreserveOrDeleteTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePreserveRowsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePreserveRowsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDeleteRowsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeleteRowsTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeVirtualGeneratedColumnTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVirtualGeneratedColumnTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeStoredGeneratedColumnTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStoredGeneratedColumnTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeCreateTriggerStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTriggerStmtTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeTriggerBodyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerBodyTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeTriggerNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerNameTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeReferencingClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReferencingClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeReferencingItemTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReferencingItemTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeReferencingNewTableAsTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReferencingNewTableAsTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeReferencingOldTableAsTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReferencingOldTableAsTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeTriggerTimingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerTimingTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTriggerBeforeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerBeforeTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTriggerAfterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerAfterTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTriggerInsteadOfTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerInsteadOfTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeTriggerEventTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerEventTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTriggerEventInsertTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerEventInsertTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTriggerEventDeleteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerEventDeleteTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTriggerEventUpdateTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerEventUpdateTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTriggerEventUpdateOfTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerEventUpdateOfTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeTriggerColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTriggerColumnListTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeForEachClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForEachClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeForEachRowTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForEachRowTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeForEachStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForEachStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeCreateTypeStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTypeStmtTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCreateTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTypeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCreateTypeFromTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateTypeFromTypeTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeEnumSelectTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEnumSelectTypeTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeEnumStringLiteralListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEnumStringLiteralListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeCreateViewStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateViewStmtTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCreateRecursiveTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateRecursiveTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCreateSecureTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCreateSecureTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDeallocateStatementTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeallocateStatementTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeDeallocatePrepareTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeallocatePrepareTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDeleteStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeleteStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeTruncateStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTruncateStatementTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeTruncateOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTruncateOptionsTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeTruncateAliasOptionsTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTruncateAliasOptionsTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeTruncateDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTruncateDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeTargetOptAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTargetOptAliasTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeTargetAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTargetAliasTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDeleteUsingClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeleteUsingClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDescribeStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeStatementTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeShowDeprecatedSelectTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowDeprecatedSelectTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeDescribeSelectTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeSelectTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeShowAllTablesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowAllTablesTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeShowTablesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowTablesTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeShowByNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowByNameTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeDescribeByNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeByNameTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeDescribeOrSummarizeTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeOrSummarizeTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeShowTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowTargetTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeShowDeprecatedQualifiedTableNameTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeShowDeprecatedQualifiedTableNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeShowSettingNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowSettingNameTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeDescribeTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeTargetTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeDescribeBaseTableNameTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeBaseTableNameTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeDescribeStringLiteralTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeStringLiteralTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeSummarizeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSummarizeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeSummarizeRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSummarizeRuleTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeShowOrDescribeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowOrDescribeTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeShowRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowRuleTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeDescribeRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeRuleTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDescribeLongRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescribeLongRuleTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeDescRuleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescRuleTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeDetachStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDetachStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeDropStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropStatementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDropEntriesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropEntriesTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDropTriggerTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropTriggerTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDropTableTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropTableTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeDropTableFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropTableFunctionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDropFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropFunctionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDropSchemaTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropSchemaTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeDropIndexTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropIndexTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeQualifiedIndexNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedIndexNameTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeQualifiedIndexNameStringTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeQualifiedIndexNameStringTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSchemaReservedIndexTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSchemaReservedIndexTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCatalogReservedSchemaIndexTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCatalogReservedSchemaIndexTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeDropSequenceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropSequenceTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDropCollationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropCollationTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDropTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropTypeTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeDropSecretTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropSecretTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeTableOrViewTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableOrViewTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMaterializedViewEntryTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMaterializedViewEntryTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeFunctionTypeMacroTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionTypeMacroTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeFunctionTypeMacroKeywordTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeFunctionTypeMacroKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFunctionTypeFunctionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionTypeFunctionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeDropBehaviorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCascadeDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCascadeDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeRestrictDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRestrictDropBehaviorTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeIfExistsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIfExistsTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeDropSecretStorageTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDropSecretStorageTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExecuteStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExecuteStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeExplainStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExplainStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeExplainOptionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExplainOptionListTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExplainOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExplainOptionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeExplainOptionNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExplainOptionNameTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExplainSelectStatementTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExplainSelectStatementTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeExplainableStatementsTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExplainableStatementsTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeExportStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExportStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeExportSourceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExportSourceTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeExportOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExportOptionsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeImportStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeImportStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeColumnReferenceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnReferenceTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeNestedSchemaTableColumnNameTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeNestedSchemaTableColumnNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCatalogReservedSchemaTableColumnNameTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCatalogReservedSchemaTableColumnNameTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static void InitializeSchemaReservedTableColumnNameTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSchemaReservedTableColumnNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeTableReservedColumnNameTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTableReservedColumnNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFunctionExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionExpressionTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeFunctionExpressionArgumentsTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeFunctionExpressionArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFunctionExpressionArgumentListTrampoline(PEGTransformer &transformer,
	                                                               GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeFunctionExpressionArgumentListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFunctionArgumentListTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionArgumentListTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeFunctionIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionIdentifierTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeFunctionNameAsQualifiedNameTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeFunctionNameAsQualifiedNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCatalogReservedSchemaFunctionNameTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCatalogReservedSchemaFunctionNameTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static void InitializeSchemaReservedFunctionNameTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSchemaReservedFunctionNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeDistinctOrAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistinctOrAllTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDistinctKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistinctKeywordTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAllKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAllKeywordTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeWithinGroupClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithinGroupClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeFilterClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFilterClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeFilterClauseExpressionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFilterClauseExpressionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeFilterClauseContentsTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFilterClauseContentsTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeIgnoreOrRespectNullsTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIgnoreOrRespectNullsTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeIgnoreNullsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIgnoreNullsTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeRespectNullsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRespectNullsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeParenthesisExpressionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeParenthesisExpressionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeConstantLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeConstantLiteralTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeNullLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullLiteralTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeTrueLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrueLiteralTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeFalseLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFalseLiteralTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCastExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCastExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCastArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCastArgumentsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCastOrTryCastTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCastOrTryCastTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCastKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCastKeywordTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeTryCastKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTryCastKeywordTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeColIdDotTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColIdDotTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeStarExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStarExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeStarQualifierListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStarQualifierListTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExcludeListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeListTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeExcludeNamesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeNamesTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeExcludeNameListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeNameListTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeExcludeNameSingleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeNameSingleTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExcludeNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeNameTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeExcludeDottedNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeDottedNameTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExcludeColumnNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeColumnNameTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeReplaceListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReplaceListTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeReplaceEntriesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReplaceEntriesTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeReplaceEntrySingleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReplaceEntrySingleTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeReplaceEntryListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReplaceEntryListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeReplaceEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReplaceEntryTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeRenameListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRenameListTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeRenameEntriesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRenameEntriesTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeRenameEntryListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRenameEntryListTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSingleRenameEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSingleRenameEntryTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeRenameEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRenameEntryTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSubqueryExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubqueryExpressionTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSubqueryNotTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubqueryNotTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSubqueryExistsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubqueryExistsTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCaseExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCaseExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeCaseWhenThenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCaseWhenThenTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCaseElseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCaseElseTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeTypeLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeLiteralTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeIntervalLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalLiteralTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeIntervalParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntervalParameterTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeIntervalStringParameterTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIntervalStringParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFrameClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFrameClauseTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeFramingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFramingTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeRowsFramingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowsFramingTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeRangeFramingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRangeFramingTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeGroupsFramingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupsFramingTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeFrameExtentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFrameExtentTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSingleFrameExtentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSingleFrameExtentTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeBetweenFrameExtentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBetweenFrameExtentTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeFrameBoundTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFrameBoundTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeFrameUnboundedTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFrameUnboundedTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeFrameExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFrameExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeFrameCurrentRowTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFrameCurrentRowTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializePrecedingOrFollowingTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePrecedingOrFollowingTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializePrecedingFrameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePrecedingFrameTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeFollowingFrameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFollowingFrameTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeWindowExcludeClauseTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowExcludeClauseTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeWindowExcludeElementTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowExcludeElementTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeExcludeCurrentRowTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeCurrentRowTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExcludeGroupTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeGroupTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeExcludeTiesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeTiesTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeExcludeNoOthersTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeNoOthersTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeWindowFrameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowFrameTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeIdentifierWindowFrameTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIdentifierWindowFrameTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeParensIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeParensIdentifierTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeWindowFrameDefinitionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowFrameDefinitionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeWindowFrameNameContentsParensTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeWindowFrameNameContentsParensTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeWindowFrameNameContentsTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeWindowFrameNameContentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeWindowFrameContentsParensTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeWindowFrameContentsParensTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeWindowFrameContentsTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowFrameContentsTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeBaseWindowNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBaseWindowNameTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeWindowPartitionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowPartitionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeListExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeListExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeArrayBoundedListExpressionTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeArrayBoundedListExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeArrayParensSelectTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeArrayParensSelectTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeBoundedListExpressionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBoundedListExpressionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeStructExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStructExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeStructFieldTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStructFieldTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMapExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMapExpressionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeMapStructExpressionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMapStructExpressionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeMapStructFieldTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMapStructFieldTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeGroupingExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupingExpressionTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeGroupingOrGroupingIdTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupingOrGroupingIdTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeGroupingKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupingKeywordTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeGroupingIdKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupingIdKeywordTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeParameterTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeQuestionMarkNumberedParameterTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeQuestionMarkNumberedParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeAnonymousParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnonymousParameterTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeNumberedParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNumberedParameterTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeColLabelParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColLabelParameterTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializePositionalExpressionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePositionalExpressionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeDefaultExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefaultExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeListComprehensionExpressionTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeListComprehensionExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeListComprehensionFilterTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeListComprehensionFilterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeParensExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeParensExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeMeasureSemanticsExpressionTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeMeasureSemanticsExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSingleExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSingleExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExpressionTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeColumnDefaultExprTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnDefaultExprTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeLambdaArrowExpressionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLambdaArrowExpressionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeSingleArrowPairTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSingleArrowPairTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeLogicalOrExpressionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLogicalOrExpressionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeLogicalOrExpressionTailTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeLogicalOrExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeColDefOrExprTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColDefOrExprTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeColDefOrExpressionTailTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColDefOrExpressionTailTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeLogicalAndExpressionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLogicalAndExpressionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeLogicalAndExpressionTailTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeLogicalAndExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeColDefAndExprTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColDefAndExprTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeColDefAndExpressionTailTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeColDefAndExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeLogicalNotExpressionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLogicalNotExpressionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeNotExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotExpressionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeNotKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotKeywordTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeIsExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsExpressionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeIsExpressionContinuationTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIsExpressionContinuationTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIsExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsExpressionTailTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeIsTestTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsTestTailTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeIsDistinctTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsDistinctTailTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeIsComparisonTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsComparisonTailTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeIsOtherOperatorTailTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsOtherOperatorTailTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeIsCastTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsCastTailTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeIsTestTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsTestTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeIsLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsLiteralTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeIsLiteralValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsLiteralValueTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeUnknownLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnknownLiteralTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeNotNullTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotNullTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeNotNullKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotNullKeywordTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeNotNullOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotNullOperatorTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeIsNullTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsNullTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeIsNullOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsNullOperatorTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeIsDistinctFromExpressionTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeIsDistinctFromExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeIsDistinctFromTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsDistinctFromTailTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeIsDistinctFromOpTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIsDistinctFromOpTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeComparisonExpressionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeComparisonExpressionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeComparisonExpressionTailTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeComparisonExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeComparisonOperandTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeComparisonOperandTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeNotComparisonOperandTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotComparisonOperandTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeComparisonOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeComparisonOperatorTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeOperatorEqualTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOperatorEqualTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOperatorNotEqualTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOperatorNotEqualTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOperatorLessThanTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOperatorLessThanTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOperatorGreaterThanTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOperatorGreaterThanTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeOperatorLessThanEqualsTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOperatorLessThanEqualsTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeOperatorGreaterThanEqualsTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeOperatorGreaterThanEqualsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeBetweenInLikeExpressionTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeBetweenInLikeExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeInPredicateTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInPredicateTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeBetweenLikeOpTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBetweenLikeOpTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeBetweenLikeOpExpressionTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeBetweenLikeOpExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeLikeClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLikeClauseTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeEscapeClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEscapeClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeLikeVariationsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLikeVariationsTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeLikeTokenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLikeTokenTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeILikeTokenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeILikeTokenTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeGlobTokenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGlobTokenTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeSimilarToTokenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSimilarToTokenTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeRegexMatchTokenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRegexMatchTokenTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeRegexInsensitiveMatchTokenTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeRegexInsensitiveMatchTokenTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeNotILikeOpTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotILikeOpTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeNotLikeOpTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotLikeOpTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeNotRegexInsensitiveMatchOpTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeNotRegexInsensitiveMatchOpTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeNotSimilarToOpTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotSimilarToOpTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeInClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInClauseTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeInExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInExpressionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeInContainsExpressionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInContainsExpressionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeInExpressionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInExpressionListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeInSelectStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInSelectStatementTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeBetweenClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBetweenClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOtherOperatorExpressionTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeOtherOperatorExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeInfixOtherOperatorExpressionTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeInfixOtherOperatorExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeOtherOperatorTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOtherOperatorTailTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeOtherOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOtherOperatorTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeAnyAllParsedOperatorTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnyAllParsedOperatorTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeNamedOtherOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNamedOtherOperatorTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeOperatorLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOperatorLiteralTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAnyOperatorLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnyOperatorLiteralTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeAnyAllOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnyAllOperatorTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAnyOrAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAnyOrAllTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeSubqueryAnyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubqueryAnyTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSubqueryAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubqueryAllTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeQualifiedOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedOperatorTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeQualifiedOperatorContentsTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeQualifiedOperatorContentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeBitwiseExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBitwiseExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeBitwiseExpressionTailTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBitwiseExpressionTailTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeBitOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBitOperatorTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeTildeExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTildeExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAdditiveExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAdditiveExpressionTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeAdditiveExpressionTailTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAdditiveExpressionTailTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeTermTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTermTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static void InitializeMultiplicativeExpressionTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeMultiplicativeExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeMultiplicativeExpressionTailTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeMultiplicativeExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFactorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFactorTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeExponentiationExpressionTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExponentiationExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeExponentiationExpressionTailTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExponentiationExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeExponentOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExponentOperatorTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeCollateExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCollateExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeCollateExpressionTailTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCollateExpressionTailTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeAtTimeZoneExpressionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAtTimeZoneExpressionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeAtTimeZoneExpressionTailTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeAtTimeZoneExpressionTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeUnaryExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnaryExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeCustomPrefixExpressionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCustomPrefixExpressionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializePrefixOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePrefixOperatorTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeMinusPrefixOperatorTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMinusPrefixOperatorTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializePlusPrefixOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePlusPrefixOperatorTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTildePrefixOperatorTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTildePrefixOperatorTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeBaseExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBaseExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeIndirectionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIndirectionListTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeIndirectionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIndirectionTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeCastOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCastOperatorTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDotOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDotOperatorTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDotMethodOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDotMethodOperatorTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDotColumnOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDotColumnOperatorTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeMethodExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMethodExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeMethodExpressionArgumentsTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeMethodExpressionArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeMethodExpressionArgumentListTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeMethodExpressionArgumentListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeMethodFunctionArgumentsTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeMethodFunctionArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSliceExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSliceExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSliceBoundTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSliceBoundTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSliceUpperBoundTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSliceUpperBoundTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeMinusStepSliceBoundTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMinusStepSliceBoundTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeEndStepSliceBoundTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEndStepSliceBoundTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeEndSliceBoundTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEndSliceBoundTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeStepSliceBoundTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStepSliceBoundTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializePostfixOperatorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePostfixOperatorTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSpecialFunctionExpressionTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSpecialFunctionExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCoalesceExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCoalesceExpressionTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeUnpackExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpackExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeTryExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTryExpressionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeColumnsExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnsExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExtractExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExtractExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeExtractArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExtractArgumentsTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeLambdaExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLambdaExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeNullIfExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullIfExpressionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeNullIfArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullIfArgumentsTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializePositionExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePositionExpressionTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializePositionArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePositionArgumentsTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeRowExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowExpressionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSubstringExpressionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubstringExpressionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeSubstringArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubstringArgumentsTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSubstringExpressionListTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSubstringExpressionListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSubstringParametersTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubstringParametersTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeSubstringFromForTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubstringFromForTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSubstringFromOptionalForTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSubstringFromOptionalForTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSubstringForTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubstringForTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTrimExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeTrimArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimArgumentsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTrimDirectionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimDirectionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTrimBothTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimBothTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeTrimLeadingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimLeadingTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeTrimTrailingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimTrailingTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTrimSourceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTrimSourceTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeOverlayExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOverlayExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeOverlayArgumentsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOverlayArgumentsTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOverlayParametersTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOverlayParametersTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeFromExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromExpressionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeForExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeForExpressionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOverlayExpressionListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOverlayExpressionListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeExtractArgumentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExtractArgumentTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeExtractDatePartArgumentTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExtractDatePartArgumentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeExtractIdentifierArgumentTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExtractIdentifierArgumentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeExtractStringArgumentTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExtractStringArgumentTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeExtractDatePartTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExtractDatePartTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeExternalResourceStatementTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExternalResourceStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCreateExternalResourceStmtTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCreateExternalResourceStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeRegisterExternalResourceStmtTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeRegisterExternalResourceStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeDestroyExternalResourceStmtTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeDestroyExternalResourceStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeShowExternalResourcesStmtTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeShowExternalResourcesStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeShowAllModifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowAllModifierTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeExternalResourceCreationOptionsTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExternalResourceCreationOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeAttachToExternalResourceTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeAttachToExternalResourceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeConnectToExternalResourceTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeConnectToExternalResourceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeExternalResourceSourceTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExternalResourceSourceTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeExternalResourceCreateClauseTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExternalResourceCreateClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeExternalResourceReferenceClauseTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExternalResourceReferenceClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeInsertStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeOrActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrActionTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeInsertOrReplaceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertOrReplaceTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeInsertOrIgnoreTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertOrIgnoreTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeByNameOrPositionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeByNameOrPositionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeInsertByNameOrderTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertByNameOrderTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeInsertByPositionOrderTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertByPositionOrderTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeInsertByNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertByNameTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeInsertByPositionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertByPositionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeInsertTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertTargetTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeInsertAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertAliasTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnListTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeInsertColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertColumnListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeInsertValuesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertValuesTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSelectInsertValuesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectInsertValuesTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeDefaultValuesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefaultValuesTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOnConflictClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnConflictClauseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOnConflictTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnConflictTargetTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOnConflictExpressionTargetTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeOnConflictExpressionTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeOnConflictIndexTargetTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnConflictIndexTargetTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeOnConflictActionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnConflictActionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOnConflictUpdateTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnConflictUpdateTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOnConflictNothingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnConflictNothingTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeReturningClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReturningClauseTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeLoadStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLoadStatementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeExtensionAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExtensionAliasTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeInstallStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInstallStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeInstallAndLoadTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInstallAndLoadTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeUpdateExtensionsStatementTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeUpdateExtensionsStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFromSourceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromSourceTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeFromSourceIdentifierTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromSourceIdentifierTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeFromSourceStringTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromSourceStringTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeVersionNumberTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVersionNumberTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeExtensionRepositoryStatementTrampoline(PEGTransformer &transformer,
	                                                             GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExtensionRepositoryStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeCreateExtensionRepositoryStmtTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCreateExtensionRepositoryStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeRepositoryPrefixTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRepositoryPrefixTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeRepositoryPublicKeyTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRepositoryPublicKeyTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeDropExtensionRepositoryStmtTrampoline(PEGTransformer &transformer,
	                                                            GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeDropExtensionRepositoryStmtTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeTableMatchRecognizeClauseTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTableMatchRecognizeClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeMatchRecognizeBodyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMatchRecognizeBodyTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeMatchRecognizeClauseTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMatchRecognizeClauseTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeMRPartitionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRPartitionTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeMROrderByTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMROrderByTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeMRMeasuresTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRMeasuresTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeMRRowsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRRowsTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeMRSkipTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRSkipTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeMRPatternTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRPatternTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeMRSubsetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRSubsetTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeMRDefineAutoTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRDefineAutoTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeMRDefineTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMRDefineTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeMeasuresClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMeasuresClauseTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeMeasuresElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMeasuresElementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeMeasureSemanticsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMeasureSemanticsTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeRunningSemanticsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRunningSemanticsTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeFinalSemanticsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFinalSemanticsTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeRowsPerMatchTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowsPerMatchTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeOneRowPerMatchTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOneRowPerMatchTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAllRowsPerMatchTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAllRowsPerMatchTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAllRowsEmptyMatchesTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAllRowsEmptyMatchesTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeShowEmptyMatchesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeShowEmptyMatchesTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeOmitEmptyMatchesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOmitEmptyMatchesTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeWithUnmatchedRowsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithUnmatchedRowsTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeAfterMatchSkipTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAfterMatchSkipTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeAfterMatchSkipTargetTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAfterMatchSkipTargetTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeSkipToNextRowTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSkipToNextRowTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSkipPastLastRowTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSkipPastLastRowTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSkipToFirstVarTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSkipToFirstVarTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSkipToLastVarTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSkipToLastVarTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSkipToVarTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSkipToVarTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializePatternClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePatternClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeRowPatternTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeRowPatternAlternativeTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternAlternativeTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeRowPatternTermTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternTermTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeRowPatternFactorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternFactorTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeRowPatternPrimaryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternPrimaryTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeRowPatternPermuteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternPermuteTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeRowPatternAnchorTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternAnchorTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePatternStartTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePatternStartTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializePatternEndTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePatternEndTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeRowPatternGroupTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternGroupTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeRowPatternEmptyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternEmptyTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeRowPatternExclusionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternExclusionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeRowPatternLabelTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternLabelTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeRowPatternQuantifierTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRowPatternQuantifierTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeRowPatternQuantifierKindTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeRowPatternQuantifierKindTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeQuantifierReluctantTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierReluctantTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeQuantifierStarTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierStarTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeQuantifierPlusTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierPlusTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeQuantifierOptionalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierOptionalTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeQuantifierRangeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierRangeTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeQuantifierBoundsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierBoundsTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeQuantifierMinMaxTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierMinMaxTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeQuantifierMinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierMinTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeQuantifierMaxTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierMaxTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeQuantifierExactTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQuantifierExactTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSubsetClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubsetClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSubsetElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubsetElementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDefineClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefineClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDefineElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDefineElementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeMergeIntoStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMergeIntoStatementTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeMergeIntoUsingClauseTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMergeIntoUsingClauseTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeMergeMatchTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMergeMatchTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeMatchedClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMatchedClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeMatchedClauseActionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMatchedClauseActionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeUpdateMatchClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateMatchClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeUpdateMatchInfoTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateMatchInfoTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeUpdateMatchSetActionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateMatchSetActionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeUpdateByNameOrPositionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateByNameOrPositionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeDeleteMatchClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDeleteMatchClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeInsertMatchClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertMatchClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeInsertMatchInfoTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertMatchInfoTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeInsertDefaultValuesTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertDefaultValuesTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeInsertByNameOrPositionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertByNameOrPositionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeInsertValuesListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInsertValuesListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeDoNothingMatchClauseTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDoNothingMatchClauseTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeErrorMatchClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeErrorMatchClauseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeUpdateMatchSetClauseTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateMatchSetClauseTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeUpdateMatchSetInfoTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateMatchSetInfoTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeAndExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAndExpressionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeNotMatchedClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNotMatchedClauseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeBySourceOrTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBySourceOrTargetTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeBySourceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBySourceTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeByTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeByTargetTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializePivotOnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotOnTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializePivotUsingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotUsingTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializePivotColumnListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotColumnListTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializePivotColumnEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotColumnEntryTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePivotColumnExpressionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotColumnExpressionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializePivotColumnSubqueryTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotColumnSubqueryTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeIntoNameValuesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntoNameValuesTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeOptionalParensNameListTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOptionalParensNameListTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeParenthesizedNameListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeParenthesizedNameListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeBareNameListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBareNameListTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeIncludeOrExcludeNullsTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIncludeOrExcludeNullsTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeIncludeNullsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIncludeNullsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeExcludeNullsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExcludeNullsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeUnpivotHeaderTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpivotHeaderTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeUnpivotHeaderSingleTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpivotHeaderSingleTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeUnpivotHeaderListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpivotHeaderListTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializePragmaStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePragmaStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializePragmaAssignOrFunctionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePragmaAssignOrFunctionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializePragmaAssignTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePragmaAssignTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializePragmaFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePragmaFunctionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializePragmaParametersTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePragmaParametersTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePrepareStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePrepareStatementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeTypeListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTypeListTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeSelectStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeSelectSetOpChainTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectSetOpChainTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSelectSetOpChainTailTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectSetOpChainTailTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeIntersectChainTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntersectChainTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeIntersectChainTailTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeIntersectChainTailTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSetIntersectClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetIntersectClauseTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSelectAtomTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectAtomTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSelectParensTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectParensTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSetopClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetopClauseTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSetopTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetopTypeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeSetopUnionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetopUnionTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSetopExceptTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetopExceptTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSelectStatementTypeTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectStatementTypeTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeResultModifiersTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResultModifiersTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeLimitOffsetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitOffsetTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeLimitOffsetClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitOffsetClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeOffsetLimitClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOffsetLimitClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeOffsetFetchClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOffsetFetchClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeFetchOffsetClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFetchOffsetClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeFetchOnlyClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFetchOnlyClauseTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeTableStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableStatementTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeOptionalParensSimpleSelectTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeOptionalParensSimpleSelectTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSimpleSelectParensTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSimpleSelectParensTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSelectFromTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectFromTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSelectFromClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectFromClauseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeFromSelectClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromSelectClauseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeWithStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithStatementTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCTEBodyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCTEBodyTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeCTESelectBodyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCTESelectBodyTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeCTEDMLBodyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCTEDMLBodyTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeUsingKeyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUsingKeyTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeMaterializedTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeMaterializedTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSelectClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTargetListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTargetListTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeColumnAliasesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColumnAliasesTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeDistinctClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistinctClauseTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeDistinctAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistinctAllTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeDistinctOnTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistinctOnTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeDistinctOnTargetsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistinctOnTargetsTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeInnerTableRefTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInnerTableRefTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTableSubqueryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableSubqueryTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeBaseTableRefTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBaseTableRefTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTableAliasColonTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableAliasColonTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeValuesRefTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeValuesRefTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeParensTableRefTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeParensTableRefTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeJoinOrPivotTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinOrPivotTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeTablePivotClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTablePivotClauseTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeTablePivotClauseBodyTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTablePivotClauseBodyTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializePivotGroupByListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotGroupByListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeTableUnpivotClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableUnpivotClauseTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeTableUnpivotClauseBodyTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableUnpivotClauseBodyTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializePivotHeaderTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotHeaderTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializePivotValueListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotValueListTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializePivotValueTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotValueTargetTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePivotEnumTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotEnumTargetTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializePivotListTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotListTargetTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeUnpivotValueListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpivotValueListTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializePivotTargetListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePivotTargetListTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeUnpivotTargetListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUnpivotTargetListTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeLateralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLateralTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeBaseTableNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBaseTableNameTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeUnqualifiedBaseTableNameTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeUnqualifiedBaseTableNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeQualifiedTableNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedTableNameTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSchemaReservedTableTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSchemaReservedTableTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCatalogReservedSchemaTableTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeCatalogReservedSchemaTableTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeTableFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableFunctionTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTableFunctionLateralOptTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTableFunctionLateralOptTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeTableFunctionAliasColonTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeTableFunctionAliasColonTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeWithOrdinalityTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWithOrdinalityTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeQualifiedTableFunctionTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifiedTableFunctionTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeTableFunctionArgumentsTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableFunctionArgumentsTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeFunctionArgumentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFunctionArgumentTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeNamedFunctionArgumentTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNamedFunctionArgumentTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializePositionalFunctionArgumentTrampoline(PEGTransformer &transformer,
	                                                           GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizePositionalFunctionArgumentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeNamedParameterTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNamedParameterTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeTableAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableAliasTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeTableAliasAsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableAliasAsTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTableAliasWithoutAsTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTableAliasWithoutAsTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeAtClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAtClauseTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeAtSpecifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAtSpecifierTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeAtUnitTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAtUnitTrampoline(PEGTransformer &transformer,
	                                                                GeneratedTransformProcess &process);
	static void InitializeVersionAtUnitTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVersionAtUnitTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeTimestampAtUnitTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTimestampAtUnitTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeJoinClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinClauseTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeNearestJoinClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestJoinClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeNearestJoinAliasedTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestJoinAliasedTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeNearestJoinBareTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestJoinBareTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeNearestBareTableRefTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestBareTableRefTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeNearestValuesRefTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestValuesRefTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeNearestTableFunctionTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestTableFunctionTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeNearestTableSubqueryTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestTableSubqueryTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeNearestBaseTableRefTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestBaseTableRefTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeNearestParensTableRefTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestParensTableRefTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeApproxOrExactTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeApproxOrExactTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeNearestApproxTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestApproxTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeNearestExactTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestExactTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeDistanceOrSimilarityTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDistanceOrSimilarityTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeNearestDistanceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestDistanceTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeNearestSimilarityTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNearestSimilarityTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeRegularJoinClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRegularJoinClauseTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeJoinByClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinByClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeAsofTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAsofTrampoline(PEGTransformer &transformer,
	                                                              GeneratedTransformProcess &process);
	static void InitializeJoinWithoutOnClauseTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinWithoutOnClauseTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeJoinQualifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinQualifierTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOnClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOnClauseTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeUsingClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUsingClauseTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeUsingColumnNameTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUsingColumnNameTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeJoinTypeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinTypeTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeJoinPrefixTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeJoinPrefixTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeCrossJoinPrefixTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCrossJoinPrefixTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeNaturalJoinPrefixTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNaturalJoinPrefixTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializePositionalJoinPrefixTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizePositionalJoinPrefixTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeFullJoinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFullJoinTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeLeftJoinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLeftJoinTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeRightJoinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRightJoinTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeSemiJoinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSemiJoinTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeAntiJoinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAntiJoinTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeInnerJoinTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeInnerJoinTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeFromClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFromClauseTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeWhereClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWhereClauseTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeGroupByClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupByClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeHavingClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeHavingClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeQualifyClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeQualifyClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSampleClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSelectSampleClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSelectSampleClauseTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeWindowClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeWindowClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSampleEntryTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleEntryTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSampleEntryCountTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleEntryCountTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSampleEntryFunctionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleEntryFunctionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeSampleFunctionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleFunctionTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeSamplePropertiesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSamplePropertiesTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeRepeatableSampleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRepeatableSampleTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSampleSeedTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleSeedTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSampleCountTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleCountTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSampleValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleValueTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSampleUnitTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleUnitTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSamplePercentageTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSamplePercentageTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeSampleRowsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSampleRowsTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeGroupByExpressionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupByExpressionsTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeGroupByAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupByAllTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeGroupByListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupByListTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeGroupByExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupByExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeGroupByBaseExpressionTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupByBaseExpressionTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeEmptyGroupingItemTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeEmptyGroupingItemTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeCubeOrRollupClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCubeOrRollupClauseTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeCubeOrRollupTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCubeOrRollupTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeCubeKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCubeKeywordTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeRollupKeywordTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRollupKeywordTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeGroupingSetsClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGroupingSetsClauseTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSubqueryReferenceTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSubqueryReferenceTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeOrderByExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrderByExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeDescOrAscTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescOrAscTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeDescendingOrderTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDescendingOrderTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeAscendingOrderTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAscendingOrderTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeNullsFirstOrLastTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullsFirstOrLastTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeNullsFirstTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullsFirstTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeNullsLastTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNullsLastTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeOrderByClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrderByClauseTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeOrderByExpressionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrderByExpressionsTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeOrderByExpressionListTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrderByExpressionListTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeOrderByAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOrderByAllTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeLimitClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitClauseTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeOffsetClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOffsetClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeOffsetValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOffsetValueTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeLimitValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitValueTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeLimitAllTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitAllTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeLimitLiteralPercentTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitLiteralPercentTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeLimitExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLimitExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeFetchClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFetchClauseTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeFetchClauseWithoutValueTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeFetchClauseWithoutValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeFetchClauseWithValueTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFetchClauseWithValueTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeFetchValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeFetchValueTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeAliasedExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeAliasedExpressionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeColIdExpressionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeColIdExpressionTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeExpressionAsCollabelTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeExpressionAsCollabelTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeExpressionOptIdentifierTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeExpressionOptIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeValuesClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeValuesClauseTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeValuesExpressionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeValuesExpressionsTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeSetStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetStatementTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeSetKeywordStatementTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetKeywordStatementTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeVariableAssignmentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVariableAssignmentTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSetAssignmentOrTimeZoneTrampoline(PEGTransformer &transformer,
	                                                        GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeSetAssignmentOrTimeZoneTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeResetStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetStatementTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeResetTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetTargetTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeResetTimeZoneTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetTimeZoneTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeResetVariableOrSettingTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeResetVariableOrSettingTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeSetSchemaTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetSchemaTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeStandardAssignmentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeStandardAssignmentTrampoline(PEGTransformer &transformer,
	                                                                            GeneratedTransformProcess &process);
	static void InitializeSetVariableOrSettingTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetVariableOrSettingTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeSetTimeZoneTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetTimeZoneTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeZoneValueTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeZoneValueTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeZoneLocalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeZoneLocalTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeZoneDefaultTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeZoneDefaultTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeZoneStringLiteralTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeZoneStringLiteralTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeZoneIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeZoneIdentifierTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeZoneIntervalWithIntervalTrampoline(PEGTransformer &transformer,
	                                                         GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeZoneIntervalWithIntervalTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeZoneIntervalWithPrecisionTrampoline(PEGTransformer &transformer,
	                                                          GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue>
	FinalizeZoneIntervalWithPrecisionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static void InitializeSetSettingTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetSettingTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSetVariableTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetVariableTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeVariableScopeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVariableScopeTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeSettingScopeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSettingScopeTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeLocalScopeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeLocalScopeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeSessionScopeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSessionScopeTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeGlobalScopeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeGlobalScopeTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeSetAssignmentTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSetAssignmentTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeVariableListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVariableListTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeTransactionStatementTrampoline(PEGTransformer &transformer,
	                                                     GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeTransactionStatementTrampoline(PEGTransformer &transformer,
	                                                                              GeneratedTransformProcess &process);
	static void InitializeBeginTransactionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBeginTransactionTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeRollbackTransactionTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeRollbackTransactionTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeCommitTransactionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCommitTransactionTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeReadOrWriteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReadOrWriteTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeReadOnlyOrReadWriteTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReadOnlyOrReadWriteTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeReadOnlyTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReadOnlyTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static void InitializeReadWriteTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeReadWriteTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeUpdateStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeUpdateTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateTargetTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeBaseTableSetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBaseTableSetTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeBaseTableAliasSetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeBaseTableAliasSetTrampoline(PEGTransformer &transformer,
	                                                                           GeneratedTransformProcess &process);
	static void InitializeUpdateAliasTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateAliasTrampoline(PEGTransformer &transformer,
	                                                                     GeneratedTransformProcess &process);
	static void InitializeUpdateSetClauseListTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateSetClauseListTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeUpdateSetClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateSetClauseTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeUpdateSetTupleTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateSetTupleTrampoline(PEGTransformer &transformer,
	                                                                        GeneratedTransformProcess &process);
	static void InitializeUpdateSetElementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateSetElementTrampoline(PEGTransformer &transformer,
	                                                                          GeneratedTransformProcess &process);
	static void InitializeUpdateSetColumnTargetTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUpdateSetColumnTargetTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeUseStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUseStatementTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeUseTargetTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUseTargetTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeSchemaNameAsUseTargetTrampoline(PEGTransformer &transformer,
	                                                      GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeSchemaNameAsUseTargetTrampoline(PEGTransformer &transformer,
	                                                                               GeneratedTransformProcess &process);
	static void InitializeCatalogNameAsUseTargetTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeCatalogNameAsUseTargetTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeUseTargetCatalogSchemaTrampoline(PEGTransformer &transformer,
	                                                       GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeUseTargetCatalogSchemaTrampoline(PEGTransformer &transformer,
	                                                                                GeneratedTransformProcess &process);
	static void InitializeDotIdentifierTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeDotIdentifierTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeVacuumStatementTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVacuumStatementTrampoline(PEGTransformer &transformer,
	                                                                         GeneratedTransformProcess &process);
	static void InitializeVacuumOptionsTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVacuumOptionsTrampoline(PEGTransformer &transformer,
	                                                                       GeneratedTransformProcess &process);
	static void InitializeVacuumParensOptionsTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVacuumParensOptionsTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeVacuumLegacyOptionsTrampoline(PEGTransformer &transformer,
	                                                    GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVacuumLegacyOptionsTrampoline(PEGTransformer &transformer,
	                                                                             GeneratedTransformProcess &process);
	static void InitializeVacuumOptionTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeVacuumOptionTrampoline(PEGTransformer &transformer,
	                                                                      GeneratedTransformProcess &process);
	static void InitializeOptAnalyzeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOptAnalyzeTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeOptFullTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOptFullTrampoline(PEGTransformer &transformer,
	                                                                 GeneratedTransformProcess &process);
	static void InitializeOptFreezeTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOptFreezeTrampoline(PEGTransformer &transformer,
	                                                                   GeneratedTransformProcess &process);
	static void InitializeOptVerboseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeOptVerboseTrampoline(PEGTransformer &transformer,
	                                                                    GeneratedTransformProcess &process);
	static void InitializeNameListTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process);
	static arena_ptr<TransformResultValue> FinalizeNameListTrampoline(PEGTransformer &transformer,
	                                                                  GeneratedTransformProcess &process);
	static unique_ptr<SQLStatement> TransformAlterStatement(PEGTransformer &transformer,
	                                                        unique_ptr<AlterInfo> alter_options);
	static unique_ptr<AlterInfo> TransformAlterTableStmt(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                     unique_ptr<BaseTableRef> base_table_name,
	                                                     vector<unique_ptr<AlterTableInfo>> alter_table_options);
	static unique_ptr<AlterInfo> TransformAlterSchemaStmt(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                      const QualifiedName &qualified_name,
	                                                      unique_ptr<AlterTableInfo> alter_schema_options);
	static unique_ptr<AlterTableInfo> TransformAddConstraint(PEGTransformer &transformer,
	                                                         unique_ptr<Constraint> top_level_constraint);
	static unique_ptr<AlterTableInfo> TransformDropConstraint(PEGTransformer &transformer,
	                                                          const optional<bool> &if_exists,
	                                                          const Identifier &identifier,
	                                                          const optional<bool> &drop_behavior);
	static unique_ptr<AlterTableInfo> TransformAddColumn(PEGTransformer &transformer, const bool &has_result,
	                                                     const optional<bool> &if_not_exists,
	                                                     AddColumnEntry add_column_entry);
	static AddColumnEntry TransformAddColumnEntry(PEGTransformer &transformer, const vector<string> &dotted_identifier,
	                                              const optional<LogicalType> &type,
	                                              optional<GeneratedColumnDefinition> generated_column,
	                                              optional<vector<ColumnConstraintEntry>> column_constraint);
	static unique_ptr<AlterTableInfo> TransformDropColumn(PEGTransformer &transformer, const bool &has_result,
	                                                      const optional<bool> &if_exists,
	                                                      unique_ptr<ColumnRefExpression> nested_column_name,
	                                                      const optional<bool> &drop_behavior);
	static unique_ptr<AlterTableInfo> TransformAlterColumn(PEGTransformer &transformer, const bool &has_result,
	                                                       unique_ptr<ColumnRefExpression> nested_column_name,
	                                                       unique_ptr<AlterTableInfo> alter_column_entry);
	static unique_ptr<AlterTableInfo> TransformRenameColumn(PEGTransformer &transformer, const bool &has_result,
	                                                        unique_ptr<ColumnRefExpression> nested_column_name,
	                                                        const Identifier &identifier);
	static unique_ptr<ColumnRefExpression> TransformNestedColumnName(PEGTransformer &transformer,
	                                                                 const optional<vector<Identifier>> &identifier_dot,
	                                                                 const Identifier &column_name);
	static Identifier TransformIdentifierDot(PEGTransformer &transformer, const Identifier &identifier);
	static unique_ptr<AlterTableInfo> TransformRenameAlter(PEGTransformer &transformer, const Identifier &identifier);
	static unique_ptr<AlterTableInfo> TransformSetPartitionedBy(PEGTransformer &transformer,
	                                                            vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<AlterTableInfo> TransformResetPartitionedBy(PEGTransformer &transformer);
	static unique_ptr<AlterTableInfo> TransformSetSortedBy(PEGTransformer &transformer,
	                                                       vector<OrderByNode> order_by_expressions);
	static unique_ptr<AlterTableInfo> TransformResetSortedBy(PEGTransformer &transformer);
	static unique_ptr<AlterTableInfo>
	TransformSetOptions(PEGTransformer &transformer,
	                    case_insensitive_map_t<unique_ptr<ParsedExpression>> rel_option_list);
	static unique_ptr<AlterTableInfo>
	TransformResetOptions(PEGTransformer &transformer,
	                      case_insensitive_map_t<unique_ptr<ParsedExpression>> rel_option_list);
	static unique_ptr<AlterTableInfo> TransformAddDefault(PEGTransformer &transformer,
	                                                      unique_ptr<ParsedExpression> expression);
	static unique_ptr<AlterTableInfo> TransformDropDefault(PEGTransformer &transformer);
	static unique_ptr<AlterTableInfo> TransformChangeNullability(PEGTransformer &transformer,
	                                                             const string &drop_or_set);
	static string TransformDropNullability(PEGTransformer &transformer);
	static string TransformSetNullability(PEGTransformer &transformer);
	static unique_ptr<AlterTableInfo> TransformAlterType(PEGTransformer &transformer, const bool &has_result,
	                                                     const optional<LogicalType> &type,
	                                                     optional<ColumnConstraintEntry> column_collation,
	                                                     optional<unique_ptr<ParsedExpression>> using_expression);
	static unique_ptr<ParsedExpression> TransformUsingExpression(PEGTransformer &transformer,
	                                                             unique_ptr<ParsedExpression> expression);
	static unique_ptr<AlterInfo> TransformAlterViewStmt(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                    unique_ptr<BaseTableRef> base_table_name,
	                                                    unique_ptr<AlterTableInfo> rename_alter);
	static unique_ptr<AlterInfo> TransformAlterSequenceStmt(PEGTransformer &transformer,
	                                                        const optional<bool> &if_exists,
	                                                        const QualifiedName &qualified_sequence_name,
	                                                        unique_ptr<AlterInfo> alter_sequence_options);
	static QualifiedName TransformQualifiedSequenceName(PEGTransformer &transformer,
	                                                    const optional<Identifier> &catalog_qualification,
	                                                    const optional<Identifier> &schema_qualification,
	                                                    const Identifier &sequence_name);
	static unique_ptr<AlterInfo> TransformRenameAlterSequenceOptions(PEGTransformer &transformer,
	                                                                 unique_ptr<AlterTableInfo> rename_alter);
	static unique_ptr<AlterInfo>
	TransformSetSequenceOption(PEGTransformer &transformer,
	                           vector<pair<string, unique_ptr<SequenceOption>>> sequence_option);
	static unique_ptr<AlterInfo> TransformAlterDatabaseStmt(PEGTransformer &transformer,
	                                                        const optional<bool> &if_exists,
	                                                        const Identifier &identifier,
	                                                        const Identifier &identifier_1);
	static unique_ptr<SQLStatement> TransformAnalyzeStatement(PEGTransformer &transformer,
	                                                          const Identifier &analyze_keyword,
	                                                          const optional<bool> &analyze_verbose,
	                                                          optional<AnalyzeTarget> analyze_target);
	static AnalyzeTarget TransformAnalyzeTarget(PEGTransformer &transformer, unique_ptr<BaseTableRef> base_table_name,
	                                            const optional<vector<string>> &name_list);
	static bool TransformAnalyzeVerbose(PEGTransformer &transformer);
	static unique_ptr<SQLStatement>
	TransformAttachStatement(PEGTransformer &transformer, const optional<bool> &or_replace,
	                         const optional<bool> &if_not_exists, const bool &has_result,
	                         unique_ptr<ParsedExpression> database_path, const optional<Identifier> &attach_alias,
	                         const optional<vector<GenericCopyOption>> &attach_options);
	static unique_ptr<ParsedExpression> TransformDatabasePath(PEGTransformer &transformer,
	                                                          unique_ptr<ParsedExpression> expression);
	static Identifier TransformAttachAlias(PEGTransformer &transformer, const Identifier &col_id);
	static vector<GenericCopyOption> TransformAttachOptions(PEGTransformer &transformer,
	                                                        const vector<GenericCopyOption> &generic_copy_option_list);
	static unique_ptr<SQLStatement> TransformCallStatement(PEGTransformer &transformer,
	                                                       const QualifiedName &qualified_table_function,
	                                                       vector<FunctionArgument> table_function_arguments);
	static unique_ptr<SQLStatement> TransformCheckpointStatement(PEGTransformer &transformer,
	                                                             const optional<bool> &checkpoint_force,
	                                                             const optional<Identifier> &catalog_name);
	static bool TransformCheckpointForce(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformCommentStatement(PEGTransformer &transformer,
	                                                          const CatalogType &comment_on_type,
	                                                          const vector<string> &comment_target,
	                                                          const Value &comment_value);
	static vector<string> TransformCommentStringLiteralIdentifier(PEGTransformer &transformer,
	                                                              const Identifier &string_literal_identifier);
	static CatalogType TransformCommentTable(PEGTransformer &transformer);
	static CatalogType TransformCommentSequence(PEGTransformer &transformer);
	static CatalogType TransformCommentFunction(PEGTransformer &transformer);
	static CatalogType TransformCommentMacroTable(PEGTransformer &transformer);
	static CatalogType TransformCommentMacro(PEGTransformer &transformer);
	static CatalogType TransformCommentView(PEGTransformer &transformer);
	static CatalogType TransformCommentDatabase(PEGTransformer &transformer);
	static CatalogType TransformCommentIndex(PEGTransformer &transformer);
	static CatalogType TransformCommentSchema(PEGTransformer &transformer);
	static CatalogType TransformCommentType(PEGTransformer &transformer);
	static CatalogType TransformCommentColumn(PEGTransformer &transformer);
	static Value TransformStringLiteralValue(PEGTransformer &transformer, const string &string_literal);
	static Identifier TransformAnalyzeKeyword(PEGTransformer &transformer);
	static Identifier TransformConstraintName(PEGTransformer &transformer, const Identifier &col_id_or_string);
	static Identifier TransformCollationName(PEGTransformer &transformer, const Identifier &identifier);
	static unique_ptr<ParsedExpression> TransformNumberLiteral(PEGTransformer &transformer, ParseResult &parse_result);
	static string TransformStringLiteral(PEGTransformer &transformer, ParseResult &parse_result);
	static LogicalType TransformType(PEGTransformer &transformer, unique_ptr<ParsedExpression> type_variations,
	                                 const optional<vector<int64_t>> &array_bounds);
	static unique_ptr<ParsedExpression>
	TransformCharacterSimpleType(PEGTransformer &transformer,
	                             optional<vector<unique_ptr<ParsedExpression>>> type_modifiers);
	static unique_ptr<ParsedExpression>
	TransformQualifiedSimpleType(PEGTransformer &transformer, const QualifiedName &qualified_type_name,
	                             optional<vector<unique_ptr<ParsedExpression>>> type_modifiers);
	static unique_ptr<ParsedExpression>
	TransformIntervalWithRangeSpecifier(PEGTransformer &transformer,
	                                    const DatePartSpecifier &interval_to_interval_as_type);
	static unique_ptr<ParsedExpression> TransformIntervalWithSimpleSpecifier(PEGTransformer &transformer,
	                                                                         const DatePartSpecifier &interval);
	static unique_ptr<ParsedExpression> TransformIntervalWithoutSpecifier(PEGTransformer &transformer);
	static DatePartSpecifier TransformIntervalToIntervalAsType(PEGTransformer &transformer, ParseResult &parse_result);
	static DatePartSpecifier TransformYearKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformMonthKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformDayKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformHourKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformMinuteKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformSecondKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformMillisecondKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformMicrosecondKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformWeekKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformQuarterKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformDecadeKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformCenturyKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformMillenniumKeyword(PEGTransformer &transformer);
	static DatePartSpecifier TransformYearToMonth(PEGTransformer &transformer, const DatePartSpecifier &year_keyword,
	                                              const DatePartSpecifier &month_keyword);
	static DatePartSpecifier TransformDayToHour(PEGTransformer &transformer, const DatePartSpecifier &day_keyword,
	                                            const DatePartSpecifier &hour_keyword);
	static DatePartSpecifier TransformDayToMinute(PEGTransformer &transformer, const DatePartSpecifier &day_keyword,
	                                              const DatePartSpecifier &minute_keyword);
	static DatePartSpecifier TransformDayToSecond(PEGTransformer &transformer, const DatePartSpecifier &day_keyword,
	                                              const DatePartSpecifier &second_keyword);
	static DatePartSpecifier TransformHourToMinute(PEGTransformer &transformer, const DatePartSpecifier &hour_keyword,
	                                               const DatePartSpecifier &minute_keyword);
	static DatePartSpecifier TransformHourToSecond(PEGTransformer &transformer, const DatePartSpecifier &hour_keyword,
	                                               const DatePartSpecifier &second_keyword);
	static DatePartSpecifier TransformMinuteToSecond(PEGTransformer &transformer,
	                                                 const DatePartSpecifier &minute_keyword,
	                                                 const DatePartSpecifier &second_keyword);
	static unique_ptr<ParsedExpression> TransformBitType(PEGTransformer &transformer, const bool &has_result,
	                                                     optional<vector<unique_ptr<ParsedExpression>>> expression);
	static unique_ptr<ParsedExpression> TransformGeometryType(PEGTransformer &transformer,
	                                                          optional<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformVariantType(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformSimpleNumericType(PEGTransformer &transformer, const string &child);
	static string TransformIntType(PEGTransformer &transformer);
	static string TransformIntegerType(PEGTransformer &transformer);
	static string TransformSmallintType(PEGTransformer &transformer);
	static string TransformBigintType(PEGTransformer &transformer);
	static string TransformRealType(PEGTransformer &transformer);
	static string TransformBooleanType(PEGTransformer &transformer);
	static string TransformDoubleType(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformFloatType(PEGTransformer &transformer,
	                                                       optional<unique_ptr<ParsedExpression>> number_literal);
	static unique_ptr<ParsedExpression>
	TransformDecimalType(PEGTransformer &transformer, optional<vector<unique_ptr<ParsedExpression>>> type_modifiers);
	static unique_ptr<ParsedExpression> TransformDecType(PEGTransformer &transformer,
	                                                     optional<vector<unique_ptr<ParsedExpression>>> type_modifiers);
	static unique_ptr<ParsedExpression>
	TransformNumericModType(PEGTransformer &transformer, optional<vector<unique_ptr<ParsedExpression>>> type_modifiers);
	static QualifiedName TransformTypeNameAsQualifiedName(PEGTransformer &transformer, const Identifier &type_name);
	static QualifiedName TransformCatalogReservedSchemaTypeName(PEGTransformer &transformer,
	                                                            const Identifier &catalog_qualification,
	                                                            const vector<Identifier> &reserved_schema_qualification,
	                                                            const Identifier &reserved_type_name);
	static QualifiedName TransformSchemaReservedTypeName(PEGTransformer &transformer,
	                                                     const Identifier &schema_qualification,
	                                                     const Identifier &reserved_type_name);
	static vector<unique_ptr<ParsedExpression>>
	TransformTypeModifiers(PEGTransformer &transformer, optional<vector<unique_ptr<ParsedExpression>>> expression);
	static unique_ptr<ParsedExpression> TransformNestedType(PEGTransformer &transformer,
	                                                        unique_ptr<ParsedExpression> type_variations,
	                                                        const optional<vector<int64_t>> &array_bounds);
	static unique_ptr<ParsedExpression>
	TransformRowType(PEGTransformer &transformer,
	                 optional<vector<pair<Identifier, unique_ptr<ParsedExpression>>>> col_id_type_list);
	static unique_ptr<ParsedExpression> TransformSetofType(PEGTransformer &transformer,
	                                                       unique_ptr<ParsedExpression> nested_type);
	static unique_ptr<ParsedExpression>
	TransformUnionType(PEGTransformer &transformer,
	                   vector<pair<Identifier, unique_ptr<ParsedExpression>>> col_id_type_list);
	static vector<pair<Identifier, unique_ptr<ParsedExpression>>>
	TransformColIdTypeList(PEGTransformer &transformer,
	                       vector<pair<Identifier, unique_ptr<ParsedExpression>>> col_id_type);
	static unique_ptr<ParsedExpression> TransformMapType(PEGTransformer &transformer,
	                                                     optional<vector<unique_ptr<ParsedExpression>>> nested_type);
	static unique_ptr<ParsedExpression> TransformTupleType(PEGTransformer &transformer,
	                                                       vector<unique_ptr<ParsedExpression>> nested_type);
	static pair<Identifier, unique_ptr<ParsedExpression>>
	TransformColIdType(PEGTransformer &transformer, const Identifier &col_id, unique_ptr<ParsedExpression> nested_type);
	static int64_t TransformArrayKeyword(PEGTransformer &transformer);
	static int64_t TransformArrayKeywordWithBounds(PEGTransformer &transformer, const int64_t &square_brackets_array);
	static int64_t TransformSquareBracketsArray(PEGTransformer &transformer,
	                                            optional<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformTimeType(PEGTransformer &transformer,
	                                                      const LogicalTypeId &time_or_timestamp,
	                                                      optional<vector<unique_ptr<ParsedExpression>>> type_modifiers,
	                                                      const optional<bool> &time_zone);
	static LogicalTypeId TransformTimeTypeId(PEGTransformer &transformer);
	static LogicalTypeId TransformTimestampTypeId(PEGTransformer &transformer);
	static bool TransformTimeZone(PEGTransformer &transformer, const bool &with_or_without);
	static bool TransformWithRule(PEGTransformer &transformer);
	static bool TransformWithoutRule(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformConnectStatement(PEGTransformer &transformer,
	                                                          optional<unique_ptr<ConnectInfo>> session_target);
	static unique_ptr<SQLStatement> TransformDisconnectStatement(PEGTransformer &transformer);
	static unique_ptr<ConnectInfo> TransformLocalSessionTarget(PEGTransformer &transformer);
	static unique_ptr<ConnectInfo>
	TransformStringSessionTarget(PEGTransformer &transformer, const string &string_literal,
	                             const optional<vector<GenericCopyOption>> &generic_copy_option_list);
	static unique_ptr<ConnectInfo> TransformCatalogSessionTarget(PEGTransformer &transformer,
	                                                             const Identifier &catalog_name);
	static unique_ptr<SQLStatement> TransformCopyStatement(PEGTransformer &transformer,
	                                                       unique_ptr<SQLStatement> copy_variations);
	static unique_ptr<SQLStatement>
	TransformCopyTable(PEGTransformer &transformer, unique_ptr<BaseTableRef> base_table_name,
	                   const optional<vector<string>> &insert_column_list, const bool &from_or_to,
	                   unique_ptr<ParsedExpression> copy_file_name, const optional<GenericCopyOption> &legacy_option,
	                   const optional<vector<GenericCopyOption>> &copy_options);
	static bool TransformCopyFrom(PEGTransformer &transformer);
	static bool TransformCopyTo(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformCopySelect(PEGTransformer &transformer,
	                                                    unique_ptr<SelectStatement> select_statement_internal,
	                                                    unique_ptr<ParsedExpression> copy_file_name,
	                                                    const optional<vector<GenericCopyOption>> &copy_options);
	static unique_ptr<ParsedExpression> TransformCopyFileNameStringLiteral(PEGTransformer &transformer,
	                                                                       const string &string_literal);
	static unique_ptr<ParsedExpression> TransformCopyFileNameIdentifier(PEGTransformer &transformer,
	                                                                    const Identifier &identifier);
	static unique_ptr<ParsedExpression> TransformCopyFileNameIdentifierColId(PEGTransformer &transformer,
	                                                                         const Identifier &identifier_col_id);
	static Identifier TransformIdentifierColId(PEGTransformer &transformer, const Identifier &identifier,
	                                           const vector<Identifier> &copy_file_name_suffix);
	static GenericCopyOption TransformCopyLegacyOption(PEGTransformer &transformer, const bool &has_result,
	                                                   const string &delimiter);
	static vector<GenericCopyOption> TransformCopyOptions(PEGTransformer &transformer, const bool &has_result,
	                                                      const vector<GenericCopyOption> &copy_option_list);
	static vector<GenericCopyOption>
	TransformSpecializedOptionList(PEGTransformer &transformer, const GenericCopyOption &specialized_option,
	                               const optional<vector<GenericCopyOption>> &specialized_option_tail);
	static GenericCopyOption TransformSpecializedOptionTail(PEGTransformer &transformer, const bool &has_result,
	                                                        const GenericCopyOption &specialized_option);
	static GenericCopyOption TransformBinaryOption(PEGTransformer &transformer);
	static GenericCopyOption TransformFreezeOption(PEGTransformer &transformer);
	static GenericCopyOption TransformOidsOption(PEGTransformer &transformer);
	static GenericCopyOption TransformCsvOption(PEGTransformer &transformer);
	static GenericCopyOption TransformHeaderOption(PEGTransformer &transformer);
	static GenericCopyOption TransformNullAsOption(PEGTransformer &transformer, const bool &has_result,
	                                               const string &string_literal);
	static GenericCopyOption TransformDelimiterAsOption(PEGTransformer &transformer, const bool &has_result,
	                                                    const string &string_literal);
	static GenericCopyOption TransformQuoteAsOption(PEGTransformer &transformer, const bool &has_result,
	                                                const string &string_literal);
	static GenericCopyOption TransformEscapeAsOption(PEGTransformer &transformer, const bool &has_result,
	                                                 const string &string_literal);
	static GenericCopyOption TransformEncodingOption(PEGTransformer &transformer, const string &string_literal);
	static GenericCopyOption TransformForceQuoteOption(PEGTransformer &transformer, const optional<bool> &force_quote,
	                                                   const vector<string> &star_symbol_column_list);
	static bool TransformForceQuote(PEGTransformer &transformer);
	static GenericCopyOption TransformPartitionByOption(PEGTransformer &transformer,
	                                                    const vector<string> &partition_by_column_list);
	static vector<string> TransformStarPartitionByColumnList(PEGTransformer &transformer);
	static vector<string> TransformParenthesizedPartitionByColumnList(PEGTransformer &transformer,
	                                                                  const vector<string> &column_list);
	static vector<string> TransformSinglePartitionByColumnList(PEGTransformer &transformer, const Identifier &col_id);
	static GenericCopyOption TransformForceNullOption(PEGTransformer &transformer, const optional<bool> &force_not_null,
	                                                  const vector<string> &column_list);
	static bool TransformForceNotNull(PEGTransformer &transformer);
	static vector<GenericCopyOption>
	TransformCopyGenericOptionList(PEGTransformer &transformer, const vector<GenericCopyOption> &copy_generic_option);
	static GenericCopyOption TransformOrderByCopyOption(PEGTransformer &transformer,
	                                                    optional<GenericCopyOptionValue> generic_copy_option_value);
	static GenericCopyOption
	TransformPartitionedByCopyOption(PEGTransformer &transformer,
	                                 optional<GenericCopyOptionValue> generic_copy_option_value);
	static vector<GenericCopyOption>
	TransformGenericCopyOptionList(PEGTransformer &transformer, const vector<GenericCopyOption> &generic_copy_option);
	static GenericCopyOption TransformGenericCopyOption(PEGTransformer &transformer, const Identifier &copy_option_name,
	                                                    optional<GenericCopyOptionValue> generic_copy_option_value);
	static GenericCopyOptionValue
	TransformGenericCopyOptionOrderList(PEGTransformer &transformer,
	                                    vector<OrderByNode> generic_copy_option_parenthesized_expression_list);
	static GenericCopyOptionValue TransformGenericCopyOptionExpression(PEGTransformer &transformer,
	                                                                   unique_ptr<ParsedExpression> expression);
	static vector<OrderByNode>
	TransformGenericCopyOptionParenthesizedExpressionList(PEGTransformer &transformer,
	                                                      vector<OrderByNode> order_by_expression_list);
	static unique_ptr<SQLStatement> TransformCopyFromDatabaseWithFlag(PEGTransformer &transformer,
	                                                                  const Identifier &col_id,
	                                                                  const Identifier &col_id_1,
	                                                                  const CopyDatabaseType &copy_database_flag);
	static unique_ptr<SQLStatement> TransformCopyFromDatabaseWithoutFlag(PEGTransformer &transformer,
	                                                                     const Identifier &col_id,
	                                                                     const Identifier &col_id_1);
	static CopyDatabaseType TransformCopyDatabaseFlag(PEGTransformer &transformer,
	                                                  const CopyDatabaseType &schema_or_data);
	static CopyDatabaseType TransformCopySchema(PEGTransformer &transformer);
	static CopyDatabaseType TransformCopyData(PEGTransformer &transformer);
	static unique_ptr<CreateStatement>
	TransformCreateIndexStmt(PEGTransformer &transformer, const optional<bool> &unique_index, const bool &concurrently,
	                         const optional<bool> &if_not_exists, const optional<Identifier> &index_name,
	                         unique_ptr<BaseTableRef> base_table_name, const optional<Identifier> &index_type,
	                         vector<unique_ptr<ParsedExpression>> index_element,
	                         optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list,
	                         optional<unique_ptr<ParsedExpression>> where_clause);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>>
	TransformWithList(PEGTransformer &transformer,
	                  case_insensitive_map_t<unique_ptr<ParsedExpression>> rel_option_or_oids);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>>
	TransformRelOptionList(PEGTransformer &transformer,
	                       vector<pair<Identifier, unique_ptr<ParsedExpression>>> rel_option);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>> TransformOids(PEGTransformer &transformer,
	                                                                          const bool &with_or_without_oids);
	static bool TransformWithOids(PEGTransformer &transformer);
	static bool TransformWithoutOids(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformIndexElement(PEGTransformer &transformer,
	                                                          unique_ptr<ParsedExpression> expression,
	                                                          const optional<OrderType> &desc_or_asc,
	                                                          const optional<OrderByNullType> &nulls_first_or_last);
	static bool TransformUniqueIndex(PEGTransformer &transformer);
	static Identifier TransformIndexType(PEGTransformer &transformer, const Identifier &identifier);
	static pair<Identifier, unique_ptr<ParsedExpression>>
	TransformRelOption(PEGTransformer &transformer, const Identifier &rel_option_name,
	                   optional<unique_ptr<ParsedExpression>> rel_option_argument_opt);
	static Identifier TransformRelOptionName(PEGTransformer &transformer, const string &child);
	static string TransformDottedColLabel(PEGTransformer &transformer, const string &col_label,
	                                      const optional<vector<string>> &dot_col_label);
	static unique_ptr<ParsedExpression> TransformRelOptionArgumentOpt(PEGTransformer &transformer,
	                                                                  unique_ptr<ParsedExpression> def_arg);
	static unique_ptr<ParsedExpression> TransformDefArgNull(PEGTransformer &transformer, const Value &null_literal);
	static unique_ptr<ParsedExpression> TransformDefArgKeyword(PEGTransformer &transformer,
	                                                           const string &reserved_keyword);
	static unique_ptr<ParsedExpression> TransformDefArgStringLiteral(PEGTransformer &transformer,
	                                                                 const string &string_literal);
	static unique_ptr<ParsedExpression> TransformNoneLiteral(PEGTransformer &transformer);
	static unique_ptr<CreateStatement> TransformCreateMacroStmt(PEGTransformer &transformer,
	                                                            const bool &macro_or_function,
	                                                            const optional<bool> &if_not_exists,
	                                                            const QualifiedName &qualified_name,
	                                                            vector<unique_ptr<MacroFunction>> macro_definition);
	static bool TransformMacroKeyword(PEGTransformer &transformer);
	static bool TransformFunctionKeyword(PEGTransformer &transformer);
	static unique_ptr<MacroFunction> TransformMacroDefinition(PEGTransformer &transformer,
	                                                          optional<vector<MacroParameter>> macro_parameters,
	                                                          const optional<LogicalType> &returns_type,
	                                                          unique_ptr<MacroFunction> macro_definition_body);
	static vector<MacroParameter> TransformMacroParameters(PEGTransformer &transformer,
	                                                       vector<MacroParameter> macro_parameter);
	static MacroParameter TransformSimpleParameter(PEGTransformer &transformer, const Identifier &type_func_name,
	                                               const optional<LogicalType> &type);
	static unique_ptr<MacroFunction> TransformScalarMacroDefinition(PEGTransformer &transformer,
	                                                                unique_ptr<ParsedExpression> expression);
	static unique_ptr<MacroFunction>
	TransformTableMacroDefinition(PEGTransformer &transformer, unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<CreateStatement>
	TransformCreateSchemaStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
	                          const QualifiedName &qualified_name,
	                          optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_option_list);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>>
	TransformWithOptionList(PEGTransformer &transformer,
	                        case_insensitive_map_t<unique_ptr<ParsedExpression>> rel_option_list);
	static unique_ptr<CreateStatement>
	TransformCreateSecretStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
	                          const optional<Identifier> &secret_name,
	                          const optional<Identifier> &secret_storage_specifier,
	                          const vector<GenericCopyOption> &generic_copy_option_list);
	static Identifier TransformSecretStorageSpecifier(PEGTransformer &transformer, const Identifier &identifier);
	static Identifier TransformSecretName(PEGTransformer &transformer, const Identifier &col_id);
	static unique_ptr<CreateStatement>
	TransformCreateSequenceStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
	                            const QualifiedName &qualified_name,
	                            optional<vector<pair<string, unique_ptr<SequenceOption>>>> sequence_option);
	static pair<string, unique_ptr<SequenceOption>> TransformSeqCycle(PEGTransformer &transformer);
	static pair<string, unique_ptr<SequenceOption>> TransformSeqNoCycle(PEGTransformer &transformer);
	static pair<string, unique_ptr<SequenceOption>> TransformSeqSetIncrement(PEGTransformer &transformer,
	                                                                         const bool &has_result,
	                                                                         unique_ptr<ParsedExpression> expression);
	static pair<string, unique_ptr<SequenceOption>> TransformSeqSetMinMax(PEGTransformer &transformer,
	                                                                      const string &seq_min_or_max,
	                                                                      unique_ptr<ParsedExpression> expression);
	static pair<string, unique_ptr<SequenceOption>> TransformSeqNoMinMax(PEGTransformer &transformer,
	                                                                     const string &seq_min_or_max);
	static pair<string, unique_ptr<SequenceOption>>
	TransformSeqStartWith(PEGTransformer &transformer, const bool &has_result, unique_ptr<ParsedExpression> expression);
	static pair<string, unique_ptr<SequenceOption>> TransformSeqOwnedBy(PEGTransformer &transformer,
	                                                                    const QualifiedName &qualified_name);
	static string TransformMinValue(PEGTransformer &transformer);
	static string TransformMaxValue(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformCreateStatement(PEGTransformer &transformer,
	                                                         const optional<bool> &or_replace,
	                                                         const optional<SecretPersistType> &temporary,
	                                                         unique_ptr<CreateStatement> create_statement_variation);
	static bool TransformOrReplace(PEGTransformer &transformer);
	static SecretPersistType TransformPersistent(PEGTransformer &transformer);
	static SecretPersistType TransformTempPersistent(PEGTransformer &transformer);
	static SecretPersistType TransformTemporaryPersistent(PEGTransformer &transformer);
	static unique_ptr<CreateStatement> TransformCreateTableStmt(PEGTransformer &transformer,
	                                                            const optional<bool> &if_not_exists,
	                                                            const QualifiedName &qualified_name,
	                                                            CreateTableDefinition create_table_definition,
	                                                            const optional<bool> &commit_action);
	static CreateTableDefinition
	TransformCreateTableAs(PEGTransformer &transformer, optional<ColumnList> identifier_list,
	                       optional<PartitionSortedOptions> partition_sorted_options,
	                       optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list,
	                       unique_ptr<SQLStatement> statement, const optional<bool> &with_data);
	static PartitionSortedOptions
	TransformPartitionOptSortedOptions(PEGTransformer &transformer,
	                                   vector<unique_ptr<ParsedExpression>> partition_options,
	                                   optional<vector<unique_ptr<ParsedExpression>>> sorted_options);
	static PartitionSortedOptions
	TransformSortedOptPartitionOptions(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> sorted_options,
	                                   optional<vector<unique_ptr<ParsedExpression>>> partition_options);
	static vector<unique_ptr<ParsedExpression>>
	TransformPartitionOptions(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static vector<unique_ptr<ParsedExpression>> TransformSortedOptions(PEGTransformer &transformer,
	                                                                   vector<unique_ptr<ParsedExpression>> expression);
	static bool TransformWithDataOnly(PEGTransformer &transformer);
	static bool TransformWithNoData(PEGTransformer &transformer);
	static ColumnList TransformIdentifierList(PEGTransformer &transformer, const vector<Identifier> &identifier);
	static CreateTableDefinition
	TransformCreateColumnList(PEGTransformer &transformer, optional<ColumnElements> create_table_column_list,
	                          optional<PartitionSortedOptions> partition_sorted_options,
	                          optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list);
	static bool TransformIfNotExists(PEGTransformer &transformer);
	static QualifiedName
	TransformSchemaReservedIdentifierOrStringLiteral(PEGTransformer &transformer,
	                                                 const Identifier &schema_qualification,
	                                                 const Identifier &reserved_identifier_or_string_literal);
	static QualifiedName
	TransformCatalogReservedSchemaIdentifier(PEGTransformer &transformer, const Identifier &catalog_qualification,
	                                         const vector<Identifier> &reserved_schema_qualification,
	                                         const Identifier &reserved_identifier_or_string_literal);
	static QualifiedName TransformIdentifierOrStringLiteral(PEGTransformer &transformer, const string &child);
	static Identifier TransformCatalogQualification(PEGTransformer &transformer, const Identifier &catalog_name);
	static Identifier TransformSchemaQualification(PEGTransformer &transformer, const Identifier &schema_name);
	static Identifier TransformReservedSchemaQualification(PEGTransformer &transformer,
	                                                       const Identifier &reserved_schema_name);
	static Identifier TransformTableQualification(PEGTransformer &transformer, const Identifier &table_name);
	static Identifier TransformReservedTableQualification(PEGTransformer &transformer,
	                                                      const Identifier &reserved_table_name);
	static ColumnElements TransformCreateTableColumnList(PEGTransformer &transformer,
	                                                     vector<CreateTableColumnElement> create_table_column_element);
	static CreateTableColumnElement TransformCreateTableColumnDefinition(PEGTransformer &transformer,
	                                                                     ConstraintColumnDefinition column_definition);
	static CreateTableColumnElement TransformCreateTableConstraint(PEGTransformer &transformer,
	                                                               unique_ptr<Constraint> top_level_constraint);
	static ConstraintColumnDefinition
	TransformColumnDefinition(PEGTransformer &transformer, const Identifier &identifier,
	                          const optional<LogicalType> &type, optional<GeneratedColumnDefinition> generated_column,
	                          optional<vector<ColumnConstraintEntry>> column_constraint);
	static ColumnConstraintEntry TransformNameableColumnConstraint(PEGTransformer &transformer, const bool &has_result,
	                                                               ColumnConstraintEntry child);
	static ColumnConstraintEntry TransformNotNullConstraint(PEGTransformer &transformer, const bool &child);
	static bool TransformNullConstraint(PEGTransformer &transformer);
	static bool TransformNotNullColumnConstraint(PEGTransformer &transformer);
	static ColumnConstraintEntry TransformUniqueConstraint(PEGTransformer &transformer,
	                                                       const optional<ConstraintCheckMode> &constraint_timing);
	static ColumnConstraintEntry TransformPrimaryKeyConstraint(PEGTransformer &transformer,
	                                                           const optional<ConstraintCheckMode> &constraint_timing);
	static ConstraintCheckMode TransformImmediateConstraint(PEGTransformer &transformer);
	static ConstraintCheckMode TransformDeferredConstraint(PEGTransformer &transformer);
	static ColumnConstraintEntry TransformDefaultValue(PEGTransformer &transformer,
	                                                   unique_ptr<ParsedExpression> column_default_expr);
	static ColumnConstraintEntry TransformCheckConstraint(PEGTransformer &transformer,
	                                                      unique_ptr<ParsedExpression> expression);
	static ColumnConstraintEntry TransformForeignKeyConstraint(PEGTransformer &transformer,
	                                                           unique_ptr<BaseTableRef> base_table_name,
	                                                           const optional<vector<string>> &column_list,
	                                                           const optional<KeyActions> &key_actions);
	static ColumnConstraintEntry TransformColumnCollation(PEGTransformer &transformer,
	                                                      const vector<string> &dotted_identifier);
	static ColumnConstraintEntry TransformColumnCompression(PEGTransformer &transformer,
	                                                        const Identifier &col_id_or_string);
	static KeyActions TransformUpdateFirstKeyActions(PEGTransformer &transformer, const string &update_action,
	                                                 const optional<string> &delete_action);
	static KeyActions TransformDeleteFirstKeyActions(PEGTransformer &transformer, const string &delete_action,
	                                                 const optional<string> &update_action);
	static string TransformUpdateAction(PEGTransformer &transformer, const string &key_action);
	static string TransformDeleteAction(PEGTransformer &transformer, const string &key_action);
	static string TransformNoKeyAction(PEGTransformer &transformer);
	static string TransformRestrictKeyAction(PEGTransformer &transformer);
	static string TransformCascadeKeyAction(PEGTransformer &transformer);
	static string TransformSetNullKeyAction(PEGTransformer &transformer);
	static string TransformSetDefaultKeyAction(PEGTransformer &transformer);
	static unique_ptr<Constraint> TransformTopLevelConstraint(PEGTransformer &transformer, const bool &has_result,
	                                                          unique_ptr<Constraint> top_level_constraint_list);
	static unique_ptr<Constraint> TransformTopCheckConstraint(PEGTransformer &transformer,
	                                                          ColumnConstraintEntry check_constraint);
	static unique_ptr<Constraint>
	TransformTopPrimaryKeyConstraint(PEGTransformer &transformer, const vector<string> &column_id_list,
	                                 const optional<ConstraintCheckMode> &constraint_timing);
	static unique_ptr<Constraint> TransformTopUniqueConstraint(PEGTransformer &transformer,
	                                                           const vector<string> &column_id_list,
	                                                           const optional<ConstraintCheckMode> &constraint_timing);
	static unique_ptr<Constraint> TransformTopForeignKeyConstraint(PEGTransformer &transformer,
	                                                               const vector<string> &column_id_list,
	                                                               ColumnConstraintEntry foreign_key_constraint);
	static vector<string> TransformColumnIdList(PEGTransformer &transformer, const vector<Identifier> &col_id);
	static vector<string> TransformDottedIdentifier(PEGTransformer &transformer, const Identifier &identifier,
	                                                const optional<vector<string>> &dot_col_label);
	static string TransformDotColLabel(PEGTransformer &transformer, const string &col_label);
	static Identifier TransformColLabelIdentifier(PEGTransformer &transformer, const string &col_label);
	static Identifier TransformStringLiteralIdentifier(PEGTransformer &transformer, const string &string_literal);
	static GeneratedColumnDefinition TransformGeneratedColumn(PEGTransformer &transformer,
	                                                          const optional<bool> &generated_always,
	                                                          unique_ptr<ParsedExpression> expression,
	                                                          const optional<bool> &generated_column_type);
	static bool TransformGeneratedAlways(PEGTransformer &transformer);
	static bool TransformGeneratedByDefault(PEGTransformer &transformer);
	static bool TransformCommitAction(PEGTransformer &transformer, const bool &preserve_or_delete);
	static bool TransformPreserveRows(PEGTransformer &transformer);
	static bool TransformDeleteRows(PEGTransformer &transformer);
	static bool TransformVirtualGeneratedColumn(PEGTransformer &transformer);
	static bool TransformStoredGeneratedColumn(PEGTransformer &transformer);
	static unique_ptr<CreateStatement>
	TransformCreateTriggerStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
	                           const Identifier &trigger_name, const TriggerTiming &trigger_timing,
	                           const TriggerEventInfo &trigger_event, unique_ptr<BaseTableRef> base_table_name,
	                           const optional<TriggerTableReferencingInfo> &referencing_clause,
	                           const optional<TriggerForEach> &for_each_clause, unique_ptr<SQLStatement> trigger_body);
	static Identifier TransformTriggerName(PEGTransformer &transformer, const Identifier &identifier);
	static TriggerTableReferencingInfo
	TransformReferencingClause(PEGTransformer &transformer, const TriggerTableReferencingInfo &referencing_item,
	                           const optional<TriggerTableReferencingInfo> &referencing_item_1);
	static TriggerTableReferencingInfo TransformReferencingNewTableAs(PEGTransformer &transformer,
	                                                                  const Identifier &col_id);
	static TriggerTableReferencingInfo TransformReferencingOldTableAs(PEGTransformer &transformer,
	                                                                  const Identifier &col_id);
	static TriggerTiming TransformTriggerBefore(PEGTransformer &transformer);
	static TriggerTiming TransformTriggerAfter(PEGTransformer &transformer);
	static TriggerTiming TransformTriggerInsteadOf(PEGTransformer &transformer);
	static TriggerEventInfo TransformTriggerEventInsert(PEGTransformer &transformer);
	static TriggerEventInfo TransformTriggerEventDelete(PEGTransformer &transformer);
	static TriggerEventInfo TransformTriggerEventUpdate(PEGTransformer &transformer);
	static TriggerEventInfo TransformTriggerEventUpdateOf(PEGTransformer &transformer,
	                                                      const vector<string> &trigger_column_list);
	static vector<string> TransformTriggerColumnList(PEGTransformer &transformer, const vector<Identifier> &col_id);
	static TriggerForEach TransformForEachRow(PEGTransformer &transformer);
	static TriggerForEach TransformForEachStatement(PEGTransformer &transformer);
	static unique_ptr<CreateStatement> TransformCreateTypeStmt(PEGTransformer &transformer,
	                                                           const optional<bool> &if_not_exists,
	                                                           const QualifiedName &qualified_name,
	                                                           unique_ptr<CreateTypeInfo> create_type);
	static unique_ptr<CreateTypeInfo> TransformCreateTypeFromType(PEGTransformer &transformer, const LogicalType &type);
	static unique_ptr<CreateTypeInfo> TransformEnumSelectType(PEGTransformer &transformer,
	                                                          unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<CreateTypeInfo> TransformEnumStringLiteralList(PEGTransformer &transformer,
	                                                                 const optional<vector<string>> &string_literal,
	                                                                 const optional<vector<int64_t>> &array_bounds);
	static unique_ptr<CreateStatement>
	TransformCreateViewStmt(PEGTransformer &transformer, const optional<bool> &create_secure,
	                        const optional<bool> &create_recursive, const optional<bool> &if_not_exists,
	                        const QualifiedName &qualified_name, const optional<vector<string>> &insert_column_list,
	                        optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list,
	                        unique_ptr<SelectStatement> select_statement_internal);
	static bool TransformCreateRecursive(PEGTransformer &transformer);
	static bool TransformCreateSecure(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformDeallocateStatement(PEGTransformer &transformer,
	                                                             const optional<bool> &deallocate_prepare,
	                                                             const Identifier &identifier);
	static bool TransformDeallocatePrepare(PEGTransformer &transformer);
	static unique_ptr<SQLStatement>
	TransformDeleteStatement(PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
	                         unique_ptr<BaseTableRef> target_opt_alias,
	                         optional<vector<unique_ptr<TableRef>>> delete_using_clause,
	                         optional<unique_ptr<ParsedExpression>> where_clause,
	                         optional<vector<unique_ptr<ParsedExpression>>> returning_clause);
	static unique_ptr<SQLStatement> TransformTruncateStatement(PEGTransformer &transformer, const bool &has_result,
	                                                           unique_ptr<BaseTableRef> base_table_name,
	                                                           optional<bool> truncate_options);
	static bool TransformTruncateAliasOptions(PEGTransformer &transformer, const Identifier &target_alias,
	                                          optional<bool> truncate_drop_behavior);
	static bool TransformTruncateDropBehavior(PEGTransformer &transformer, const bool &drop_behavior);
	static unique_ptr<BaseTableRef> TransformTargetOptAlias(PEGTransformer &transformer,
	                                                        unique_ptr<BaseTableRef> base_table_name,
	                                                        const optional<Identifier> &target_alias);
	static Identifier TransformTargetAlias(PEGTransformer &transformer, const bool &has_result,
	                                       const Identifier &col_id);
	static vector<unique_ptr<TableRef>> TransformDeleteUsingClause(PEGTransformer &transformer,
	                                                               vector<unique_ptr<TableRef>> table_ref);
	static unique_ptr<SelectStatement> TransformDescribeStatement(PEGTransformer &transformer,
	                                                              unique_ptr<QueryNode> child);
	static unique_ptr<QueryNode> TransformShowDeprecatedSelect(PEGTransformer &transformer, const ShowType &show_rule,
	                                                           unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<QueryNode> TransformDescribeSelect(PEGTransformer &transformer,
	                                                     const ShowType &describe_or_summarize,
	                                                     unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<QueryNode> TransformShowAllTables(PEGTransformer &transformer, const ShowType &show_or_describe,
	                                                    const bool &has_result);
	static unique_ptr<QueryNode> TransformShowTables(PEGTransformer &transformer, const ShowType &show_or_describe,
	                                                 const QualifiedName &qualified_name);
	static unique_ptr<QueryNode> TransformShowByName(PEGTransformer &transformer, const ShowType &show_rule,
	                                                 optional<DescribeTarget> show_target);
	static unique_ptr<QueryNode> TransformDescribeByName(PEGTransformer &transformer,
	                                                     const ShowType &describe_or_summarize,
	                                                     optional<DescribeTarget> describe_target);
	static DescribeTarget TransformShowDeprecatedQualifiedTableName(PEGTransformer &transformer,
	                                                                unique_ptr<BaseTableRef> qualified_table_name);
	static DescribeTarget TransformShowSettingName(PEGTransformer &transformer, const Identifier &setting_name);
	static DescribeTarget TransformDescribeBaseTableName(PEGTransformer &transformer,
	                                                     unique_ptr<BaseTableRef> base_table_name);
	static DescribeTarget TransformDescribeStringLiteral(PEGTransformer &transformer, const string &string_literal);
	static ShowType TransformSummarize(PEGTransformer &transformer, const ShowType &summarize_rule);
	static ShowType TransformSummarizeRule(PEGTransformer &transformer);
	static ShowType TransformShowRule(PEGTransformer &transformer);
	static ShowType TransformDescribeLongRule(PEGTransformer &transformer);
	static ShowType TransformDescRule(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformDetachStatement(PEGTransformer &transformer, const bool &has_result,
	                                                         const optional<bool> &if_exists,
	                                                         const Identifier &catalog_name);
	static unique_ptr<SQLStatement> TransformDropStatement(PEGTransformer &transformer,
	                                                       unique_ptr<DropStatement> drop_entries,
	                                                       const optional<bool> &drop_behavior);
	static unique_ptr<DropStatement> TransformDropTrigger(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                      const Identifier &trigger_name,
	                                                      unique_ptr<BaseTableRef> base_table_name);
	static unique_ptr<DropStatement> TransformDropTable(PEGTransformer &transformer, const CatalogType &table_or_view,
	                                                    const optional<bool> &if_exists,
	                                                    vector<unique_ptr<BaseTableRef>> base_table_name);
	static unique_ptr<DropStatement> TransformDropTableFunction(PEGTransformer &transformer,
	                                                            const CatalogType &comment_macro_table,
	                                                            const optional<bool> &if_exists,
	                                                            const vector<QualifiedName> &table_function_name);
	static unique_ptr<DropStatement> TransformDropFunction(PEGTransformer &transformer, const bool &function_type_macro,
	                                                       const optional<bool> &if_exists,
	                                                       const vector<QualifiedName> &function_identifier);
	static unique_ptr<DropStatement> TransformDropSchema(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                     const vector<QualifiedName> &qualified_name);
	static unique_ptr<DropStatement> TransformDropIndex(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                    const vector<QualifiedName> &qualified_index_name);
	static QualifiedName TransformQualifiedIndexNameString(PEGTransformer &transformer, const Identifier &index_name);
	static QualifiedName TransformSchemaReservedIndex(PEGTransformer &transformer,
	                                                  const Identifier &schema_qualification,
	                                                  const Identifier &reserved_index_name);
	static QualifiedName TransformCatalogReservedSchemaIndex(PEGTransformer &transformer,
	                                                         const Identifier &catalog_qualification,
	                                                         const Identifier &reserved_schema_qualification,
	                                                         const Identifier &reserved_index_name);
	static unique_ptr<DropStatement> TransformDropSequence(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                       const vector<QualifiedName> &qualified_sequence_name);
	static unique_ptr<DropStatement> TransformDropCollation(PEGTransformer &transformer,
	                                                        const optional<bool> &if_exists,
	                                                        const vector<Identifier> &collation_name);
	static unique_ptr<DropStatement> TransformDropType(PEGTransformer &transformer, const optional<bool> &if_exists,
	                                                   const vector<QualifiedName> &qualified_type_name);
	static unique_ptr<DropStatement> TransformDropSecret(PEGTransformer &transformer,
	                                                     const optional<SecretPersistType> &temporary,
	                                                     const optional<bool> &if_exists, const Identifier &secret_name,
	                                                     const optional<Identifier> &drop_secret_storage);
	static CatalogType TransformMaterializedViewEntry(PEGTransformer &transformer);
	static bool TransformFunctionTypeMacroKeyword(PEGTransformer &transformer);
	static bool TransformFunctionTypeFunction(PEGTransformer &transformer);
	static bool TransformCascadeDropBehavior(PEGTransformer &transformer);
	static bool TransformRestrictDropBehavior(PEGTransformer &transformer);
	static bool TransformIfExists(PEGTransformer &transformer);
	static Identifier TransformDropSecretStorage(PEGTransformer &transformer, const Identifier &identifier);
	static unique_ptr<SQLStatement>
	TransformExecuteStatement(PEGTransformer &transformer, const Identifier &identifier,
	                          optional<vector<FunctionArgument>> table_function_arguments);
	static unique_ptr<SQLStatement>
	TransformExplainStatement(PEGTransformer &transformer, const optional<Identifier> &analyze_keyword,
	                          const optional<vector<GenericCopyOption>> &explain_option_list,
	                          unique_ptr<SQLStatement> explainable_statements);
	static vector<GenericCopyOption> TransformExplainOptionList(PEGTransformer &transformer,
	                                                            const vector<GenericCopyOption> &explain_option);
	static GenericCopyOption TransformExplainOption(PEGTransformer &transformer, const Identifier &explain_option_name,
	                                                optional<unique_ptr<ParsedExpression>> expression);
	static Identifier TransformExplainOptionName(PEGTransformer &transformer, ParseResult &choice_result);
	static unique_ptr<SQLStatement>
	TransformExplainSelectStatement(PEGTransformer &transformer, unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<SQLStatement> TransformExportStatement(PEGTransformer &transformer,
	                                                         const optional<string> &export_source,
	                                                         const string &string_literal,
	                                                         const optional<vector<GenericCopyOption>> &export_options);
	static string TransformExportSource(PEGTransformer &transformer, const Identifier &catalog_name);
	static unique_ptr<SQLStatement> TransformImportStatement(PEGTransformer &transformer, const string &string_literal);
	static unique_ptr<ParsedExpression> TransformColumnReference(PEGTransformer &transformer,
	                                                             unique_ptr<ColumnRefExpression> child);
	static unique_ptr<ColumnRefExpression> TransformNestedSchemaTableColumnName(
	    PEGTransformer &transformer, const Identifier &catalog_qualification,
	    const Identifier &reserved_schema_qualification, const Identifier &reserved_schema_qualification_1,
	    const vector<Identifier> &reserved_schema_qualification_2, const Identifier &reserved_column_name);
	static unique_ptr<ColumnRefExpression>
	TransformCatalogReservedSchemaTableColumnName(PEGTransformer &transformer, const Identifier &catalog_qualification,
	                                              const Identifier &reserved_schema_qualification,
	                                              const Identifier &reserved_table_qualification,
	                                              const Identifier &reserved_column_name);
	static unique_ptr<ColumnRefExpression>
	TransformSchemaReservedTableColumnName(PEGTransformer &transformer, const Identifier &schema_qualification,
	                                       const Identifier &reserved_table_qualification,
	                                       const Identifier &reserved_column_name);
	static unique_ptr<ColumnRefExpression> TransformTableReservedColumnName(PEGTransformer &transformer,
	                                                                        const Identifier &table_qualification,
	                                                                        const Identifier &reserved_column_name);
	static unique_ptr<ParsedExpression>
	TransformFunctionExpression(PEGTransformer &transformer, const QualifiedName &function_identifier,
	                            MethodArguments function_expression_arguments,
	                            optional<vector<OrderByNode>> within_group_clause,
	                            optional<unique_ptr<ParsedExpression>> filter_clause, const bool &has_result,
	                            optional<unique_ptr<WindowExpression>> over_clause);
	static MethodArguments TransformFunctionExpressionArguments(PEGTransformer &transformer,
	                                                            MethodArguments function_expression_argument_list);
	static MethodArguments
	TransformFunctionExpressionArgumentList(PEGTransformer &transformer, const optional<bool> &distinct_or_all,
	                                        optional<vector<FunctionArgument>> function_argument_list,
	                                        optional<vector<OrderByNode>> order_by_clause,
	                                        const optional<bool> &ignore_or_respect_nulls);
	static vector<FunctionArgument> TransformFunctionArgumentList(PEGTransformer &transformer,
	                                                              vector<FunctionArgument> function_argument);
	static QualifiedName TransformFunctionNameAsQualifiedName(PEGTransformer &transformer,
	                                                          const Identifier &function_name);
	static QualifiedName
	TransformCatalogReservedSchemaFunctionName(PEGTransformer &transformer, const Identifier &catalog_qualification,
	                                           const optional<vector<Identifier>> &reserved_schema_qualification,
	                                           const Identifier &reserved_function_name);
	static QualifiedName TransformSchemaReservedFunctionName(PEGTransformer &transformer,
	                                                         const Identifier &schema_qualification,
	                                                         const Identifier &reserved_function_name);
	static bool TransformDistinctKeyword(PEGTransformer &transformer);
	static bool TransformAllKeyword(PEGTransformer &transformer);
	static vector<OrderByNode> TransformWithinGroupClause(PEGTransformer &transformer,
	                                                      vector<OrderByNode> order_by_clause);
	static unique_ptr<ParsedExpression> TransformFilterClause(PEGTransformer &transformer,
	                                                          unique_ptr<ParsedExpression> filter_clause_expression);
	static unique_ptr<ParsedExpression>
	TransformFilterClauseExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> filter_clause_contents);
	static unique_ptr<ParsedExpression> TransformFilterClauseContents(PEGTransformer &transformer,
	                                                                  const bool &has_result,
	                                                                  unique_ptr<ParsedExpression> expression);
	static bool TransformIgnoreNulls(PEGTransformer &transformer);
	static bool TransformRespectNulls(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression>
	TransformParenthesisExpression(PEGTransformer &transformer,
	                               optional<vector<unique_ptr<ParsedExpression>>> expression);
	static unique_ptr<ParsedExpression> TransformLiteralExpression(PEGTransformer &transformer,
	                                                               ParseResult &choice_result);
	static unique_ptr<ParsedExpression> TransformConstantLiteral(PEGTransformer &transformer, const Value &child);
	static Value TransformNullLiteral(PEGTransformer &transformer);
	static Value TransformTrueLiteral(PEGTransformer &transformer);
	static Value TransformFalseLiteral(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression>
	TransformCastExpression(PEGTransformer &transformer, const bool &cast_or_try_cast, CastArguments cast_arguments);
	static CastArguments TransformCastArguments(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                                            const LogicalType &type);
	static bool TransformCastKeyword(PEGTransformer &transformer);
	static bool TransformTryCastKeyword(PEGTransformer &transformer);
	static string TransformColIdDot(PEGTransformer &transformer, const Identifier &col_id);
	static unique_ptr<ParsedExpression>
	TransformStarExpression(PEGTransformer &transformer, const optional<vector<string>> &star_qualifier_list,
	                        const optional<qualified_column_set_t> &exclude_list,
	                        optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> replace_list,
	                        const optional<qualified_column_map_t<string>> &rename_list);
	static vector<string> TransformStarQualifierList(PEGTransformer &transformer, const vector<string> &col_id_dot);
	static qualified_column_set_t TransformExcludeList(PEGTransformer &transformer,
	                                                   const qualified_column_set_t &exclude_names);
	static qualified_column_set_t TransformExcludeNameList(PEGTransformer &transformer,
	                                                       const vector<QualifiedColumnName> &exclude_name);
	static qualified_column_set_t TransformExcludeNameSingle(PEGTransformer &transformer,
	                                                         const QualifiedColumnName &exclude_name);
	static QualifiedColumnName TransformExcludeDottedName(PEGTransformer &transformer,
	                                                      const vector<string> &dotted_identifier);
	static QualifiedColumnName TransformExcludeColumnName(PEGTransformer &transformer,
	                                                      const Identifier &col_id_or_string);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>>
	TransformReplaceList(PEGTransformer &transformer,
	                     case_insensitive_map_t<unique_ptr<ParsedExpression>> replace_entries);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>>
	TransformReplaceEntrySingle(PEGTransformer &transformer, pair<string, unique_ptr<ParsedExpression>> replace_entry);
	static case_insensitive_map_t<unique_ptr<ParsedExpression>>
	TransformReplaceEntryList(PEGTransformer &transformer,
	                          vector<pair<string, unique_ptr<ParsedExpression>>> replace_entry);
	static pair<string, unique_ptr<ParsedExpression>>
	TransformReplaceEntry(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                      unique_ptr<ParsedExpression> column_reference);
	static qualified_column_map_t<string> TransformRenameList(PEGTransformer &transformer,
	                                                          const qualified_column_map_t<string> &rename_entries);
	static qualified_column_map_t<string>
	TransformRenameEntryList(PEGTransformer &transformer,
	                         const vector<pair<QualifiedColumnName, string>> &rename_entry);
	static qualified_column_map_t<string>
	TransformSingleRenameEntry(PEGTransformer &transformer, const pair<QualifiedColumnName, string> &rename_entry);
	static pair<QualifiedColumnName, string> TransformRenameEntry(PEGTransformer &transformer,
	                                                              const QualifiedColumnName &exclude_name,
	                                                              const Identifier &identifier);
	static unique_ptr<ParsedExpression> TransformSubqueryExpression(PEGTransformer &transformer,
	                                                                const optional<bool> &subquery_not,
	                                                                const optional<bool> &subquery_exists,
	                                                                unique_ptr<TableRef> subquery_reference);
	static bool TransformSubqueryNot(PEGTransformer &transformer);
	static bool TransformSubqueryExists(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformCaseExpression(PEGTransformer &transformer,
	                                                            optional<unique_ptr<ParsedExpression>> expression,
	                                                            vector<CaseCheck> case_when_then,
	                                                            optional<unique_ptr<ParsedExpression>> case_else);
	static CaseCheck TransformCaseWhenThen(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                                       unique_ptr<ParsedExpression> expression_1);
	static unique_ptr<ParsedExpression> TransformCaseElse(PEGTransformer &transformer,
	                                                      unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformTypeLiteral(PEGTransformer &transformer, const LogicalType &type,
	                                                         const string &string_literal);
	static unique_ptr<ParsedExpression> TransformIntervalLiteral(PEGTransformer &transformer,
	                                                             unique_ptr<ParsedExpression> interval_parameter,
	                                                             const optional<DatePartSpecifier> &interval);
	static unique_ptr<ParsedExpression> TransformIntervalStringParameter(PEGTransformer &transformer,
	                                                                     const string &string_literal);
	static WindowFrame TransformFrameClause(PEGTransformer &transformer, const string &framing,
	                                        vector<WindowBoundaryExpression> frame_extent,
	                                        const optional<WindowExcludeMode> &window_exclude_clause);
	static string TransformRowsFraming(PEGTransformer &transformer);
	static string TransformRangeFraming(PEGTransformer &transformer);
	static string TransformGroupsFraming(PEGTransformer &transformer);
	static vector<WindowBoundaryExpression> TransformSingleFrameExtent(PEGTransformer &transformer,
	                                                                   WindowBoundaryExpression frame_bound);
	static vector<WindowBoundaryExpression> TransformBetweenFrameExtent(PEGTransformer &transformer,
	                                                                    WindowBoundaryExpression frame_bound,
	                                                                    WindowBoundaryExpression frame_bound_1);
	static WindowBoundaryExpression TransformFrameUnbounded(PEGTransformer &transformer,
	                                                        const bool &preceding_or_following);
	static WindowBoundaryExpression TransformFrameExpression(PEGTransformer &transformer,
	                                                         unique_ptr<ParsedExpression> expression,
	                                                         const bool &preceding_or_following);
	static WindowBoundaryExpression TransformFrameCurrentRow(PEGTransformer &transformer);
	static bool TransformPrecedingFrame(PEGTransformer &transformer);
	static bool TransformFollowingFrame(PEGTransformer &transformer);
	static WindowExcludeMode TransformWindowExcludeClause(PEGTransformer &transformer,
	                                                      const WindowExcludeMode &window_exclude_element);
	static WindowExcludeMode TransformExcludeCurrentRow(PEGTransformer &transformer);
	static WindowExcludeMode TransformExcludeGroup(PEGTransformer &transformer);
	static WindowExcludeMode TransformExcludeTies(PEGTransformer &transformer);
	static WindowExcludeMode TransformExcludeNoOthers(PEGTransformer &transformer);
	static unique_ptr<WindowExpression> TransformIdentifierWindowFrame(PEGTransformer &transformer,
	                                                                   const Identifier &identifier);
	static unique_ptr<WindowExpression> TransformParensIdentifier(PEGTransformer &transformer,
	                                                              const Identifier &identifier);
	static unique_ptr<WindowExpression>
	TransformWindowFrameNameContentsParens(PEGTransformer &transformer,
	                                       unique_ptr<WindowExpression> window_frame_name_contents);
	static unique_ptr<WindowExpression>
	TransformWindowFrameNameContents(PEGTransformer &transformer, const optional<Identifier> &base_window_name,
	                                 unique_ptr<WindowExpression> window_frame_contents);
	static unique_ptr<WindowExpression>
	TransformWindowFrameContentsParens(PEGTransformer &transformer, unique_ptr<WindowExpression> window_frame_contents);
	static unique_ptr<WindowExpression>
	TransformWindowFrameContents(PEGTransformer &transformer,
	                             optional<vector<unique_ptr<ParsedExpression>>> window_partition,
	                             optional<vector<OrderByNode>> order_by_clause, optional<WindowFrame> frame_clause);
	static Identifier TransformBaseWindowName(PEGTransformer &transformer, const Identifier &identifier);
	static vector<unique_ptr<ParsedExpression>>
	TransformWindowPartition(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression>
	TransformArrayBoundedListExpression(PEGTransformer &transformer, const bool &has_result,
	                                    vector<unique_ptr<ParsedExpression>> bounded_list_expression);
	static unique_ptr<ParsedExpression>
	TransformArrayParensSelect(PEGTransformer &transformer, unique_ptr<SelectStatement> select_statement_internal);
	static vector<unique_ptr<ParsedExpression>>
	TransformBoundedListExpression(PEGTransformer &transformer,
	                               optional<vector<unique_ptr<ParsedExpression>>> expression);
	static unique_ptr<ParsedExpression> TransformStructExpression(PEGTransformer &transformer,
	                                                              optional<vector<FunctionArgument>> struct_field);
	static FunctionArgument TransformStructField(PEGTransformer &transformer, const Identifier &col_id_or_string,
	                                             unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression>
	TransformMapExpression(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> map_struct_expression);
	static vector<unique_ptr<ParsedExpression>>
	TransformMapStructExpression(PEGTransformer &transformer,
	                             optional<vector<vector<unique_ptr<ParsedExpression>>>> map_struct_field);
	static vector<unique_ptr<ParsedExpression>> TransformMapStructField(PEGTransformer &transformer,
	                                                                    unique_ptr<ParsedExpression> expression,
	                                                                    unique_ptr<ParsedExpression> expression_1);
	static unique_ptr<ParsedExpression>
	TransformGroupingExpression(PEGTransformer &transformer, const bool &grouping_or_grouping_id,
	                            optional<vector<unique_ptr<ParsedExpression>>> expression);
	static bool TransformGroupingKeyword(PEGTransformer &transformer);
	static bool TransformGroupingIdKeyword(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression>
	TransformQuestionMarkNumberedParameter(PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal);
	static unique_ptr<ParsedExpression> TransformAnonymousParameter(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformNumberedParameter(PEGTransformer &transformer,
	                                                               unique_ptr<ParsedExpression> number_literal);
	static unique_ptr<ParsedExpression> TransformColLabelParameter(PEGTransformer &transformer,
	                                                               const string &col_label);
	static unique_ptr<ParsedExpression> TransformPositionalExpression(PEGTransformer &transformer,
	                                                                  unique_ptr<ParsedExpression> number_literal);
	static unique_ptr<ParsedExpression> TransformDefaultExpression(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression>
	TransformListComprehensionExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                                     const vector<Identifier> &col_id_or_string,
	                                     unique_ptr<ParsedExpression> expression_1,
	                                     optional<unique_ptr<ParsedExpression>> list_comprehension_filter);
	static unique_ptr<ParsedExpression> TransformListComprehensionFilter(PEGTransformer &transformer,
	                                                                     unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformParensExpression(PEGTransformer &transformer,
	                                                              unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression>
	TransformMeasureSemanticsExpression(PEGTransformer &transformer, const bool &measure_semantics,
	                                    unique_ptr<ParsedExpression> function_expression);
	static unique_ptr<ParsedExpression> TransformColumnDefaultExpr(PEGTransformer &transformer,
	                                                               unique_ptr<ParsedExpression> col_def_or_expr);
	static unique_ptr<ParsedExpression>
	TransformLambdaArrowExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_or_expression,
	                               optional<vector<unique_ptr<ParsedExpression>>> single_arrow_pair);
	static unique_ptr<ParsedExpression> TransformSingleArrowPair(PEGTransformer &transformer,
	                                                             unique_ptr<ParsedExpression> logical_or_expression);
	static unique_ptr<ParsedExpression>
	TransformLogicalOrExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_and_expression,
	                             optional<vector<unique_ptr<ParsedExpression>>> logical_or_expression_tail);
	static unique_ptr<ParsedExpression>
	TransformLogicalOrExpressionTail(PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_and_expression);
	static unique_ptr<ParsedExpression>
	TransformColDefOrExpr(PEGTransformer &transformer, unique_ptr<ParsedExpression> col_def_and_expr,
	                      optional<vector<unique_ptr<ParsedExpression>>> col_def_or_expression_tail);
	static unique_ptr<ParsedExpression> TransformColDefOrExpressionTail(PEGTransformer &transformer,
	                                                                    unique_ptr<ParsedExpression> col_def_and_expr);
	static unique_ptr<ParsedExpression>
	TransformLogicalAndExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_not_expression,
	                              optional<vector<unique_ptr<ParsedExpression>>> logical_and_expression_tail);
	static unique_ptr<ParsedExpression>
	TransformLogicalAndExpressionTail(PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_not_expression);
	static unique_ptr<ParsedExpression>
	TransformColDefAndExpr(PEGTransformer &transformer, unique_ptr<ParsedExpression> is_distinct_from_expression,
	                       optional<vector<unique_ptr<ParsedExpression>>> col_def_and_expression_tail);
	static unique_ptr<ParsedExpression>
	TransformColDefAndExpressionTail(PEGTransformer &transformer,
	                                 unique_ptr<ParsedExpression> is_distinct_from_expression);
	static unique_ptr<ParsedExpression> TransformLogicalNotExpression(PEGTransformer &transformer,
	                                                                  const optional<vector<bool>> &not_expression,
	                                                                  unique_ptr<ParsedExpression> is_expression);
	static vector<bool> TransformNotExpression(PEGTransformer &transformer, const vector<bool> &not_keyword);
	static bool TransformNotKeyword(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression>
	TransformIsExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> is_distinct_from_expression,
	                      optional<vector<IsExpressionTail>> is_expression_continuation);
	static vector<IsExpressionTail>
	TransformIsExpressionContinuation(PEGTransformer &transformer, unique_ptr<ParsedExpression> is_test,
	                                  optional<vector<IsExpressionTail>> is_expression_tail);
	static IsExpressionTail TransformIsTestTail(PEGTransformer &transformer, unique_ptr<ParsedExpression> is_test);
	static IsExpressionTail TransformIsDistinctTail(PEGTransformer &transformer,
	                                                IsDistinctFromTail is_distinct_from_tail);
	static IsExpressionTail TransformIsComparisonTail(PEGTransformer &transformer,
	                                                  ComparisonExpressionTail comparison_expression_tail);
	static IsExpressionTail TransformIsOtherOperatorTail(PEGTransformer &transformer,
	                                                     OtherOperatorTail other_operator_tail);
	static IsExpressionTail TransformIsCastTail(PEGTransformer &transformer,
	                                            unique_ptr<ParsedExpression> cast_operator);
	static unique_ptr<ParsedExpression> TransformIsLiteral(PEGTransformer &transformer, const bool &has_result,
	                                                       const Value &is_literal_value);
	static Value TransformUnknownLiteral(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformNotNullKeyword(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformNotNullOperator(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformIsNull(PEGTransformer &transformer,
	                                                    unique_ptr<ParsedExpression> is_null_operator);
	static unique_ptr<ParsedExpression> TransformIsNullOperator(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression>
	TransformIsDistinctFromExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> comparison_expression,
	                                  optional<vector<IsDistinctFromTail>> is_distinct_from_tail);
	static IsDistinctFromTail TransformIsDistinctFromTail(PEGTransformer &transformer,
	                                                      const ExpressionType &is_distinct_from_op,
	                                                      unique_ptr<ParsedExpression> comparison_expression);
	static ExpressionType TransformIsDistinctFromOp(PEGTransformer &transformer, const bool &has_result);
	static unique_ptr<ParsedExpression>
	TransformComparisonExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> between_in_like_expression,
	                              optional<vector<ComparisonExpressionTail>> comparison_expression_tail);
	static ComparisonExpressionTail TransformComparisonExpressionTail(PEGTransformer &transformer,
	                                                                  const ExpressionType &comparison_operator,
	                                                                  unique_ptr<ParsedExpression> comparison_operand);
	static unique_ptr<ParsedExpression> TransformNotComparisonOperand(PEGTransformer &transformer,
	                                                                  const vector<bool> &not_expression,
	                                                                  unique_ptr<ParsedExpression> is_expression);
	static ExpressionType TransformOperatorEqual(PEGTransformer &transformer);
	static ExpressionType TransformOperatorNotEqual(PEGTransformer &transformer);
	static ExpressionType TransformOperatorLessThan(PEGTransformer &transformer);
	static ExpressionType TransformOperatorGreaterThan(PEGTransformer &transformer);
	static ExpressionType TransformOperatorLessThanEquals(PEGTransformer &transformer);
	static ExpressionType TransformOperatorGreaterThanEquals(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformBetweenInLikeExpression(
	    PEGTransformer &transformer, unique_ptr<ParsedExpression> other_operator_expression,
	    optional<vector<BetweenInLikeOperator>> in_predicate, optional<BetweenInLikeOperator> between_like_op);
	static BetweenInLikeOperator TransformInPredicate(PEGTransformer &transformer, const bool &has_result,
	                                                  unique_ptr<ParsedExpression> in_clause,
	                                                  optional<vector<unique_ptr<ParsedExpression>>> cast_operator);
	static BetweenInLikeOperator TransformBetweenLikeOp(PEGTransformer &transformer, const bool &has_result,
	                                                    unique_ptr<ParsedExpression> between_like_op_expression);
	static unique_ptr<ParsedExpression> TransformLikeClause(PEGTransformer &transformer, const string &like_variations,
	                                                        unique_ptr<ParsedExpression> other_operator_expression,
	                                                        optional<unique_ptr<ParsedExpression>> escape_clause);
	static unique_ptr<ParsedExpression> TransformEscapeClause(PEGTransformer &transformer,
	                                                          unique_ptr<ParsedExpression> other_operator_expression);
	static string TransformLikeToken(PEGTransformer &transformer);
	static string TransformILikeToken(PEGTransformer &transformer);
	static string TransformGlobToken(PEGTransformer &transformer);
	static string TransformSimilarToToken(PEGTransformer &transformer);
	static string TransformRegexMatchToken(PEGTransformer &transformer);
	static string TransformRegexInsensitiveMatchToken(PEGTransformer &transformer);
	static string TransformNotILikeOp(PEGTransformer &transformer);
	static string TransformNotLikeOp(PEGTransformer &transformer);
	static string TransformNotRegexInsensitiveMatchOp(PEGTransformer &transformer);
	static string TransformNotSimilarToOp(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformInClause(PEGTransformer &transformer,
	                                                      unique_ptr<ParsedExpression> in_expression);
	static unique_ptr<ParsedExpression>
	TransformInContainsExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> other_operator_expression);
	static unique_ptr<ParsedExpression> TransformInExpressionList(PEGTransformer &transformer,
	                                                              vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression>
	TransformInSelectStatement(PEGTransformer &transformer, unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<ParsedExpression>
	TransformBetweenClause(PEGTransformer &transformer, const bool &,
	                       unique_ptr<ParsedExpression> other_operator_expression,
	                       unique_ptr<ParsedExpression> other_operator_expression_1);
	static unique_ptr<ParsedExpression>
	TransformInfixOtherOperatorExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> bitwise_expression,
	                                      optional<vector<OtherOperatorTail>> other_operator_tail);
	static OtherOperatorTail TransformOtherOperatorTail(PEGTransformer &transformer, ParsedOperator other_operator,
	                                                    unique_ptr<ParsedExpression> bitwise_expression);
	static ParsedOperator TransformAnyAllParsedOperator(PEGTransformer &transformer,
	                                                    const pair<string, bool> &any_all_operator);
	static ParsedOperator TransformNamedOtherOperator(PEGTransformer &transformer, const string &child);
	static pair<string, bool> TransformAnyAllOperator(PEGTransformer &transformer, const string &any_operator_literal,
	                                                  const bool &any_or_all);
	static bool TransformSubqueryAny(PEGTransformer &transformer);
	static bool TransformSubqueryAll(PEGTransformer &transformer);
	static string TransformQualifiedOperator(PEGTransformer &transformer, const string &qualified_operator_contents);
	static string TransformQualifiedOperatorContents(PEGTransformer &transformer,
	                                                 const optional<vector<string>> &col_id_dot,
	                                                 const string &any_operator_literal);
	static unique_ptr<ParsedExpression>
	TransformBitwiseExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> tilde_expression,
	                           optional<vector<BinaryExpressionTail>> bitwise_expression_tail);
	static BinaryExpressionTail TransformBitwiseExpressionTail(PEGTransformer &transformer, const string &bit_operator,
	                                                           unique_ptr<ParsedExpression> tilde_expression);
	static unique_ptr<ParsedExpression> TransformTildeExpression(PEGTransformer &transformer,
	                                                             optional<vector<string>> tilde_prefix_operator,
	                                                             unique_ptr<ParsedExpression> additive_expression);
	static unique_ptr<ParsedExpression>
	TransformAdditiveExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> multiplicative_expression,
	                            optional<vector<BinaryExpressionTail>> additive_expression_tail);
	static BinaryExpressionTail TransformAdditiveExpressionTail(PEGTransformer &transformer, const string &term,
	                                                            unique_ptr<ParsedExpression> multiplicative_expression,
	                                                            optional_idx query_location);
	static unique_ptr<ParsedExpression>
	TransformMultiplicativeExpression(PEGTransformer &transformer,
	                                  unique_ptr<ParsedExpression> exponentiation_expression,
	                                  optional<vector<BinaryExpressionTail>> multiplicative_expression_tail);
	static BinaryExpressionTail
	TransformMultiplicativeExpressionTail(PEGTransformer &transformer, const string &factor,
	                                      unique_ptr<ParsedExpression> exponentiation_expression);
	static unique_ptr<ParsedExpression>
	TransformExponentiationExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> collate_expression,
	                                  optional<vector<BinaryExpressionTail>> exponentiation_expression_tail);
	static BinaryExpressionTail TransformExponentiationExpressionTail(PEGTransformer &transformer,
	                                                                  const string &exponent_operator,
	                                                                  unique_ptr<ParsedExpression> collate_expression);
	static unique_ptr<ParsedExpression>
	TransformCollateExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> at_time_zone_expression,
	                           optional<vector<unique_ptr<ParsedExpression>>> collate_expression_tail);
	static unique_ptr<ParsedExpression>
	TransformCollateExpressionTail(PEGTransformer &transformer, unique_ptr<ParsedExpression> at_time_zone_expression);
	static unique_ptr<ParsedExpression>
	TransformAtTimeZoneExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> prefix_expression,
	                              optional<vector<unique_ptr<ParsedExpression>>> at_time_zone_expression_tail);
	static unique_ptr<ParsedExpression>
	TransformAtTimeZoneExpressionTail(PEGTransformer &transformer, unique_ptr<ParsedExpression> prefix_expression);
	static unique_ptr<ParsedExpression>
	TransformCustomPrefixExpression(PEGTransformer &transformer, const string &operator_literal,
	                                unique_ptr<ParsedExpression> other_operator_expression);
	static unique_ptr<ParsedExpression>
	TransformBaseExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> single_expression,
	                        optional<vector<unique_ptr<ParsedExpression>>> indirection_list);
	static vector<unique_ptr<ParsedExpression>>
	TransformIndirectionList(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> indirection);
	static unique_ptr<ParsedExpression> TransformCastOperator(PEGTransformer &transformer, const LogicalType &type);
	static unique_ptr<ParsedExpression> TransformDotMethodOperator(PEGTransformer &transformer,
	                                                               unique_ptr<ParsedExpression> method_expression);
	static unique_ptr<ParsedExpression> TransformDotColumnOperator(PEGTransformer &transformer,
	                                                               const string &col_label);
	static unique_ptr<ParsedExpression> TransformMethodExpression(PEGTransformer &transformer, const string &col_label,
	                                                              MethodArguments method_expression_arguments);
	static MethodArguments TransformMethodExpressionArguments(PEGTransformer &transformer,
	                                                          MethodArguments method_expression_argument_list);
	static MethodArguments
	TransformMethodExpressionArgumentList(PEGTransformer &transformer, const optional<bool> &distinct_or_all,
	                                      optional<vector<FunctionArgument>> method_function_arguments,
	                                      optional<vector<OrderByNode>> order_by_clause,
	                                      const optional<bool> &ignore_or_respect_nulls);
	static vector<FunctionArgument> TransformMethodFunctionArguments(PEGTransformer &transformer,
	                                                                 vector<FunctionArgument> function_argument);
	static unique_ptr<ParsedExpression> TransformSliceExpression(PEGTransformer &transformer,
	                                                             vector<unique_ptr<ParsedExpression>> slice_bound);
	static vector<unique_ptr<ParsedExpression>>
	TransformSliceBound(PEGTransformer &transformer, optional<unique_ptr<ParsedExpression>> expression,
	                    optional<vector<unique_ptr<ParsedExpression>>> slice_upper_bound);
	static vector<unique_ptr<ParsedExpression>>
	TransformMinusStepSliceBound(PEGTransformer &transformer, unique_ptr<ParsedExpression> step_slice_bound);
	static vector<unique_ptr<ParsedExpression>>
	TransformEndStepSliceBound(PEGTransformer &transformer, unique_ptr<ParsedExpression> end_slice_bound,
	                           optional<unique_ptr<ParsedExpression>> step_slice_bound);
	static unique_ptr<ParsedExpression> TransformEndSliceBound(PEGTransformer &transformer,
	                                                           optional<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformStepSliceBound(PEGTransformer &transformer,
	                                                            optional<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformPostfixOperator(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformCoalesceExpression(PEGTransformer &transformer,
	                                                                vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformUnpackExpression(PEGTransformer &transformer,
	                                                              unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformTryExpression(PEGTransformer &transformer,
	                                                           unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformColumnsExpression(PEGTransformer &transformer, const bool &has_result,
	                                                               unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression>
	TransformExtractExpression(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> extract_arguments);
	static vector<unique_ptr<ParsedExpression>> TransformExtractArguments(PEGTransformer &transformer,
	                                                                      unique_ptr<ParsedExpression> extract_argument,
	                                                                      unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformLambdaExpression(PEGTransformer &transformer,
	                                                              const vector<Identifier> &col_id_or_string,
	                                                              unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression>
	TransformNullIfExpression(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> null_if_arguments);
	static vector<unique_ptr<ParsedExpression>> TransformNullIfArguments(PEGTransformer &transformer,
	                                                                     unique_ptr<ParsedExpression> expression,
	                                                                     unique_ptr<ParsedExpression> expression_1);
	static unique_ptr<ParsedExpression>
	TransformPositionExpression(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> position_arguments);
	static vector<unique_ptr<ParsedExpression>>
	TransformPositionArguments(PEGTransformer &transformer, unique_ptr<ParsedExpression> other_operator_expression,
	                           unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression>
	TransformRowExpression(PEGTransformer &transformer, optional<vector<unique_ptr<ParsedExpression>>> expression);
	static unique_ptr<ParsedExpression>
	TransformSubstringExpression(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> substring_arguments);
	static vector<unique_ptr<ParsedExpression>>
	TransformSubstringExpressionList(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static vector<unique_ptr<ParsedExpression>>
	TransformSubstringParameters(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                             vector<unique_ptr<ParsedExpression>> substring_from_for);
	static vector<unique_ptr<ParsedExpression>>
	TransformSubstringFromOptionalFor(PEGTransformer &transformer, unique_ptr<ParsedExpression> from_expression,
	                                  optional<unique_ptr<ParsedExpression>> for_expression);
	static vector<unique_ptr<ParsedExpression>> TransformSubstringFor(PEGTransformer &transformer,
	                                                                  unique_ptr<ParsedExpression> for_expression);
	static unique_ptr<ParsedExpression> TransformTrimExpression(PEGTransformer &transformer,
	                                                            TrimArguments trim_arguments);
	static TrimArguments TransformTrimArguments(PEGTransformer &transformer, const optional<string> &trim_direction,
	                                            optional<unique_ptr<ParsedExpression>> trim_source,
	                                            vector<unique_ptr<ParsedExpression>> expression);
	static string TransformTrimBoth(PEGTransformer &transformer);
	static string TransformTrimLeading(PEGTransformer &transformer);
	static string TransformTrimTrailing(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformTrimSource(PEGTransformer &transformer,
	                                                        optional<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression>
	TransformOverlayExpression(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> overlay_arguments);
	static vector<unique_ptr<ParsedExpression>>
	TransformOverlayParameters(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                           unique_ptr<ParsedExpression> expression_1, unique_ptr<ParsedExpression> from_expression,
	                           optional<unique_ptr<ParsedExpression>> for_expression);
	static unique_ptr<ParsedExpression> TransformFromExpression(PEGTransformer &transformer,
	                                                            unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformForExpression(PEGTransformer &transformer,
	                                                           unique_ptr<ParsedExpression> expression);
	static vector<unique_ptr<ParsedExpression>>
	TransformOverlayExpressionList(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformExtractDatePartArgument(PEGTransformer &transformer,
	                                                                     const DatePartSpecifier &extract_date_part);
	static unique_ptr<ParsedExpression> TransformExtractIdentifierArgument(PEGTransformer &transformer,
	                                                                       const Identifier &identifier);
	static unique_ptr<ParsedExpression> TransformExtractStringArgument(PEGTransformer &transformer,
	                                                                   const string &string_literal);
	static unique_ptr<SQLStatement>
	TransformCreateExternalResourceStmt(PEGTransformer &transformer, const string &string_literal,
	                                    const optional<Identifier> &attach_alias,
	                                    const optional<vector<GenericCopyOption>> &external_resource_creation_options);
	static unique_ptr<SQLStatement> TransformRegisterExternalResourceStmt(PEGTransformer &transformer,
	                                                                      const string &string_literal,
	                                                                      const optional<Identifier> &attach_alias,
	                                                                      unique_ptr<ParsedExpression> expression);
	static unique_ptr<SQLStatement> TransformDestroyExternalResourceStmt(PEGTransformer &transformer,
	                                                                     const Identifier &col_id);
	static unique_ptr<SQLStatement> TransformShowExternalResourcesStmt(PEGTransformer &transformer,
	                                                                   const optional<bool> &show_all_modifier);
	static bool TransformShowAllModifier(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformAttachToExternalResource(
	    PEGTransformer &transformer, unique_ptr<ExternalResourceOptions> external_resource_source,
	    const Identifier &attach_alias, const optional<vector<GenericCopyOption>> &attach_options);
	static unique_ptr<SQLStatement>
	TransformConnectToExternalResource(PEGTransformer &transformer,
	                                   unique_ptr<ExternalResourceOptions> external_resource_source,
	                                   const optional<vector<GenericCopyOption>> &attach_options);
	static unique_ptr<ExternalResourceOptions> TransformExternalResourceCreateClause(
	    PEGTransformer &transformer, const string &string_literal,
	    const optional<vector<GenericCopyOption>> &external_resource_creation_options);
	static unique_ptr<ExternalResourceOptions> TransformExternalResourceReferenceClause(PEGTransformer &transformer,
	                                                                                    const Identifier &col_id);
	static unique_ptr<SQLStatement>
	TransformInsertStatement(PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
	                         const optional<OnConflictAction> &or_action, unique_ptr<BaseTableRef> insert_target,
	                         const optional<InsertColumnOrder> &by_name_or_position,
	                         const optional<vector<string>> &insert_column_list, bool has_overriding_clause,
	                         InsertValues insert_values, optional<unique_ptr<OnConflictInfo>> on_conflict_clause,
	                         optional<vector<unique_ptr<ParsedExpression>>> returning_clause);
	static OnConflictAction TransformInsertOrReplace(PEGTransformer &transformer);
	static OnConflictAction TransformInsertOrIgnore(PEGTransformer &transformer);
	static InsertColumnOrder TransformInsertByName(PEGTransformer &transformer);
	static InsertColumnOrder TransformInsertByPosition(PEGTransformer &transformer);
	static unique_ptr<BaseTableRef> TransformInsertTarget(PEGTransformer &transformer,
	                                                      unique_ptr<BaseTableRef> base_table_name,
	                                                      const optional<Identifier> &insert_alias);
	static Identifier TransformInsertAlias(PEGTransformer &transformer, const Identifier &identifier);
	static vector<string> TransformColumnList(PEGTransformer &transformer, const vector<Identifier> &col_id);
	static vector<string> TransformInsertColumnList(PEGTransformer &transformer, const vector<string> &column_list);
	static InsertValues TransformSelectInsertValues(PEGTransformer &transformer,
	                                                unique_ptr<SelectStatement> select_statement_internal);
	static InsertValues TransformDefaultValues(PEGTransformer &transformer);
	static unique_ptr<OnConflictInfo> TransformOnConflictClause(PEGTransformer &transformer,
	                                                            optional<OnConflictExpressionTarget> on_conflict_target,
	                                                            unique_ptr<OnConflictInfo> on_conflict_action);
	static OnConflictExpressionTarget
	TransformOnConflictExpressionTarget(PEGTransformer &transformer, const vector<string> &column_id_list,
	                                    optional<unique_ptr<ParsedExpression>> where_clause);
	static OnConflictExpressionTarget TransformOnConflictIndexTarget(PEGTransformer &transformer,
	                                                                 const Identifier &constraint_name);
	static unique_ptr<OnConflictInfo> TransformOnConflictUpdate(PEGTransformer &transformer,
	                                                            unique_ptr<UpdateSetInfo> update_set_clause,
	                                                            optional<unique_ptr<ParsedExpression>> where_clause);
	static unique_ptr<OnConflictInfo> TransformOnConflictNothing(PEGTransformer &transformer);
	static vector<unique_ptr<ParsedExpression>>
	TransformReturningClause(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> target_list);
	static unique_ptr<SQLStatement> TransformLoadStatement(PEGTransformer &transformer,
	                                                       const Identifier &col_id_or_string,
	                                                       const optional<ExtensionRepositoryInfo> &from_source,
	                                                       const optional<Identifier> &extension_alias);
	static Identifier TransformExtensionAlias(PEGTransformer &transformer, const Identifier &identifier);
	static unique_ptr<SQLStatement> TransformInstallStatement(PEGTransformer &transformer, const bool &has_result,
	                                                          const optional<bool> &install_and_load,
	                                                          const QualifiedName &identifier_or_string_literal,
	                                                          const optional<ExtensionRepositoryInfo> &from_source,
	                                                          const optional<string> &version_number);
	static bool TransformInstallAndLoad(PEGTransformer &transformer);
	static unique_ptr<SQLStatement> TransformUpdateExtensionsStatement(PEGTransformer &transformer,
	                                                                   const optional<vector<Identifier>> &identifier);
	static ExtensionRepositoryInfo TransformFromSourceIdentifier(PEGTransformer &transformer,
	                                                             const Identifier &identifier);
	static ExtensionRepositoryInfo TransformFromSourceString(PEGTransformer &transformer, const string &string_literal);
	static string TransformVersionNumber(PEGTransformer &transformer,
	                                     const QualifiedName &identifier_or_string_literal);
	static unique_ptr<SQLStatement>
	TransformCreateExtensionRepositoryStmt(PEGTransformer &transformer, const optional<bool> &or_replace,
	                                       const optional<bool> &if_not_exists, const Identifier &col_id_or_string,
	                                       const string &repository_prefix,
	                                       const optional<vector<string>> &repository_public_key);
	static string TransformRepositoryPrefix(PEGTransformer &transformer, const bool &has_result,
	                                        const string &string_literal);
	static vector<string> TransformRepositoryPublicKey(PEGTransformer &transformer, const bool &has_result,
	                                                   const vector<string> &string_literal);
	static unique_ptr<SQLStatement> TransformDropExtensionRepositoryStmt(PEGTransformer &transformer,
	                                                                     const optional<bool> &if_exists,
	                                                                     const Identifier &col_id_or_string);
	static unique_ptr<TableRef> TransformTableMatchRecognizeClause(PEGTransformer &transformer,
	                                                               unique_ptr<TableRef> match_recognize_body,
	                                                               const optional<TableAlias> &table_alias);
	static unique_ptr<TableRef> TransformMatchRecognizeBody(PEGTransformer &transformer,
	                                                        vector<MatchRecognizeClause> match_recognize_clause);
	static MatchRecognizeClause TransformMRPartition(PEGTransformer &transformer,
	                                                 vector<unique_ptr<ParsedExpression>> window_partition);
	static MatchRecognizeClause TransformMROrderBy(PEGTransformer &transformer, vector<OrderByNode> order_by_clause);
	static MatchRecognizeClause TransformMRMeasures(PEGTransformer &transformer,
	                                                vector<unique_ptr<ParsedExpression>> measures_clause);
	static MatchRecognizeClause TransformMRRows(PEGTransformer &transformer, const MatchRecognizeRows &rows_per_match);
	static MatchRecognizeClause TransformMRSkip(PEGTransformer &transformer,
	                                            MatchRecognizeAfterMatchClause after_match_skip);
	static MatchRecognizeClause TransformMRPattern(PEGTransformer &transformer,
	                                               unique_ptr<ParsedExpression> pattern_clause);
	static MatchRecognizeClause TransformMRSubset(PEGTransformer &transformer,
	                                              vector<MatchRecognizeSubset> subset_clause);
	static MatchRecognizeClause TransformMRDefineAuto(PEGTransformer &transformer);
	static MatchRecognizeClause TransformMRDefine(PEGTransformer &transformer,
	                                              vector<unique_ptr<ParsedExpression>> define_clause);
	static vector<unique_ptr<ParsedExpression>>
	TransformMeasuresClause(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> measures_element);
	static unique_ptr<ParsedExpression> TransformMeasuresElement(PEGTransformer &transformer,
	                                                             unique_ptr<ParsedExpression> expression,
	                                                             const Identifier &col_label_or_string);
	static bool TransformRunningSemantics(PEGTransformer &transformer);
	static bool TransformFinalSemantics(PEGTransformer &transformer);
	static MatchRecognizeRows TransformOneRowPerMatch(PEGTransformer &transformer);
	static MatchRecognizeRows TransformAllRowsPerMatch(PEGTransformer &transformer,
	                                                   const optional<MatchRecognizeRows> &all_rows_empty_matches);
	static MatchRecognizeRows TransformShowEmptyMatches(PEGTransformer &transformer);
	static MatchRecognizeRows TransformOmitEmptyMatches(PEGTransformer &transformer);
	static MatchRecognizeRows TransformWithUnmatchedRows(PEGTransformer &transformer);
	static MatchRecognizeAfterMatchClause TransformSkipToNextRow(PEGTransformer &transformer);
	static MatchRecognizeAfterMatchClause TransformSkipPastLastRow(PEGTransformer &transformer);
	static MatchRecognizeAfterMatchClause TransformSkipToFirstVar(PEGTransformer &transformer,
	                                                              const Identifier &col_label_or_string);
	static MatchRecognizeAfterMatchClause TransformSkipToLastVar(PEGTransformer &transformer,
	                                                             const Identifier &col_label_or_string);
	static MatchRecognizeAfterMatchClause TransformSkipToVar(PEGTransformer &transformer,
	                                                         const Identifier &col_label_or_string);
	static unique_ptr<ParsedExpression>
	TransformRowPattern(PEGTransformer &transformer, unique_ptr<ParsedExpression> row_pattern_term,
	                    optional<vector<unique_ptr<ParsedExpression>>> row_pattern_alternative);
	static unique_ptr<ParsedExpression>
	TransformRowPatternTerm(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> row_pattern_factor);
	static unique_ptr<ParsedExpression>
	TransformRowPatternFactor(PEGTransformer &transformer, unique_ptr<ParsedExpression> row_pattern_primary,
	                          const optional<MatchRecognizeQuantifier> &row_pattern_quantifier);
	static unique_ptr<ParsedExpression> TransformRowPatternPermute(PEGTransformer &transformer,
	                                                               vector<unique_ptr<ParsedExpression>> row_pattern);
	static unique_ptr<ParsedExpression> TransformPatternStart(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformPatternEnd(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformRowPatternEmpty(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformRowPatternExclusion(PEGTransformer &transformer,
	                                                                 unique_ptr<ParsedExpression> row_pattern);
	static unique_ptr<ParsedExpression> TransformRowPatternLabel(PEGTransformer &transformer,
	                                                             const Identifier &col_label_or_string);
	static MatchRecognizeQuantifier
	TransformRowPatternQuantifier(PEGTransformer &transformer,
	                              const MatchRecognizeQuantifier &row_pattern_quantifier_kind,
	                              const optional<bool> &quantifier_reluctant);
	static bool TransformQuantifierReluctant(PEGTransformer &transformer);
	static MatchRecognizeQuantifier TransformQuantifierStar(PEGTransformer &transformer);
	static MatchRecognizeQuantifier TransformQuantifierPlus(PEGTransformer &transformer);
	static MatchRecognizeQuantifier TransformQuantifierOptional(PEGTransformer &transformer);
	static MatchRecognizeQuantifier TransformQuantifierMinMax(PEGTransformer &transformer,
	                                                          unique_ptr<ParsedExpression> number_literal,
	                                                          unique_ptr<ParsedExpression> number_literal_1);
	static MatchRecognizeQuantifier TransformQuantifierMin(PEGTransformer &transformer,
	                                                       unique_ptr<ParsedExpression> number_literal);
	static MatchRecognizeQuantifier TransformQuantifierMax(PEGTransformer &transformer,
	                                                       unique_ptr<ParsedExpression> number_literal);
	static MatchRecognizeQuantifier TransformQuantifierExact(PEGTransformer &transformer,
	                                                         unique_ptr<ParsedExpression> number_literal);
	static vector<MatchRecognizeSubset> TransformSubsetClause(PEGTransformer &transformer,
	                                                          vector<MatchRecognizeSubset> subset_element);
	static MatchRecognizeSubset TransformSubsetElement(PEGTransformer &transformer,
	                                                   const Identifier &col_label_or_string,
	                                                   const vector<Identifier> &col_label_or_string_1);
	static vector<unique_ptr<ParsedExpression>>
	TransformDefineClause(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> define_element);
	static unique_ptr<ParsedExpression> TransformDefineElement(PEGTransformer &transformer,
	                                                           const Identifier &col_label_or_string,
	                                                           unique_ptr<ParsedExpression> expression);
	static unique_ptr<SQLStatement>
	TransformMergeIntoStatement(PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
	                            unique_ptr<BaseTableRef> target_opt_alias, unique_ptr<TableRef> merge_into_using_clause,
	                            JoinQualifier join_qualifier,
	                            vector<pair<MergeActionCondition, unique_ptr<MergeIntoAction>>> merge_match,
	                            optional<vector<unique_ptr<ParsedExpression>>> returning_clause);
	static unique_ptr<TableRef> TransformMergeIntoUsingClause(PEGTransformer &transformer,
	                                                          unique_ptr<TableRef> table_ref);
	static pair<MergeActionCondition, unique_ptr<MergeIntoAction>>
	TransformMatchedClause(PEGTransformer &transformer, optional<unique_ptr<ParsedExpression>> and_expression,
	                       unique_ptr<MergeIntoAction> matched_clause_action);
	static unique_ptr<MergeIntoAction>
	TransformUpdateMatchClause(PEGTransformer &transformer, optional<unique_ptr<MergeIntoAction>> update_match_info);
	static unique_ptr<MergeIntoAction> TransformUpdateMatchSetAction(PEGTransformer &transformer,
	                                                                 unique_ptr<UpdateSetInfo> update_match_set_clause);
	static unique_ptr<MergeIntoAction> TransformUpdateByNameOrPosition(PEGTransformer &transformer,
	                                                                   const InsertColumnOrder &by_name_or_position);
	static unique_ptr<MergeIntoAction> TransformDeleteMatchClause(PEGTransformer &transformer);
	static unique_ptr<MergeIntoAction>
	TransformInsertMatchClause(PEGTransformer &transformer, optional<unique_ptr<MergeIntoAction>> insert_match_info);
	static unique_ptr<MergeIntoAction> TransformInsertDefaultValues(PEGTransformer &transformer);
	static unique_ptr<MergeIntoAction>
	TransformInsertByNameOrPosition(PEGTransformer &transformer, const optional<InsertColumnOrder> &by_name_or_position,
	                                const bool &has_result);
	static unique_ptr<MergeIntoAction> TransformInsertValuesList(PEGTransformer &transformer,
	                                                             const optional<vector<string>> &insert_column_list,
	                                                             vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<MergeIntoAction> TransformDoNothingMatchClause(PEGTransformer &transformer);
	static unique_ptr<MergeIntoAction> TransformErrorMatchClause(PEGTransformer &transformer,
	                                                             optional<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<ParsedExpression> TransformAndExpression(PEGTransformer &transformer,
	                                                           unique_ptr<ParsedExpression> expression);
	static pair<MergeActionCondition, unique_ptr<MergeIntoAction>>
	TransformNotMatchedClause(PEGTransformer &transformer, const optional<MergeActionCondition> &by_source_or_target,
	                          optional<unique_ptr<ParsedExpression>> and_expression,
	                          unique_ptr<MergeIntoAction> matched_clause_action);
	static MergeActionCondition TransformBySource(PEGTransformer &transformer);
	static MergeActionCondition TransformByTarget(PEGTransformer &transformer);
	static vector<PivotColumn> TransformPivotOn(PEGTransformer &transformer, vector<PivotColumn> pivot_column_list);
	static vector<unique_ptr<ParsedExpression>> TransformPivotUsing(PEGTransformer &transformer,
	                                                                vector<unique_ptr<ParsedExpression>> target_list);
	static vector<PivotColumn> TransformPivotColumnList(PEGTransformer &transformer,
	                                                    vector<PivotColumn> pivot_column_entry);
	static PivotColumn TransformPivotColumnExpression(PEGTransformer &transformer,
	                                                  unique_ptr<ParsedExpression> expression);
	static PivotColumn TransformPivotColumnSubquery(PEGTransformer &transformer,
	                                                unique_ptr<ParsedExpression> base_expression,
	                                                unique_ptr<SelectStatement> select_statement_internal);
	static UnpivotNameValues TransformIntoNameValues(PEGTransformer &transformer, const Identifier &col_id_or_string,
	                                                 const vector<string> &optional_parens_name_list);
	static vector<string> TransformParenthesizedNameList(PEGTransformer &transformer,
	                                                     const vector<Identifier> &col_id_or_string);
	static vector<string> TransformBareNameList(PEGTransformer &transformer,
	                                            const vector<Identifier> &col_id_or_string);
	static bool TransformIncludeNulls(PEGTransformer &transformer);
	static bool TransformExcludeNulls(PEGTransformer &transformer);
	static vector<string> TransformUnpivotHeaderSingle(PEGTransformer &transformer, const Identifier &col_id_or_string);
	static vector<string> TransformUnpivotHeaderList(PEGTransformer &transformer,
	                                                 const vector<Identifier> &col_id_or_string);
	static unique_ptr<SQLStatement> TransformPragmaStatement(PEGTransformer &transformer,
	                                                         unique_ptr<SQLStatement> pragma_assign_or_function);
	static unique_ptr<SQLStatement> TransformPragmaAssign(PEGTransformer &transformer, const Identifier &setting_name,
	                                                      vector<unique_ptr<ParsedExpression>> variable_list);
	static unique_ptr<SQLStatement>
	TransformPragmaFunction(PEGTransformer &transformer, const Identifier &pragma_name,
	                        optional<vector<unique_ptr<ParsedExpression>>> pragma_parameters);
	static vector<unique_ptr<ParsedExpression>>
	TransformPragmaParameters(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<SQLStatement> TransformPrepareStatement(PEGTransformer &transformer, const Identifier &identifier,
	                                                          const optional<vector<LogicalType>> &type_list,
	                                                          unique_ptr<SQLStatement> statement);
	static vector<LogicalType> TransformTypeList(PEGTransformer &transformer, const vector<LogicalType> &type);
	static unique_ptr<SQLStatement> TransformSelectStatement(PEGTransformer &transformer,
	                                                         unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<SelectStatement> TransformSelectSetOpChain(
	    PEGTransformer &transformer, unique_ptr<SelectStatement> intersect_chain,
	    optional<vector<pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>>> select_set_op_chain_tail);
	static pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>
	TransformSelectSetOpChainTail(PEGTransformer &transformer, unique_ptr<SetOperationNode> setop_clause,
	                              unique_ptr<SelectStatement> intersect_chain);
	static unique_ptr<SelectStatement> TransformIntersectChain(
	    PEGTransformer &transformer, unique_ptr<SelectStatement> select_atom,
	    optional<vector<pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>>> intersect_chain_tail);
	static pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>
	TransformIntersectChainTail(PEGTransformer &transformer, unique_ptr<SetOperationNode> set_intersect_clause,
	                            unique_ptr<SelectStatement> select_atom);
	static unique_ptr<SetOperationNode> TransformSetIntersectClause(PEGTransformer &transformer,
	                                                                const optional<bool> &distinct_or_all);
	static unique_ptr<SelectStatement> TransformSelectParens(PEGTransformer &transformer,
	                                                         unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<SetOperationNode> TransformSetopClause(PEGTransformer &transformer,
	                                                         const SetOperationType &setop_type,
	                                                         const optional<bool> &distinct_or_all,
	                                                         const bool &has_result);
	static SetOperationType TransformSetopUnion(PEGTransformer &transformer);
	static SetOperationType TransformSetopExcept(PEGTransformer &transformer);
	static vector<unique_ptr<ResultModifier>>
	TransformResultModifiers(PEGTransformer &transformer, optional<vector<OrderByNode>> order_by_clause,
	                         optional<unique_ptr<ResultModifier>> limit_offset);
	static unique_ptr<ResultModifier> TransformLimitOffsetClause(PEGTransformer &transformer,
	                                                             LimitPercentResult limit_clause,
	                                                             optional<LimitPercentResult> offset_clause);
	static unique_ptr<ResultModifier> TransformOffsetLimitClause(PEGTransformer &transformer,
	                                                             LimitPercentResult offset_clause,
	                                                             optional<LimitPercentResult> limit_clause);
	static unique_ptr<ResultModifier> TransformOffsetFetchClause(PEGTransformer &transformer,
	                                                             LimitPercentResult offset_clause,
	                                                             LimitPercentResult fetch_clause);
	static unique_ptr<ResultModifier> TransformFetchOffsetClause(PEGTransformer &transformer,
	                                                             LimitPercentResult fetch_clause,
	                                                             LimitPercentResult offset_clause);
	static unique_ptr<ResultModifier> TransformFetchOnlyClause(PEGTransformer &transformer,
	                                                           LimitPercentResult fetch_clause);
	static unique_ptr<SelectStatement> TransformTableStatement(PEGTransformer &transformer,
	                                                           unique_ptr<BaseTableRef> base_table_name);
	static unique_ptr<SelectStatement> TransformSimpleSelectParens(PEGTransformer &transformer,
	                                                               unique_ptr<SelectStatement> simple_select);
	static unique_ptr<SelectNode> TransformSelectFromClause(PEGTransformer &transformer,
	                                                        unique_ptr<SelectNode> select_clause,
	                                                        optional<unique_ptr<TableRef>> from_clause);
	static unique_ptr<SelectNode> TransformFromSelectClause(PEGTransformer &transformer,
	                                                        unique_ptr<TableRef> from_clause,
	                                                        optional<unique_ptr<SelectNode>> select_clause);
	static pair<Identifier, unique_ptr<CommonTableExpressionInfo>>
	TransformWithStatement(PEGTransformer &transformer, const Identifier &col_id_or_string,
	                       const optional<vector<string>> &column_aliases,
	                       optional<vector<unique_ptr<ParsedExpression>>> using_key, const optional<bool> &materialized,
	                       unique_ptr<TableRef> cte_body);
	static unique_ptr<TableRef> TransformCTESelectBody(PEGTransformer &transformer,
	                                                   unique_ptr<SelectStatement> select_statement_internal);
	static unique_ptr<TableRef> TransformCTEDMLBody(PEGTransformer &transformer, unique_ptr<SQLStatement> statement);
	static vector<unique_ptr<ParsedExpression>> TransformUsingKey(PEGTransformer &transformer,
	                                                              vector<unique_ptr<ParsedExpression>> target_list);
	static bool TransformMaterialized(PEGTransformer &transformer, const bool &has_result);
	static unique_ptr<SelectNode> TransformSelectClause(PEGTransformer &transformer,
	                                                    optional<DistinctClause> distinct_clause,
	                                                    optional<vector<unique_ptr<ParsedExpression>>> target_list);
	static vector<unique_ptr<ParsedExpression>>
	TransformTargetList(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> aliased_expression);
	static vector<string> TransformColumnAliases(PEGTransformer &transformer,
	                                             const vector<Identifier> &col_id_or_string);
	static DistinctClause TransformDistinctAll(PEGTransformer &transformer);
	static DistinctClause TransformDistinctOn(PEGTransformer &transformer,
	                                          optional<vector<unique_ptr<ParsedExpression>>> distinct_on_targets);
	static vector<unique_ptr<ParsedExpression>>
	TransformDistinctOnTargets(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<TableRef>
	TransformTableSubquery(PEGTransformer &transformer, const optional<Identifier> &table_alias_colon,
	                       const optional<bool> &lateral, unique_ptr<TableRef> subquery_reference,
	                       const optional<TableAlias> &table_alias, optional<unique_ptr<SampleOptions>> sample_clause);
	static unique_ptr<TableRef>
	TransformBaseTableRef(PEGTransformer &transformer, const optional<Identifier> &table_alias_colon,
	                      unique_ptr<BaseTableRef> base_table_name, const optional<TableAlias> &table_alias,
	                      optional<unique_ptr<AtClause>> at_clause, optional<unique_ptr<SampleOptions>> sample_clause);
	static Identifier TransformTableAliasColon(PEGTransformer &transformer, const Identifier &col_id_or_string);
	static unique_ptr<TableRef> TransformValuesRef(PEGTransformer &transformer,
	                                               const optional<Identifier> &table_alias_colon,
	                                               unique_ptr<SelectStatement> values_clause,
	                                               const optional<TableAlias> &table_alias,
	                                               optional<unique_ptr<SampleOptions>> sample_clause);
	static unique_ptr<TableRef> TransformParensTableRef(PEGTransformer &transformer,
	                                                    const optional<Identifier> &table_alias_colon,
	                                                    unique_ptr<TableRef> table_ref,
	                                                    const optional<TableAlias> &table_alias,
	                                                    optional<unique_ptr<SampleOptions>> sample_clause);
	static unique_ptr<TableRef> TransformTablePivotClause(PEGTransformer &transformer,
	                                                      unique_ptr<TableRef> table_pivot_clause_body,
	                                                      const optional<TableAlias> &table_alias);
	static unique_ptr<TableRef> TransformTablePivotClauseBody(PEGTransformer &transformer,
	                                                          vector<unique_ptr<ParsedExpression>> target_list,
	                                                          vector<PivotColumn> pivot_value_list,
	                                                          const optional<vector<string>> &pivot_group_by_list);
	static vector<string> TransformPivotGroupByList(PEGTransformer &transformer,
	                                                const vector<string> &optional_parens_name_list);
	static unique_ptr<TableRef> TransformTableUnpivotClause(PEGTransformer &transformer,
	                                                        const optional<bool> &include_or_exclude_nulls,
	                                                        unique_ptr<TableRef> table_unpivot_clause_body,
	                                                        const optional<TableAlias> &table_alias);
	static unique_ptr<TableRef> TransformTableUnpivotClauseBody(PEGTransformer &transformer,
	                                                            const vector<string> &unpivot_header,
	                                                            vector<PivotColumn> unpivot_value_list);
	static unique_ptr<ParsedExpression> TransformPivotHeader(PEGTransformer &transformer,
	                                                         unique_ptr<ParsedExpression> base_expression);
	static PivotColumn TransformPivotValueList(PEGTransformer &transformer, unique_ptr<ParsedExpression> pivot_header,
	                                           PivotColumn pivot_value_target);
	static PivotColumn TransformPivotEnumTarget(PEGTransformer &transformer, const Identifier &identifier);
	static PivotColumn TransformPivotListTarget(PEGTransformer &transformer,
	                                            vector<PivotColumnEntry> pivot_target_list);
	static PivotColumn TransformUnpivotValueList(PEGTransformer &transformer, const vector<string> &unpivot_header,
	                                             vector<PivotColumnEntry> unpivot_target_list);
	static vector<PivotColumnEntry> TransformPivotTargetList(PEGTransformer &transformer,
	                                                         vector<unique_ptr<ParsedExpression>> target_list);
	static vector<PivotColumnEntry> TransformUnpivotTargetList(PEGTransformer &transformer,
	                                                           vector<unique_ptr<ParsedExpression>> target_list);
	static bool TransformLateral(PEGTransformer &transformer);
	static unique_ptr<BaseTableRef> TransformUnqualifiedBaseTableName(PEGTransformer &transformer,
	                                                                  const Identifier &table_name);
	static unique_ptr<BaseTableRef> TransformSchemaReservedTable(PEGTransformer &transformer,
	                                                             const Identifier &schema_qualification,
	                                                             const Identifier &reserved_table_name);
	static unique_ptr<BaseTableRef>
	TransformCatalogReservedSchemaTable(PEGTransformer &transformer, const Identifier &catalog_qualification,
	                                    const vector<Identifier> &reserved_schema_qualification,
	                                    const Identifier &reserved_table_name);
	static unique_ptr<TableRef> TransformTableFunctionLateralOpt(
	    PEGTransformer &transformer, const optional<bool> &lateral, const QualifiedName &qualified_table_function,
	    vector<FunctionArgument> table_function_arguments, const optional<bool> &with_ordinality,
	    const optional<TableAlias> &table_alias, optional<unique_ptr<SampleOptions>> sample_clause);
	static unique_ptr<TableRef> TransformTableFunctionAliasColon(PEGTransformer &transformer,
	                                                             const Identifier &table_alias_colon,
	                                                             const QualifiedName &qualified_table_function,
	                                                             vector<FunctionArgument> table_function_arguments,
	                                                             const optional<bool> &with_ordinality,
	                                                             optional<unique_ptr<SampleOptions>> sample_clause);
	static bool TransformWithOrdinality(PEGTransformer &transformer);
	static QualifiedName TransformQualifiedTableFunction(PEGTransformer &transformer,
	                                                     const optional<Identifier> &catalog_qualification,
	                                                     const optional<vector<Identifier>> &schema_qualification,
	                                                     const Identifier &table_function_name);
	static vector<FunctionArgument>
	TransformTableFunctionArguments(PEGTransformer &transformer, optional<vector<FunctionArgument>> function_argument);
	static FunctionArgument TransformNamedFunctionArgument(PEGTransformer &transformer,
	                                                       const Identifier &type_func_name,
	                                                       unique_ptr<ParsedExpression> expression);
	static FunctionArgument TransformPositionalFunctionArgument(PEGTransformer &transformer,
	                                                            unique_ptr<ParsedExpression> expression);
	static MacroParameter TransformNamedParameter(PEGTransformer &transformer, const Identifier &type_func_name,
	                                              const optional<LogicalType> &type,
	                                              unique_ptr<ParsedExpression> expression);
	static TableAlias TransformTableAliasAs(PEGTransformer &transformer,
	                                        const QualifiedName &identifier_or_string_literal,
	                                        const optional<vector<string>> &column_aliases);
	static TableAlias TransformTableAliasWithoutAs(PEGTransformer &transformer, const Identifier &identifier,
	                                               const optional<vector<string>> &column_aliases);
	static unique_ptr<AtClause> TransformAtClause(PEGTransformer &transformer, unique_ptr<AtClause> at_specifier);
	static unique_ptr<AtClause> TransformAtSpecifier(PEGTransformer &transformer, const string &at_unit,
	                                                 unique_ptr<ParsedExpression> expression);
	static string TransformVersionAtUnit(PEGTransformer &transformer);
	static string TransformTimestampAtUnit(PEGTransformer &transformer);
	static unique_ptr<TableRef>
	TransformNearestJoinAliased(PEGTransformer &transformer, const optional<JoinType> &join_type,
	                            unique_ptr<TableRef> table_ref, const optional<bool> &approx_or_exact,
	                            optional<unique_ptr<ParsedExpression>> number_literal,
	                            const OrderType &distance_or_similarity, unique_ptr<ParsedExpression> expression);
	static unique_ptr<TableRef>
	TransformNearestJoinBare(PEGTransformer &transformer, const optional<JoinType> &join_type,
	                         unique_ptr<TableRef> nearest_bare_table_ref, const optional<bool> &approx_or_exact,
	                         optional<unique_ptr<ParsedExpression>> number_literal,
	                         const OrderType &distance_or_similarity, unique_ptr<ParsedExpression> expression);
	static unique_ptr<TableRef> TransformNearestValuesRef(PEGTransformer &transformer,
	                                                      unique_ptr<SelectStatement> values_clause);
	static unique_ptr<TableRef> TransformNearestTableFunction(PEGTransformer &transformer,
	                                                          const optional<bool> &lateral,
	                                                          const QualifiedName &qualified_table_function,
	                                                          vector<FunctionArgument> table_function_arguments,
	                                                          const optional<bool> &with_ordinality);
	static unique_ptr<TableRef> TransformNearestTableSubquery(PEGTransformer &transformer,
	                                                          const optional<bool> &lateral,
	                                                          unique_ptr<TableRef> subquery_reference);
	static unique_ptr<TableRef> TransformNearestBaseTableRef(PEGTransformer &transformer,
	                                                         unique_ptr<BaseTableRef> base_table_name,
	                                                         optional<unique_ptr<AtClause>> at_clause,
	                                                         optional<unique_ptr<SampleOptions>> sample_clause);
	static unique_ptr<TableRef> TransformNearestParensTableRef(PEGTransformer &transformer,
	                                                           unique_ptr<TableRef> table_ref,
	                                                           optional<unique_ptr<SampleOptions>> sample_clause);
	static bool TransformNearestApprox(PEGTransformer &transformer);
	static bool TransformNearestExact(PEGTransformer &transformer);
	static OrderType TransformNearestDistance(PEGTransformer &transformer);
	static OrderType TransformNearestSimilarity(PEGTransformer &transformer);
	static unique_ptr<TableRef> TransformRegularJoinClause(PEGTransformer &transformer, const optional<bool> &asof,
	                                                       const optional<JoinType> &join_type,
	                                                       unique_ptr<TableRef> table_ref,
	                                                       JoinQualifier join_qualifier);
	static unique_ptr<TableRef> TransformJoinByClause(PEGTransformer &transformer, const string &col_label,
	                                                  unique_ptr<TableRef> table_ref, JoinQualifier join_qualifier);
	static bool TransformAsof(PEGTransformer &transformer);
	static unique_ptr<TableRef> TransformJoinWithoutOnClause(PEGTransformer &transformer, const JoinPrefix &join_prefix,
	                                                         unique_ptr<TableRef> inner_table_ref);
	static JoinQualifier TransformOnClause(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression);
	static JoinQualifier TransformUsingClause(PEGTransformer &transformer, const vector<Identifier> &column_name);
	static JoinPrefix TransformCrossJoinPrefix(PEGTransformer &transformer);
	static JoinPrefix TransformNaturalJoinPrefix(PEGTransformer &transformer, const optional<JoinType> &join_type);
	static JoinPrefix TransformPositionalJoinPrefix(PEGTransformer &transformer);
	static JoinType TransformFullJoin(PEGTransformer &transformer, const bool &has_result);
	static JoinType TransformLeftJoin(PEGTransformer &transformer, const bool &has_result);
	static JoinType TransformRightJoin(PEGTransformer &transformer, const bool &has_result);
	static JoinType TransformSemiJoin(PEGTransformer &transformer);
	static JoinType TransformAntiJoin(PEGTransformer &transformer);
	static JoinType TransformInnerJoin(PEGTransformer &transformer);
	static unique_ptr<TableRef> TransformFromClause(PEGTransformer &transformer,
	                                                vector<unique_ptr<TableRef>> table_ref);
	static unique_ptr<ParsedExpression> TransformWhereClause(PEGTransformer &transformer,
	                                                         unique_ptr<ParsedExpression> expression);
	static GroupByNode TransformGroupByClause(PEGTransformer &transformer, GroupByNode group_by_expressions);
	static unique_ptr<ParsedExpression> TransformHavingClause(PEGTransformer &transformer,
	                                                          unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformQualifyClause(PEGTransformer &transformer,
	                                                           unique_ptr<ParsedExpression> expression);
	static unique_ptr<SampleOptions> TransformSampleClause(PEGTransformer &transformer,
	                                                       unique_ptr<SampleOptions> sample_entry);
	static unique_ptr<SampleOptions> TransformSelectSampleClause(PEGTransformer &transformer,
	                                                             unique_ptr<SampleOptions> sample_entry);
	static vector<unique_ptr<ParsedExpression>>
	TransformWindowClause(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> window_definition);
	static unique_ptr<SampleOptions>
	TransformSampleEntryCount(PEGTransformer &transformer, unique_ptr<SampleOptions> sample_count,
	                          const optional<pair<SampleMethod, optional_idx>> &sample_properties);
	static unique_ptr<SampleOptions> TransformSampleEntryFunction(PEGTransformer &transformer,
	                                                              const optional<SampleMethod> &sample_function,
	                                                              unique_ptr<SampleOptions> sample_count,
	                                                              const optional<optional_idx> &repeatable_sample);
	static SampleMethod TransformSampleFunction(PEGTransformer &transformer, const Identifier &col_id);
	static pair<SampleMethod, optional_idx> TransformSampleProperties(PEGTransformer &transformer,
	                                                                  const Identifier &col_id,
	                                                                  const optional<optional_idx> &sample_seed);
	static optional_idx TransformRepeatableSample(PEGTransformer &transformer, const optional_idx &sample_seed);
	static optional_idx TransformSampleSeed(PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal);
	static unique_ptr<SampleOptions> TransformSampleCount(PEGTransformer &transformer,
	                                                      unique_ptr<ParsedExpression> sample_value,
	                                                      const optional<bool> &sample_unit);
	static bool TransformSamplePercentage(PEGTransformer &transformer);
	static bool TransformSampleRows(PEGTransformer &transformer);
	static GroupByNode TransformGroupByAll(PEGTransformer &transformer);
	static GroupByNode TransformGroupByList(PEGTransformer &transformer,
	                                        vector<GroupByExpressionInfo> group_by_expression);
	static GroupByExpressionInfo TransformGroupByBaseExpression(PEGTransformer &transformer,
	                                                            unique_ptr<ParsedExpression> expression);
	static GroupByExpressionInfo TransformEmptyGroupingItem(PEGTransformer &transformer);
	static GroupByExpressionInfo TransformCubeOrRollupClause(PEGTransformer &transformer, const string &cube_or_rollup,
	                                                         optional<vector<unique_ptr<ParsedExpression>>> expression);
	static string TransformCubeKeyword(PEGTransformer &transformer);
	static string TransformRollupKeyword(PEGTransformer &transformer);
	static GroupByExpressionInfo TransformGroupingSetsClause(PEGTransformer &transformer,
	                                                         vector<GroupByExpressionInfo> group_by_expression);
	static unique_ptr<TableRef> TransformSubqueryReference(PEGTransformer &transformer,
	                                                       unique_ptr<SelectStatement> select_statement_internal);
	static OrderByNode TransformOrderByExpression(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                                              const optional<OrderType> &desc_or_asc,
	                                              const optional<OrderByNullType> &nulls_first_or_last);
	static OrderType TransformDescendingOrder(PEGTransformer &transformer);
	static OrderType TransformAscendingOrder(PEGTransformer &transformer);
	static OrderByNullType TransformNullsFirst(PEGTransformer &transformer);
	static OrderByNullType TransformNullsLast(PEGTransformer &transformer);
	static vector<OrderByNode> TransformOrderByClause(PEGTransformer &transformer,
	                                                  vector<OrderByNode> order_by_expressions);
	static vector<OrderByNode> TransformOrderByExpressionList(PEGTransformer &transformer,
	                                                          vector<OrderByNode> order_by_expression);
	static vector<OrderByNode> TransformOrderByAll(PEGTransformer &transformer, const optional<OrderType> &desc_or_asc,
	                                               const optional<OrderByNullType> &nulls_first_or_last);
	static LimitPercentResult TransformLimitClause(PEGTransformer &transformer, LimitPercentResult limit_value);
	static LimitPercentResult TransformOffsetClause(PEGTransformer &transformer, LimitPercentResult offset_value);
	static LimitPercentResult TransformOffsetValue(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
	                                               const bool &has_result);
	static LimitPercentResult TransformLimitAll(PEGTransformer &transformer);
	static LimitPercentResult TransformLimitLiteralPercent(PEGTransformer &transformer,
	                                                       unique_ptr<ParsedExpression> number_literal);
	static LimitPercentResult TransformLimitExpression(PEGTransformer &transformer,
	                                                   unique_ptr<ParsedExpression> expression, const bool &has_result);
	static LimitPercentResult TransformFetchClauseWithoutValue(PEGTransformer &transformer);
	static LimitPercentResult TransformFetchValue(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformColIdExpression(PEGTransformer &transformer, const Identifier &col_id,
	                                                             unique_ptr<ParsedExpression> expression);
	static unique_ptr<ParsedExpression> TransformExpressionAsCollabel(PEGTransformer &transformer,
	                                                                  unique_ptr<ParsedExpression> expression,
	                                                                  const Identifier &col_label_or_string);
	static unique_ptr<ParsedExpression> TransformExpressionOptIdentifier(PEGTransformer &transformer,
	                                                                     unique_ptr<ParsedExpression> expression,
	                                                                     const optional<Identifier> &identifier);
	static unique_ptr<SelectStatement>
	TransformValuesClause(PEGTransformer &transformer, vector<vector<unique_ptr<ParsedExpression>>> values_expressions);
	static vector<unique_ptr<ParsedExpression>>
	TransformValuesExpressions(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<SQLStatement> TransformSetKeywordStatement(PEGTransformer &transformer,
	                                                             unique_ptr<SetStatement> set_assignment_or_time_zone);
	static unique_ptr<SQLStatement> TransformVariableAssignment(PEGTransformer &transformer,
	                                                            const Identifier &identifier,
	                                                            unique_ptr<ParsedExpression> expression);
	static unique_ptr<SQLStatement> TransformResetStatement(PEGTransformer &transformer,
	                                                        unique_ptr<SQLStatement> reset_target);
	static unique_ptr<SQLStatement> TransformResetTimeZone(PEGTransformer &transformer,
	                                                       const optional<SetScope> &setting_scope);
	static unique_ptr<SQLStatement> TransformResetVariableOrSetting(PEGTransformer &transformer,
	                                                                const SettingInfo &set_variable_or_setting);
	static unique_ptr<SetStatement> TransformSetSchema(PEGTransformer &transformer,
	                                                   const optional<SetScope> &setting_scope,
	                                                   const string &string_literal);
	static unique_ptr<SetStatement> TransformStandardAssignment(PEGTransformer &transformer,
	                                                            const SettingInfo &set_variable_or_setting,
	                                                            vector<unique_ptr<ParsedExpression>> set_assignment);
	static unique_ptr<SetStatement> TransformSetTimeZone(PEGTransformer &transformer,
	                                                     const optional<SetScope> &setting_scope,
	                                                     unique_ptr<ParsedExpression> zone_value);
	static unique_ptr<ParsedExpression> TransformZoneLocal(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformZoneDefault(PEGTransformer &transformer);
	static unique_ptr<ParsedExpression> TransformZoneStringLiteral(PEGTransformer &transformer,
	                                                               const string &string_literal);
	static unique_ptr<ParsedExpression> TransformZoneIdentifier(PEGTransformer &transformer,
	                                                            const Identifier &identifier);
	static unique_ptr<ParsedExpression> TransformZoneIntervalWithInterval(PEGTransformer &transformer,
	                                                                      const string &string_literal,
	                                                                      const optional<DatePartSpecifier> &interval);
	static unique_ptr<ParsedExpression> TransformZoneIntervalWithPrecision(PEGTransformer &transformer,
	                                                                       unique_ptr<ParsedExpression> number_literal,
	                                                                       const string &string_literal);
	static SettingInfo TransformSetSetting(PEGTransformer &transformer, const optional<SetScope> &setting_scope,
	                                       const Identifier &setting_name);
	static SettingInfo TransformSetVariable(PEGTransformer &transformer, const SetScope &variable_scope,
	                                        const Identifier &identifier);
	static SetScope TransformVariableScope(PEGTransformer &transformer);
	static SetScope TransformLocalScope(PEGTransformer &transformer);
	static SetScope TransformSessionScope(PEGTransformer &transformer);
	static SetScope TransformGlobalScope(PEGTransformer &transformer);
	static vector<unique_ptr<ParsedExpression>>
	TransformSetAssignment(PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> variable_list);
	static vector<unique_ptr<ParsedExpression>> TransformVariableList(PEGTransformer &transformer,
	                                                                  vector<unique_ptr<ParsedExpression>> expression);
	static unique_ptr<SQLStatement> TransformBeginTransaction(PEGTransformer &transformer, const bool &has_result,
	                                                          const optional<TransactionModifierType> &read_or_write);
	static unique_ptr<SQLStatement> TransformRollbackTransaction(PEGTransformer &transformer, const bool &has_result);
	static unique_ptr<SQLStatement> TransformCommitTransaction(PEGTransformer &transformer, const bool &has_result);
	static TransactionModifierType TransformReadOrWrite(PEGTransformer &transformer,
	                                                    const TransactionModifierType &read_only_or_read_write);
	static TransactionModifierType TransformReadOnly(PEGTransformer &transformer);
	static TransactionModifierType TransformReadWrite(PEGTransformer &transformer);
	static unique_ptr<SQLStatement>
	TransformUpdateStatement(PEGTransformer &transformer, optional<CommonTableExpressionMap> with_clause,
	                         unique_ptr<TableRef> update_target, unique_ptr<UpdateSetInfo> update_set_clause,
	                         optional<unique_ptr<TableRef>> from_clause,
	                         optional<unique_ptr<ParsedExpression>> where_clause,
	                         optional<vector<unique_ptr<ParsedExpression>>> returning_clause);
	static unique_ptr<TableRef> TransformBaseTableSet(PEGTransformer &transformer,
	                                                  unique_ptr<BaseTableRef> base_table_name);
	static unique_ptr<TableRef> TransformBaseTableAliasSet(PEGTransformer &transformer,
	                                                       unique_ptr<BaseTableRef> base_table_name,
	                                                       const optional<Identifier> &update_alias);
	static Identifier TransformUpdateAlias(PEGTransformer &transformer, const bool &has_result,
	                                       const Identifier &col_id);
	static unique_ptr<UpdateSetInfo> TransformUpdateSetClauseList(PEGTransformer &transformer,
	                                                              vector<unique_ptr<UpdateSetInfo>> update_set_info);
	static unique_ptr<UpdateSetInfo> TransformUpdateSetTuple(PEGTransformer &transformer,
	                                                         const vector<Identifier> &column_name,
	                                                         unique_ptr<ParsedExpression> expression);
	static unique_ptr<UpdateSetInfo> TransformUpdateSetElement(PEGTransformer &transformer,
	                                                           const string &update_set_column_target,
	                                                           unique_ptr<ParsedExpression> expression);
	static string TransformUpdateSetColumnTarget(PEGTransformer &transformer, const Identifier &column_name,
	                                             const optional<vector<Identifier>> &dot_identifier);
	static unique_ptr<SQLStatement> TransformUseStatement(PEGTransformer &transformer, const QualifiedName &use_target);
	static QualifiedName TransformSchemaNameAsUseTarget(PEGTransformer &transformer, const string &schema_name);
	static QualifiedName TransformCatalogNameAsUseTarget(PEGTransformer &transformer, const string &catalog_name);
	static QualifiedName TransformUseTargetCatalogSchema(PEGTransformer &transformer, const Identifier &catalog_name,
	                                                     const Identifier &reserved_schema_name,
	                                                     const optional<vector<Identifier>> &dot_identifier);
	static Identifier TransformDotIdentifier(PEGTransformer &transformer, const Identifier &identifier);
	static unique_ptr<SQLStatement> TransformVacuumStatement(PEGTransformer &transformer,
	                                                         const optional<VacuumOptions> &vacuum_options,
	                                                         optional<AnalyzeTarget> analyze_target);
	static VacuumOptions TransformVacuumParensOptions(PEGTransformer &transformer, const vector<string> &vacuum_option);
	static VacuumOptions TransformVacuumLegacyOptions(PEGTransformer &transformer, const optional<string> &opt_full,
	                                                  const optional<string> &opt_freeze,
	                                                  const optional<string> &opt_verbose,
	                                                  const optional<string> &opt_analyze);
	static string TransformOptAnalyze(PEGTransformer &transformer, const Identifier &analyze_keyword);
	static string TransformOptFull(PEGTransformer &transformer);
	static string TransformOptFreeze(PEGTransformer &transformer);
	static string TransformOptVerbose(PEGTransformer &transformer);
	static vector<string> TransformNameList(PEGTransformer &transformer, const vector<Identifier> &col_id);
	//===--------------------------------------------------------------------===//
	// END GENERATED TRAMPOLINE RULES
	//===--------------------------------------------------------------------===//

	PEGTransformerFactory(const PEGTransformerFactory &) = delete;

	static const case_insensitive_map_t<const TransformFrameOps *> &GeneratedTransformFrameOps();

	static string ExtractFormat(const string &file_path);

	static string TransformIdentifierOrKeyword(PEGTransformer &transformer, ParseResult &parse_result);

private:
	static idx_t CountParseResultsFromList(ParseResult &parse_result);
	explicit PEGTransformerFactory(ParsedGrammar &grammar_p);
	ParsedGrammar &grammar;
};

} // namespace duckdb
