#include "duckdb/optimizer/filter_combiner.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

using ExpressionValueInformation = FilterCombiner::ExpressionValueInformation;

static ValueComparisonResult CompareValueInformation(ExpressionValueInformation &left,
                                                     ExpressionValueInformation &right);

FilterCombiner::FilterCombiner(ClientContext &context) : context(context) {
}

FilterCombiner::FilterCombiner(Optimizer &optimizer) : FilterCombiner(optimizer.context) {
}

Expression &FilterCombiner::GetNode(Expression &expr) {
	auto entry = stored_expressions.find(expr);
	if (entry != stored_expressions.end()) {
		// expression already exists: return a reference to the stored expression
		return *entry->second;
	}
	// expression does not exist yet: create a copy and store it
	auto copy = expr.Copy();
	auto &copy_ref = *copy;
	D_ASSERT(stored_expressions.find(copy_ref) == stored_expressions.end());
	stored_expressions[copy_ref] = std::move(copy);
	return copy_ref;
}

idx_t FilterCombiner::GetEquivalenceSet(Expression &expr) {
	D_ASSERT(stored_expressions.find(expr) != stored_expressions.end());
	D_ASSERT(stored_expressions.find(expr)->second.get() == &expr);
	auto entry = equivalence_set_map.find(expr);
	if (entry == equivalence_set_map.end()) {
		idx_t index = set_index++;
		equivalence_set_map[expr] = index;
		equivalence_map[index].push_back(expr);
		constant_values.insert(make_pair(index, vector<ExpressionValueInformation>()));
		return index;
	} else {
		return entry->second;
	}
}

FilterResult FilterCombiner::AddConstantComparison(vector<ExpressionValueInformation> &info_list,
                                                   ExpressionValueInformation info) {
	if (info.constant.IsNull()) {
		return FilterResult::UNSATISFIABLE;
	}
	for (idx_t i = 0; i < info_list.size(); i++) {
		auto comparison = CompareValueInformation(info_list[i], info);
		switch (comparison) {
		case ValueComparisonResult::PRUNE_LEFT:
			// prune the entry from the info list
			info_list.erase_at(i);
			i--;
			break;
		case ValueComparisonResult::PRUNE_RIGHT:
			// prune the current info
			return FilterResult::SUCCESS;
		case ValueComparisonResult::UNSATISFIABLE_CONDITION:
			// combination of filters is unsatisfiable: prune the entire branch
			info_list.push_back(info);
			return FilterResult::UNSATISFIABLE;
		default:
			// prune nothing, move to the next condition
			break;
		}
	}
	// finally add the entry to the list
	info_list.push_back(info);
	return FilterResult::SUCCESS;
}

FilterResult FilterCombiner::AddFilter(unique_ptr<Expression> expr) {
	//	LookUpConjunctions(expr.get());
	// try to push the filter into the combiner
	auto result = AddFilter(*expr);
	if (result == FilterResult::UNSUPPORTED) {
		// unsupported filter, push into remaining filters
		remaining_filters.push_back(std::move(expr));
		return FilterResult::SUCCESS;
	}
	return result;
}

FilterResult FilterCombiner::AddConjuncts(unique_ptr<Expression> expr) {
	vector<unique_ptr<Expression>> conjuncts;
	conjuncts.push_back(std::move(expr));
	LogicalFilter::SplitPredicates(conjuncts);
	for (auto &conjunct : conjuncts) {
		if (AddFilter(std::move(conjunct)) == FilterResult::UNSATISFIABLE) {
			return FilterResult::UNSATISFIABLE;
		}
	}
	return FilterResult::SUCCESS;
}

void FilterCombiner::GenerateEquivalentFilters(const Expression &filter,
                                               const std::function<void(unique_ptr<Expression> filter)> &callback) {
	if (filter.IsVolatile()) {
		return;
	}
	// collect every column reference that belongs to an equivalence set
	vector<reference<const BoundColumnRefExpression>> candidate_columns;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(filter, [&](const BoundColumnRefExpression &col) {
		auto entry = equivalence_set_map.find(col);
		if (entry == equivalence_set_map.end()) {
			return;
		}
		for (auto &existing : candidate_columns) {
			if (existing.get().Equals(col)) {
				return;
			}
		}
		candidate_columns.push_back(col);
	});
	// for each such column, generate an equivalent filter with the columns swapped
	for (auto &col_ref : candidate_columns) {
		auto &col = col_ref.get();
		auto set_id = equivalence_set_map.find(col)->second;
		for (auto &item : equivalence_map[set_id]) {
			if (item.get().Equals(col)) {
				continue;
			}
			callback(ExpressionIterator::ReplaceExpression(filter, col, item.get()));
		}
	}
}

void FilterCombiner::GenerateFilters(const std::function<void(unique_ptr<Expression> filter)> &callback) {
	// first loop over the remaining filters
	for (auto &filter : remaining_filters) {
		callback(std::move(filter));
	}
	remaining_filters.clear();
	// now loop over the equivalence sets
	for (auto &entry : equivalence_map) {
		auto equivalence_set = entry.first;
		auto &entries = entry.second;
		D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
		auto &constant_list = constant_values.find(equivalence_set)->second;
		// for each entry generate an equality expression comparing to each other
		for (idx_t i = 0; i < entries.size(); i++) {
			for (idx_t k = i + 1; k < entries.size(); k++) {
				auto comparison = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL,
				                                                    entries[i].get().Copy(), entries[k].get().Copy());
				callback(std::move(comparison));
			}
			// for each entry also create a comparison with each constant
			auto lower_index = optional_idx::Invalid();
			auto upper_index = optional_idx::Invalid();
			bool lower_inclusive = false;
			bool upper_inclusive = false;
			for (idx_t k = 0; k < constant_list.size(); k++) {
				auto &info = constant_list[k];
				if (info.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
				    info.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
					lower_index = k;
					lower_inclusive = info.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
				} else if (info.comparison_type == ExpressionType::COMPARE_LESSTHAN ||
				           info.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
					upper_index = k;
					upper_inclusive = info.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
				} else {
					auto constant = make_uniq<BoundConstantExpression>(info.constant);
					auto comparison = BoundComparisonExpression::Create(info.comparison_type, entries[i].get().Copy(),
					                                                    std::move(constant));
					// column refs are already covered above
					if (entries[i].get().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
						GenerateEquivalentFilters(*comparison, callback);
					}
					callback(std::move(comparison));
				}
			}
			if (lower_index.IsValid() && upper_index.IsValid()) {
				auto &lower_value = constant_list[lower_index.GetIndex()].constant;
				auto &upper_value = constant_list[upper_index.GetIndex()].constant;
				if (lower_inclusive && upper_inclusive && lower_value == upper_value) {
					auto constant = make_uniq<BoundConstantExpression>(lower_value);
					auto comparison = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL,
					                                                    entries[i].get().Copy(), std::move(constant));
					callback(std::move(comparison));
				} else {
					auto lower_constant = make_uniq<BoundConstantExpression>(lower_value);
					auto upper_constant = make_uniq<BoundConstantExpression>(upper_value);
					auto between =
					    BoundBetweenExpression::Create(entries[i].get().Copy(), std::move(lower_constant),
					                                   std::move(upper_constant), lower_inclusive, upper_inclusive);
					callback(std::move(between));
				}
			} else if (lower_index.IsValid()) {
				// only lower index found, create simple comparison expression
				auto constant = make_uniq<BoundConstantExpression>(constant_list[lower_index.GetIndex()].constant);
				auto comparison =
				    BoundComparisonExpression::Create(constant_list[lower_index.GetIndex()].comparison_type,
				                                      entries[i].get().Copy(), std::move(constant));
				callback(std::move(comparison));
			} else if (upper_index.IsValid()) {
				// only upper index found, create simple comparison expression
				auto constant = make_uniq<BoundConstantExpression>(constant_list[upper_index.GetIndex()].constant);
				auto comparison =
				    BoundComparisonExpression::Create(constant_list[upper_index.GetIndex()].comparison_type,
				                                      entries[i].get().Copy(), std::move(constant));
				callback(std::move(comparison));
			}
		}
	}
	stored_expressions.clear();
	equivalence_set_map.clear();
	constant_values.clear();
	equivalence_map.clear();
}

