#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"

namespace duckdb {

namespace {

static bool IsRemappable(const LogicalType &type) {
	return type.IsNested() && type.id() != LogicalTypeId::VARIANT;
}

struct RemapColumnInfo {
	optional_idx index;
	optional_idx default_index;
	vector<RemapColumnInfo> child_remap_info;

	inline bool operator==(const RemapColumnInfo &rhs) const {
		return index == rhs.index && default_index == rhs.default_index && child_remap_info == rhs.child_remap_info;
	};
};

struct RemapStructBindData : public FunctionData {
	explicit RemapStructBindData(vector<RemapColumnInfo> remap_info_p) : remap_info(std::move(remap_info_p)) {
	}

	vector<RemapColumnInfo> remap_info;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RemapStructBindData>(remap_info);
	};

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RemapStructBindData>();
		return remap_info == other.remap_info;
	}
};

void RemapNested(Vector &input, Vector &default_vector, Vector &result, idx_t result_size,
                 const vector<RemapColumnInfo> &remap_info);

//! Marks the children of every struct row that is NULL in the validity as NULL
void PropagateStructNulls(Vector &vec, const ValidityMask &validity, idx_t count) {
	if (vec.GetType().id() != LogicalTypeId::STRUCT) {
		return;
	}
	for (auto &child : StructVector::GetEntries(vec)) {
		child.Flatten();
		auto &child_validity = FlatVector::ValidityMutable(child);
		for (idx_t row = 0; row < count; row++) {
			if (!validity.RowIsValid(row)) {
				child_validity.SetInvalid(row);
			}
		}
		PropagateStructNulls(child, child_validity, count);
	}
}

void RemapChildVectors(const Vector &result, const vector<reference<Vector>> &input_vectors,
                       const vector<reference<Vector>> &result_vectors, const vector<RemapColumnInfo> &remap_info,
                       Vector &default_vector, const bool has_top_level_null, idx_t count) {
	// set up the correct vector references
	for (idx_t i = 0; i < remap_info.size(); i++) {
		auto &remap = remap_info[i];
		if (remap.index.IsValid() && !remap.child_remap_info.empty()) {
			// nested remap - recurse
			auto &input_vector = input_vectors[remap.index.GetIndex()];
			reference<Vector> child_default = default_vector;
			if (remap.default_index.IsValid()) {
				auto &defaults = StructVector::GetEntries(default_vector);
				child_default = defaults[remap.default_index.GetIndex()];
			}
			RemapNested(input_vector, child_default.get(), result_vectors[i], count, remap.child_remap_info);
			continue;
		}
		if (!remap.index.IsValid() && remap.default_index.IsValid() && !remap.child_remap_info.empty()) {
			// nested default without a source - remap the fields of the default to the target type
			auto &defaults = StructVector::GetEntries(default_vector);
			auto &nested_default = defaults[remap.default_index.GetIndex()];
			if (nested_default.GetType().id() == LogicalTypeId::STRUCT &&
			    result_vectors[i].get().GetType().id() == LogicalTypeId::STRUCT) {
				RemapNested(nested_default, nested_default, result_vectors[i], count, remap.child_remap_info);
				if (has_top_level_null) {
					FlatVector::SetValidity(result_vectors[i], FlatVector::Validity(result));
					PropagateStructNulls(result_vectors[i], FlatVector::Validity(result), count);
				}
				continue;
			}
		}
		// primitive type remap
		if (remap.default_index.IsValid()) {
			auto &defaults = StructVector::GetEntries(default_vector);
			result_vectors[i].get().Reference(defaults[remap.default_index.GetIndex()]);
			if (result_vectors[i].get().GetVectorType() != VectorType::CONSTANT_VECTOR) {
				throw InternalException("Default value in remap struct must be a constant");
			}
			bool default_is_null = ConstantVector::IsNull(result_vectors[i]);
			FlatVector::SetSize(result_vectors[i], count);
			result_vectors[i].get().Flatten();
			if (has_top_level_null && !default_is_null) {
				// if we have any top-level NULL values and the default value is not NULL, we need to propagate the NULL
				// values to the default value
				FlatVector::SetValidity(result_vectors[i], FlatVector::Validity(result));
			}
		} else {
			result_vectors[i].get().Reference(input_vectors[remap.index.GetIndex()]);
		}
	}
}

