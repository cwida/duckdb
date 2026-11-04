#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

enum class JSONModifyType : uint8_t { SET, INSERT, REPLACE, REMOVE };

//! Parse a JSONPath or bare key into path elements (throws on malformed paths and wildcards)
static vector<JSONPathElement> ParseModifyPath(const char *ptr, idx_t len, bool binder) {
	vector<JSONPathElement> elements;
	if (len == 0) {
		return elements; // The empty path addresses the whole document
	}
	if (*ptr == '$') {
		elements = JSONCommon::ParsePathElements(ptr, len, binder);
		for (const auto &element : elements) {
			if (element.type == JSONPathElementType::WILDCARD ||
			    element.type == JSONPathElementType::RECURSIVE_WILDCARD) {
				throw InvalidInputException("JSON path wildcards are not supported in JSON modification functions");
			}
		}
	} else {
		// Bare key name
		JSONPathElement element;
		element.type = JSONPathElementType::KEY;
		element.key = string(ptr, len);
		elements.push_back(std::move(element));
	}
	return elements;
}

//! Bind data for the JSON modification functions, holds the path elements of a constant path
struct JSONModifyFunctionData : public FunctionData {
public:
	JSONModifyFunctionData(bool constant_p, string path_p) : constant(constant_p), path(std::move(path_p)) {
		if (constant && (path.empty() || path[0] != '/')) {
			// JSONPath, bare key, or empty path: parse it once so execution can skip tokenizing it per row
			try {
				elements = ParseModifyPath(path.c_str(), path.size(), true);
				use_elements = true;
			} catch (const std::exception &) {
				// Invalid path: parse per row instead, so the error surfaces only if a row is actually
				// evaluated, keeping it catchable by TRY and silent in unreachable branches
			}
		}
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JSONModifyFunctionData>(constant, path);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JSONModifyFunctionData>();
		return constant == other.constant && path == other.path;
	}
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();
		bool constant = false;
		string path;
		if (arguments[1]->IsFoldable()) {
			const auto path_val = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
			if (!path_val.IsNull()) {
				constant = true;
				path = path_val.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>();
			}
		}
		return make_uniq<JSONModifyFunctionData>(constant, std::move(path));
	}

public:
	const bool constant;
	const string path;
	vector<JSONPathElement> elements;
	bool use_elements = false;
};

//! Resolve an array path element to a position, returns false if it is out of range
static bool ResolveArrayPosition(yyjson_mut_val *arr, const JSONPathElement &element, idx_t &pos) {
	idx_t arr_len = yyjson_mut_arr_size(arr);
	switch (element.type) {
	case JSONPathElementType::INDEX:
		pos = element.index;
		break;
	case JSONPathElementType::REVERSE_INDEX:
		if (element.index > arr_len) {
			return false;
		}
		pos = arr_len - element.index;
		break;
	case JSONPathElementType::APPEND:
		pos = arr_len;
		break;
	default:
		return false;
	}
	return pos <= arr_len;
}

//! Attach a child container to a parent at the given path element
static void AttachChild(yyjson_mut_doc *doc, yyjson_mut_val *parent, const JSONPathElement &element,
                        yyjson_mut_val *child) {
	if (element.type == JSONPathElementType::KEY) {
		yyjson_mut_obj_put(parent, yyjson_mut_strncpy(doc, element.key.c_str(), element.key.size()), child);
	} else {
		yyjson_mut_arr_append(parent, child);
	}
}

struct JSONModifyTarget {
	//! Container holding the final path element (existing, or freshly created and not yet attached)
	yyjson_mut_val *parent = nullptr;
	//! Existing container to attach a freshly created chain to (nullptr if nothing was created)
	yyjson_mut_val *attach_to = nullptr;
	//! First created container of the chain
	yyjson_mut_val *created = nullptr;
	//! Path element at which the created chain attaches
	const JSONPathElement *attach_element = nullptr;
};

//! Resolve the parent of the final path element. When create is set, missing containers are created
//! like SQLite does, but only attached to the document once the modification succeeds.
static bool ResolveParent(yyjson_mut_doc *doc, const vector<JSONPathElement> &elements, bool create,
                          JSONModifyTarget &target) {
	auto val = yyjson_mut_doc_get_root(doc);
	for (idx_t i = 0; i + 1 < elements.size(); i++) {
		auto &element = elements[i];
		auto &next = elements[i + 1];
		yyjson_mut_val *child;
		if (element.type == JSONPathElementType::KEY) {
			if (!yyjson_mut_is_obj(val)) {
				return false;
			}
			child = yyjson_mut_obj_getn(val, element.key.c_str(), element.key.size());
		} else {
			if (!yyjson_mut_is_arr(val)) {
				return false;
			}
			idx_t pos;
			if (!ResolveArrayPosition(val, element, pos)) {
				return false;
			}
			child = yyjson_mut_arr_get(val, pos); // NULL when pos equals the length
		}
		if (!child) {
			if (!create) {
				return false;
			}
			// Create the missing container based on the next path element
			child = next.type == JSONPathElementType::KEY ? yyjson_mut_obj(doc) : yyjson_mut_arr(doc);
			if (!target.created) {
				// Attach to the document only once the modification succeeds
				target.created = child;
				target.attach_to = val;
				target.attach_element = &element;
			} else {
				// val is part of the detached chain, safe to link immediately
				AttachChild(doc, val, element, child);
			}
		}
		val = child;
	}
	target.parent = val;
	return val != nullptr;
}