bool FilterCombiner::HasFilters() {
	bool has_filters = false;
	GenerateFilters([&](unique_ptr<Expression> child) { has_filters = true; });
	return has_filters;
}

// Try to extract a column index from a bound column ref expression, or a column ref recursively nested
// inside of a struct_extract call. If the expression is not a column ref (or nested column ref), return false.
static bool TryGetProjectionIndex(const Expression &expr, ProjectionIndex &result) {
	switch (expr.GetExpressionType()) {
	case ExpressionType::BOUND_COLUMN_REF: {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		result = ref.Binding().column_index;
		return true;
	}
	case ExpressionType::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == "struct_extract" || func.Function().GetName() == "struct_extract_at") {
			auto &child_expr = func.GetChildren()[0];
			return TryGetProjectionIndex(*child_expr, result);
		}
		return false;
	}
	default:
		return false;
	}
}

static void ReplaceWithBoundReference(unique_ptr<Expression> &root_expr);

static unique_ptr<Expression> CreateFilterTargetExpression(const Expression &expr) {
	auto result = expr.Copy();
	ReplaceWithBoundReference(result);
	return result;
}

static unique_ptr<Expression> CreateComparisonExpression(const Expression &expr, ExpressionType comparison_type,
                                                         Value constant) {
	auto left = CreateFilterTargetExpression(expr);
	auto right = make_uniq<BoundConstantExpression>(std::move(constant));
	return BoundComparisonExpression::Create(comparison_type, std::move(left), std::move(right));
}

static unique_ptr<ExpressionFilter> CreateOptionalExpressionFilter(unique_ptr<Expression> child_expr,
                                                                   const LogicalType &col_type) {
	return make_uniq<ExpressionFilter>(CreateOptionalFilterExpression(std::move(child_expr), col_type));
}

bool FilterCombiner::ContainsNull(vector<Value> &in_list) {
	for (idx_t i = 0; i < in_list.size(); i++) {
		if (in_list[i].IsNull()) {
			return true;
		}
	}
	return false;
}

bool FilterCombiner::IsDenseRange(vector<Value> &in_list) {
	if (in_list.empty()) {
		return true;
	}
	if (!in_list[0].type().IsIntegral() || in_list[0].type() == LogicalType::UHUGEINT) {
		return false;
	}
	// sort the input list
	sort(in_list.begin(), in_list.end());

	// check if the gap between each value is exactly one
	hugeint_t prev_value = in_list[0].GetValue<hugeint_t>();
	for (idx_t i = 1; i < in_list.size(); i++) {
		hugeint_t current_value = in_list[i].GetValue<hugeint_t>();
		hugeint_t diff;
		if (!TrySubtractOperator::Operation(current_value, prev_value, diff)) {
			// if subtract would overflow then it's certainly not 1
			return false;
		}
		if (diff != 1) {
			// gap is not 1 - this is not a dense range
			return false;
		}
		prev_value = current_value;
	}
	// dense range
	return true;
}

static bool SupportedFilterComparison(ExpressionType expression_type) {
	switch (expression_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_NOTEQUAL:
		return true;
	default:
		return false;
	}
}

static bool TypeSupportsConstantFilter(const LogicalType &type) {
	if (TypeIsNumeric(type.InternalType())) {
		return true;
	}
	if (type.InternalType() == PhysicalType::VARCHAR || type.InternalType() == PhysicalType::BOOL) {
		return true;
	}
	return false;
}

FilterPushdownResult FilterCombiner::TryPushdownConstantFilter(TableFilterSet &table_filters,
                                                               const vector<ColumnIndex> &column_ids, column_t expr_id,
                                                               vector<ExpressionValueInformation> &info_list) {
	if (info_list.empty()) {
		// no constants - already removed
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &constant_info = info_list[0];
	// check if the type is supported
	if (!TypeSupportsConstantFilter(constant_info.constant.type())) {
		// not supported
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	if (!SupportedFilterComparison(constant_info.comparison_type)) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	//! Here we check if these filters are column references
	auto filter_exp = equivalence_map.find(expr_id);
	D_ASSERT(filter_exp != equivalence_map.end());
	if (filter_exp->second.size() != 1) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}

	auto &expr = filter_exp->second[0];
	auto equiv_set = filter_exp->first;

	// Try to get the column index, either from bound column ref, or a column ref nested inside of a
	// struct_extract call
	ProjectionIndex proj_index;
	if (!TryGetProjectionIndex(expr, proj_index)) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	D_ASSERT(constant_values.find(equiv_set) != constant_values.end());
	auto &constant_list = constant_values.find(equiv_set)->second;
	for (auto &constant_cmp : constant_list) {
		auto filter_expr = CreateComparisonExpression(expr, constant_cmp.comparison_type, constant_cmp.constant);
		table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(filter_expr)));
	}
	equivalence_map.erase(filter_exp);
	return FilterPushdownResult::PUSHED_DOWN_FULLY;
}

void ReplaceWithBoundReference(unique_ptr<Expression> &root_expr) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    root_expr, [&](BoundColumnRefExpression &col_ref, unique_ptr<Expression> &expr) {
		    expr = make_uniq<BoundReferenceExpression>(col_ref.GetAlias(), col_ref.GetReturnType(), 0ULL);
	    });
}

static unique_ptr<ExpressionFilter> TryCreateMultiColumnExpressionFilter(LogicalGet &get, const Expression &expr,
                                                                         const vector<ColumnBinding> &bindings) {
	vector<ColumnBinding> distinct_bindings;
	distinct_bindings.reserve(bindings.size());
	// Maps column bindings to dense BoundReference indexes.
	column_binding_map_t<idx_t> binding_indexes;
	for (const auto &binding : bindings) {
		const auto insert_result = binding_indexes.emplace(binding, distinct_bindings.size());
		if (insert_result.second) {
			distinct_bindings.push_back(binding);
		}
	}
	if (distinct_bindings.size() <= 1) {
		return nullptr;
	}

	vector<ProjectionIndex> column_indexes;
	column_indexes.reserve(distinct_bindings.size());
	for (const auto &binding : distinct_bindings) {
		if (binding.table_index != get.table_index || binding.column_index >= get.GetColumnIds().size()) {
			return nullptr;
		}
		auto &column_id = get.GetColumnIds()[binding.column_index];
		if (column_id.IsVirtualColumn() && !column_id.IsRowIdColumn()) {
			return nullptr;
		}
		column_indexes.emplace_back(binding.column_index);
	}

	auto filter_expr = expr.Copy();
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    filter_expr, [&](BoundColumnRefExpression &column_ref, unique_ptr<Expression> &child) {
		    const auto entry = binding_indexes.find(column_ref.Binding());
		    D_ASSERT(entry != binding_indexes.end());
		    child =
		        make_uniq<BoundReferenceExpression>(column_ref.GetAlias(), column_ref.GetReturnType(), entry->second);
	    });
	// Remove query-specific metadata so equivalent filters serialize identically for common-subplan matching.
	ExpressionIterator::EnumerateExpression(filter_expr, [](Expression &expr) {
		expr.ClearAlias();
		expr.SetQueryLocation(optional_idx());
	});
	return make_uniq<ExpressionFilter>(std::move(filter_expr), std::move(column_indexes));
}

