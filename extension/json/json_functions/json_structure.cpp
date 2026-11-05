#include "duckdb/common/assert.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "json_common.hpp"
#include "json_executors.hpp"
#include "json_functions.hpp"
#include "yyjson.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Structure nodes
//===--------------------------------------------------------------------===//
//! A structure node represents a subtree of the output schema.
struct StructureNode {
	explicit StructureNode(yyjson_mut_val &value_p) : value(value_p) {
	}

public:
	bool MergeType(LogicalTypeId type_p, yyjson_mut_doc *doc);

public:
	//! The inferred schema for the subtree associated with this structure node.
	reference<yyjson_mut_val> value;
	//! Merged input type; INVALID before discovery, ignored once incompatible.
	LogicalTypeId type = LogicalTypeId::INVALID;
	//! Conflicting types at this schema position forced the output to "JSON".
	bool incompatible = false;

	//! Shared schema node for all array elements.
	optional_ptr<StructureNode> array_child;
	//! Schema nodes for object fields, indexed by key.
	json_key_map_t<reference<StructureNode>> key_map;

private:
	void SetType(LogicalTypeId type_p);
	void MarkIncompatible(yyjson_mut_doc *doc);
};

bool StructureNode::MergeType(LogicalTypeId type_p, yyjson_mut_doc *doc) {
	D_ASSERT(type_p != LogicalTypeId::INVALID);
	if (incompatible) {
		return false;
	}
	if (type == LogicalTypeId::INVALID || type == LogicalTypeId::SQLNULL) {
		SetType(type_p);
		return true;
	}
	if (type_p == LogicalTypeId::SQLNULL || type == type_p) {
		return true;
	}
	if (LogicalType::IsNumeric(type) && LogicalType::IsNumeric(type_p)) {
		SetType(LogicalType::DefaultForceMaxLogicalType(LogicalType(type), LogicalType(type_p)).id());
		return true;
	}
	MarkIncompatible(doc);
	return false;
}

void StructureNode::SetType(LogicalTypeId type_p) {
	D_ASSERT(!incompatible && type_p != LogicalTypeId::INVALID);
	if (type == type_p) {
		return;
	}
	type = type_p;
	if (type == LogicalTypeId::LIST) {
		yyjson_mut_set_arr(&value.get());
	} else {
		const auto type_name = type == LogicalTypeId::STRUCT ? LogicalType::JSON_TYPE_NAME : EnumUtil::ToChars(type);
		yyjson_mut_set_str(&value.get(), type_name);
	}
}

void StructureNode::MarkIncompatible(yyjson_mut_doc *doc) {
	optional_ptr<yyjson_mut_val> fallback = yyjson_mut_str(doc, LogicalType::JSON_TYPE_NAME);
	if (!fallback) {
		throw OutOfMemoryException("Failed to allocate JSON structure value");
	}
	incompatible = true;
	array_child = nullptr;
	key_map.clear();
	// Replace the payload while preserving the link to the next sibling.
	value.get().tag = fallback->tag;
	value.get().uni = fallback->uni;
}

//===--------------------------------------------------------------------===//
// Node creation and lookup
//===--------------------------------------------------------------------===//
using NodeStorage = vector<unique_ptr<StructureNode>>;

StructureNode &CreateStructureNode(NodeStorage &nodes, yyjson_mut_doc *doc) {
	optional_ptr<yyjson_mut_val> value = yyjson_mut_str(doc, JSONCommon::TYPE_STRING_NULL);
	if (!value) {
		throw OutOfMemoryException("Failed to allocate JSON structure value");
	}
	nodes.push_back(make_uniq<StructureNode>(*value));
	return *nodes.back();
}

StructureNode &GetOrCreateArrayChild(NodeStorage &nodes, StructureNode &node, yyjson_mut_doc *doc) {
	D_ASSERT(node.type == LogicalTypeId::LIST && !node.incompatible);
	// All array elements contribute to one shared schema node.
	if (!node.array_child) {
		node.array_child = CreateStructureNode(nodes, doc);
		yyjson_mut_arr_append(&node.value.get(), &node.array_child->value.get());
	}
	return *node.array_child;
}

StructureNode &GetOrCreateObjectChild(NodeStorage &nodes, StructureNode &node, yyjson_val &key, yyjson_mut_doc *doc) {
	D_ASSERT(node.type == LogicalTypeId::STRUCT && !node.incompatible);
	const JSONKey lookup_key {unsafe_yyjson_get_str(&key), unsafe_yyjson_get_len(&key)};
	const auto entry = node.key_map.find(lookup_key);
	if (entry != node.key_map.end()) {
		return entry->second.get();
	}

	optional_ptr<yyjson_mut_val> output_key = yyjson_mut_strncpy(doc, lookup_key.ptr, lookup_key.len);
	if (!output_key) {
		throw OutOfMemoryException("Failed to allocate JSON structure key");
	}
	auto &child = CreateStructureNode(nodes, doc);
	// Empty objects keep the JSON fallback until their first field.
	if (node.key_map.empty()) {
		yyjson_mut_set_obj(&node.value.get());
	}
	yyjson_mut_obj_add(&node.value.get(), output_key.get(), &child.value.get());
	const JSONKey persistent_key {yyjson_mut_get_str(output_key.get()), lookup_key.len};
	node.key_map.emplace(persistent_key, child);
	return child;
}

