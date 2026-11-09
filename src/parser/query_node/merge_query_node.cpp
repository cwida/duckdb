#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

MergeQueryNode::MergeQueryNode() : QueryNode(QueryNodeType::MERGE_QUERY_NODE) {
}

string MergeQueryNode::ActionConditionToString(MergeActionCondition condition) {
	switch (condition) {
	case MergeActionCondition::WHEN_MATCHED:
		return "WHEN MATCHED";
	case MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET:
		return "WHEN NOT MATCHED";
	case MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE:
		return "WHEN NOT MATCHED BY SOURCE";
	default:
		throw InternalException("Unknown match condition");
	}
}

string MergeQueryNode::ToString() const {
	string result;
	result = cte_map.ToString();
	result += "MERGE INTO ";
	result += target->ToString();
	result += " USING ";
	result += source->ToString();
	if (join_condition) {
		result += " ON ";
		result += join_condition->ToString();
	} else {
		result += " USING (";
		for (idx_t c = 0; c < using_columns.size(); c++) {
			if (c > 0) {
				result += ", ";
			}
			result += SQLIdentifier(using_columns[c]);
		}
		result += ")";
	}
	for (auto &entry : actions) {
		for (auto &action : entry.second) {
			result += " ";
			result += MergeQueryNode::ActionConditionToString(entry.first);
			result += " ";
			result += action->ToString();
		}
	}
	if (!returning_list.empty()) {
		result += " RETURNING ";
		for (idx_t i = 0; i < returning_list.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			auto column = returning_list[i]->ToString();
			if (!returning_list[i]->GetAlias().empty()) {
				column += StringUtil::Format(" AS %s", SQLIdentifier(returning_list[i]->GetAlias()));
			}
			result += column;
		}
	}
	return result;
}

bool MergeQueryNode::Equals(const QueryNode *other_p) const {
	if (this == other_p) {
		return true;
	}
	if (!QueryNode::Equals(other_p)) {
		return false;
	}
	auto &other = other_p->Cast<MergeQueryNode>();
	if (!TableRef::Equals(target, other.target)) {
		return false;
	}
	if (!TableRef::Equals(source, other.source)) {
		return false;
	}
	if (!ParsedExpression::Equals(join_condition, other.join_condition)) {
		return false;
	}
	if (using_columns != other.using_columns) {
		return false;
	}
	if (actions.size() != other.actions.size()) {
		return false;
	}
	for (auto &entry : actions) {
		auto other_entry = other.actions.find(entry.first);
		if (other_entry == other.actions.end()) {
			return false;
		}
		if (entry.second.size() != other_entry->second.size()) {
			return false;
		}
		for (idx_t i = 0; i < entry.second.size(); i++) {
			if (!MergeIntoAction::Equals(*entry.second[i], *other_entry->second[i])) {
				return false;
			}
		}
	}
	if (returning_list.size() != other.returning_list.size()) {
		return false;
	}
	for (idx_t i = 0; i < returning_list.size(); i++) {
		if (!ParsedExpression::Equals(returning_list[i], other.returning_list[i])) {
			return false;
		}
	}
	return true;
}

unique_ptr<QueryNode> MergeQueryNode::Copy() const {
	auto result = make_uniq<MergeQueryNode>();
	result->target = target ? target->Copy() : nullptr;
	result->source = source ? source->Copy() : nullptr;
	result->join_condition = join_condition ? join_condition->Copy() : nullptr;
	result->using_columns = using_columns;
	for (auto &entry : actions) {
		auto &action_list = result->actions[entry.first];
		for (auto &action : entry.second) {
			action_list.push_back(action->Copy());
		}
	}
	for (auto &expr : returning_list) {
		result->returning_list.push_back(expr->Copy());
	}
	CopyProperties(*result);
	return std::move(result);
}

} // namespace duckdb
