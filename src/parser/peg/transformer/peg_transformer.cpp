#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

namespace duckdb {

TransformStep TransformStep::Child(TransformInput input) {
	return TransformStep(input, nullptr);
}

TransformStep TransformStep::Complete(arena_ptr<TransformResultValue> result) {
	D_ASSERT(result);
	return TransformStep(nullopt, std::move(result));
}

optional<TransformInput> TransformStep::GetChild() {
	return child;
}

arena_ptr<TransformResultValue> TransformStep::TakeResult() {
	D_ASSERT(!child);
	D_ASSERT(result);
	return std::move(result);
}

GeneratedTransformProcess::GeneratedTransformProcess(PEGTransformer &transformer_p, TransformInput input,
                                                     const TransformFrameOps &info_p)
    : parse_result(input.parse_result), info(info_p), child_results(transformer_p.process_allocator),
      transformer(transformer_p), pending_children(transformer_p.process_allocator) {
	if (!info.initialize || !info.finalize) {
		throw InternalException("Incomplete transformer process for rule '%s'", info.name);
	}
	info.initialize(transformer, *this);
}

void GeneratedTransformProcess::ThrowMissingResult(idx_t slot) const {
	throw InternalException("Missing transformer result for slot %llu in rule '%s'", slot, info.name);
}

void GeneratedTransformProcess::ThrowUnexpectedResultType(idx_t slot) const {
	throw InternalException("Unexpected transformer result type for slot %llu in rule '%s'", slot, info.name);
}

void GeneratedTransformProcess::ReserveChildSlots(idx_t count) {
	child_results.resize(count);
	// a rule pushes at most one child per slot, so the pending list never has to grow either
	pending_children.reserve(count);
}

void GeneratedTransformProcess::SetChildResult(idx_t slot, arena_ptr<TransformResultValue> result) {
	if (slot >= child_results.size()) {
		throw InternalException("Invalid transformer result slot %llu for rule '%s'", slot, info.name);
	}
	if (!result) {
		throw InternalException("Cannot set nullptr transformer result for slot %llu in rule '%s'", slot, info.name);
	}
	if (child_results[slot]) {
		throw InternalException("Duplicate transformer result for slot %llu in rule '%s'", slot, info.name);
	}
	child_results[slot] = std::move(result);
}

void GeneratedTransformProcess::PushChild(TransformInput input, idx_t slot) {
	if (slot >= child_results.size()) {
		throw InternalException("Invalid transformer child slot %llu for rule '%s'", slot, info.name);
	}
	pending_children.push_back({input, slot});
}

TransformStep GeneratedTransformProcess::NextStep() {
	if (!pending_children.empty()) {
		auto child = pending_children.back();
		pending_children.pop_back();
		child_result_slot = child.slot;
		return TransformStep::Child(child.input);
	}
	auto result = info.finalize(transformer, *this);
	if (result) {
		completed = true;
		return TransformStep::Complete(std::move(result));
	}
	if (pending_children.empty()) {
		throw InternalException("Transformer process for rule '%s' returned nullptr without requesting a child",
		                        info.name);
	}
	return NextStep();
}

FinalizeTransformProcess::FinalizeTransformProcess(PEGTransformer &transformer_p, ParseResult &parse_result_p,
                                                   transform_finalize_function_t finalize_p)
    : transformer(transformer_p), parse_result(parse_result_p), finalize(std::move(finalize_p)) {
}

TransformStep FinalizeTransformProcess::Resume(arena_ptr<TransformResultValue> child_result) {
	D_ASSERT(!completed);
	D_ASSERT(!child_result);
	auto result = finalize(transformer, parse_result);
	if (!result) {
		throw InternalException("Transformer for rule '%s' returned a nullptr", parse_result.Name());
	}
	completed = true;
	return TransformStep::Complete(std::move(result));
}

arena_ptr<TransformProcess> CompiledGrammarRule::StartTransform(PEGTransformer &transformer,
                                                                ParseResult &parse_result) const {
	if (!transform_process) {
		throw NotImplementedException("No transform process found for rule '%s'", parse_result.Name());
	}
	auto result = transform_process(transformer, parse_result);
	if (!result) {
		throw InternalException("Transform process factory for rule '%s' returned a nullptr", parse_result.Name());
	}
	return result;
}

TransformStep GeneratedTransformProcess::Resume(arena_ptr<TransformResultValue> child_result) {
	D_ASSERT(!completed);
	D_ASSERT(child_result_slot.IsValid() == bool(child_result));
	if (child_result) {
		SetChildResult(child_result_slot.GetIndex(), std::move(child_result));
		child_result_slot = optional_idx();
	}
	return NextStep();
}

TransformStackFrame::TransformStackFrame(TransformInput input)
    : rule(input.GetRule()), parse_result(input.parse_result) {
}

TransformStack::TransformStack(PEGTransformer &transformer_p) : transformer(transformer_p) {
	frames.reserve(32);
}

TransformStack::~TransformStack() {
	// release the expression depth of frames that remain after an exception
	while (!frames.empty()) {
		PopFrame();
	}
}

void TransformStack::PushFrame(TransformInput input) {
	frames.emplace_back(input);
	auto &frame = frames.back();
	frame.process_arena_position = transformer.process_allocator.GetPosition();
	auto weight = transformer.ExpressionDepthWeight(frame.rule, frame.parse_result);
	if (weight) {
		// recorded first: the frame is released (and the weight subtracted) when entering throws
		frame.expression_depth_weight = weight;
		transformer.EnterExpression(weight);
	}
}

void TransformStack::PopFrame() {
	const auto process_arena_position = frames.back().process_arena_position;
	if (frames.back().expression_depth_weight) {
		transformer.ExitExpression(frames.back().expression_depth_weight);
	}
	frames.pop_back();
	transformer.process_allocator.Rewind(process_arena_position);
}

void TransformStack::InitializeFrame(TransformStackFrame &frame) {
	if (!frame.rule) {
		throw InternalException("No registered data exists for rule '%s'", frame.parse_result.Name());
	}
	frame.process = frame.rule->StartTransform(transformer, frame.parse_result);
}

arena_ptr<TransformResultValue> TransformStack::ExecuteFrame(TransformStackFrame &frame) {
	if (!frame.process) {
		InitializeFrame(frame);
	}
	D_ASSERT(frame.process);
	auto step = frame.process->Resume(std::move(frame.child_result));
	auto child = step.GetChild();
	if (!child) {
		return step.TakeResult();
	}
	PushFrame(*child);
	return nullptr;
}

arena_ptr<TransformResultValue> TransformStack::Execute(TransformInput input) {
	D_ASSERT(frames.empty());
	if (!input.GetRule()) {
		throw InternalException("No registered data exists for rule '%s'", input.parse_result.Name());
	}
	PushFrame(input);
	while (!frames.empty()) {
		auto result = ExecuteFrame(frames.back());
		if (!result) {
			continue;
		}
		transformer.SetResultLocation(frames.back().parse_result, *result);
		PopFrame();
		if (frames.empty()) {
			return result;
		}
		auto &parent = frames.back();
		D_ASSERT(!parent.child_result);
		parent.child_result = std::move(result);
	}
	throw InternalException("Transformer stack completed without a result");
}

#ifdef DEBUG
string TransformStack::FormatStack() const {
	stringstream result;
	for (idx_t i = 0; i < frames.size(); i++) {
		if (i > 0) {
			result << "\n";
		}
		auto &parse_result = frames[i].parse_result;
		result << "#" << i << " " << parse_result.Name();
		auto offset = parse_result.GetOffset();
		if (offset.IsValid()) {
			result << " offset=" << offset.GetIndex();
		}
	}
	return result.str();
}
#endif

arena_ptr<TransformResultValue> PEGTransformer::TransformInternal(ParseResult &parse_result) {
	auto rule = parse_result.GetRule();
	if (!rule) {
		throw InternalException("No registered data exists for rule '%s'", parse_result.Name());
	}
	TransformInput input {*rule, parse_result};
	TransformStack stack(*this);
	return stack.Execute(input);
}

//! The number of repeated elements directly in a rule's match: the links of an operator chain
static idx_t CountChainLinks(ParseResult &parse_result) {
	switch (parse_result.type) {
	case ParseResultType::REPEAT:
		return parse_result.Cast<RepeatParseResult>().GetChildren().size();
	case ParseResultType::OPTIONAL: {
		auto &optional = parse_result.Cast<OptionalParseResult>();
		return optional.HasResult() ? CountChainLinks(optional.GetResultUnsafe()) : 0;
	}
	case ParseResultType::LIST: {
		idx_t links = 0;
		for (auto &child : parse_result.Cast<ListParseResult>().GetChildren()) {
			auto type = child.get().type;
			if (type == ParseResultType::REPEAT || type == ParseResultType::OPTIONAL) {
				links += CountChainLinks(child.get());
			}
		}
		return links;
	}
	default:
		return 0;
	}
}

idx_t PEGTransformer::ExpressionDepthWeight(optional_ptr<const CompiledGrammarRule> rule, ParseResult &parse_result) {
	if (!rule) {
		return 0;
	}
	switch (rule->expression_depth) {
	case ExpressionDepthKind::NESTING:
		return 1;
	case ExpressionDepthKind::CHAIN:
		return CountChainLinks(parse_result);
	default:
		return 0;
	}
}

void PEGTransformer::EnterExpression(idx_t weight) {
	// the transformer does not recurse, but binding the resulting expressions does - reject them early
	expression_depth += weight;
	if (expression_depth > options.max_expression_depth) {
		throw ParserException("Max expression depth limit of %lld exceeded. Use \"SET max_expression_depth TO x\" to "
		                      "increase the maximum expression depth.",
		                      options.max_expression_depth);
	}
}

void PEGTransformer::ExitExpression(idx_t weight) {
	D_ASSERT(expression_depth >= weight);
	expression_depth -= weight;
}

const CompiledGrammarRule &PEGTransformer::GetRule(PEGRuleId rule_id) const {
	auto rule = grammar.GetRule(rule_id);
	if (!rule) {
		auto rule_name = PEGRuleIdToString(rule_id);
		throw InternalException("No registered data exists for rule '%s'", rule_name ? rule_name : "<runtime rule>");
	}
	return *rule;
}

const CompiledGrammarRule &PEGTransformer::GetRule(const string &rule_name) const {
	auto rule = grammar.GetRule(rule_name);
	if (!rule) {
		throw InternalException("No registered data exists for rule '%s'", rule_name);
	}
	return *rule;
}

void PEGTransformer::SetResultLocation(ParseResult &parse_result, TransformResultValue &result) {
	if (!parse_result.GetOffset().IsValid()) {
		return;
	}
	auto expression_result = TryGetTransformResult<unique_ptr<ParsedExpression>>(result);
	if (expression_result && *expression_result && !(*expression_result)->HasQueryLocation()) {
		SetQueryLocation(**expression_result, parse_result.GetLocation());
		return;
	}
	auto table_ref_result = TryGetTransformResult<unique_ptr<TableRef>>(result);
	if (table_ref_result && *table_ref_result && !(*table_ref_result)->query_location.IsValid()) {
		SetQueryLocation(**table_ref_result, parse_result.GetLocation());
	}
}

void PEGTransformer::ParamTypeCheck(PreparedParamType last_type, PreparedParamType new_type) {
	// Mixing positional/auto-increment and named parameters is not supported
	if (last_type == PreparedParamType::INVALID) {
		return;
	}
	if (last_type == PreparedParamType::NAMED) {
		if (new_type != PreparedParamType::NAMED) {
			throw NotImplementedException("Mixing named and positional parameters is not supported yet");
		}
	}
	if (last_type != PreparedParamType::NAMED) {
		if (new_type == PreparedParamType::NAMED) {
			throw NotImplementedException("Mixing named and positional parameters is not supported yet");
		}
	}
}

bool PEGTransformer::GetParam(const Identifier &identifier, idx_t &index, PreparedParamType type) {
	ParamTypeCheck(last_param_type, type);
	auto entry = named_parameter_map.find(identifier);
	if (entry == named_parameter_map.end()) {
		return false;
	}
	index = entry->second;
	return true;
}

void PEGTransformer::SetParam(const Identifier &identifier, idx_t index, PreparedParamType type) {
	ParamTypeCheck(last_param_type, type);
	last_param_type = type;
	D_ASSERT(!named_parameter_map.count(identifier));
	named_parameter_map[identifier] = index;
}

void PEGTransformer::ClearParameters() {
	prepared_statement_parameter_index = 0;
	last_param_type = PreparedParamType::INVALID;
	named_parameter_map.clear();
	has_anonymous_parameters = false;
}

void PEGTransformer::Clear() {
	ClearParameters();
	pivot_entries.clear();
	stored_cte_map.clear();
	window_clauses.clear();
}

idx_t PEGTransformer::ParamCount() const {
	return prepared_statement_parameter_index;
}

void PEGTransformer::SetParamCount(idx_t new_count) {
	prepared_statement_parameter_index = new_count;
}

unique_ptr<SQLStatement> PEGTransformer::GenerateCreateEnumStmt(unique_ptr<CreatePivotEntry> entry) {
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTypeInfo>();
	info->temporary = true;
	info->internal = false;
	info->SetQualifiedName(QualifiedName(Identifier(std::move(entry->enum_name))));
	info->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;

	// generate the query that will result in the enum creation
	unique_ptr<QueryNode> subselect;
	if (!entry->subquery) {
		auto select_node = std::move(entry->base);
		auto columnref = entry->column->Copy();
		auto cast = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(columnref));
		select_node->select_list.push_back(std::move(cast));

		auto is_not_null =
		    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(entry->column));
		select_node->where_clause = std::move(is_not_null);

