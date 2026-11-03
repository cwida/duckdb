#include "parquet_field_id.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

constexpr const char *FieldID::DUCKDB_FIELD_ID;

ChildFieldIDs::ChildFieldIDs() : ids(make_uniq<identifier_map_t<FieldID>>()) {
}

ChildFieldIDs ChildFieldIDs::Copy() const {
	ChildFieldIDs result;
	for (const auto &id : *ids) {
		result.ids->emplace(id.first, id.second.Copy());
	}
	return result;
}

FieldID::FieldID() : set(false) {
}

FieldID::FieldID(int32_t field_id_p) : set(true), field_id(field_id_p) {
}

FieldID FieldID::Copy() const {
	auto result = set ? FieldID(field_id) : FieldID();
	result.child_field_ids = child_field_ids.Copy();
	return result;
}

static case_insensitive_map_t<LogicalType> GetChildNameToTypeMap(const LogicalType &type) {
	case_insensitive_map_t<LogicalType> name_to_type_map;
	for (auto &[name, child_type] : LogicalType::GetNamedChildTypes(type)) {
		if (name == FieldID::DUCKDB_FIELD_ID) {
			throw BinderException("Cannot have column named \"%s\" with FIELD_IDS", FieldID::DUCKDB_FIELD_ID);
		}
		name_to_type_map.emplace(name.GetIdentifierName(), child_type);
	}
	return name_to_type_map;
}

void FieldID::GenerateFieldIDs(ChildFieldIDs &field_ids, idx_t &field_id, const vector<Identifier> &names,
                               const vector<LogicalType> &sql_types) {
	D_ASSERT(names.size() == sql_types.size());
	for (idx_t col_idx = 0; col_idx < names.size(); col_idx++) {
		const auto &col_name = names[col_idx];
		auto inserted = field_ids.ids->insert(make_pair(col_name, FieldID(UnsafeNumericCast<int32_t>(field_id++))));
		D_ASSERT(inserted.second);

		const auto &col_type = sql_types[col_idx];
		if (col_type.id() != LogicalTypeId::LIST && col_type.id() != LogicalTypeId::MAP &&
		    !StructType::IsStruct(col_type.id())) {
			continue;
		}

		vector<Identifier> child_names;
		vector<LogicalType> child_types;
		for (auto &[name, child_type] : LogicalType::GetNamedChildTypes(col_type)) {
			child_names.push_back(name);
			child_types.push_back(child_type);
		}
		GenerateFieldIDs(inserted.first->second.child_field_ids, field_id, child_names, child_types);
	}
}

void FieldID::GetFieldIDs(const Value &field_ids_value, ChildFieldIDs &field_ids,
                          unordered_set<uint32_t> &unique_field_ids,
                          const case_insensitive_map_t<LogicalType> &name_to_type_map) {
	const auto &struct_type = field_ids_value.type();
	if (struct_type.id() != LogicalTypeId::STRUCT) {
		throw BinderException(
		    "Expected FIELD_IDS to be a STRUCT, e.g., {col1: 42, col2: {%s: 43, nested_col: 44}, col3: 44}",
		    FieldID::DUCKDB_FIELD_ID);
	}
	const auto &struct_children = StructValue::GetChildren(field_ids_value);
	D_ASSERT(StructType::GetChildTypes(struct_type).size() == struct_children.size());
	for (idx_t i = 0; i < struct_children.size(); i++) {
		const auto &col_name = StringUtil::Lower(StructType::GetChildName(struct_type, i).GetIdentifierName());
		if (col_name == FieldID::DUCKDB_FIELD_ID) {
			continue;
		}

		auto it = name_to_type_map.find(col_name);
		if (it == name_to_type_map.end()) {
			string names;
			for (const auto &name : name_to_type_map) {
				if (!names.empty()) {
					names += ", ";
				}
				names += name.first;
			}
			throw BinderException(
			    "Column name \"%s\" specified in FIELD_IDS not found. Consider using WRITE_PARTITION_COLUMNS if this "
			    "column is a partition column. Available column names: [%s]",
			    col_name, names);
		}
		D_ASSERT(field_ids.ids->find(Identifier(col_name)) ==
		         field_ids.ids->end()); // Caught by STRUCT - deduplicates keys

		const auto &child_value = struct_children[i];
		const auto &child_type = child_value.type();
		optional_ptr<const Value> field_id_value;
		optional_ptr<const Value> child_field_ids_value;

		if (child_type.id() == LogicalTypeId::STRUCT) {
			const auto &nested_children = StructValue::GetChildren(child_value);
			D_ASSERT(StructType::GetChildTypes(child_type).size() == nested_children.size());
			for (idx_t nested_i = 0; nested_i < nested_children.size(); nested_i++) {
				const auto &field_id_or_nested_col = StructType::GetChildName(child_type, nested_i);
				if (field_id_or_nested_col == FieldID::DUCKDB_FIELD_ID) {
					field_id_value = &nested_children[nested_i];
				} else {
					child_field_ids_value = &child_value;
				}
			}
		} else {
			field_id_value = &child_value;
		}

		FieldID field_id;
		if (field_id_value) {
			Value field_id_integer_value = field_id_value->DefaultCastAs(LogicalType::INTEGER);
			if (field_id_integer_value.IsNull() || IntegerValue::Get(field_id_integer_value) < 0) {
				throw BinderException("Invalid field_id %s found in FIELD_IDS: field ids must be non-negative integers",
				                      field_id_value->ToString());
			}
			const uint32_t field_id_int = UnsafeNumericCast<uint32_t>(IntegerValue::Get(field_id_integer_value));
			if (!unique_field_ids.insert(field_id_int).second) {
				throw BinderException("Duplicate field_id %s found in FIELD_IDS", field_id_integer_value.ToString());
			}
			field_id = FieldID(UnsafeNumericCast<int32_t>(field_id_int));
		}
		auto inserted = field_ids.ids->insert(make_pair(Identifier(col_name), std::move(field_id)));
		D_ASSERT(inserted.second);

		if (child_field_ids_value) {
			const auto &col_type = it->second;
			if (col_type.id() != LogicalTypeId::LIST && col_type.id() != LogicalTypeId::MAP &&
			    !StructType::IsStruct(col_type.id())) {
				throw BinderException("Column \"%s\" with type \"%s\" cannot have a nested FIELD_IDS specification",
				                      col_name, LogicalTypeIdToString(col_type.id()));
			}

			GetFieldIDs(*child_field_ids_value, inserted.first->second.child_field_ids, unique_field_ids,
			            GetChildNameToTypeMap(col_type));
		}
	}
}

} // namespace duckdb
