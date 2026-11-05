#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

SetOperationNode::SetOperationNode() : QueryNode(QueryNodeType::SET_OPERATION_NODE) {
}

string SetOperationNode::ToString() const {
	string result;
	result = cte_map.ToString();
	result += "(" + children[0]->ToString() + ") ";

	for (idx_t i = 1; i < children.size(); i++) {
		switch (setop_type) {
		case SetOperationType::UNION:
			result += setop_all ? "UNION ALL" : "UNION";
			break;
		case SetOperationType::UNION_BY_NAME:
			result += setop_all ? "UNION ALL BY NAME" : "UNION BY NAME";
			break;
		case SetOperationType::EXCEPT:
			result += setop_all ? "EXCEPT ALL" : "EXCEPT";
			break;
		case SetOperationType::INTERSECT:
			result += setop_all ? "INTERSECT ALL" : "INTERSECT";
			break;
		default:
			throw InternalException("Unsupported set operation type");
		}
		result += " (" + children[i]->ToString() + ")";
	}
	return result + ResultModifiersToString();
}

bool SetOperationNode::Equals(const QueryNode *other_p) const {
	if (!QueryNode::Equals(other_p)) {
		return false;
	}
	if (this == other_p) {
		return true;
	}
	auto &other = other_p->Cast<SetOperationNode>();
	if (setop_type != other.setop_type) {
		return false;
	}
	if (setop_all != other.setop_all) {
		return false;
	}
	if (children.size() != other.children.size()) {
		return false;
	}
	for (idx_t i = 0; i < children.size(); i++) {
		if (!children[i]->Equals(other.children[i].get())) {
			return false;
		}
	}
	return true;
}

unique_ptr<QueryNode> SetOperationNode::Copy() const {
	auto result = make_uniq<SetOperationNode>();
	result->setop_type = setop_type;
	result->setop_all = setop_all;
	for (auto &child : children) {
		result->children.push_back(child->Copy());
	}
	this->CopyProperties(*result);
	return std::move(result);
}

SetOperationNode::SetOperationNode(SetOperationType setop_type, unique_ptr<QueryNode> left, unique_ptr<QueryNode> right,
                                   vector<unique_ptr<QueryNode>> children_p, bool setop_all)
    : QueryNode(QueryNodeType::SET_OPERATION_NODE), setop_type(setop_type), setop_all(setop_all) {
	if (children_p.empty()) {
		if (!left || !right) {
			throw SerializationException("Error deserializing SetOperationNode - left/right or children must be set");
		}
		children.push_back(std::move(left));
		children.push_back(std::move(right));
	} else {
		if (left || right) {
			throw SerializationException("Error deserializing SetOperationNode - left/right or children must be set");
		}
		children = std::move(children_p);
	}
	if (children.size() < 2) {
		throw SerializationException("SetOperationNode must have at least two children");
	}
	if (children.size() > 2 && setop_type != SetOperationType::UNION && setop_type != SetOperationType::UNION_BY_NAME) {
		throw SerializationException("SetOperationNode must have exactly two children, except for UNION");
	}
	if (setop_type == SetOperationType::NONE) {
		throw SerializationException("SetOperationNode must have a set operation type");
	}
	for (auto &child : children) {
		if (!child) {
			throw SerializationException("SetOperationNode children cannot be NULL");
		}
	}
}

unique_ptr<QueryNode> SetOperationNode::SerializeChildNode(Serializer &serializer, idx_t index) const {
	if (SerializeChildList(serializer)) {
		// serialize new version - we are serializing all children in the new "children" field
		return nullptr;
	}
	// backwards compatibility - we are targeting an older version
	// we need to serialize two children - "left" and "right"
	if (index == 0) {
		// for the left child, just directly emit the first child
		return children[0]->Copy();
	}
	if (index != 1) {
		throw InternalException("SerializeChildNode should have index 0 or 1");
	}
	vector<unique_ptr<QueryNode>> nodes;
	for (idx_t i = 1; i < children.size(); i++) {
		nodes.push_back(children[i]->Copy());
	}
	// for the right child we construct a new tree by generating the set operation over all of the nodes
	// we construct a balanced tree to avoid
	while (nodes.size() > 1) {
		vector<unique_ptr<QueryNode>> new_children;
		for (idx_t i = 0; i < nodes.size(); i += 2) {
			if (i + 1 == nodes.size()) {
				new_children.push_back(std::move(nodes[i]));
			} else {
				vector<unique_ptr<QueryNode>> empty_children;
				auto setop_node = make_uniq<SetOperationNode>(setop_type, std::move(nodes[i]), std::move(nodes[i + 1]),
				                                              std::move(empty_children), setop_all);
				new_children.push_back(std::move(setop_node));
			}
		}
		nodes = std::move(new_children);
	}
	return std::move(nodes[0]);
}

bool SetOperationNode::SerializeChildList(Serializer &serializer) const {
	return serializer.ShouldSerialize(StorageVersion::V1_5_0);
}

} // namespace duckdb
