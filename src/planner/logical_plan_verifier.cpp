#include "duckdb/planner/logical_plan_verifier.hpp"

#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

struct LogicalPlanVerificationState {
	explicit LogicalPlanVerificationState(LogicalOperator &root) {
		LogicalPlanVerificationPath root_path;
		IndexOperator(root, root_path);
	}

	reference_map_t<LogicalOperator, LogicalPlanVerificationPath> operator_paths;
	reference_map_t<Expression, LogicalPlanVerificationPath> expression_paths;
	reference_set_t<LogicalOperator> resolved_inputs;
	reference_set_t<LogicalOperator> resolved_outputs;
	vector<LogicalPlanVerificationIssue> issues;

	const LogicalPlanVerificationPath &GetPath(LogicalOperator &op) const {
		auto entry = operator_paths.find(reference<LogicalOperator>(op));
		if (entry == operator_paths.end()) {
			throw InternalException("Logical operator is missing from the logical plan verification path index");
		}
		return entry->second;
	}

	const LogicalPlanVerificationPath &GetPath(Expression &expr) const {
		auto entry = expression_paths.find(reference<Expression>(expr));
		if (entry == expression_paths.end()) {
			throw InternalException("Expression is missing from the logical plan verification path index");
		}
		return entry->second;
	}