FilterPushdownResult FilterCombiner::TryPushdownGenericExpression(LogicalGet &get, Expression &expr) {
	if (!get.function.pushdown_expression) {
		// the scan does not support pushing down generic expressions
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	// extract the column bindings from this expression
	vector<ColumnBinding> bindings;
	ColumnLifetimeAnalyzer::ExtractColumnBindings(expr, bindings);
	if (bindings.empty()) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	bool has_multiple_bindings = false;
	for (idx_t binding_idx = 1; binding_idx < bindings.size(); ++binding_idx) {
		if (bindings[binding_idx] != bindings[0]) {
			has_multiple_bindings = true;
			break;
		}
	}
	if (has_multiple_bindings) {
		auto table = get.GetTable();
		if (table && table->IsDuckTable() && ExpressionFilter::CanPropagateExpressionStatistics(expr)) {
			auto filter = TryCreateMultiColumnExpressionFilter(get, expr, bindings);
			if (filter) {
				get.table_filters.PushMultiColumnFilter(std::move(filter));
				return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
			}
		}
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	if (!get.function.pushdown_expression(context, get, expr)) {
		// the scan does not support pushing down THIS expression
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	// replace the BoundColumnRefExpression with a BoundReference
	auto filter_expr = expr.Copy();
	ReplaceWithBoundReference(filter_expr);

	// push the expression filter
	auto expr_filter = make_uniq<ExpressionFilter>(std::move(filter_expr));
	get.table_filters.PushFilter(bindings[0].column_index, std::move(expr_filter));
	return FilterPushdownResult::PUSHED_DOWN_FULLY;
}

FilterPushdownResult FilterCombiner::TryPushdownPrefixFilter(TableFilterSet &table_filters,
                                                             const vector<ColumnIndex> &column_ids, Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &function_name = func.Function().GetName();
	if (function_name != "prefix" && function_name != "starts_with" && function_name != "^@") {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	if (func.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    func.GetChildren()[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		// we need prefix(col, 'literal') in order to push this down
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &column_ref = func.GetChildren()[0]->Cast<BoundColumnRefExpression>();
	auto &constant_value_expr = func.GetChildren()[1]->Cast<BoundConstantExpression>();
	if (constant_value_expr.GetValue().IsNull()) {
		// prefix(col, NULL) is NULL and filters out every row - leave the filter in place, do not push it down
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto prefix_string = StringValue::Get(constant_value_expr.GetValue());
	if (prefix_string.empty()) {
		// empty prefix - skip
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto filter_idx = column_ref.Binding().column_index;
	//! Replace prefix with a set of comparisons
	auto lower_bound = CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	                                              Value(prefix_string));
	table_filters.PushFilter(filter_idx, make_uniq<ExpressionFilter>(std::move(lower_bound)));
	if (Utf8Proc::FindNextLegalUTF8(prefix_string)) {
		auto upper_bound =
		    CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_LESSTHAN, Value(prefix_string));
		table_filters.PushFilter(filter_idx, make_uniq<ExpressionFilter>(std::move(upper_bound)));
		return FilterPushdownResult::PUSHED_DOWN_FULLY;
	}
	// could not find next legal utf8 string - skip upper bound
	return FilterPushdownResult::NO_PUSHDOWN;
}

static bool GetCaseInsensitivePrefixBounds(const string &prefix, string &min_prefix, string &max_prefix) {
	min_prefix.reserve(prefix.size());
	max_prefix.reserve(prefix.size());
	for (auto c : prefix) {
		auto byte = static_cast<uint8_t>(c);
		if (byte & 0x80) {
			return false;
		}
		auto lower_byte = StringUtil::ASCII_TO_LOWER_MAP[byte];
		min_prefix.push_back(UnsafeNumericCast<char>(StringUtil::ASCII_TO_UPPER_MAP[byte]));
		switch (lower_byte) {
		case 'i':
			max_prefix += "\xC4\xB0"; // U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE
			break;
		case 'k':
			max_prefix += "\xE2\x84\xAA"; // U+212A KELVIN SIGN
			break;
		default:
			max_prefix.push_back(UnsafeNumericCast<char>(lower_byte));
			break;
		}
	}
	return !min_prefix.empty() && Utf8Proc::FindNextLegalUTF8(max_prefix);
}

FilterPushdownResult FilterCombiner::TryPushdownLikeFilter(TableFilterSet &table_filters,
                                                           const vector<ColumnIndex> &column_ids, Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &function_name = func.Function().GetName();
	const bool case_insensitive = function_name == "~~*";
	if (function_name != "~~" && !case_insensitive) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	if (func.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    func.GetChildren()[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		// we need col LIKE 'literal' in order to generate extra filters
		return FilterPushdownResult::NO_PUSHDOWN;
	}

	//! This is a like function.
	auto &column_ref = func.GetChildren()[0]->Cast<BoundColumnRefExpression>();
	auto &constant_value_expr = func.GetChildren()[1]->Cast<BoundConstantExpression>();
	auto proj_index = column_ref.Binding().column_index;

	if (constant_value_expr.GetValue().IsNull()) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &like_string = StringValue::Get(constant_value_expr.GetValue());
	if (like_string[0] == '%' || like_string[0] == '_') {
		//! If the like starts with a special character we have no fixed prefix so nothing to pushdown
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	string prefix;
	bool equality = true;
	for (char const &c : like_string) {
		if (c == '%' || c == '_') {
			equality = false;
			break;
		}
		prefix += c;
	}
	if (equality && !case_insensitive) {
		//! If the LIKE has no special characters we can turn it into an equality and push that down
		auto equal_filter =
		    CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_EQUAL, Value(prefix));
		table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(equal_filter)));
		return FilterPushdownResult::PUSHED_DOWN_FULLY;
	}
	if (case_insensitive) {
		string min_prefix;
		string max_prefix;
		if (!GetCaseInsensitivePrefixBounds(prefix, min_prefix, max_prefix)) {
			return FilterPushdownResult::NO_PUSHDOWN;
		}
		auto lower_bound = CreateComparisonExpression(
		    *func.GetChildren()[0], ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value(std::move(min_prefix)));
		table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(lower_bound)));
		auto upper_bound = CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_LESSTHAN,
		                                              Value(std::move(max_prefix)));
		table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(upper_bound)));
		return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
	}

	//! We have a prefix - we can push down the prefix using a bound (x >= PREFIX AND x < next_prefix)
	// Note that we still need to execute the LIKE filter
	auto lower_bound =
	    CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value(prefix));
	table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(lower_bound)));
	if (Utf8Proc::FindNextLegalUTF8(prefix)) {
		auto upper_bound =
		    CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_LESSTHAN, Value(prefix));
		table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(upper_bound)));
	}
	return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
}

static bool TryPushdownDenseInFilter(TableFilterSet &table_filters, const BoundColumnRefExpression &column,
                                     vector<Value> &values) {
	if (values.empty() || !FilterCombiner::IsDenseRange(values)) {
		return false;
	}
	// IsDenseRange sorts the values and only accepts consecutive integers.
	auto lower_bound = CreateComparisonExpression(column, ExpressionType::COMPARE_GREATERTHANOREQUALTO, values.front());
	auto upper_bound =
	    CreateComparisonExpression(column, ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(values.back()));
	table_filters.PushFilter(column.Binding().column_index, make_uniq<ExpressionFilter>(std::move(lower_bound)));
	table_filters.PushFilter(column.Binding().column_index, make_uniq<ExpressionFilter>(std::move(upper_bound)));
	return true;
}

