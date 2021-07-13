#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

Node::Node(ART &art, NodeType type, size_t compressed_prefix_size) : prefix_length(0), count(0), type(type) {
	this->prefix = unique_ptr<uint8_t[]>(new uint8_t[compressed_prefix_size]);
}

void Node::CopyPrefix(ART &art, Node *src, Node *dst) {
	dst->prefix_length = src->prefix_length;
	memcpy(dst->prefix.get(), src->prefix.get(), src->prefix_length);
}

// LCOV_EXCL_START
unique_ptr<Node> *Node::GetChild(idx_t pos) {
	D_ASSERT(0);
	return nullptr;
}

idx_t Node::GetMin() {
	D_ASSERT(0);
	return 0;
}
// LCOV_EXCL_STOP

uint32_t Node::PrefixMismatch(ART &art, Node *node, Key &key, uint64_t depth) {
	uint64_t pos;
	for (pos = 0; pos < node->prefix_length; pos++) {
		if (key[depth + pos] != node->prefix[pos]) {
			return pos;
		}
	}
	return pos;
}

void Node::InsertLeaf(ART &art, unique_ptr<Node> &node, uint8_t key, unique_ptr<Node> &new_node) {
	switch (node->type) {
	case NodeType::N4:
		Node4::Insert(art, node, key, new_node);
		break;
	case NodeType::N16:
		Node16::Insert(art, node, key, new_node);
		break;
	case NodeType::N48:
		Node48::Insert(art, node, key, new_node);
		break;
	case NodeType::N256:
		Node256::Insert(art, node, key, new_node);
		break;
	default:
		throw InternalException("Unrecognized leaf type for insert");
	}
}

void Node::Erase(ART &art, unique_ptr<Node> &node, idx_t pos) {
	switch (node->type) {
	case NodeType::N4: {
		Node4::Erase(art, node, pos);
		break;
	}
	case NodeType::N16: {
		Node16::Erase(art, node, pos);
		break;
	}
	case NodeType::N48: {
		Node48::Erase(art, node, pos);
		break;
	}
	case NodeType::N256:
		Node256::Erase(art, node, pos);
		break;
	default:
		throw InternalException("Unrecognized leaf type for erase");
	}
}

} // namespace duckdb