		// order by the column
		select_node->modifiers.push_back(make_uniq<DistinctModifier>());
		auto modifier = make_uniq<OrderModifier>();
		modifier->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT,
		                              ConstantExpression::Integer(1));
		select_node->modifiers.push_back(std::move(modifier));
		subselect = std::move(select_node);
	} else {
		subselect = std::move(entry->subquery);
	}

	auto select = make_uniq<SelectStatement>();
	select->node = std::move(subselect);
	info->query = std::move(select);
	info->type = LogicalType::INVALID;

	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformer::CreatePivotStatement(unique_ptr<SQLStatement> statement) {
	auto result = make_uniq<MultiStatement>();
	for (auto &pivot : pivot_entries) {
		if (pivot->has_parameters) {
			throw ParserException(
			    "PIVOT statements with pivot elements extracted from the data cannot have parameters in their source.\n"
			    "In order to use parameters the PIVOT values must be manually specified, e.g.:\n"
			    "PIVOT ... ON %s IN (val1, val2, ...)",
			    pivot->column->ToString());
		}
		auto enum_stmt = GenerateCreateEnumStmt(std::move(pivot));
		enum_stmt->query = enum_stmt->ToString();
		result->statements.push_back(std::move(enum_stmt));
	}
	result->stmt_location = statement->stmt_location;
	statement->query = statement->ToString();
	result->statements.push_back(std::move(statement));
	return std::move(result);
}