FilterPushdownResult FilterCombiner::TryPushdownInFilter(TableFilterSet &table_filters,
                                                         const vector<ColumnIndex> &column_ids, Expression &expr) {
	if (expr.GetExpressionType() != ExpressionType::COMPARE_IN) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &func = expr.Cast<BoundOperatorExpression>();
	D_ASSERT(func.GetChildren().size() > 1);
	if (func.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		// we need col IN (...) to be able to push this down
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &column_ref = func.GetChildren()[0]->Cast<BoundColumnRefExpression>();
	auto proj_index = column_ref.Binding().column_index;

	//! check if all children are const expr
	bool children_constant = true;
	for (size_t i {1}; i < func.GetChildren().size(); i++) {
		if (func.GetChildren()[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			children_constant = false;
			break;
		}
		auto &const_value_expr = func.GetChildren()[i]->Cast<BoundConstantExpression>();
		if (const_value_expr.GetValue().IsNull()) {
			// cannot simplify NULL values
			children_constant = false;
			break;
		}
	}
	if (!children_constant) {
		// all children must be constant
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &fst_const_value_expr = func.GetChildren()[1]->Cast<BoundConstantExpression>();
	auto &type = fst_const_value_expr.GetValue().type();

	if (func.GetChildren().size() == 2 && TypeSupportsConstantFilter(type)) {
		// col IN (literal) is equivalent to an equality comparison - push that down
		auto bound_eq_comparison = CreateComparisonExpression(*func.GetChildren()[0], ExpressionType::COMPARE_EQUAL,
		                                                      fst_const_value_expr.GetValue());
		table_filters.PushFilter(proj_index, make_uniq<ExpressionFilter>(std::move(bound_eq_comparison)));
		return FilterPushdownResult::PUSHED_DOWN_FULLY;
	}

	//! Check if values are consecutive, if yes transform them to >= <= (only for integers)
	// e.g. if we have x IN (1, 2, 3, 4, 5) we transform this into x >= 1 AND x <= 5
	vector<Value> in_list;
	for (idx_t i = 1; i < func.GetChildren().size(); i++) {
		auto &const_value_expr = func.GetChildren()[i]->Cast<BoundConstantExpression>();
		D_ASSERT(!const_value_expr.GetValue().IsNull());
		in_list.push_back(const_value_expr.GetValue());
	}
	if (TryPushdownDenseInFilter(table_filters, column_ref, in_list)) {
		return FilterPushdownResult::PUSHED_DOWN_FULLY;
	}
	// if this is not a dense range we can push an optional filter for zone-map pruning
	auto in_expr =
	    ExpressionFilter::CreateInExpression(CreateFilterTargetExpression(*func.GetChildren()[0]), std::move(in_list));
	table_filters.PushFilter(proj_index, CreateOptionalExpressionFilter(std::move(in_expr), type));
	return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
}

FilterPushdownResult FilterCombiner::TryPushdownOrClause(TableFilterSet &table_filters,
                                                         const vector<ColumnIndex> &column_ids, Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &conj = expr.Cast<BoundConjunctionExpression>();
	if (conj.GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto conj_filter = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
	if (conj.GetChildren().empty()) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	ProjectionIndex proj_id;
	LogicalType col_type = LogicalType::INVALID;
	optional_ptr<const BoundColumnRefExpression> equality_column;
	vector<Value> equality_values;
	bool only_equalities = true;
	for (idx_t i = 0; i < conj.GetChildren().size(); i++) {
		auto &child = conj.GetChildren()[i];
		if (!BoundComparisonExpression::IsComparison(*child)) {
			return FilterPushdownResult::NO_PUSHDOWN;
		}
		optional_ptr<const BoundColumnRefExpression> column_ref;
		optional_ptr<const BoundConstantExpression> const_val;
		auto &comp = child->Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comp);
		auto &right = BoundComparisonExpression::Right(comp);
		bool invert = false;
		if (left.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		    right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			column_ref = left.Cast<BoundColumnRefExpression>();
			const_val = right.Cast<BoundConstantExpression>();
		} else if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		           right.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			column_ref = right.Cast<BoundColumnRefExpression>();
			const_val = left.Cast<BoundConstantExpression>();
			invert = true;
		} else {
			// child of OR filter is not simple so we do not push the or filter down at all
			return FilterPushdownResult::NO_PUSHDOWN;
		}
		if (!proj_id.IsValid()) {
			proj_id = column_ref->Binding().column_index;
			col_type = column_ref->GetReturnType();
		} else if (proj_id != column_ref->Binding().column_index) {
			return FilterPushdownResult::NO_PUSHDOWN;
		}

		auto comparison_type = invert ? FlipComparisonExpression(comp.GetExpressionType()) : comp.GetExpressionType();
		only_equalities &= comparison_type == ExpressionType::COMPARE_EQUAL;
		if (comparison_type == ExpressionType::COMPARE_EQUAL && !const_val->GetValue().IsNull()) {
			equality_column = column_ref;
			equality_values.push_back(const_val->GetValue());
		}
		if (const_val->GetValue().IsNull()) {
			switch (comparison_type) {
			case ExpressionType::COMPARE_DISTINCT_FROM: {
				auto null_expr = ExpressionFilter::CreateNullCheckExpression(CreateFilterTargetExpression(*column_ref),
				                                                             ExpressionType::OPERATOR_IS_NOT_NULL);
				conj_filter->GetChildrenMutable().push_back(std::move(null_expr));
				break;
			}
			case ExpressionType::COMPARE_NOT_DISTINCT_FROM: {
				auto null_expr = ExpressionFilter::CreateNullCheckExpression(CreateFilterTargetExpression(*column_ref),
				                                                             ExpressionType::OPERATOR_IS_NULL);
				conj_filter->GetChildrenMutable().push_back(std::move(null_expr));
				break;
			}
			default:
				// if any other comparison type (i.e EQUAL, NOT_EQUAL) do not push a table filter - this is a nop
				// since x = NULL is always falsey, and this is a chain of OR conditions, we can just ignore it
				break;
			}
		} else {
			conj_filter->GetChildrenMutable().push_back(
			    CreateComparisonExpression(*column_ref, comparison_type, const_val->GetValue()));
		}
	}
	if (only_equalities && equality_column) {
		if (TryPushdownDenseInFilter(table_filters, *equality_column, equality_values)) {
			return FilterPushdownResult::PUSHED_DOWN_FULLY;
		}
		// Push the same filter as for `col IN (...)`, which index scans can use.
		auto in_expr = ExpressionFilter::CreateInExpression(CreateFilterTargetExpression(*equality_column),
		                                                    std::move(equality_values));
		table_filters.PushFilter(proj_id, CreateOptionalExpressionFilter(std::move(in_expr), col_type));
		return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
	}
	table_filters.PushFilter(proj_id, CreateOptionalExpressionFilter(std::move(conj_filter), col_type));
	return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
}

static bool IsGreaterThan(ExpressionType type) {
	return type == ExpressionType::COMPARE_GREATERTHAN || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
}

static bool IsLessThan(ExpressionType type) {
	return type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
}

//! Returns the relaxation margin in native units of the source type for a temporal cast.
//! Returns -1 if the cast pair is not supported for relaxed pushdown.
//! Margins: 2h for DST ambiguity (TIMESTAMP<->TIMESTAMPTZ), 24h for time-of-day loss, 1s for rounding loss.
static int64_t GetTemporalCastMargin(LogicalTypeId source, LogicalTypeId target) {
	if (source == target) {
		switch (source) {
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_TZ_NS:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
			return 0;
		default:
			// non-temporal types with the same id can differ (e.g. UNION or DECIMAL types)
			return -1;
		}
	}
	switch (source) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// native unit: microseconds
		switch (target) {
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_TZ:
			return 2LL * Interval::MICROS_PER_HOUR;
		case LogicalTypeId::DATE:
			return Interval::MICROS_PER_DAY;
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
			return Interval::MICROS_PER_SEC;
		default:
			return -1;
		}
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		// native unit: nanoseconds
		switch (target) {
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
			return Interval::MICROS_PER_SEC * 1000LL;
		case LogicalTypeId::DATE:
			return Interval::MICROS_PER_DAY * 1000LL;
		default:
			return -1;
		}
	case LogicalTypeId::TIMESTAMP_MS:
		// native unit: milliseconds
		switch (target) {
		case LogicalTypeId::DATE:
			return Interval::MICROS_PER_DAY / Interval::MICROS_PER_MSEC;
		case LogicalTypeId::TIMESTAMP_SEC:
			return Interval::MICROS_PER_SEC / Interval::MICROS_PER_MSEC;
		default:
			return -1;
		}
	case LogicalTypeId::TIMESTAMP_SEC:
		// native unit: seconds
		switch (target) {
		case LogicalTypeId::DATE:
			return Interval::MICROS_PER_DAY / Interval::MICROS_PER_SEC;
		default:
			return -1;
		}
	case LogicalTypeId::DATE:
		// native unit: days
		switch (target) {
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_NS:
			return 0;
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return 1;
		default:
			return -1;
		}
	default:
		return -1;
	}
}

//! Create a temporal Value from a raw int64 (or int32 for DATE) value.
static Value MakeTemporalValue(LogicalTypeId type_id, int64_t result) {
	switch (type_id) {
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t(result));
	case LogicalTypeId::TIMESTAMP_TZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t(result));
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Value::TIMESTAMPTZNS(timestamp_tz_ns_t(result));
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TIMESTAMPMS(timestamp_ms_t(result));
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TIMESTAMPSEC(timestamp_sec_t(result));
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TIMESTAMPNS(timestamp_ns_t(result));
	case LogicalTypeId::DATE:
		return Value::DATE(date_t(static_cast<int32_t>(std::max<int64_t>(
		    std::min<int64_t>(result, NumericLimits<int32_t>::Maximum()), NumericLimits<int32_t>::Minimum()))));
	default:
		throw InternalException("MakeTemporalValue: unsupported type");
	}
}

