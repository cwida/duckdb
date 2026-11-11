#include "parquet_shredding.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {
class ClientContext;

ChildShreddingTypes::ChildShreddingTypes() : types(make_uniq<unordered_map<string, ShreddingType>>()) {
}

ChildShreddingTypes ChildShreddingTypes::Copy() const {
	ChildShreddingTypes result;
	for (const auto &type : *types) {
		result.types->emplace(type.first, type.second.Copy());
	}
	return result;
}

ShreddingType::ShreddingType() : set(false) {
}

ShreddingType::ShreddingType(const LogicalType &type) : set(true), type(type) {
}

bool ShreddingType::operator==(const ShreddingType &other) const {
	if (set != other.set) {
		return false;
	}
	if (set && type != other.type) {
		return false;
	}
	if (!children.types && !other.children.types) {
		return true;
	}
	if (!children.types || !other.children.types) {
		return false;
	}
	auto &a_children = *children.types;
	auto &b_children = *other.children.types;
	if (a_children.size() != b_children.size()) {
		return false;
	}
	for (auto a_it = a_children.begin(); a_it != a_children.end(); ++a_it) {
		auto &a_name = a_it->first;
		auto b_it = b_children.find(a_name);
		if (b_it == b_children.end()) {
			return false;
		}

		auto &a_child = a_it->second;
		auto &b_child = b_it->second;
		if (!(a_child == b_child)) {
			return false;
		}
	}
	return true;
}

ShreddingType ShreddingType::Copy() const {
	auto result = set ? ShreddingType(type) : ShreddingType();
	result.children = children.Copy();
	return result;
}

static ShreddingType ConvertShreddingTypeRecursive(const LogicalType &type) {
	if (type.id() == LogicalTypeId::VARIANT) {
		return ShreddingType(LogicalType(LogicalTypeId::ANY));
	}
	if (!type.IsNested()) {
		// the values are written without further conversion or validation, so types with an alias (e.g., JSON) and
		// types without a Parquet typed_value representation cannot be shredded on
		if (type.HasAlias() || type.id() == LogicalTypeId::BIT || type.id() == LogicalTypeId::BIGNUM) {
			throw BinderException("VARIANT cannot be shredded on type %s in Parquet", type.ToString());
		}
		return ShreddingType(type);
	}

	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::LIST: {
		ShreddingType res(type);
		for (auto &entry : LogicalType::GetNamedChildTypes(type)) {
			res.AddChild(entry.first, ConvertShreddingTypeRecursive(entry.second));
		}
		return res;
	}
	default:
		break;
	}
	throw BinderException("VARIANT can only be shredded on LIST/STRUCT/ANY/non-nested type, not %s", type.ToString());
}

void ShreddingType::AddChild(const Identifier &name, ShreddingType &&child) {
	children.types->emplace(name, std::move(child));
}

optional_ptr<const ShreddingType> ShreddingType::GetChild(const Identifier &name) const {
	auto it = children.types->find(name.GetIdentifierName());
	if (it == children.types->end()) {
		return nullptr;
	}
	return it->second;
}

ShreddingType ShreddingType::GetShreddingTypes(const Value &val, ClientContext &context) {
	if (val.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("SHREDDING value should be of type VARCHAR, a stringified type to use for the column");
	}
	auto type_str = val.GetValue<string>();
	auto logical_type = TransformStringToLogicalType(type_str, context);

	return ConvertShreddingTypeRecursive(logical_type);
}

} // namespace duckdb