//! Copy the list_entry_t values from the input vector into the result, preserving top-level validity
//! Returns false if the input is a constant NULL - in that case the result is set to a constant NULL
bool CopyListEntries(Vector &input, Vector &result, idx_t result_size) {
	bool is_constant = input.GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (is_constant && ConstantVector::IsNull(input)) {
		ConstantVector::SetNull(result, count_t(result_size));
		return false;
	}
	auto writer = FlatVector::Writer<list_entry_t>(result, result_size);
	if (is_constant) {
		// broadcast the single list entry over all result rows
		auto list_entry = *ConstantVector::GetData<list_entry_t>(input);
		for (idx_t i = 0; i < result_size; i++) {
			writer.WriteValue(list_entry);
		}
		return true;
	}
	for (const auto entry : input.Values<list_entry_t>()) {
		if (entry.IsValid()) {
			writer.WriteValue(entry.GetValueUnsafe());
		} else {
			writer.WriteNull();
		}
	}
	return true;
}

void RemapMap(Vector &input, Vector &default_vector, Vector &result, idx_t result_size,
              const vector<RemapColumnInfo> &remap_info) {
	auto &input_key_vector = MapVector::GetKeys(input);
	auto &input_value_vector = MapVector::GetValues(input);

	auto &result_key_vector = MapVector::GetKeys(result);
	auto &result_value_vector = MapVector::GetValues(result);
	auto list_size = ListVector::GetListSize(input);
	ListVector::Reserve(result, list_size);
	ListVector::SetListSize(result, list_size);

	// copy over the list_entry_t values from the input vector, preserving top-level validity
	if (!CopyListEntries(input, result, result_size)) {
		return;
	}
	// set up the correct vector references
	D_ASSERT(remap_info.size() == 2);

	//! Build up the inputs for remapping the children of the map
	vector<reference<Vector>> input_vectors;
	input_vectors.emplace_back(input_key_vector);
	input_vectors.emplace_back(input_value_vector);

	vector<reference<Vector>> result_vectors;
	result_vectors.emplace_back(result_key_vector);
	result_vectors.emplace_back(result_value_vector);

	RemapChildVectors(result, input_vectors, result_vectors, remap_info, default_vector, false, list_size);
}

void RemapList(Vector &input, Vector &default_vector, Vector &result, idx_t result_size,
               const vector<RemapColumnInfo> &remap_info) {
	auto &input_vector = ListVector::GetChildMutable(input);
	auto &result_vector = ListVector::GetChildMutable(result);
	auto list_size = ListVector::GetListSize(input);
	ListVector::Reserve(result, list_size);
	ListVector::SetListSize(result, list_size);

	// copy over the list_entry_t values from the input vector, preserving top-level validity
	if (!CopyListEntries(input, result, result_size)) {
		return;
	}

	//! Build up the input for remapping the child of the list
	vector<reference<Vector>> input_vectors;
	input_vectors.emplace_back(input_vector);

	vector<reference<Vector>> result_vectors;
	result_vectors.emplace_back(result_vector);

	RemapChildVectors(result, input_vectors, result_vectors, remap_info, default_vector, false, list_size);
}

void RemapStruct(Vector &input, Vector &default_vector, Vector &result, idx_t result_size,
                 const vector<RemapColumnInfo> &remap_info) {
	auto &input_child_vectors = StructVector::GetEntries(input);
	auto &result_child_vectors = StructVector::GetEntries(result);
	if (result_child_vectors.size() != remap_info.size()) {
		throw InternalException("Remap info unaligned in remap struct");
	}
	bool has_top_level_null = false;
	// copy over the NULL values from the input vector
	if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(input)) {
			ConstantVector::SetNull(result, count_t(result_size));
			return;
		}
	} else {
		auto validity_entries = input.Validity();
		if (validity_entries.CanHaveNull()) {
			auto &result_validity = FlatVector::ValidityMutable(result);
			for (idx_t i = 0; i < result_size; i++) {
				if (!validity_entries.IsValid(i)) {
					result_validity.SetInvalid(i);
				}
			}
			has_top_level_null = result_validity.CanHaveNull();
		}
	}

	//! Build up the input for remapping the children of the struct
	vector<reference<Vector>> input_vectors;
	for (auto &child : input_child_vectors) {
		input_vectors.emplace_back(child);
	}

	vector<reference<Vector>> result_vectors;
	for (auto &child : result_child_vectors) {
		result_vectors.emplace_back(child);
	}

	RemapChildVectors(result, input_vectors, result_vectors, remap_info, default_vector, has_top_level_null,
	                  result_size);
}

