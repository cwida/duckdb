#include "duckdb/execution/index/art/node_handle.hpp"

#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

NodeHandle::NodeHandle(ART &art, const NodePtr node_ptr)
    : handle(NodePtr::GetAllocator(art, node_ptr.GetType()).GetHandle(node_ptr)), type(node_ptr.GetType()) {
	handle.MarkModified();
}

NodeHandle::NodeHandle(FixedSizeAllocator &allocator, const NodePtr node_ptr, NType type)
    : handle(allocator.GetHandle(node_ptr)), type(type) {
	D_ASSERT(node_ptr.GetType() == type);
	handle.MarkModified();
}

NodeHandle::NodeHandle(NodeHandle &&other) noexcept : handle(std::move(other.handle)), type(other.type) {
}

NodeHandle &NodeHandle::operator=(NodeHandle &&other) noexcept {
	if (this != &other) {
		handle = std::move(other.handle);
		type = other.type;
	}
	return *this;
}

ConstNodeHandle::ConstNodeHandle(const ART &art, const NodePtr node_ptr)
    : handle(NodePtr::GetAllocator(art, node_ptr.GetType()).GetHandle(node_ptr)), type(node_ptr.GetType()) {
}

} // namespace duckdb