//! Adjust a temporal Value by margin (positive = widen upper bound, negative = widen lower bound).
//! The margin is in native units of the value's type. Returns false on overflow (filter not useful).
static bool AdjustTemporalValue(Value &val, int64_t margin) {
	if (margin == 0) {
		return true;
	}
	int64_t raw;
	switch (val.type().id()) {
	case LogicalTypeId::TIMESTAMP:
		raw = val.GetValueUnsafe<timestamp_t>().value;
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		raw = val.GetValueUnsafe<timestamp_tz_t>().value;
		break;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		raw = val.GetValueUnsafe<timestamp_tz_ns_t>().value;
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		raw = val.GetValueUnsafe<timestamp_ms_t>().value;
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
		raw = val.GetValueUnsafe<timestamp_sec_t>().value;
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		raw = val.GetValueUnsafe<timestamp_ns_t>().value;
		break;
	case LogicalTypeId::DATE:
		raw = val.GetValueUnsafe<date_t>().days;
		break;
	default:
		return false;
	}
	int64_t result;
	if (!TryAddOperator::Operation(raw, margin, result)) {
		return false;
	}
	val = MakeTemporalValue(val.type().id(), result);
	return true;
}

FilterPushdownResult FilterCombiner::TryPushdownTemporalCastFilter(TableFilterSet &table_filters,
                                                                   const vector<ColumnIndex> &column_ids,
                                                                   Expression &expr) {
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &comp = expr.Cast<BoundFunctionExpression>();
	if (!SupportedFilterComparison(comp.GetExpressionType())) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	if (comp.GetExpressionType() == ExpressionType::COMPARE_NOTEQUAL) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &left = BoundComparisonExpression::Left(comp);
	auto &right = BoundComparisonExpression::Right(comp);

	// identify which side is CAST(col) and which is the scalar constant
	bool invert = false;
	if (BoundCastExpression::IsCast(left) && right.IsFoldable()) {
		// cast on left, constant on right
	} else if (BoundCastExpression::IsCast(right) && left.IsFoldable()) {
		invert = true;
	} else {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto &cast_side = invert ? right : left;
	auto &const_side = invert ? left : right;
	auto &cast_expr = cast_side.Cast<BoundFunctionExpression>();
	auto source_type = BoundCastExpression::SourceType(cast_expr);
	auto &target_type = cast_expr.GetReturnType();
	int64_t margin = GetTemporalCastMargin(source_type.id(), target_type.id());
	if (margin < 0) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}

	// the child of the cast must resolve to a column ref
	ProjectionIndex proj_index;
	if (!TryGetProjectionIndex(BoundCastExpression::Child(cast_expr), proj_index)) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}

	// evaluate the constant side
	Value constant_value;
	string error_msg;
	if (!ExpressionExecutor::TryEvaluateScalar(context, const_side, constant_value)) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	if (constant_value.IsNull()) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto cast_result = constant_value.TryCastAs(context, source_type, &error_msg);
	if (!cast_result) {
		return FilterPushdownResult::NO_PUSHDOWN;
	}
	auto cast_value = std::move(*cast_result);

	auto push_optional = [&](ExpressionType filter_type, Value filter_val) {
		auto filter_expr =
		    CreateComparisonExpression(BoundCastExpression::Child(cast_expr), filter_type, std::move(filter_val));
		table_filters.PushFilter(proj_index, CreateOptionalExpressionFilter(std::move(filter_expr), source_type));
	};

	// push relaxed filter(s) as OptionalFilter
	auto comparison_type = invert ? FlipComparisonExpression(comp.GetExpressionType()) : comp.GetExpressionType();
	if (IsGreaterThan(comparison_type) || comparison_type == ExpressionType::COMPARE_EQUAL) {
		Value lower = cast_value;
		if (!AdjustTemporalValue(lower, -margin)) {
			return FilterPushdownResult::NO_PUSHDOWN;
		}
		push_optional(ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(lower));
	}
	if (IsLessThan(comparison_type) || comparison_type == ExpressionType::COMPARE_EQUAL) {
		Value upper = cast_value;
		if (!AdjustTemporalValue(upper, margin)) {
			return FilterPushdownResult::NO_PUSHDOWN;
		}
		push_optional(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(upper));
	}
	return FilterPushdownResult::PUSHED_DOWN_PARTIALLY;
}

FilterPushdownResult FilterCombiner::TryPushdownExpression(TableFilterSet &table_filters,
                                                           const vector<ColumnIndex> &column_ids, Expression &expr) {
	auto pushdown_result = TryPushdownPrefixFilter(table_filters, column_ids, expr);
	if (pushdown_result != FilterPushdownResult::NO_PUSHDOWN) {
		return pushdown_result;
	}
	pushdown_result = TryPushdownLikeFilter(table_filters, column_ids, expr);
	if (pushdown_result != FilterPushdownResult::NO_PUSHDOWN) {
		return pushdown_result;
	}
	pushdown_result = TryPushdownInFilter(table_filters, column_ids, expr);
	if (pushdown_result != FilterPushdownResult::NO_PUSHDOWN) {
		return pushdown_result;
	}
	pushdown_result = TryPushdownOrClause(table_filters, column_ids, expr);
	if (pushdown_result != FilterPushdownResult::NO_PUSHDOWN) {
		return pushdown_result;
	}
	return TryPushdownTemporalCastFilter(table_filters, column_ids, expr);
}

