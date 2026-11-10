#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"

namespace duckdb {

InsertQueryNode::InsertQueryNode()
    : QueryNode(QueryNodeType::INSERT_QUERY_NODE), qualified_name({Identifier::DefaultSchema()}, Identifier()),
      column_order(InsertColumnOrder::INSERT_BY_POSITION) {
}

string InsertQueryNode::ToString() const {
	bool or_replace_shorthand_set = false;
	string result;

	result = cte_map.ToString();
	result += "INSERT";
	if (on_conflict_info && on_conflict_info->action_type == OnConflictAction::REPLACE) {
		or_replace_shorthand_set = true;
		result += " OR REPLACE";
	}
	result += " INTO ";
	result += qualified_name.ToString();
	// Write the (optional) alias of the insert target
	if (table_ref && !table_ref->alias.empty()) {
		result += StringUtil::Format(" AS %s", SQLIdentifier(table_ref->alias));
	}
	if (column_order == InsertColumnOrder::INSERT_BY_NAME) {
		result += " BY NAME";
	}
	if (!columns.empty()) {
		result += " (";
		for (idx_t i = 0; i < columns.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += SQLIdentifier(columns[i]);
		}
		result += ")";
	}
	result += " ";
	auto values_list = GetValuesList();
	if (values_list) {
		D_ASSERT(!default_values);
		result += values_list->ValuesToString();
		result += select_statement->node->ResultModifiersToString();
	} else if (select_statement) {
		D_ASSERT(!default_values);
		result += select_statement->ToString();
	} else {
		D_ASSERT(default_values);
		result += "DEFAULT VALUES";
	}
	if (!or_replace_shorthand_set && on_conflict_info) {
		auto &conflict_info = *on_conflict_info;
		result += " ON CONFLICT";
		// (optional) conflict target
		if (!conflict_info.indexed_columns.empty()) {
			result += " (";
			auto &cols = conflict_info.indexed_columns;
			for (auto it = cols.begin(); it != cols.end();) {
				result += SQLIdentifier(*it);
				if (++it != cols.end()) {
					result += ", ";
				}
			}
			result += ")";
		}

		// (optional) where clause
		if (conflict_info.condition) {
			result += " WHERE " + conflict_info.condition->ToString();
		}
		result += " " + OnConflictInfo::ActionToString(conflict_info.action_type);
		if (conflict_info.set_info) {
			D_ASSERT(conflict_info.action_type == OnConflictAction::UPDATE);
			auto &set_info = *conflict_info.set_info;
			result += " " + set_info.ToString();
			// (optional) where clause
			if (set_info.condition) {
				result += " WHERE " + set_info.condition->ToString();
			}
		}
	}
	if (!returning_list.empty()) {
		result += " RETURNING ";
		for (idx_t i = 0; i < returning_list.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			auto col = returning_list[i]->ToString();
			if (!returning_list[i]->GetAlias().empty()) {
				col += StringUtil::Format(" AS %s", SQLIdentifier(returning_list[i]->GetAlias()));
			}
			result += col;
		}
	}
	return result;
}

bool InsertQueryNode::Equals(const QueryNode *other_p) const {
	if (this == other_p) {
		return true;
	}
	if (!QueryNode::Equals(other_p)) {
		return false;
	}
	auto &other = other_p->Cast<InsertQueryNode>();
	if (qualified_name != other.qualified_name) {
		return false;
	}
	if (columns != other.columns) {
		return false;
	}
	if (default_values != other.default_values) {
		return false;
	}
	if (column_order != other.column_order) {
		return false;
	}
	if (!TableRef::Equals(table_ref, other.table_ref)) {
		return false;
	}
	// compare select_statement
	if (select_statement && other.select_statement) {
		if (!select_statement->Equals(*other.select_statement)) {
			return false;
		}
	} else if (select_statement || other.select_statement) {
		return false;
	}
	if (!OnConflictInfo::Equals(on_conflict_info, other.on_conflict_info)) {
		return false;
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

unique_ptr<QueryNode> InsertQueryNode::Copy() const {
	auto result = make_uniq<InsertQueryNode>();
	result->qualified_name = qualified_name;
	result->columns = columns;
	result->default_values = default_values;
	result->column_order = column_order;
	if (select_statement) {
		result->select_statement = unique_ptr_cast<SQLStatement, SelectStatement>(select_statement->Copy());
	}
	for (auto &expr : returning_list) {
		result->returning_list.push_back(expr->Copy());
	}
	if (table_ref) {
		result->table_ref = table_ref->Copy();
	}
	if (on_conflict_info) {
		result->on_conflict_info = on_conflict_info->Copy();
	}
	CopyProperties(*result);
	return std::move(result);
}

optional_ptr<ExpressionListRef> InsertQueryNode::GetValuesList() const {
	if (!select_statement) {
		return nullptr;
	}
	if (select_statement->node->type != QueryNodeType::SELECT_NODE) {
		return nullptr;
	}
	auto &node = select_statement->node->Cast<SelectNode>();
	if (node.where_clause || node.qualify || node.having) {
		return nullptr;
	}
	if (!node.cte_map.map.empty()) {
		return nullptr;
	}
	if (!node.groups.grouping_sets.empty()) {
		return nullptr;
	}
	if (node.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return nullptr;
	}
	if (node.select_list.size() != 1 || node.select_list[0]->GetExpressionType() != ExpressionType::STAR) {
		return nullptr;
	}
	if (!node.from_table || node.from_table->type != TableReferenceType::EXPRESSION_LIST) {
		return nullptr;
	}
	return &node.from_table->Cast<ExpressionListRef>();
}

} // namespace duckdb