void RemapNested(Vector &input, Vector &default_vector, Vector &result, idx_t result_size,
                 const vector<RemapColumnInfo> &remap_info) {
	auto &source_type = input.GetType();
	D_ASSERT(IsRemappable(source_type));
	switch (source_type.id()) {
	case LogicalTypeId::STRUCT:
		return RemapStruct(input, default_vector, result, result_size, remap_info);
	case LogicalTypeId::LIST:
		return RemapList(input, default_vector, result, result_size, remap_info);
	case LogicalTypeId::MAP:
		return RemapMap(input, default_vector, result, result_size, remap_info);
	default:
		throw InvalidInputException("Can't RemapNested for type '%s'", source_type.ToString());
	}
}

void RemapStructFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<RemapStructBindData>();

	auto &input = args.data[0];

	RemapNested(input, args.data[3], result, args.size(), info.remap_info);
}
struct RemapIndex {
	idx_t index;
	LogicalType type;
	unique_ptr<identifier_map_t<RemapIndex>> child_map;

	static identifier_map_t<RemapIndex> GetMap(const LogicalType &type) {
		identifier_map_t<RemapIndex> result;
		switch (type.id()) {
		case LogicalTypeId::STRUCT: {
			auto &children = StructType::GetChildTypes(type);
			for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
				auto &child = children[child_idx];
				result.emplace(child.first, GetIndex(child_idx, child.second));
			}
			break;
		}
		case LogicalTypeId::LIST: {
			auto &child = ListType::GetChildType(type);
			result.emplace("list", GetIndex(0, child));
			break;
		}
		case LogicalTypeId::MAP: {
			auto &key = MapType::KeyType(type);
			auto &value = MapType::ValueType(type);
			result.emplace("key", GetIndex(0, key));
			result.emplace("value", GetIndex(1, value));
			break;
		}
		default:
			throw BinderException("Can't remap type %s", type.ToString());
		}
		return result;
	}

	static RemapIndex GetIndex(idx_t idx, const LogicalType &type) {
		RemapIndex index;
		index.index = idx;
		index.type = type;
		if (IsRemappable(type)) {
			index.child_map = make_uniq<identifier_map_t<RemapIndex>>(GetMap(type));
		}
		return index;
	}
};

struct RemapEntry {
	optional_idx index;
	optional_idx default_index;
	LogicalType target_type;
	unique_ptr<identifier_map_t<RemapEntry>> child_remaps;

	static void PerformRemap(const Identifier &remap_target, const Value &remap_val,
	                         identifier_map_t<RemapIndex> &source_map, identifier_map_t<RemapIndex> &target_map,
	                         identifier_map_t<RemapEntry> &result, const LogicalType &parent_type) {
		string remap_source;
		Value struct_val;
		if (remap_val.type().id() == LogicalTypeId::VARCHAR) {
			remap_source = remap_val.ToString();
		} else if (StructType::IsStruct(remap_val.type())) {
			// the remap spec is a (source_name, nested_spec) pair, built as an unnamed TUPLE
			if (remap_val.type().id() != LogicalTypeId::TUPLE) {
				throw BinderException("Remap keys for remap_struct needs to be an unnamed struct");
			}
			auto &children = StructValue::GetChildren(remap_val);
			if (children.size() != 2) {
				throw BinderException("Remap keys for remap_struct needs to have two children");
			}
			if (children[0].type().id() != LogicalTypeId::VARCHAR || !StructType::IsStruct(children[1].type())) {
				throw BinderException("Remap keys for remap_struct need to be varchar and struct");
			}
			remap_source = children[0].ToString();
			struct_val = children[1];
		} else {
			throw BinderException("Remap keys for remap_struct needs to be a string or struct");
		}

		// find the source index
		auto entry = source_map.find(Identifier(remap_source));
		if (entry == source_map.end() && parent_type.id() == LogicalTypeId::LIST) {
			// A LIST has exactly one child, which GetMap canonically keys as "list". Some producers
			// carry the physical child name instead (e.g. the multi-file reader for Parquet files whose
			// repeated group is named "array" for parquet-avro/Hive or "bag" for Spark legacy). The
			// mapping is unambiguous for a LIST, so fall back to the canonical "list" child.
			entry = source_map.find(Identifier("list"));
		}
		if (entry == source_map.end()) {
			throw BinderException("Source value %s not found", remap_source);
		}
		auto target_entry = target_map.find(remap_target);
		if (target_entry == target_map.end()) {
			throw BinderException("Target value %s not found", remap_target.GetIdentifierName());
		}

		auto &source_type = entry->second.type;
		auto &target_type = target_entry->second.type;

		bool source_is_nested = IsRemappable(source_type);
		bool target_is_nested = IsRemappable(target_type);
		RemapEntry remap;
		remap.index = entry->second.index;
		remap.target_type = target_entry->second.type;
		if (source_type.id() == LogicalTypeId::SQLNULL && struct_val.IsNull()) {
			// NULL can be cast directly to a nested type without remapping its children.
			result.emplace(remap_target, std::move(remap));
			return;
		}
		if (source_is_nested || target_is_nested || !struct_val.IsNull()) {
			if (source_type.id() != target_type.id()) {
				throw BinderException("Can't change source type (%s) to target type (%s), type conversion not allowed",
				                      source_type.ToString(), target_type.ToString());
			}
			if (!struct_val.IsNull()) {
				// this is a struct - we actually need all 3 of these to be true (or none of them to be true)
				if (!source_is_nested || !target_is_nested || struct_val.IsNull()) {
					throw BinderException("Found a struct value (%s) as a remap, this is only expected for a nested "
					                      "type, source type is '%s', target type is '%s'",
					                      struct_val.ToString(), entry->second.type.ToString(),
					                      target_entry->second.type.ToString());
				}
				remap.child_remaps = make_uniq<identifier_map_t<RemapEntry>>();
				auto &remap_types = StructType::GetChildTypes(struct_val.type());
				auto &remap_values = StructValue::GetChildren(struct_val);
				for (idx_t child_idx = 0; child_idx < remap_types.size(); child_idx++) {
					PerformRemap(remap_types[child_idx].first, remap_values[child_idx], *entry->second.child_map,
					             *target_entry->second.child_map, *remap.child_remaps, source_type);
				}
			}
		}
		result.emplace(remap_target, std::move(remap));
	}