void FilterCombiner::TryPushdownRelaxedFilter(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
                                              vector<FilterPushdownResult> &pushdown_results, column_t expr_id,
                                              vector<ExpressionValueInformation> &info_list) {
	auto filter_exp = equivalence_map.find(expr_id);
	if (filter_exp == equivalence_map.end() || filter_exp->second.size() != 1) {
		return;
	}
	auto &node_expr = filter_exp->second[0].get();
	for (auto &info : info_list) {
		auto constant = make_uniq<BoundConstantExpression>(info.constant);
		auto comparison =
		    BoundComparisonExpression::Create(info.comparison_type, node_expr.Copy(), std::move(constant));
		auto result = TryPushdownTemporalCastFilter(table_filters, column_ids, *comparison);
		if (result != FilterPushdownResult::NO_PUSHDOWN) {
			pushdown_results.push_back(result);
		}
	}
}

TableFilterSet FilterCombiner::GenerateTableScanFilters(const vector<ColumnIndex> &column_ids,
                                                        vector<FilterPushdownResult> &pushdown_results) {
	TableFilterSet table_filters;
	//! First, we figure the filters that have constant expressions that we can push down to the table scan
	for (auto &constant_value : constant_values) {
		auto expr_id = constant_value.first;
		auto &const_list = constant_value.second;
		auto result = TryPushdownConstantFilter(table_filters, column_ids, expr_id, const_list);
		if (result == FilterPushdownResult::NO_PUSHDOWN) {
			TryPushdownRelaxedFilter(table_filters, column_ids, pushdown_results, expr_id, const_list);
		}
	}
	//! Here we look for LIKE or IN filters
	for (idx_t rem_fil_idx = 0; rem_fil_idx < remaining_filters.size(); rem_fil_idx++) {
		auto &remaining_filter = remaining_filters[rem_fil_idx];
		auto pushdown_result = TryPushdownExpression(table_filters, column_ids, *remaining_filter);
		if (pushdown_result == FilterPushdownResult::PUSHED_DOWN_FULLY) {
			// the filter has been pushed down entirely - we can prune it
			remaining_filters.erase_at(rem_fil_idx--);
		} else {
			pushdown_results.push_back(pushdown_result);
		}
	}
	return table_filters;
}

FilterResult FilterCombiner::AddBoundComparisonFilter(Expression &expr) {
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	if (!SupportedFilterComparison(comparison.GetExpressionType())) {
		// only support [>, >=, <, <=, ==, !=] expressions
		return FilterResult::UNSUPPORTED;
	}
	// check if one of the sides is a scalar value
	auto &left = BoundComparisonExpression::LeftMutable(comparison);
	auto &right = BoundComparisonExpression::RightMutable(comparison);
	bool left_is_scalar = left->IsFoldable();
	bool right_is_scalar = right->IsFoldable();
	if (left_is_scalar || right_is_scalar) {
		// comparison with scalar
		auto &node = GetNode(left_is_scalar ? *right : *left);
		idx_t equivalence_set = GetEquivalenceSet(node);
		auto &scalar = left_is_scalar ? left : right;
		Value constant_value;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *scalar, constant_value)) {
			return FilterResult::UNSUPPORTED;
		}
		if (constant_value.IsNull()) {
			// comparisons with null are always null (i.e. will never result in rows)
			return FilterResult::UNSATISFIABLE;
		}

		// create the ExpressionValueInformation
		ExpressionValueInformation info;
		info.comparison_type =
		    left_is_scalar ? FlipComparisonExpression(comparison.GetExpressionType()) : comparison.GetExpressionType();
		info.constant = constant_value;

		// get the current bucket of constant values
		D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
		auto &info_list = constant_values.find(equivalence_set)->second;
		if (node.GetReturnType() != info.constant.type()) {
			return FilterResult::UNSUPPORTED;
		}
		// check the existing constant comparisons to see if we can do any pruning
		auto ret = AddConstantComparison(info_list, info);

		auto &non_scalar = left_is_scalar ? *right : *left;
		auto transitive_filter = FindTransitiveFilter(non_scalar);
		if (transitive_filter != nullptr) {
			// try to add transitive filters
			auto &transitive_cast = transitive_filter->Cast<BoundFunctionExpression>();
			auto transitive_result = AddTransitiveFilters(transitive_cast);
			if (transitive_result == FilterResult::UNSUPPORTED) {
				// in case of unsuccessful re-add filter into remaining ones
				remaining_filters.push_back(std::move(transitive_filter));
			}
			if (transitive_result == FilterResult::UNSATISFIABLE) {
				// in case transitive filter is unsatisfiable - abort filter pushdown
				return FilterResult::UNSATISFIABLE;
			}
		}
		return ret;
	} else {
		// comparison between two non-scalars
		if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return FilterResult::UNSUPPORTED;
		}
		// get the LHS and RHS nodes
		auto &left_node = GetNode(*left);
		auto &right_node = GetNode(*right);
		if (left_node.Equals(right_node)) {
			return FilterResult::UNSUPPORTED;
		}
		// get the equivalence sets of the LHS and RHS
		auto left_equivalence_set = GetEquivalenceSet(left_node);
		auto right_equivalence_set = GetEquivalenceSet(right_node);
		if (left_equivalence_set == right_equivalence_set) {
			// this equality filter already exists, prune it
			return FilterResult::SUCCESS;
		}
		// add the right bucket into the left bucket
		D_ASSERT(equivalence_map.find(left_equivalence_set) != equivalence_map.end());
		D_ASSERT(equivalence_map.find(right_equivalence_set) != equivalence_map.end());

		auto &left_bucket = equivalence_map.find(left_equivalence_set)->second;
		auto &right_bucket = equivalence_map.find(right_equivalence_set)->second;
		for (auto &right_expr : right_bucket) {
			// rewrite the equivalence set mapping for this node
			equivalence_set_map[right_expr] = left_equivalence_set;
			// add the node to the left bucket
			left_bucket.push_back(right_expr);
		}
		// now add all constant values from the right bucket to the left bucket
		D_ASSERT(constant_values.find(left_equivalence_set) != constant_values.end());
		D_ASSERT(constant_values.find(right_equivalence_set) != constant_values.end());
		auto &left_constant_bucket = constant_values.find(left_equivalence_set)->second;
		auto &right_constant_bucket = constant_values.find(right_equivalence_set)->second;
		for (auto &right_constant : right_constant_bucket) {
			if (AddConstantComparison(left_constant_bucket, right_constant) == FilterResult::UNSATISFIABLE) {
				return FilterResult::UNSATISFIABLE;
			}
		}
	}
	return FilterResult::SUCCESS;
}