//! Apply the modification at the final path element, returns true if the document was modified
static bool ApplyModify(yyjson_mut_doc *doc, yyjson_mut_val *parent, const JSONPathElement &element,
                        JSONModifyType type, yyjson_mut_val *new_val) {
	if (element.type == JSONPathElementType::KEY) {
		if (!yyjson_mut_is_obj(parent)) {
			return false;
		}
		auto existing = yyjson_mut_obj_getn(parent, element.key.c_str(), element.key.size());
		if (type == JSONModifyType::INSERT && existing) {
			return false; // Existing values are never overwritten
		}
		if ((type == JSONModifyType::REPLACE || type == JSONModifyType::REMOVE) && !existing) {
			return false; // Missing paths are never created
		}
		if (type == JSONModifyType::REMOVE) {
			yyjson_mut_obj_remove_keyn(parent, element.key.c_str(), element.key.size());
		} else {
			yyjson_mut_obj_put(parent, yyjson_mut_strncpy(doc, element.key.c_str(), element.key.size()), new_val);
		}
		return true;
	}
	if (!yyjson_mut_is_arr(parent)) {
		return false;
	}
	idx_t pos;
	if (!ResolveArrayPosition(parent, element, pos)) {
		return false;
	}
	const auto exists = pos < yyjson_mut_arr_size(parent);
	switch (type) {
	case JSONModifyType::SET:
		if (exists) {
			yyjson_mut_arr_replace(parent, pos, new_val);
		} else {
			yyjson_mut_arr_append(parent, new_val);
		}
		return true;
	case JSONModifyType::INSERT:
		if (exists) {
			return false; // Existing elements are never overwritten
		}
		yyjson_mut_arr_append(parent, new_val);
		return true;
	case JSONModifyType::REPLACE:
		if (!exists) {
			return false;
		}
		yyjson_mut_arr_replace(parent, pos, new_val);
		return true;
	case JSONModifyType::REMOVE:
		if (!exists) {
			return false;
		}
		yyjson_mut_arr_remove(parent, pos);
		return true;
	}
	return false;
}

//! Modify at the document root
static void ModifyRoot(yyjson_mut_doc *doc, JSONModifyType type, yyjson_mut_val *new_val) {
	switch (type) {
	case JSONModifyType::SET:
	case JSONModifyType::REPLACE:
		yyjson_mut_doc_set_root(doc, new_val);
		break;
	case JSONModifyType::INSERT:
		break; // The root always exists, never overwritten
	case JSONModifyType::REMOVE:
		yyjson_mut_doc_set_root(doc, nullptr);
		break;
	}
}

//! Apply a modification at a JSON Pointer (paths starting with '/')
static void ModifyAtPointer(yyjson_mut_doc *doc, const string_t &path_str, yyjson_mut_val *new_val,
                            JSONModifyType type) {
	auto ptr = path_str.GetData();
	auto len = path_str.GetSize();
	switch (type) {
	case JSONModifyType::SET:
		// Try set first (overwrites existing, creates missing).
		// Fall back to add for cases set cannot handle (e.g. appending with /-).
		if (!yyjson_mut_doc_ptr_setx(doc, ptr, len, new_val, true, nullptr, nullptr)) {
			yyjson_mut_doc_ptr_addx(doc, ptr, len, new_val, true, nullptr, nullptr);
		}
		break;
	case JSONModifyType::INSERT:
		// Only insert if there is no value at the pointer yet
		if (!yyjson_mut_doc_ptr_getx(doc, ptr, len, nullptr, nullptr)) {
			yyjson_mut_doc_ptr_addx(doc, ptr, len, new_val, true, nullptr, nullptr);
		}
		break;
	case JSONModifyType::REPLACE:
		yyjson_mut_doc_ptr_replacex(doc, ptr, len, new_val, nullptr, nullptr);
		break;
	case JSONModifyType::REMOVE:
		yyjson_mut_doc_ptr_removex(doc, ptr, len, nullptr, nullptr);
		break;
	}
}

