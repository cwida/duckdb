#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/planner/expression_binder/insert_binder.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"

namespace duckdb {

static void WarnIfDecimalScaleIsReduced(ClientContext &context, const LogicalType &source_type,
                                        const LogicalType &target_type, idx_t column_idx,
                                        vector<idx_t> &warned_about_scale_reduction) {
	if (warned_about_scale_reduction[column_idx]) {
		return;
	}
	if (source_type.id() != LogicalTypeId::DECIMAL || target_type.id() != LogicalTypeId::DECIMAL) {
		return;
	}
	auto source_scale = DecimalType::GetScale(source_type);
	auto target_scale = DecimalType::GetScale(target_type);
	if (source_scale <= target_scale) {
		return;
	}
	DUCKDB_LOG_WARNING(context,
	                   "Potential loss of decimal precision while resolving a VALUES column: type %s is being cast "
	                   "to %s, reducing the scale from %d to %d. This may cause values to be rounded. Explicitly "
	                   "cast all values to a compatible DECIMAL type to avoid this warning.",
	                   source_type.ToString(), target_type.ToString(), source_scale, target_scale);
	warned_about_scale_reduction[column_idx] = true;
}

static void VerifyExpressionListRef(const ExpressionListRef &expr) {
	// the parser guarantees these, but a deserialized ExpressionListRef might not
	if (expr.values.empty()) {
		return;
	}
	auto column_count = expr.values[0].size();
	for (auto &expression_list : expr.values) {
		if (expression_list.size() != column_count) {
			throw BinderException("VALUES lists must all be the same length");
		}
	}
	if (!expr.expected_types.empty() && expr.expected_types.size() != column_count) {
		throw BinderException("VALUES list has %d columns but %d expected types", column_count,
		                      expr.expected_types.size());
	}
	if (!expr.expected_names.empty() && expr.expected_names.size() != column_count) {
		throw BinderException("VALUES list has %d columns but %d expected names", column_count,
		                      expr.expected_names.size());
	}
	for (auto &type : expr.expected_types) {
		if (type.IsValid() && !type.IsComplete()) {
			throw BinderException("VALUES list has an incomplete expected type %s", type.ToString());
		}
	}
}

BoundStatement Binder::Bind(ExpressionListRef &expr) {
	VerifyExpressionListRef(expr);
	BoundStatement result;
	result.types = expr.expected_types;
	result.names = expr.expected_names;

	vector<vector<unique_ptr<Expression>>> values;
	auto prev_can_contain_nulls = CanContainNulls();
	// bind value list
	InsertBinder binder(*this, context);
	binder.target_type = LogicalType(LogicalTypeId::INVALID);
	for (idx_t list_idx = 0; list_idx < expr.values.size(); list_idx++) {
		auto &expression_list = expr.values[list_idx];
		if (result.names.empty()) {
			// no names provided, generate them
			for (idx_t val_idx = 0; val_idx < expression_list.size(); val_idx++) {
				result.names.emplace_back("col" + to_string(val_idx));
			}
		}

		SetCanContainNulls(true);
		vector<unique_ptr<Expression>> list;
		for (idx_t val_idx = 0; val_idx < expression_list.size(); val_idx++) {
			if (!result.types.empty()) {
				D_ASSERT(result.types.size() == expression_list.size());
				binder.target_type = result.types[val_idx];
			} else {
				binder.target_type = LogicalType(LogicalTypeId::INVALID);
			}
			auto bound_expr = binder.Bind(expression_list[val_idx]);
			list.push_back(std::move(bound_expr));
		}
		values.push_back(std::move(list));
		this->SetCanContainNulls(prev_can_contain_nulls);
	}
	bool infer_types = result.types.empty();
	if (!infer_types) {
		for (auto &type : result.types) {
			if (!type.IsValid()) {
				infer_types = true;
				break;
			}
		}
	}
	if (infer_types && !expr.values.empty()) {
		// there are no types specified, or some types were left invalid
		// we have to figure out the result types for those columns
		// for each column, we iterate over all of the expressions and select the max logical type
		// we initialize all types to SQLNULL
		vector<uint8_t> should_infer(expr.values[0].size(), true);
		if (result.types.empty()) {
			result.types.resize(expr.values[0].size(), LogicalType::SQLNULL);
		} else {
			for (idx_t i = 0; i < result.types.size(); i++) {
				auto &type = result.types[i];
				if (!type.IsValid()) {
					type = LogicalType::SQLNULL;
				} else {
					should_infer[i] = false;
				}
			}
		}
		// now loop over the lists and select the max logical type
		for (idx_t list_idx = 0; list_idx < values.size(); list_idx++) {
			auto &list = values[list_idx];
			for (idx_t val_idx = 0; val_idx < list.size(); val_idx++) {
				if (!should_infer[val_idx]) {
					continue;
				}
				auto &current_type = result.types[val_idx];
				auto next_type = ExpressionBinder::GetExpressionReturnType(*list[val_idx]);
				result.types[val_idx] = LogicalType::MaxLogicalType(context, current_type, next_type);
			}
		}
		for (auto &type : result.types) {
			type = LogicalType::NormalizeType(type);
		}
		// finally do another loop over the expressions and add casts where required
		vector<idx_t> warned_about_scale_reduction(result.types.size(), false);
		for (idx_t list_idx = 0; list_idx < values.size(); list_idx++) {
			auto &list = values[list_idx];
			for (idx_t val_idx = 0; val_idx < list.size(); val_idx++) {
				if (!should_infer[val_idx]) {
					continue;
				}
				auto source_type = ExpressionBinder::GetExpressionReturnType(*list[val_idx]);
				auto &target_type = result.types[val_idx];
				WarnIfDecimalScaleIsReduced(context, source_type, target_type, val_idx, warned_about_scale_reduction);
				list[val_idx] = BoundCastExpression::AddCastToType(context, std::move(list[val_idx]), target_type);
			}
		}
	}
	auto bind_index = GenerateTableIndex();
	bind_context.AddGenericBinding(bind_index, expr.alias, result.names, result.types);

	// values list, first plan any subqueries in the list
	auto root = make_uniq_base<LogicalOperator, LogicalDummyScan>(GenerateTableIndex());
	for (auto &expr_list : values) {
		for (auto &expr : expr_list) {
			PlanSubqueries(expr, root);
		}
	}

	auto expr_get = make_uniq<LogicalExpressionGet>(bind_index, result.types, std::move(values));
	expr_get->AddChild(std::move(root));
	result.plan = std::move(expr_get);
	return result;
}

} // namespace duckdb