	static void HandleDefault(idx_t default_idx, const string &default_target, const LogicalType &default_type,
	                          identifier_map_t<RemapIndex> &target_map, identifier_map_t<RemapEntry> &result) {
		auto entry = target_map.find(Identifier(default_target));
		if (entry == target_map.end()) {
			throw BinderException("Default value %s not found for remap", default_target);
		}
		auto &target_type = entry->second.type;

		RemapEntry remap;
		remap.default_index = default_idx;
		if (default_type.id() == LogicalTypeId::STRUCT) {
			// nested remap - recurse
			if (!IsRemappable(target_type)) {
				throw BinderException("Default value is a struct - target value should be a nested type, is '%s'",
				                      target_type.ToString());
			}
			// add to the map at this level only if it does not yet exist
			auto result_entry = result.find(Identifier(default_target));
			if (result_entry == result.end()) {
				result.emplace(default_target, std::move(remap));
				result_entry = result.find(Identifier(default_target));
				result_entry->second.child_remaps = make_uniq<identifier_map_t<RemapEntry>>();
			} else {
				// the entry exists - add the default index
				result_entry->second.default_index = default_idx;
			}
			auto &child_types = StructType::GetChildTypes(default_type);
			for (idx_t child_idx = 0; child_idx < child_types.size(); child_idx++) {
				auto &child_default = child_types[child_idx];
				if (!result_entry->second.child_remaps || !entry->second.child_map) {
					throw BinderException("No child remaps found");
				}
				HandleDefault(child_idx, child_default.first.GetIdentifierName(), child_default.second,
				              *entry->second.child_map, *result_entry->second.child_remaps);
			}
			return;
		}
		// non-nested type - add it to the map
		if (default_type != target_type) {
			throw BinderException("Default key %s does not match target type %s - add a cast", default_target,
			                      target_type);
		}
		auto added = result.emplace(default_target, std::move(remap));
		if (!added.second) {
			throw BinderException("Duplicate value provided for target %s", default_target);
		}
	}

	static vector<RemapColumnInfo> ConstructMapFromChildren(const child_list_t<LogicalType> &target_children,
	                                                        const identifier_map_t<RemapEntry> &remap_map) {
		vector<RemapColumnInfo> result;
		for (idx_t target_idx = 0; target_idx < target_children.size(); target_idx++) {
			auto &target_name = target_children[target_idx].first;
			auto &child_type = target_children[target_idx].second;
			auto entry = remap_map.find(target_name);
			if (entry == remap_map.end()) {
				throw BinderException("Missing target value %s for remap", target_name);
			}
			RemapColumnInfo info;
			info.index = entry->second.index;
			info.default_index = entry->second.default_index;
			if (IsRemappable(child_type) && entry->second.child_remaps) {
				// type is nested and a mapping for it is given - recurse
				info.child_remap_info = ConstructMap(child_type, *entry->second.child_remaps);
			}
			result.push_back(std::move(info));
		}
		return result;
	}