//! Apply a modification at pre-parsed path elements
static void ModifyDocumentElements(yyjson_mut_doc *doc, const vector<JSONPathElement> &elements,
                                   yyjson_mut_val *new_val, JSONModifyType type) {
	if (elements.empty()) {
		ModifyRoot(doc, type, new_val);
		return;
	}
	const auto create = type == JSONModifyType::SET || type == JSONModifyType::INSERT;
	JSONModifyTarget target;
	if (ResolveParent(doc, elements, create, target) &&
	    ApplyModify(doc, target.parent, elements.back(), type, new_val) && target.created) {
		AttachChild(doc, target.attach_to, *target.attach_element, target.created);
	}
}

//! Apply a modification at a JSONPath or JSON Pointer, using the bind data's path elements when available
static bool ModifyDocument(yyjson_mut_doc *doc, const JSONModifyFunctionData &info, const string_t &path_str,
                           yyjson_mut_val *new_val, JSONModifyType type) {
	if (info.use_elements) {
		ModifyDocumentElements(doc, info.elements, new_val, type);
	} else if (info.constant && !info.path.empty() && info.path[0] == '/') {
		ModifyAtPointer(doc, string_t(info.path.c_str(), UnsafeNumericCast<uint32_t>(info.path.size())), new_val, type);
	} else {
		// Non constant path, or a constant path that did not parse at bind time (the error surfaces here)
		auto ptr = path_str.GetData();
		auto len = path_str.GetSize();
		if (len != 0 && *ptr == '/') {
			ModifyAtPointer(doc, path_str, new_val, type);
		} else {
			ModifyDocumentElements(doc, ParseModifyPath(ptr, len, false), new_val, type);
		}
	}
	return yyjson_mut_doc_get_root(doc) != nullptr;
}

//! Shared implementation of json_set, json_insert, and json_replace
static void ModifyFunction(JSONModifyType type, DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONModifyFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, [&](string_t doc_str, string_t path_str, string_t val_str) {
		    auto doc = JSONCommon::ReadDocument(doc_str, JSONCommon::READ_FLAG, alc);
		    auto mut_doc = yyjson_doc_mut_copy(doc, alc);

		    auto val_doc = JSONCommon::ReadDocument(val_str, JSONCommon::READ_FLAG, alc);
		    auto new_val = yyjson_val_mut_copy(mut_doc, val_doc->root);

		    ModifyDocument(mut_doc, info, path_str, new_val, type);
		    return JSONCommon::WriteVal<yyjson_mut_val>(yyjson_mut_doc_get_root(mut_doc), alc);
	    });

	JSONAllocator::AddBuffer(result, alc);
}

//! Set a value at a path in a JSON document (create if missing, overwrite if exists)
static void JsonSetFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ModifyFunction(JSONModifyType::SET, args, state, result);
}

//! Insert a value at a path in a JSON document (no-op if a value already exists at the path)
static void JsonInsertFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ModifyFunction(JSONModifyType::INSERT, args, state, result);
}

//! Replace the value at a path in a JSON document (no-op if the path does not exist)
static void JsonReplaceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ModifyFunction(JSONModifyType::REPLACE, args, state, result);
}

//! Remove the value at a path in a JSON document (no-op if the path does not exist)
static void JsonRemoveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONModifyFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t doc_str, string_t path_str) -> optional<string_t> {
		    auto doc = JSONCommon::ReadDocument(doc_str, JSONCommon::READ_FLAG, alc);
		    auto mut_doc = yyjson_doc_mut_copy(doc, alc);

		    if (!ModifyDocument(mut_doc, info, path_str, nullptr, JSONModifyType::REMOVE)) {
			    return nullopt; // Removing the root leaves no document behind
		    }
		    return JSONCommon::WriteVal<yyjson_mut_val>(yyjson_mut_doc_get_root(mut_doc), alc);
	    });

	JSONAllocator::AddBuffer(result, alc);
}

ScalarFunctionSet JSONFunctions::GetSetFunction() {
	ScalarFunction fun("json_set", {}, LogicalType::JSON(), JsonSetFunction, JSONModifyFunctionData::Bind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature()
	    .AddParameter("json", LogicalType::JSON())
	    .AddParameter("path", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::JSON());
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetInsertFunction() {
	ScalarFunction fun("json_insert", {}, LogicalType::JSON(), JsonInsertFunction, JSONModifyFunctionData::Bind,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature()
	    .AddParameter("json", LogicalType::JSON())
	    .AddParameter("path", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::JSON());
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetReplaceFunction() {
	ScalarFunction fun("json_replace", {}, LogicalType::JSON(), JsonReplaceFunction, JSONModifyFunctionData::Bind,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature()
	    .AddParameter("json", LogicalType::JSON())
	    .AddParameter("path", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::JSON());
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetRemoveFunction() {
	ScalarFunction fun("json_remove", {}, LogicalType::JSON(), JsonRemoveFunction, JSONModifyFunctionData::Bind,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", LogicalType::JSON()).AddParameter("path", LogicalType::VARCHAR);
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

} // namespace duckdb