//===--------------------------------------------------------------------===//
// Traversal
//===--------------------------------------------------------------------===//
//! Each frame tracks one input value; multiple frames can reuse the same structure node.
struct Frame {
	Frame(optional_ptr<yyjson_val> val_p, StructureNode &node_p) : val(val_p), node(node_p) {
	}
	//! The input JSON value being processed.
	optional_ptr<yyjson_val> val;
	//! The schema node to which this input value contributes.
	reference<StructureNode> node;
	//! Once initialized, the input value's container type determines the active iterator.
	union {
		yyjson_arr_iter arr_it;
		yyjson_obj_iter obj_it;
	};
	bool initialized = false;
};

optional<Frame> HandleArrayFrame(NodeStorage &nodes, Frame &frame, yyjson_mut_doc *doc) {
	D_ASSERT(yyjson_is_arr(frame.val.get()));
	auto &node = frame.node.get();
	if (node.incompatible) {
		return nullopt;
	}

	if (!frame.initialized) {
		if (!node.MergeType(LogicalTypeId::LIST, doc)) {
			return nullopt;
		}

		frame.arr_it = yyjson_arr_iter_with(frame.val.get());
		frame.initialized = true;
	}
	auto &child = GetOrCreateArrayChild(nodes, node, doc);
	if (child.incompatible) {
		return nullopt;
	}

	// Process scalar values without creating child frames.
	while (optional_ptr<yyjson_val> next = yyjson_arr_iter_next(&frame.arr_it)) {
		if (yyjson_is_ctn(next.get())) {
			return Frame(next, child);
		}
		if (!child.MergeType(JSONCommon::ValTypeToLogicalTypeId(next.get()), doc)) {
			return nullopt;
		}
	}
	return nullopt;
}

optional<Frame> HandleObjectFrame(NodeStorage &nodes, Frame &frame, yyjson_mut_doc *doc) {
	D_ASSERT(yyjson_is_obj(frame.val.get()));
	auto &node = frame.node.get();
	if (node.incompatible) {
		return nullopt;
	}

	if (!frame.initialized) {
		if (!node.MergeType(LogicalTypeId::STRUCT, doc)) {
			return nullopt;
		}

		frame.obj_it = yyjson_obj_iter_with(frame.val.get());
		frame.initialized = true;
	}

	// Process scalar values without creating child frames.
	while (optional_ptr<yyjson_val> key = yyjson_obj_iter_next(&frame.obj_it)) {
		auto &child = GetOrCreateObjectChild(nodes, node, *key, doc);
		if (child.incompatible) {
			continue;
		}
		const auto val = yyjson_obj_iter_get_val(key.get());
		if (yyjson_is_ctn(val)) {
			return Frame(val, child);
		}
		child.MergeType(JSONCommon::ValTypeToLogicalTypeId(val), doc);
	}
	return nullopt;
}

void HandleValueFrame(Frame &frame, yyjson_mut_doc *doc) {
	const auto val = frame.val.get();
	D_ASSERT(!yyjson_is_arr(val) && !yyjson_is_obj(val));
	frame.node.get().MergeType(JSONCommon::ValTypeToLogicalTypeId(val), doc);
}

yyjson_mut_val *ExtractStructure(yyjson_val *val, yyjson_mut_doc *doc) {
	NodeStorage nodes;
	vector<Frame> stack;
	stack.reserve(64);

	auto &root = CreateStructureNode(nodes, doc);
	stack.emplace_back(val, root);

	while (!stack.empty()) {
		auto &frame = stack.back();

		switch (yyjson_get_tag(frame.val.get())) {
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE: {
			auto child = HandleArrayFrame(nodes, frame, doc);
			if (child) {
				stack.push_back(std::move(*child));
			} else {
				stack.pop_back();
			}
			break;
		}
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
			auto child = HandleObjectFrame(nodes, frame, doc);
			if (child) {
				stack.push_back(std::move(*child));
			} else {
				stack.pop_back();
			}
			break;
		}
		default: {
			HandleValueFrame(frame, doc);
			stack.pop_back();
			break;
		}
		}
	}
	return &root.value.get();
}

//===--------------------------------------------------------------------===//
// SQL function
//===--------------------------------------------------------------------===//
optional<string_t> JSONStructureFunction(yyjson_val *val, yyjson_alc *alc, Vector &) {
	optional_ptr<yyjson_mut_doc> doc = yyjson_mut_doc_new(alc);
	if (!doc) {
		throw OutOfMemoryException("Failed to allocate JSON structure document");
	}
	return JSONCommon::WriteVal<yyjson_mut_val>(ExtractStructure(val, doc.get()), alc);
}

void StructureFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::UnaryExecute<string_t>(args, state, result, JSONStructureFunction);
}

ScalarFunction GetStructureFunctionInternal(const LogicalType &input_type) {
	ScalarFunction fun({}, LogicalType::JSON(), StructureFunction, nullptr, nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", input_type);
	return fun;
}

} // namespace

ScalarFunctionSet JSONFunctions::GetStructureFunction() {
	ScalarFunctionSet fun_set("json_structure");

	fun_set.AddFunction(GetStructureFunctionInternal(LogicalType::VARCHAR));
	fun_set.AddFunction(GetStructureFunctionInternal(LogicalType::JSON()));
	fun_set.SetFallible();

	return fun_set;
}

} // namespace duckdb