	static vector<RemapColumnInfo> ConstructMap(const LogicalType &type,
	                                            const identifier_map_t<RemapEntry> &remap_map) {
		D_ASSERT(IsRemappable(type));
		switch (type.id()) {
		case LogicalTypeId::STRUCT: {
			auto &target_children = StructType::GetChildTypes(type);
			return ConstructMapFromChildren(target_children, remap_map);
		}
		case LogicalTypeId::LIST: {
			auto &child_type = ListType::GetChildType(type);
			child_list_t<LogicalType> target_children;
			target_children.emplace_back("list", child_type);
			return ConstructMapFromChildren(target_children, remap_map);
		}
		case LogicalTypeId::MAP: {
			auto &key_type = MapType::KeyType(type);
			auto &value_type = MapType::ValueType(type);
			child_list_t<LogicalType> target_children;
			target_children.emplace_back("key", key_type);
			target_children.emplace_back("value", value_type);
			return ConstructMapFromChildren(target_children, remap_map);
		}
		default:
			throw BinderException("Can't ConstructMap for type '%s'", type.ToString());
		}
	}

	static child_list_t<LogicalType> RemapCastChildren(const child_list_t<LogicalType> &source_children,
	                                                   const identifier_map_t<RemapEntry> &remap_map,
	                                                   const unordered_map<idx_t, string> &source_name_map) {
		child_list_t<LogicalType> new_source_children;
		for (idx_t source_idx = 0; source_idx < source_children.size(); source_idx++) {
			auto &child_name = source_children[source_idx].first;
			auto &child_type = source_children[source_idx].second;
			auto entry = source_name_map.find(source_idx);
			if (entry != source_name_map.end()) {
				auto remap_entry = remap_map.find(Identifier(entry->second));
				D_ASSERT(remap_entry != remap_map.end());
				// this entry is remapped - fetch the target type
				if (IsRemappable(child_type) && remap_entry->second.child_remaps) {
					// type is nested and a mapping for it is given - recurse
					new_source_children.emplace_back(child_name,
					                                 RemapCast(child_type, *remap_entry->second.child_remaps));
				} else {
					new_source_children.emplace_back(child_name, remap_entry->second.target_type);
				}
			} else {
				// entry is not remapped - keep the original type
				new_source_children.push_back(source_children[source_idx]);
			}
		}
		return new_source_children;
	}

	static LogicalType RemapCast(const LogicalType &type, const identifier_map_t<RemapEntry> &remap_map) {
		unordered_map<idx_t, string> source_name_map;
		for (auto &entry : remap_map) {
			if (!entry.second.index.IsValid()) {
				continue;
			}
			auto result = source_name_map.emplace(entry.second.index.GetIndex(), entry.first);
			if (!result.second) {
				auto &other = remap_map.find(Identifier(result.first->second))->second;
				if (other.target_type != entry.second.target_type) {
					throw BinderException("remap_struct cannot map a single source field to targets of different "
					                      "types ('%s' and '%s')",
					                      other.target_type.ToString(), entry.second.target_type.ToString());
				}
			}
		}

		child_list_t<LogicalType> source_children;
		switch (type.id()) {
		case LogicalTypeId::STRUCT:
			source_children = StructType::GetChildTypes(type);
			break;
		case LogicalTypeId::LIST:
			source_children.emplace_back("list", ListType::GetChildType(type));
			break;
		case LogicalTypeId::MAP:
			source_children.emplace_back("key", MapType::KeyType(type));
			source_children.emplace_back("value", MapType::ValueType(type));
			break;
		default:
			throw BinderException("Can't RemapCast for type '%s'", type.ToString());
		}
		return LogicalType::ConstructNestedType(type, RemapCastChildren(source_children, remap_map, source_name_map));
	}
};