FilterResult FilterCombiner::AddFilter(Expression &expr) {
	if (expr.HasParameter()) {
		return FilterResult::UNSUPPORTED;
	}
	if (expr.IsFoldable()) {
		// scalar condition, evaluate it
		Value result;
		if (!ExpressionExecutor::TryEvaluateScalar(context, expr, result)) {
			return FilterResult::UNSUPPORTED;
		}
		result = result.DefaultCastAs(LogicalType::BOOLEAN);
		// check if the filter passes
		if (result.IsNull() || !BooleanValue::Get(result)) {
			// the filter does not pass the scalar test, create an empty result
			return FilterResult::UNSATISFIABLE;
		} else {
			// the filter passes the scalar test, just remove the condition
			return FilterResult::SUCCESS;
		}
	}
	D_ASSERT(!expr.IsFoldable());
	if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
		auto &between = expr.Cast<BoundFunctionExpression>();
		//! check if one of the sides is a scalar value
		auto &input = BoundBetweenExpression::Input(between);
		auto &lower_bound = BoundBetweenExpression::LowerBound(between);
		auto &upper_bound = BoundBetweenExpression::UpperBound(between);
		auto lower_inclusive = BoundBetweenExpression::LowerInclusive(between);
		auto upper_inclusive = BoundBetweenExpression::UpperInclusive(between);
		bool lower_is_scalar = lower_bound.IsFoldable();
		bool upper_is_scalar = upper_bound.IsFoldable();
		if (lower_is_scalar || upper_is_scalar) {
			//! comparison with scalar - break apart
			auto &node = GetNode(*BoundBetweenExpression::InputMutable(between));
			idx_t equivalence_set = GetEquivalenceSet(node);
			auto result = FilterResult::UNSATISFIABLE;

			if (lower_is_scalar) {
				auto &scalar = lower_bound;
				Value constant_value;
				if (!ExpressionExecutor::TryEvaluateScalar(context, scalar, constant_value)) {
					return FilterResult::UNSUPPORTED;
				}

				// create the ExpressionValueInformation
				ExpressionValueInformation info;
				if (lower_inclusive) {
					info.comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
				} else {
					info.comparison_type = ExpressionType::COMPARE_GREATERTHAN;
				}
				info.constant = constant_value;

				// get the current bucket of constant values
				D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
				auto &info_list = constant_values.find(equivalence_set)->second;
				// check the existing constant comparisons to see if we can do any pruning
				result = AddConstantComparison(info_list, info);
			} else {
				D_ASSERT(upper_is_scalar);
				const auto type = lower_inclusive ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
				                                  : ExpressionType::COMPARE_GREATERTHAN;
				auto left = input.Copy();
				auto right = lower_bound.Copy();
				auto lower_comp = BoundComparisonExpression::Create(type, std::move(left), std::move(right));
				result = AddBoundComparisonFilter(*lower_comp);
			}

			//	 Stop if we failed
			if (result != FilterResult::SUCCESS) {
				return result;
			}

			if (upper_is_scalar) {
				auto &scalar = upper_bound;
				Value constant_value;
				if (!ExpressionExecutor::TryEvaluateScalar(context, scalar, constant_value)) {
					return FilterResult::UNSUPPORTED;
				}

				// create the ExpressionValueInformation
				ExpressionValueInformation info;
				if (upper_inclusive) {
					info.comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
				} else {
					info.comparison_type = ExpressionType::COMPARE_LESSTHAN;
				}
				info.constant = constant_value;

				// get the current bucket of constant values
				D_ASSERT(constant_values.find(equivalence_set) != constant_values.end());
				// check the existing constant comparisons to see if we can do any pruning
				result = AddConstantComparison(constant_values.find(equivalence_set)->second, info);
			} else {
				D_ASSERT(lower_is_scalar);
				const auto type =
				    upper_inclusive ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN;
				auto left = input.Copy();
				auto right = upper_bound.Copy();
				auto upper_comp = BoundComparisonExpression::Create(type, std::move(left), std::move(right));
				result = AddBoundComparisonFilter(*upper_comp);
			}

			return result;
		}
	} else if (BoundComparisonExpression::IsComparison(expr)) {
		return AddBoundComparisonFilter(expr);
	}
	// only comparisons supported for now
	return FilterResult::UNSUPPORTED;
}

// Infer constant bounds from comparisons between non-scalar expressions.
FilterResult FilterCombiner::AddTransitiveFilters(BoundFunctionExpression &comparison, bool is_root) {
	if (!IsGreaterThan(comparison.GetExpressionType()) && !IsLessThan(comparison.GetExpressionType())) {
		return FilterResult::UNSUPPORTED;
	}
	// Derived bounds must not duplicate volatile or throwing expressions.
	if (comparison.IsVolatile() || comparison.CanThrow()) {
		return FilterResult::UNSUPPORTED;
	}
	// get the LHS and RHS nodes
	auto &left_node = GetNode(*BoundComparisonExpression::LeftMutable(comparison));
	auto &right_node = GetNode(*BoundComparisonExpression::RightMutable(comparison));

	if (left_node.Equals(right_node)) {
		return FilterResult::UNSUPPORTED;
	}
	// get the equivalence sets of the LHS and RHS
	idx_t left_equivalence_set = GetEquivalenceSet(left_node);
	idx_t right_equivalence_set = GetEquivalenceSet(right_node);
	if (left_equivalence_set == right_equivalence_set) {
		if (comparison.GetExpressionType() == ExpressionType::COMPARE_GREATERTHAN ||
		    comparison.GetExpressionType() == ExpressionType::COMPARE_LESSTHAN) {
			// non equal comparison has equal equivalence set, then it is unsatisfiable
			// e.g., j > i AND i < j is unsatisfiable
			return FilterResult::UNSATISFIABLE;
		}
		// this equality filter already exists, prune it
		return FilterResult::SUCCESS;
	}
	D_ASSERT(constant_values.find(left_equivalence_set) != constant_values.end());
	D_ASSERT(constant_values.find(right_equivalence_set) != constant_values.end());
	vector<ExpressionValueInformation> &left_constants = constant_values.find(left_equivalence_set)->second;
	vector<ExpressionValueInformation> &right_constants = constant_values.find(right_equivalence_set)->second;
	bool is_successful = false;
	bool is_inserted = false;
	// read every constant filters already inserted for the right scalar variable
	// and see if we can create new transitive filters, e.g., there is already a filter i > 10,
	// suppose that we have now the j >= i, then we can infer a new filter j > 10
	auto &left = BoundComparisonExpression::Left(comparison);
	auto &right = BoundComparisonExpression::Right(comparison);
	for (const auto &right_constant : right_constants) {
		ExpressionValueInformation info;
		info.constant = right_constant.constant;
		// there is already an equality filter, e.g., i = 10
		if (right_constant.comparison_type == ExpressionType::COMPARE_EQUAL) {
			// create filter j [>, >=, <, <=] 10
			// suppose the new comparison is j >= i and we have already a filter i = 10,
			// then we create a new filter j >= 10
			// and the filter j >= i can be pruned by not adding it into the remaining filters
			info.comparison_type = comparison.GetExpressionType();
		} else if ((comparison.GetExpressionType() == ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
		            IsGreaterThan(right_constant.comparison_type)) ||
		           (comparison.GetExpressionType() == ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		            IsLessThan(right_constant.comparison_type))) {
			// filters (j >= i AND i [>, >=] 10) OR (j <= i AND i [<, <=] 10)
			// create filter j [>, >=] 10 and add the filter j [>=, <=] i into the remaining filters
			info.comparison_type = right_constant.comparison_type; // create filter j [>, >=, <, <=] 10
			if (!is_inserted) {
				// Add the filter j >= i in the remaining filters
				auto filter =
				    BoundComparisonExpression::Create(comparison.GetExpressionType(), left.Copy(), right.Copy());
				remaining_filters.push_back(std::move(filter));
				is_inserted = true;
			}
		} else if ((comparison.GetExpressionType() == ExpressionType::COMPARE_GREATERTHAN &&
		            IsGreaterThan(right_constant.comparison_type)) ||
		           (comparison.GetExpressionType() == ExpressionType::COMPARE_LESSTHAN &&
		            IsLessThan(right_constant.comparison_type))) {
			// filters (j > i AND i [>, >=] 10) OR j < i AND i [<, <=] 10
			// create filter j [>, <] 10 and add the filter j [>, <] i into the remaining filters
			// the comparisons j > i and j < i are more restrictive
			info.comparison_type = comparison.GetExpressionType();
			if (!is_inserted) {
				// Add the filter j [>, <] i
				auto filter =
				    BoundComparisonExpression::Create(comparison.GetExpressionType(), left.Copy(), right.Copy());
				remaining_filters.push_back(std::move(filter));
				is_inserted = true;
			}
		} else {
			// we cannot add a new filter
			continue;
		}
		// Add the new filer into the left set
		if (AddConstantComparison(left_constants, info) == FilterResult::UNSATISFIABLE) {
			return FilterResult::UNSATISFIABLE;
		}
		is_successful = true;
	}
	if (is_successful) {
		if (is_root) {
			// now check for remaining transitive filters from the left column
			auto transitive_filter = FindTransitiveFilter(left);
			if (transitive_filter != nullptr) {
				// try to add transitive filters
				auto &transitive_cast = transitive_filter->Cast<BoundFunctionExpression>();
				auto transitive_result = AddTransitiveFilters(transitive_cast, false);
				if (transitive_result == FilterResult::UNSUPPORTED) {
					// in case of unsuccessful re-add filter into remaining ones
					remaining_filters.push_back(std::move(transitive_filter));
				}
				if (transitive_result == FilterResult::UNSATISFIABLE) {
					// while adding transitive filters we discovered the filter is unsatisfisable - we can prune
					return FilterResult::UNSATISFIABLE;
				}
			}
		}
		return FilterResult::SUCCESS;
	}

	return FilterResult::UNSUPPORTED;
}

