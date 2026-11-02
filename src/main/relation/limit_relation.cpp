#include "duckdb/main/relation/limit_relation.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

LimitRelation::LimitRelation(shared_ptr<Relation> child_p, int64_t limit, int64_t offset)
    : Relation(child_p->context, RelationType::LIMIT_RELATION), limit(limit), offset(offset),
      child(std::move(child_p)) {
	D_ASSERT(child.get() != this);
}

unique_ptr<QueryNode> LimitRelation::GetQueryNode() {
	auto child_node = child->GetQueryNode();
	auto limit_node = make_uniq<LimitModifier>();
	if (limit >= 0) {
		limit_node->limit = ConstantExpression::Integer(limit);
	}
	if (offset > 0) {
		limit_node->offset = ConstantExpression::Integer(offset);
	}

	child_node->modifiers.push_back(std::move(limit_node));
	return child_node;
}

Identifier LimitRelation::GetAlias() {
	return child->GetAlias();
}

const vector<ColumnDefinition> &LimitRelation::Columns() {
	return child->Columns();
}

string LimitRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Limit " + to_string(limit);
	if (offset > 0) {
		str += " Offset " + to_string(offset);
	}
	str += "\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