//! Verifies that the (uncast) argument types can be remapped
void VerifyRemapArgumentTypes(const vector<LogicalType> &types) {
	D_ASSERT(types.size() == 4);
	for (idx_t arg_idx = 0; arg_idx < 3; arg_idx++) {
		auto &arg_type = types[arg_idx];
		if (arg_type.id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
		if (arg_type.id() == LogicalTypeId::SQLNULL && arg_idx == 2) {
			// remap target can be NULL
			continue;
		}
		if (!IsRemappable(arg_type)) {
			throw BinderException("Struct remap can only remap nested types, not '%s'", arg_type.ToString());
		} else if (StructType::IsStruct(arg_type) && StructType::IsUnnamed(arg_type)) {
			throw BinderException("Struct remap can only remap named structs");
		}
	}
	auto &from_type = types[0];
	auto &to_type = types[1];

	auto &defaults_type = types[3];
	if (defaults_type.id() != LogicalTypeId::SQLNULL && !StructType::IsStruct(defaults_type)) {
		throw BinderException("The defaults provided to 'remap_struct' should be of type STRUCT if they're not NULL");
	}
	if (StructType::IsStruct(defaults_type) && StructType::IsUnnamed(defaults_type)) {
		throw BinderException("The defaults have to be either NULL or a named STRUCT, not an unnamed struct");
	}

	if ((IsRemappable(from_type) || IsRemappable(to_type)) && from_type.id() != to_type.id()) {
		throw BinderException("Can't change source type (%s) to target type (%s), type conversion not allowed",
		                      from_type.ToString(), to_type.ToString());
	}
}

//! (Recursively) generates the remap entries from the (uncast) argument types
identifier_map_t<RemapEntry> GetRemapEntries(const vector<LogicalType> &types, const Value &remap_val,
                                             bool defaults_are_constant) {
	auto &from_type = types[0];
	auto &to_type = types[1];
	auto source_map = RemapIndex::GetMap(from_type);
	auto target_map = RemapIndex::GetMap(to_type);

	identifier_map_t<RemapEntry> remap_map;
	if (!remap_val.IsNull()) {
		auto &remap_types = StructType::GetChildTypes(types[2]);
		auto &remap_values = StructValue::GetChildren(remap_val);
		for (idx_t remap_idx = 0; remap_idx < remap_values.size(); remap_idx++) {
			auto &remap_val = remap_values[remap_idx];
			auto &remap_target = remap_types[remap_idx].first;
			RemapEntry::PerformRemap(remap_target, remap_val, source_map, target_map, remap_map, from_type);
		}
	}
	if (!defaults_are_constant) {
		throw BinderException("Default values must be constants");
	}

	if (types[3].id() != LogicalTypeId::SQLNULL) {
		// (recursively) handle the defaults (if there are any)
		auto &default_types = StructType::GetChildTypes(types[3]);
		for (idx_t default_idx = 0; default_idx < default_types.size(); default_idx++) {
			auto &default_target = default_types[default_idx].first;
			auto &default_type = default_types[default_idx].second;
			RemapEntry::HandleDefault(default_idx, default_target.GetIdentifierName(), default_type, target_map,
			                          remap_map);
		}
	}
	return remap_map;
}

void RemapStructResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto types = input.GetArgumentTypes();
	VerifyRemapArgumentTypes(types);
	auto remap_map = GetRemapEntries(types, input.GetConstant(2), input.GetArgument(3).IsFoldable());

	// push a cast for argument 0 to match up the source types to the target
	bound_function.GetArguments()[0] = RemapEntry::RemapCast(types[0], remap_map);
	bound_function.GetArguments()[1] = types[1];
	bound_function.GetArguments()[2] = types[2];
	bound_function.GetArguments()[3] = types[3];
	bound_function.SetReturnType(types[1]);
}

//! The remapping is derived from the types the function was called with, before the input was cast
unique_ptr<FunctionData> RemapStructBind(BindScalarFunctionInput &input) {
	auto &types = input.GetBoundFunction().GetLogicalArguments();
	auto remap_map = GetRemapEntries(types, input.GetConstant(2), input.GetArguments()[3]->IsFoldable());
	return make_uniq<RemapStructBindData>(RemapEntry::ConstructMap(types[1], remap_map));
}

} // namespace

ScalarFunction RemapStructFun::GetFunction() {
	ScalarFunction remap("remap_struct", {}, LogicalTypeId::ANY, RemapStructFunction, RemapStructBind);
	remap.GetSignature()
	    .AddParameter("input", LogicalTypeId::ANY)
	    .AddParameter("target_type", LogicalTypeId::ANY)
	    .AddParameter("mapping", LogicalTypeId::ANY)
	    .AddParameter("defaults", LogicalTypeId::ANY);
	remap.SetResolveTypesCallback(RemapStructResolveTypes);
	remap.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return remap;
}

} // namespace duckdb