	void AddInvalidBinding(BoundColumnRefExpression &expr, const vector<ColumnBinding> &bindings) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INVALID_BINDING;
		issue.path = GetPath(expr);
		AddBindingFacts(issue, expr.Binding());
		issue.message = StringUtil::Format(
		    "Failed to bind column reference %s [table=%llu, column=%llu] against %llu bindings", expr.GetAlias(),
		    expr.Binding().table_index.index, expr.Binding().column_index.GetIndexUnsafe(), bindings.size());
		issues.push_back(std::move(issue));
	}

	void AddTypeMismatch(BoundColumnRefExpression &expr, const LogicalType &expected_type) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::TYPE_MISMATCH;
		issue.path = GetPath(expr);
		issue.construct =
		    LogicalPlanVerificationConstructIdentity::BindingTypeMismatch(expected_type, expr.GetReturnType());
		AddBindingFacts(issue, expr.Binding());
		issue.message = StringUtil::Format(
		    "Failed to bind column reference %s [table=%llu, column=%llu]: inequal types (%s != %s)", expr.GetAlias(),
		    expr.Binding().table_index.index, expr.Binding().column_index.GetIndexUnsafe(),
		    expr.GetReturnType().ToString(), expected_type.ToString());
		issues.push_back(std::move(issue));
	}

	void AddIncompleteBindingType(BoundColumnRefExpression &expr, const LogicalType &expected_type) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = GetPath(expr);
		issue.construct = LogicalPlanVerificationConstructIdentity::Expression(expr.GetExpressionClass());
		issue.facts.emplace_back("invariant", Value("incomplete_binding_type"));
		issue.facts.emplace_back("expected_type_complete", Value::BOOLEAN(expected_type.IsComplete()));
		issue.facts.emplace_back("actual_type_complete", Value::BOOLEAN(expr.GetReturnType().IsComplete()));
		AddBindingFacts(issue, expr.Binding());
		issue.message = StringUtil::Format(
		    "Failed to bind column reference %s [table=%llu, column=%llu]: expected and actual types must be complete",
		    expr.GetAlias(), expr.Binding().table_index.index, expr.Binding().column_index.GetIndexUnsafe());
		issues.push_back(std::move(issue));
	}

	void AddBindingTypeArityMismatch(BoundColumnRefExpression &expr, idx_t binding_count, idx_t type_count) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = GetPath(expr);
		issue.construct = LogicalPlanVerificationConstructIdentity::Expression(expr.GetExpressionClass());
		issue.facts.emplace_back("invariant", Value("binding_type_arity"));
		issue.facts.emplace_back("binding_count", Value::UBIGINT(binding_count));
		issue.facts.emplace_back("type_count", Value::UBIGINT(type_count));
		AddBindingFacts(issue, expr.Binding());
		issue.message = StringUtil::Format(
		    "Failed to bind column reference %s [table=%llu, column=%llu]: inequal num bindings/types (%llu != %llu)",
		    expr.GetAlias(), expr.Binding().table_index.index, expr.Binding().column_index.GetIndexUnsafe(),
		    binding_count, type_count);
		issues.push_back(std::move(issue));
	}

	void AddMalformedExtensionArity(LogicalExtensionOperator &op, const string &identifier, idx_t binding_count,
	                                idx_t type_count) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT;
		issue.path = GetPath(op);
		issue.construct = LogicalPlanVerificationConstructIdentity::Extension(identifier);
		issue.facts.emplace_back("binding_count", Value::UBIGINT(binding_count));
		issue.facts.emplace_back("type_count", Value::UBIGINT(type_count));
		issue.message = StringUtil::Format("Logical extension operator returned %llu bindings and %llu types",
		                                   binding_count, type_count);
		issues.push_back(std::move(issue));
	}

	void AddMalformedExtensionType(LogicalExtensionOperator &op, const string &identifier, idx_t type_index) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT;
		issue.path = GetPath(op);
		issue.construct = LogicalPlanVerificationConstructIdentity::Extension(identifier);
		issue.facts.emplace_back("invalid_type_index", Value::UBIGINT(type_index));
		issue.message =
		    StringUtil::Format("Logical extension operator returned an invalid type at index %llu", type_index);
		issues.push_back(std::move(issue));
	}

	void AddMalformedExtensionBinding(LogicalExtensionOperator &op, const string &identifier, idx_t binding_index,
	                                  const ColumnBinding &binding) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT;
		issue.path = GetPath(op);
		issue.construct = LogicalPlanVerificationConstructIdentity::Extension(identifier);
		issue.facts.emplace_back("invalid_binding_index", Value::UBIGINT(binding_index));
		issue.facts.emplace_back("table_index_valid", Value::BOOLEAN(binding.table_index.IsValid()));
		issue.facts.emplace_back("column_index_valid", Value::BOOLEAN(binding.column_index.IsValid()));
		issue.message =
		    StringUtil::Format("Logical extension operator returned an invalid binding at index %llu", binding_index);
		issues.push_back(std::move(issue));
	}

	void AddDuplicateExtensionBinding(LogicalExtensionOperator &op, const string &identifier, idx_t first_index,
	                                  idx_t duplicate_index, const ColumnBinding &binding, bool types_available,
	                                  bool types_equal) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT;
		issue.path = GetPath(op);
		issue.construct = LogicalPlanVerificationConstructIdentity::Extension(identifier);
		issue.facts.emplace_back("first_binding_index", Value::UBIGINT(first_index));
		issue.facts.emplace_back("duplicate_binding_index", Value::UBIGINT(duplicate_index));
		issue.facts.emplace_back("table_index", Value::UBIGINT(binding.table_index.index));
		issue.facts.emplace_back("column_index", Value::UBIGINT(binding.column_index.GetIndexUnsafe()));
		issue.facts.emplace_back("types_available", Value::BOOLEAN(types_available));
		issue.facts.emplace_back("types_equal", Value::BOOLEAN(types_equal));
		issue.message = StringUtil::Format(
		    "Logical extension operator returned duplicate binding [table=%llu, column=%llu] at indexes %llu and %llu",
		    binding.table_index.index, binding.column_index.GetIndexUnsafe(), first_index, duplicate_index);
		issues.push_back(std::move(issue));
	}

	void AddMissingExtensionIdentifier(LogicalExtensionOperator &op) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = GetPath(op);
		issue.construct = LogicalPlanVerificationConstructIdentity::LogicalOperator(op.type);
		issue.facts.emplace_back("invariant", Value("missing_type_binding_verification_identifier"));
		issue.message = "An extension operator with type-binding verification enabled must provide a verification "
		                "identifier";
		issues.push_back(std::move(issue));
	}

	void AddDuplicateTableIndex(LogicalOperator &op, TableIndex table_index) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = GetPath(op);
		issue.construct = GetOperatorConstruct(op);
		issue.facts.emplace_back("invariant", Value("duplicate_table_index"));
		issue.facts.emplace_back("table_index", Value::UBIGINT(table_index.index));
		issue.message = StringUtil::Format("Duplicate table index \"%lld\" found", table_index.index);
		issues.push_back(std::move(issue));
	}

	void AddInvalidTableIndex(LogicalOperator &op, idx_t table_index_ordinal, TableIndex table_index) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = GetPath(op);
		issue.construct = GetOperatorConstruct(op);
		issue.facts.emplace_back("invariant", Value("invalid_table_index"));
		issue.facts.emplace_back("table_index_ordinal", Value::UBIGINT(table_index_ordinal));
		issue.facts.emplace_back("table_index", Value::UBIGINT(table_index.index));
		issue.facts.emplace_back("table_index_valid", Value::BOOLEAN(false));
		issue.message = StringUtil::Format("Invalid table index at ownership ordinal %llu", table_index_ordinal);
		issues.push_back(std::move(issue));
	}

	void AddNullOperatorChild(LogicalOperator &op, const LogicalPlanVerificationPath &path, idx_t child_index) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = path;
		issue.construct = GetOperatorConstruct(op);
		issue.facts.emplace_back("invariant", Value("null_operator_child"));
		issue.facts.emplace_back("child_index", Value::UBIGINT(child_index));
		issue.message = StringUtil::Format("Logical operator child %llu is null", child_index);
		issues.push_back(std::move(issue));
	}

	void AddNullOperatorExpression(LogicalOperator &op, const LogicalPlanVerificationPath &path,
	                               idx_t expression_index) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = path;
		issue.construct = GetOperatorConstruct(op);
		issue.facts.emplace_back("invariant", Value("null_operator_expression"));
		issue.facts.emplace_back("expression_index", Value::UBIGINT(expression_index));
		issue.message = StringUtil::Format("Logical operator expression %llu is null", expression_index);
		issues.push_back(std::move(issue));
	}

	bool HasResolvedInputs(LogicalOperator &op) const {
		return resolved_inputs.find(reference<LogicalOperator>(op)) != resolved_inputs.end();
	}

	bool HasResolvedOutputs(LogicalOperator &op) const {
		return resolved_outputs.find(reference<LogicalOperator>(op)) != resolved_outputs.end();
	}

	void AddInvalidResolvedSchema(LogicalOperator &op) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
		issue.path = GetPath(op);
		issue.construct = GetOperatorConstruct(op);
		issue.message = "Resolved operator bindings require a complete output type for every column";
		issues.push_back(std::move(issue));
	}

	bool HasNullSlots(LogicalOperator &op) const {
		for (auto &child : op.children) {
			if (!child) {
				return true;
			}
		}
		bool has_null = false;
		LogicalOperatorVisitor::EnumerateExpressions(
		    op, [&](unique_ptr<Expression> *expression) { has_null |= !expression || !*expression; });
		return has_null;
	}

