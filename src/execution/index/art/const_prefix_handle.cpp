#include "duckdb/execution/index/art/const_prefix_handle.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

namespace {
// Tree-style branch characters
constexpr const char *const PREFIX_BRANCH_END = "└── ";
constexpr const char *const PREFIX_SPACE = "    ";

// ASCII printable character range
constexpr uint8_t PREFIX_ASCII_PRINTABLE_MIN = 32;
constexpr uint8_t PREFIX_ASCII_PRINTABLE_MAX = 126;
} // namespace

string ConstPrefixHandle::ToString(ART &art, const NodePtr &node_ptr, const ToStringOptions &options) {
	auto format_byte = [&](const uint8_t byte) {
		if (!options.inside_gate && options.display_ascii && byte >= PREFIX_ASCII_PRINTABLE_MIN &&
		    byte <= PREFIX_ASCII_PRINTABLE_MAX) {
			return string(1, static_cast<char>(byte));
		}
		return to_string(byte);
	};

	// Print prefix bytes (single branch, child follows on next line)
	auto str = StringUtil::Format("%s%sPrefix: |", options.tree_prefix, PREFIX_BRANCH_END);
	auto child_options = options;
	auto tail = Iterator(art, node_ptr, true,
	                     [&](const ConstNodeHandle &handle, const_data_ptr_t data, const NodePtr &child_ptr) {
		                     for (idx_t i = 0; i < data[art.PrefixCount()]; i++) {
			                     str += StringUtil::Format("%s|", format_byte(data[i]));
			                     if (options.key_path) {
				                     child_options.key_depth++;
			                     }
		                     }
	                     });
	str += "\n";

	// Child is printed indented under the prefix (not as a sibling)
	child_options.tree_prefix = StringUtil::Format("%s%s", options.tree_prefix, PREFIX_SPACE);
	str += tail.ToString(art, child_options);
	return str;
}

void ConstPrefixHandle::Verify(ART &art, const NodePtr &node_ptr) {
	auto tail = Iterator(art, node_ptr, true,
	                     [&](const ConstNodeHandle &handle, const_data_ptr_t data, const NodePtr &child_ptr) {
		                     D_ASSERT(data[art.PrefixCount()] != 0);
		                     D_ASSERT(data[art.PrefixCount()] <= art.PrefixCount());
	                     });

	tail.Verify(art);
}

} // namespace duckdb