void PEGTransformer::PivotEntryCheck(const string &type) {
	if (!pivot_entries.empty()) {
		throw ParserException(
		    "PIVOT statements with pivot elements extracted from the data cannot be used in %ss.\nIn order to use "
		    "PIVOT in a %s the PIVOT values must be manually specified, e.g.:\nPIVOT ... ON %s IN (val1, val2, ...)",
		    type, type, pivot_entries[0]->column->ToString());
	}
}

void PEGTransformer::ExtractCTEsRecursive(CommonTableExpressionMap &cte_map) {
	// Traverse the stack from the most recent scope back to the global scope
	// Use reverse iterator if you push new scopes to the back
	for (auto it = stored_cte_map.rbegin(); it != stored_cte_map.rend(); ++it) {
		auto &current_scope = it->get();
		for (auto &entry : current_scope.map) {
			// Check if this CTE name is already in our result map
			if (cte_map.map.find(entry.first) == cte_map.map.end()) {
				cte_map.map[entry.first] = entry.second->Copy();
			}
		}
	}
}

bool PEGTransformer::IsWindowFrameDefault(WindowBoundary start, WindowBoundary end) {
	bool start_is_default = (start == WindowBoundary::UNBOUNDED_PRECEDING);
	bool end_is_default = (end == WindowBoundary::CURRENT_ROW_RANGE);
	return start_is_default && end_is_default;
}

unique_ptr<WindowExpression> PEGTransformer::GetWindowClause(const Identifier &window_name) {
	if (window_clauses.empty()) {
		throw ParserException("window %s does not exist", window_name);
	}
	auto &current_windows = window_clauses.back();
	auto it = current_windows.find(window_name);
	if (it == current_windows.end()) {
		throw ParserException("window %s does not exist", window_name);
	}
	return unique_ptr_cast<ParsedExpression, WindowExpression>(it->second->Copy());
}

void PEGTransformer::SetQueryLocation(ParsedExpression &expr, QueryLocation query_location) {
	expr.SetQueryLocation(query_location);
}

void PEGTransformer::SetQueryLocation(TableRef &ref, QueryLocation query_location) {
	ref.query_location = query_location;
}

} // namespace duckdb