private:
	static void AddBindingFacts(LogicalPlanVerificationIssue &issue, const ColumnBinding &binding) {
		issue.facts.emplace_back("table_index", Value::UBIGINT(binding.table_index.index));
		issue.facts.emplace_back("column_index", Value::UBIGINT(binding.column_index.GetIndexUnsafe()));
		issue.facts.emplace_back("table_index_valid", Value::BOOLEAN(binding.table_index.IsValid()));
		issue.facts.emplace_back("column_index_valid", Value::BOOLEAN(binding.column_index.IsValid()));
	}

	static LogicalPlanVerificationConstructIdentity GetOperatorConstruct(LogicalOperator &op) {
		if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
			auto identifier = op.Cast<LogicalExtensionOperator>().GetTypeBindingVerificationIdentifier();
			if (identifier && !identifier->empty()) {
				return LogicalPlanVerificationConstructIdentity::Extension(*identifier);
			}
		}
		return LogicalPlanVerificationConstructIdentity::LogicalOperator(op.type);
	}

	void IndexExpression(Expression &expr, const LogicalPlanVerificationPath &path) {
		expression_paths.emplace(reference<Expression>(expr), path);
		idx_t child_index = 0;
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			auto child_path = path;
			child_path.components.push_back(
			    {LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD, child_index++});
			IndexExpression(child, child_path);
		});
	}

	void IndexOperator(LogicalOperator &op, const LogicalPlanVerificationPath &path) {
		operator_paths.emplace(reference<LogicalOperator>(op), path);
		idx_t expression_index = 0;
		LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
			auto expression_path = path;
			expression_path.components.push_back(
			    {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, expression_index++});
			if (!expression || !*expression) {
				AddNullOperatorExpression(op, expression_path, expression_index - 1);
				return;
			}
			IndexExpression(**expression, expression_path);
		});
		for (idx_t child_index = 0; child_index < op.children.size(); child_index++) {
			auto child_path = path;
			child_path.components.push_back({LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, child_index});
			if (!op.children[child_index]) {
				AddNullOperatorChild(op, child_path, child_index);
				continue;
			}
			IndexOperator(*op.children[child_index], child_path);
		}
	}
};

