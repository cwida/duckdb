#include "duckdb/parser/tableref/match_recognize_ref.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/expression_util.hpp"

namespace duckdb {

MatchRecognizeRef::MatchRecognizeRef() : TableRef(TableReferenceType::MATCH_RECOGNIZE) {
}

static vector<unique_ptr<ParsedExpression>> CopyExpressions(const vector<unique_ptr<ParsedExpression>> &expressions) {
	vector<unique_ptr<ParsedExpression>> result;
	result.reserve(expressions.size());
	for (auto &expression : expressions) {
		result.push_back(expression->Copy());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Config
//===--------------------------------------------------------------------===//
unique_ptr<MatchRecognizeConfig> MatchRecognizeConfig::Copy() const {
	auto result = make_uniq<MatchRecognizeConfig>();
	result->partition_expressions = CopyExpressions(partition_expressions);
	for (auto &order : order_by_expressions) {
		result->order_by_expressions.emplace_back(order.type, order.null_order, order.expression->Copy());
	}
	result->measures_expression_list = CopyExpressions(measures_expression_list);
	result->defines_expression_list = CopyExpressions(defines_expression_list);
	result->rows_per_match = rows_per_match;
	result->after_match = after_match;
	result->after_match_variable = after_match_variable;
	result->pattern = pattern ? pattern->Copy() : nullptr;
	result->subsets = subsets;
	result->define_auto = define_auto;
	return result;
}

bool MatchRecognizeConfig::Equals(const MatchRecognizeConfig &other) const {
	if (rows_per_match != other.rows_per_match || after_match != other.after_match ||
	    after_match_variable != other.after_match_variable || define_auto != other.define_auto) {
		return false;
	}
	if (!ExpressionUtil::ListEquals(partition_expressions, other.partition_expressions) ||
	    !ExpressionUtil::ListEquals(measures_expression_list, other.measures_expression_list) ||
	    !ExpressionUtil::ListEquals(defines_expression_list, other.defines_expression_list)) {
		return false;
	}
	if (order_by_expressions.size() != other.order_by_expressions.size()) {
		return false;
	}
	for (idx_t i = 0; i < order_by_expressions.size(); i++) {
		auto &mine = order_by_expressions[i];
		auto &theirs = other.order_by_expressions[i];
		if (mine.type != theirs.type || mine.null_order != theirs.null_order ||
		    !mine.expression->Equals(*theirs.expression)) {
			return false;
		}
	}
	if (subsets.size() != other.subsets.size()) {
		return false;
	}
	for (idx_t i = 0; i < subsets.size(); i++) {
		if (subsets[i].name != other.subsets[i].name || subsets[i].members != other.subsets[i].members) {
			return false;
		}
	}
	return ParsedExpression::Equals(pattern, other.pattern);
}

//===--------------------------------------------------------------------===//
// Table reference
//===--------------------------------------------------------------------===//
string MatchRecognizeRef::ToString() const {
	string result = input->ToString();
	result += " MATCH_RECOGNIZE(";
	if (!config->partition_expressions.empty()) {
		result += "PARTITION BY ";
		for (idx_t i = 0; i < config->partition_expressions.size(); i++) {
			result += i > 0 ? ", " : "";
			result += config->partition_expressions[i]->ToString();
		}
		result += " ";
	}
	if (!config->order_by_expressions.empty()) {
		result += "ORDER BY ";
		for (idx_t i = 0; i < config->order_by_expressions.size(); i++) {
			result += i > 0 ? ", " : "";
			result += config->order_by_expressions[i].ToString();
		}
		result += " ";
	}
	if (!config->measures_expression_list.empty()) {
		result += "MEASURES ";
		for (idx_t i = 0; i < config->measures_expression_list.size(); i++) {
			auto &measure = config->measures_expression_list[i];
			result += i > 0 ? ", " : "";
			result += measure->ToString() + " AS " + SQLIdentifier::ToString(measure->GetAlias());
		}
		result += " ";
	}
	switch (config->rows_per_match) {
	case MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ONE:
		result += "ONE ROW PER MATCH ";
		break;
	case MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL:
		result += "ALL ROWS PER MATCH ";
		break;
	case MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_OMIT_EMPTY:
		result += "ALL ROWS PER MATCH OMIT EMPTY MATCHES ";
		break;
	case MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_UNMATCHED:
		result += "ALL ROWS PER MATCH WITH UNMATCHED ROWS ";
		break;
	default:
		break;
	}
	switch (config->after_match) {
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_NEXT_ROW:
		result += "AFTER MATCH SKIP TO NEXT ROW ";
		break;
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_ROW:
		result += "AFTER MATCH SKIP PAST LAST ROW ";
		break;
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_FIRST_VAR:
		result += "AFTER MATCH SKIP TO FIRST " + SQLIdentifier::ToString(config->after_match_variable) + " ";
		break;
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_VAR:
		result += "AFTER MATCH SKIP TO LAST " + SQLIdentifier::ToString(config->after_match_variable) + " ";
		break;
	default:
		break;
	}
	result += "PATTERN (" + config->pattern->ToString() + ")";
	if (!config->subsets.empty()) {
		result += " SUBSET ";
		for (idx_t i = 0; i < config->subsets.size(); i++) {
			auto &subset = config->subsets[i];
			result += i > 0 ? ", " : "";
			result += SQLIdentifier::ToString(subset.name) + " = (";
			for (idx_t member_idx = 0; member_idx < subset.members.size(); member_idx++) {
				result += member_idx > 0 ? ", " : "";
				result += SQLIdentifier::ToString(subset.members[member_idx]);
			}
			result += ")";
		}
	}
	if (config->define_auto) {
		result += " DEFINE AUTO";
	} else if (!config->defines_expression_list.empty()) {
		result += " DEFINE ";
		for (idx_t i = 0; i < config->defines_expression_list.size(); i++) {
			auto &define = config->defines_expression_list[i];
			result += i > 0 ? ", " : "";
			result += SQLIdentifier::ToString(define->GetAlias()) + " AS " + define->ToString();
		}
	}
	result += ")";
	return BaseToString(result, column_name_alias);
}

bool MatchRecognizeRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<MatchRecognizeRef>();
	if (!input->Equals(*other.input)) {
		return false;
	}
	return config->Equals(*other.config);
}

unique_ptr<TableRef> MatchRecognizeRef::Copy() {
	auto copy = make_uniq<MatchRecognizeRef>(input->Copy(), config->Copy());
	copy->column_name_alias = column_name_alias;
	CopyProperties(*copy);
	return std::move(copy);
}

} // namespace duckdb
