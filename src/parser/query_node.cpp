#include "duckdb/parser/query_node.hpp"

#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/result_modifier.hpp"
namespace duckdb {

CommonTableExpressionMap::CommonTableExpressionMap() {
}

CommonTableExpressionMap CommonTableExpressionMap::Copy() const {
	CommonTableExpressionMap res;
	for (auto &kv : this->map) {
		res.map[kv.first] = kv.second->Copy();
	}

	return res;
}

string CommonTableExpressionMap::ToString() const {
	if (map.empty()) {
		return string();
	}
	// check if there are any recursive CTEs
	bool has_recursive = false;
	for (auto &kv : map) {
		if (kv.second->query_node && kv.second->query_node->type == QueryNodeType::RECURSIVE_CTE_NODE) {
			has_recursive = true;
			break;
		}
	}
	string result = "WITH ";
	if (has_recursive) {
		result += "RECURSIVE ";
	}
	bool first_cte = true;

	for (auto &kv : map) {
		if (!first_cte) {
			result += ", ";
		}
		auto &cte = *kv.second;
		result += SQLIdentifier(kv.first);
		if (!cte.aliases.empty()) {
			result += " (";
			for (idx_t k = 0; k < cte.aliases.size(); k++) {
				if (k > 0) {
					result += ", ";
				}
				result += SQLIdentifier(cte.aliases[k]);
			}
			result += ")";
		}
		if (!cte.key_targets.empty()) {
			result += " USING KEY (";
			for (idx_t k = 0; k < cte.key_targets.size(); k++) {
				if (k > 0) {
					result += ", ";
				}
				result += cte.key_targets[k]->ToString();
				if (cte.key_targets[k]->HasAlias()) {
					result += StringUtil::Format(" AS %s", SQLIdentifier(cte.key_targets[k]->GetAlias()));
				}
			}
			result += ") ";
		}
		if (kv.second->materialized == CTEMaterialize::CTE_MATERIALIZE_ALWAYS) {
			result += " AS MATERIALIZED (";
		} else if (kv.second->materialized == CTEMaterialize::CTE_MATERIALIZE_NEVER) {
			result += " AS NOT MATERIALIZED (";
		} else {
			result += " AS (";
		}
		D_ASSERT(cte.query_node);
		result += cte.query_node->ToString();
		result += ")";
		first_cte = false;
	}
	return result + " ";
}

string QueryNode::ResultModifiersToString() const {
	string result;
	for (idx_t modifier_idx = 0; modifier_idx < modifiers.size(); modifier_idx++) {
		auto &modifier = *modifiers[modifier_idx];
		if (modifier.type == ResultModifierType::ORDER_MODIFIER) {
			auto &order_modifier = modifier.Cast<OrderModifier>();
			result += " ORDER BY ";
			for (idx_t k = 0; k < order_modifier.orders.size(); k++) {
				if (k > 0) {
					result += ", ";
				}
				result += order_modifier.orders[k].ToString();
			}
		} else if (modifier.type == ResultModifierType::LIMIT_MODIFIER) {
			auto &limit_modifier = modifier.Cast<LimitModifier>();
			if (limit_modifier.limit) {
				if (limit_modifier.limit_type == LimitValueType::PERCENTAGE) {
					result += " LIMIT (" + limit_modifier.limit->ToString() + ") %";
				} else {
					result += " LIMIT " + limit_modifier.limit->ToString();
				}
			}
			if (limit_modifier.offset) {
				result += " OFFSET " + limit_modifier.offset->ToString();
			}
		}
	}
	return result;
}

bool QueryNode::Equals(const QueryNode *other) const {
	if (!other) {
		return false;
	}
	if (this == other) {
		return true;
	}
	if (other->type != this->type) {
		return false;
	}

	if (modifiers.size() != other->modifiers.size()) {
		return false;
	}
	for (idx_t i = 0; i < modifiers.size(); i++) {
		if (!modifiers[i]->Equals(*other->modifiers[i])) {
			return false;
		}
	}
	// WITH clauses (CTEs)
	if (cte_map.map.size() != other->cte_map.map.size()) {
		return false;
	}

	for (auto &entry : cte_map.map) {
		auto other_entry = other->cte_map.map.find(entry.first);
		if (other_entry == other->cte_map.map.end()) {
			return false;
		}

		if (entry.second->aliases != other->cte_map.map.at(entry.first)->aliases) {
			return false;
		}
		if (!ParsedExpression::ListEquals(entry.second->key_targets, other_entry->second->key_targets)) {
			return false;
		}
		if (!ParsedExpression::ListEquals(entry.second->payload_aggregates, other_entry->second->payload_aggregates)) {
			return false;
		}
		if (!entry.second->query_node ||
		    !entry.second->query_node->Equals(other->cte_map.map.at(entry.first)->query_node.get())) {
			return false;
		}
	}
	return true;
}

void QueryNode::CopyProperties(QueryNode &other) const {
	for (auto &modifier : modifiers) {
		other.modifiers.push_back(modifier->Copy());
	}
	for (auto &kv : cte_map.map) {
		other.cte_map.map[kv.first] = kv.second->Copy();
	}
}

void QueryNode::AddDistinct() {
	// check if we already have a DISTINCT modifier
	for (idx_t modifier_idx = modifiers.size(); modifier_idx > 0; modifier_idx--) {
		auto &modifier = *modifiers[modifier_idx - 1];
		if (modifier.type == ResultModifierType::DISTINCT_MODIFIER) {
			auto &distinct_modifier = modifier.Cast<DistinctModifier>();
			if (distinct_modifier.distinct_on_targets.empty()) {
				// we have a DISTINCT without an ON clause - this distinct does not need to be added
				return;
			}
		} else if (modifier.type == ResultModifierType::LIMIT_MODIFIER) {
			// we encountered a LIMIT or LIMIT PERCENT - these change the result of DISTINCT, so we do need to push a
			// DISTINCT relation
			break;
		}
	}
	modifiers.push_back(make_uniq<DistinctModifier>());
}

void QueryNode::VerifyDeserialized(const unique_ptr<QueryNode> &node) {
	if (!node) {
		throw SerializationException("Failed to deserialize query node: missing query node");
	}
	for (auto &modifier : node->modifiers) {
		if (!modifier) {
			throw SerializationException("Failed to deserialize query node: missing result modifier");
		}
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				if (!order.expression) {
					throw SerializationException("Failed to deserialize query node: missing ORDER BY expression");
				}
			}
		} else if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			for (auto &target : modifier->Cast<DistinctModifier>().distinct_on_targets) {
				if (!target) {
					throw SerializationException("Failed to deserialize query node: missing DISTINCT ON expression");
				}
			}
		}
	}
	for (auto &cte : node->cte_map.map) {
		if (!cte.second || !cte.second->query_node) {
			throw SerializationException("Failed to deserialize query node: missing CTE query");
		}
	}
	switch (node->type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select_node = node->Cast<SelectNode>();
		if (select_node.select_list.empty()) {
			throw SerializationException("Failed to deserialize query node: empty select list");
		}
		for (auto &expr : select_node.select_list) {
			if (!expr) {
				throw SerializationException("Failed to deserialize query node: missing select list expression");
			}
		}
		for (auto &expr : select_node.groups.group_expressions) {
			if (!expr) {
				throw SerializationException("Failed to deserialize query node: missing group expression");
			}
		}
		break;
	}
	case QueryNodeType::RECURSIVE_CTE_NODE: {
		auto &cte_node = node->Cast<RecursiveCTENode>();
		if (!cte_node.left || !cte_node.right) {
			throw SerializationException("Failed to deserialize query node: missing recursive CTE child");
		}
		break;
	}
	default:
		break;
	}
}

} // namespace duckdb