LogicalPlanVerifier::LogicalPlanVerifier(LogicalPlanVerificationState &verification_state_p)
    : ColumnBindingResolver(true), verification_state(verification_state_p) {
}

unique_ptr<Expression> LogicalPlanVerifier::VisitReplace(BoundColumnRefExpression &expr,
                                                         unique_ptr<Expression> *expr_ptr) {
	D_ASSERT(expr.Depth() == 0);
	for (idx_t binding_index = 0; binding_index < bindings.size(); binding_index++) {
		if (expr.Binding() != bindings[binding_index]) {
			continue;
		}
		if (types.empty()) {
			return nullptr;
		}
		if (bindings.size() != types.size()) {
			verification_state.AddBindingTypeArityMismatch(expr, bindings.size(), types.size());
			return nullptr;
		}
		if (!types[binding_index].IsComplete() || !expr.GetReturnType().IsComplete()) {
			verification_state.AddIncompleteBindingType(expr, types[binding_index]);
			return nullptr;
		}
		if (expr.GetReturnType() != types[binding_index]) {
			verification_state.AddTypeMismatch(expr, types[binding_index]);
		}
		return nullptr;
	}
	verification_state.AddInvalidBinding(expr, bindings);
	return nullptr;
}

void LogicalPlanVerifier::Verify(ClientContext &context, LogicalOperator &op) {
	if (!Settings::Get<DebugVerifyColumnBindingsSetting>(context)) {
		return;
	}
	string first_error;
	auto result = VerifyAlwaysInternal(op, first_error);
	if (result.HasError()) {
		throw InternalException("%s", first_error);
	}
}

bool LogicalPlanVerifier::ResolveOperatorTypes(LogicalOperator &op, LogicalPlanVerificationState &verification_state,
                                               TypeResolution resolution) {
	if (resolution == TypeResolution::RESOLVE) {
		op.types.clear();
	}
	bool children_resolved = true;
	for (auto &child : op.children) {
		if (!child) {
			children_resolved = false;
			continue;
		}
		if (!ResolveOperatorTypes(*child, verification_state, resolution)) {
			children_resolved = false;
		}
	}
	if (!children_resolved || verification_state.HasNullSlots(op)) {
		return false;
	}
	verification_state.resolved_inputs.insert(reference<LogicalOperator>(op));
	if (resolution == TypeResolution::RESOLVE) {
		op.ResolveTypes();
	}
	auto bindings = op.GetColumnBindings();
	if (resolution == TypeResolution::EXISTING) {
		bool valid = bindings.size() == op.types.size();
		for (auto &type : op.types) {
			valid &= type.IsComplete();
		}
		for (auto &binding : bindings) {
			valid &= binding.table_index.IsValid() && binding.column_index.IsValid();
		}
		if (!valid) {
			verification_state.AddInvalidResolvedSchema(op);
			return false;
		}
	}
	if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &extension_op = op.Cast<LogicalExtensionOperator>();
		auto identifier = extension_op.GetTypeBindingVerificationIdentifier();
		if (identifier) {
			if (identifier->empty()) {
				verification_state.AddMissingExtensionIdentifier(extension_op);
				return false;
			}
			bool output_valid = true;
			if (bindings.size() != op.types.size()) {
				verification_state.AddMalformedExtensionArity(extension_op, *identifier, bindings.size(),
				                                              op.types.size());
				output_valid = false;
			}
			column_binding_map_t<idx_t> binding_indexes;
			for (idx_t binding_index = 0; binding_index < bindings.size(); binding_index++) {
				auto &binding = bindings[binding_index];
				if (!binding.table_index.IsValid() || !binding.column_index.IsValid()) {
					verification_state.AddMalformedExtensionBinding(extension_op, *identifier, binding_index, binding);
					output_valid = false;
					continue;
				}
				auto entry = binding_indexes.find(binding);
				if (entry != binding_indexes.end()) {
					auto first_index = entry->second;
					auto types_available = first_index < op.types.size() && binding_index < op.types.size();
					auto types_equal = types_available && op.types[first_index] == op.types[binding_index];
					verification_state.AddDuplicateExtensionBinding(
					    extension_op, *identifier, first_index, binding_index, binding, types_available, types_equal);
					output_valid = false;
				} else {
					binding_indexes.emplace(binding, binding_index);
				}
			}
			for (idx_t type_index = 0; type_index < op.types.size(); type_index++) {
				if (!op.types[type_index].IsComplete()) {
					verification_state.AddMalformedExtensionType(extension_op, *identifier, type_index);
					output_valid = false;
				}
			}
			if (!output_valid) {
				return false;
			}
		}
	}
	D_ASSERT(op.types.size() == bindings.size());
	verification_state.resolved_outputs.insert(reference<LogicalOperator>(op));
	return true;
}