/*
 * Find a transitive filter already inserted into the remaining filters
 * Check for a match between the right column of bound comparisons and the expression,
 * then removes the bound comparison from the remaining filters and returns it
 */
unique_ptr<Expression> FilterCombiner::FindTransitiveFilter(const Expression &expr) {
	// We only check for bound column ref
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr;
	}
	for (idx_t i = 0; i < remaining_filters.size(); i++) {
		if (BoundComparisonExpression::IsComparison(*remaining_filters[i])) {
			auto &comparison = remaining_filters[i]->Cast<BoundFunctionExpression>();
			auto &right = BoundComparisonExpression::Right(comparison);
			if (expr.Equals(right) && comparison.GetExpressionType() != ExpressionType::COMPARE_NOTEQUAL) {
				auto filter = std::move(remaining_filters[i]);
				remaining_filters.erase_at(i);
				return filter;
			}
		}
	}
	return nullptr;
}

static ValueComparisonResult InvertValueComparisonResult(ValueComparisonResult result) {
	if (result == ValueComparisonResult::PRUNE_RIGHT) {
		return ValueComparisonResult::PRUNE_LEFT;
	}
	if (result == ValueComparisonResult::PRUNE_LEFT) {
		return ValueComparisonResult::PRUNE_RIGHT;
	}
	return result;
}

ValueComparisonResult CompareValueInformation(ExpressionValueInformation &left, ExpressionValueInformation &right) {
	if (left.comparison_type == ExpressionType::COMPARE_EQUAL) {
		// left is COMPARE_EQUAL, we can either
		// (1) prune the right side or
		// (2) return UNSATISFIABLE
		bool prune_right_side = false;
		switch (right.comparison_type) {
		case ExpressionType::COMPARE_LESSTHAN:
			prune_right_side = left.constant < right.constant;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			prune_right_side = left.constant <= right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			prune_right_side = left.constant > right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			prune_right_side = left.constant >= right.constant;
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			prune_right_side = left.constant != right.constant;
			break;
		default:
			D_ASSERT(right.comparison_type == ExpressionType::COMPARE_EQUAL);
			prune_right_side = left.constant == right.constant;
			break;
		}
		if (prune_right_side) {
			return ValueComparisonResult::PRUNE_RIGHT;
		} else {
			return ValueComparisonResult::UNSATISFIABLE_CONDITION;
		}
	} else if (right.comparison_type == ExpressionType::COMPARE_EQUAL) {
		// right is COMPARE_EQUAL
		return InvertValueComparisonResult(CompareValueInformation(right, left));
	} else if (left.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
		// left is COMPARE_NOTEQUAL, we can either
		// (1) prune the left side or
		// (2) not prune anything
		bool prune_left_side = false;
		switch (right.comparison_type) {
		case ExpressionType::COMPARE_LESSTHAN:
			prune_left_side = left.constant >= right.constant;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			prune_left_side = left.constant > right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			prune_left_side = left.constant <= right.constant;
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			prune_left_side = left.constant < right.constant;
			break;
		default:
			D_ASSERT(right.comparison_type == ExpressionType::COMPARE_NOTEQUAL);
			prune_left_side = left.constant == right.constant;
			break;
		}
		if (prune_left_side) {
			return ValueComparisonResult::PRUNE_LEFT;
		} else {
			return ValueComparisonResult::PRUNE_NOTHING;
		}
	} else if (right.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
		return InvertValueComparisonResult(CompareValueInformation(right, left));
	} else if (IsGreaterThan(left.comparison_type) && IsGreaterThan(right.comparison_type)) {
		// both comparisons are [>], we can either
		// (1) prune the left side or
		// (2) prune the right side
		if (left.constant > right.constant) {
			// left constant is more selective, prune right
			return ValueComparisonResult::PRUNE_RIGHT;
		} else if (left.constant < right.constant) {
			// right constant is more selective, prune left
			return ValueComparisonResult::PRUNE_LEFT;
		} else {
			// constants are equivalent
			// however we can still have the scenario where one is [>=] and the other is [>]
			// we want to prune the [>=] because [>] is more selective
			// if left is [>=] we prune the left, else we prune the right
			if (left.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				return ValueComparisonResult::PRUNE_LEFT;
			} else {
				return ValueComparisonResult::PRUNE_RIGHT;
			}
		}
	} else if (IsLessThan(left.comparison_type) && IsLessThan(right.comparison_type)) {
		// both comparisons are [<], we can either
		// (1) prune the left side or
		// (2) prune the right side
		if (left.constant < right.constant) {
			// left constant is more selective, prune right
			return ValueComparisonResult::PRUNE_RIGHT;
		} else if (left.constant > right.constant) {
			// right constant is more selective, prune left
			return ValueComparisonResult::PRUNE_LEFT;
		} else {
			// constants are equivalent
			// however we can still have the scenario where one is [<=] and the other is [<]
			// we want to prune the [<=] because [<] is more selective
			// if left is [<=] we prune the left, else we prune the right
			if (left.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				return ValueComparisonResult::PRUNE_LEFT;
			} else {
				return ValueComparisonResult::PRUNE_RIGHT;
			}
		}
	} else if (IsLessThan(left.comparison_type)) {
		D_ASSERT(IsGreaterThan(right.comparison_type));
		// left is [<] and right is [>], in this case we can either
		// (1) prune nothing or
		// (2) return UNSATISFIABLE
		// the SMALLER THAN constant has to be greater than the BIGGER THAN constant
		if (left.constant > right.constant) {
			return ValueComparisonResult::PRUNE_NOTHING;
		} else if (left.constant < right.constant) {
			return ValueComparisonResult::UNSATISFIABLE_CONDITION;
		} else {
			// the constants are equal
			// This is only satisfiable if both bounds are inclusive
			if (left.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO &&
			    right.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				return ValueComparisonResult::PRUNE_NOTHING;
			} else {
				return ValueComparisonResult::UNSATISFIABLE_CONDITION;
			}
		}
	} else {
		// left is [>] and right is [<] or [!=]
		D_ASSERT(IsLessThan(right.comparison_type) && IsGreaterThan(left.comparison_type));
		return InvertValueComparisonResult(CompareValueInformation(right, left));
	}
}

} // namespace duckdb