void LogicalPlanVerifier::VerifyColumnBindings(LogicalOperator &op, LogicalPlanVerificationState &verification_state) {
	auto extension_identifier = op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR
	                                ? op.Cast<LogicalExtensionOperator>().GetTypeBindingVerificationIdentifier()
	                                : optional_ptr<const string>();
	if (verification_state.HasResolvedOutputs(op) ||
	    (verification_state.HasResolvedInputs(op) && extension_identifier)) {
		LogicalPlanVerifier verifier(verification_state);
		verifier.VisitOperator(op);
		return;
	}
	for (auto &child : op.children) {
		if (child) {
			VerifyColumnBindings(*child, verification_state);
		}
	}
}

static void VerifyTableIndexes(LogicalOperator &op, LogicalPlanVerificationState &verification_state,
                               unordered_map<TableIndex, LogicalPlanVerificationPath> &seen_indexes) {
	for (auto &child : op.children) {
		if (child) {
			VerifyTableIndexes(*child, verification_state, seen_indexes);
		}
	}
	if (verification_state.HasNullSlots(op)) {
		return;
	}
	auto table_indexes = op.GetTableIndex();
	for (idx_t table_index_ordinal = 0; table_index_ordinal < table_indexes.size(); table_index_ordinal++) {
		auto index = table_indexes[table_index_ordinal];
		if (!index.IsValid()) {
			verification_state.AddInvalidTableIndex(op, table_index_ordinal, index);
			continue;
		}
		if (seen_indexes.find(index) != seen_indexes.end()) {
			verification_state.AddDuplicateTableIndex(op, index);
		} else {
			seen_indexes.emplace(index, verification_state.GetPath(op));
		}
	}
}

LogicalPlanVerificationResult<LogicalPlanVerificationSuccess> LogicalPlanVerifier::VerifyAlways(LogicalOperator &op) {
	return VerifyAlwaysInternal(op, nullptr);
}

LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>
LogicalPlanVerifier::VerifyResolvedBindings(LogicalOperator &op) {
	LogicalPlanVerificationState verification_state(op);
	ResolveOperatorTypes(op, verification_state, TypeResolution::EXISTING);
	VerifyColumnBindings(op, verification_state);
	unordered_map<TableIndex, LogicalPlanVerificationPath> seen_indexes;
	VerifyTableIndexes(op, verification_state, seen_indexes);
	if (!verification_state.issues.empty()) {
		return LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure(
		    std::move(verification_state.issues));
	}
	return LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Success(LogicalPlanVerificationSuccess());
}

LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>
LogicalPlanVerifier::VerifyAlwaysInternal(LogicalOperator &op, optional_ptr<string> first_error) {
	LogicalPlanVerificationState verification_state(op);
	ResolveOperatorTypes(op, verification_state);
	VerifyColumnBindings(op, verification_state);

	unordered_map<TableIndex, LogicalPlanVerificationPath> seen_indexes;
	VerifyTableIndexes(op, verification_state, seen_indexes);
	if (!verification_state.issues.empty()) {
		if (first_error) {
			*first_error = verification_state.issues[0].message;
		}
		return LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure(
		    std::move(verification_state.issues));
	}
	return LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Success(LogicalPlanVerificationSuccess());
}

} // namespace duckdb
