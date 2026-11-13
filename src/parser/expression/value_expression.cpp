#include "duckdb/common/string_util.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"

#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"

#include <cmath>

namespace duckdb {

unique_ptr<ConstantExpression> ConstantExpression::FromLiteral(Literal literal) {
	return make_uniq<ConstantExpression>(std::move(literal));
}

unique_ptr<ConstantExpression> ConstantExpression::Null() {
	return FromLiteral(Literal::Null());
}

unique_ptr<ConstantExpression> ConstantExpression::Boolean(bool value) {
	return FromLiteral(Literal::Boolean(value));
}

unique_ptr<ConstantExpression> ConstantExpression::Integer(int64_t value) {
	return FromLiteral(Literal::Integer(value));
}

unique_ptr<ConstantExpression> ConstantExpression::Number(string text) {
	return FromLiteral(Literal::Number(std::move(text)));
}

unique_ptr<ConstantExpression> ConstantExpression::String(string text) {
	return FromLiteral(Literal::String(std::move(text)));
}

unique_ptr<ConstantExpression> ConstantExpression::Hex(string text) {
	return FromLiteral(Literal::Hex(std::move(text)));
}

unique_ptr<ConstantExpression> ConstantExpression::Bit(string text) {
	return FromLiteral(Literal::Bit(std::move(text)));
}

//! Whether binding the literal yields exactly this value (same type, same contents)
static bool RoundTrips(const Literal &literal, const Value &value) {
	auto bound = literal.ToValue();
	return bound.type() == value.type() && !ValueOperations::DistinctFrom(bound, value);
}

static unique_ptr<ParsedExpression> CastTo(const LogicalType &type, unique_ptr<ParsedExpression> child) {
	return make_uniq<CastExpression>(type, std::move(child));
}

static unique_ptr<ParsedExpression> LiteralOrCast(Literal literal, const Value &value) {
	if (RoundTrips(literal, value)) {
		return ConstantExpression::FromLiteral(std::move(literal));
	}
	return CastTo(value.type(), ConstantExpression::FromLiteral(std::move(literal)));
}

static unique_ptr<ParsedExpression> StringCast(const Value &value) {
	return CastTo(value.type(), ConstantExpression::String(value.ToString()));
}

static unique_ptr<ParsedExpression> ListValueExpression(vector<unique_ptr<ParsedExpression>> children) {
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", "list_value"), std::move(children));
}

static vector<unique_ptr<ParsedExpression>> ChildExpressions(const vector<Value> &values) {
	vector<unique_ptr<ParsedExpression>> result;
	result.reserve(values.size());
	for (auto &child : values) {
		result.push_back(ConstantExpression::FromValue(child));
	}
	return result;
}

static unique_ptr<ParsedExpression> NamedArgument(const Identifier &name, const Value &value,
                                                  const char *function_name) {
	// the parser sets both the argument name and the alias, and the binders read the alias
	auto child = ConstantExpression::FromValue(value);
	child->SetAlias(name);
	vector<FunctionArgument> arguments;
	arguments.emplace_back(name, std::move(child));
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", Identifier(function_name)),
	                                     std::move(arguments));
}

static unique_ptr<ParsedExpression> StructExpression(const Value &value) {
	auto &type = value.type();
	auto &children = StructValue::GetChildren(value);
	if (StructType::IsUnnamed(type)) {
		return make_uniq<FunctionExpression>(QualifiedName("system", "main", "row"), ChildExpressions(children));
	}
	vector<FunctionArgument> arguments;
	for (idx_t i = 0; i < children.size(); i++) {
		auto &name = StructType::GetChildName(type, i);
		auto child = ConstantExpression::FromValue(children[i]);
		child->SetAlias(name);
		arguments.emplace_back(name, std::move(child));
	}
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", "struct_pack"), std::move(arguments));
}

static unique_ptr<ParsedExpression> MapExpression(const Value &value) {
	vector<unique_ptr<ParsedExpression>> keys;
	vector<unique_ptr<ParsedExpression>> values;
	for (auto &entry : MapValue::GetChildren(value)) {
		auto &pair = StructValue::GetChildren(entry);
		keys.push_back(ConstantExpression::FromValue(pair[0]));
		values.push_back(ConstantExpression::FromValue(pair[1]));
	}
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(ListValueExpression(std::move(keys)));
	arguments.push_back(ListValueExpression(std::move(values)));
	// the map constructor cannot reproduce empty or NULL-only key/value types
	return CastTo(value.type(),
	              make_uniq<FunctionExpression>(QualifiedName("system", "main", "map"), std::move(arguments)));
}

static unique_ptr<ParsedExpression> ValueFunction(const string &name, vector<unique_ptr<ParsedExpression>> arguments) {
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", Identifier(name)), std::move(arguments));
}

static unique_ptr<ParsedExpression> IntervalExpression(const interval_t &value) {
	vector<unique_ptr<ParsedExpression>> parts;
	for (auto &part : vector<pair<string, Value>> {{"to_months", Value::INTEGER(value.months)},
	                                               {"to_days", Value::INTEGER(value.days)},
	                                               {"to_microseconds", Value::BIGINT(value.micros)}}) {
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(ConstantExpression::FromValue(part.second));
		parts.push_back(ValueFunction(part.first, std::move(arguments)));
	}
	vector<unique_ptr<ParsedExpression>> sum;
	sum.push_back(std::move(parts[0]));
	sum.push_back(std::move(parts[1]));
	auto months_and_days = ValueFunction("add", std::move(sum));
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(std::move(months_and_days));
	result.push_back(std::move(parts[2]));
	return ValueFunction("add", std::move(result));
}

static unique_ptr<ParsedExpression> GeometryExpression(const Value &value) {
	auto &type = value.type();
	unique_ptr<ParsedExpression> result;
	if (value.IsNull()) {
		auto geometry = GeoType::HasCRS(type) ? Value("GEOMETRYCOLLECTION EMPTY") : Value();
		result = CastTo(LogicalType::GEOMETRY(), ConstantExpression::FromValue(geometry));
	} else {
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(ConstantExpression::FromValue(Value::BLOB_RAW(StringValue::Get(value))));
		result = ValueFunction("st_geomfromwkb", std::move(arguments));
	}
	if (!GeoType::HasCRS(type)) {
		return result;
	}
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(result));
	arguments.push_back(ConstantExpression::String(GeoType::GetCRS(type).GetDefinition()));
	result = ValueFunction("st_setcrs", std::move(arguments));
	if (value.IsNull()) {
		auto typed_null = make_uniq<CaseExpression>();
		typed_null->CaseChecksMutable().push_back({ConstantExpression::Boolean(false), std::move(result)});
		typed_null->ElseMutable() = ConstantExpression::Null();
		result = std::move(typed_null);
	}
	return result;
}

static bool HasUnsupportedVariantKeys(const VariantNode &node) {
	// Struct literals require nonempty, case-insensitively unique keys; inspect before the lossy STRUCT conversion.
	if (node.GetTypeId() == VariantLogicalType::OBJECT) {
		identifier_set_t names;
		for (auto &child : node.GetObjectChildren()) {
			if (child.key.GetSize() == 0 || !names.insert(Identifier(child.key.GetString())).second ||
			    HasUnsupportedVariantKeys(child.value)) {
				return true;
			}
		}
	} else if (node.GetTypeId() == VariantLogicalType::ARRAY) {
		for (auto child : node.GetArrayChildren()) {
			if (HasUnsupportedVariantKeys(child)) {
				return true;
			}
		}
	}
	return false;
}

bool ConstantExpression::RequiresTypeWitness(const LogicalType &type) {
	return TypeVisitor::Contains(type, [&](const LogicalType &child) {
		return child.IsAggregateState() || child.id() == LogicalTypeId::TYPE ||
		       (child.id() == LogicalTypeId::GEOMETRY && GeoType::HasCRS(child)) ||
		       (type.id() != LogicalTypeId::VARCHAR && child.id() == LogicalTypeId::VARCHAR &&
		        !StringType::GetCollation(child).empty());
	});
}

static unique_ptr<ParsedExpression> NestedValueExpression(const LogicalType &type, optional_ptr<const Value> value) {
	if (type.IsAggregateState() || type.id() == LogicalTypeId::GEOMETRY || type.id() == LogicalTypeId::TYPE ||
	    (type.id() != LogicalTypeId::TUPLE && TypeExpression::CanRepresent(type) &&
	     !ConstantExpression::RequiresTypeWitness(type))) {
		return ConstantExpression::FromValue(value ? value->WithType(type) : Value(type));
	}
	const bool empty_list =
	    value && !value->IsNull() && type.id() == LogicalTypeId::LIST && ListValue::GetChildren(*value).empty();
	if (value && (value->IsNull() || empty_list)) {
		auto result = make_uniq<CaseExpression>();
		result->CaseChecksMutable().push_back(
		    {ConstantExpression::Boolean(false), NestedValueExpression(type, nullptr)});
		result->ElseMutable() = empty_list ? ListValueExpression({}) : ConstantExpression::Null();
		return std::move(result);
	}
	if (type.id() == LogicalTypeId::MAP) {
		vector<Value> keys, values;
		if (value) {
			for (auto &entry : MapValue::GetChildren(*value)) {
				auto &children = StructValue::GetChildren(entry);
				keys.push_back(children[0]);
				values.push_back(children[1]);
			}
		}
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(ConstantExpression::FromValue(Value::LIST(MapType::KeyType(type), std::move(keys))));
		arguments.push_back(ConstantExpression::FromValue(Value::LIST(MapType::ValueType(type), std::move(values))));
		auto result = ValueFunction("map", std::move(arguments));
		return type.HasAlias() ? CastTo(type, std::move(result)) : std::move(result);
	}
	if (type.id() == LogicalTypeId::UNION && UnionType::GetMemberCount(type) == 1) {
		auto &member_type = UnionType::GetMemberType(type, 0);
		auto child = NestedValueExpression(member_type, value ? &UnionValue::GetValue(*value) : nullptr);
		vector<FunctionArgument> arguments;
		arguments.emplace_back(UnionType::GetMemberName(type, 0), std::move(child));
		unique_ptr<ParsedExpression> result =
		    make_uniq<FunctionExpression>(QualifiedName("system", "main", "union_value"), std::move(arguments));
		return type.HasAlias() ? CastTo(type, std::move(result)) : std::move(result);
	}
	vector<FunctionArgument> arguments;
	child_list_t<LogicalType> child_types;
	optional_ptr<const vector<Value>> children;
	string function;
	switch (type.id()) {
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::STRUCT:
		function = type.id() == LogicalTypeId::TUPLE ? "row" : "struct_pack";
		child_types = StructType::GetChildTypes(type);
		if (value) {
			children = StructValue::GetChildren(*value);
		}
		break;
	case LogicalTypeId::LIST:
		function = "list_value";
		if (value) {
			children = ListValue::GetChildren(*value);
		}
		child_types.resize(children ? children->size() : 1, {Identifier(), ListType::GetChildType(type)});
		break;
	case LogicalTypeId::ARRAY:
		function = "array_value";
		if (value) {
			children = ArrayValue::GetChildren(*value);
		}
		child_types.resize(ArrayType::GetSize(type), {Identifier(), ArrayType::GetChildType(type)});
		break;
	default:
		throw NotImplementedException("The nested value type has no SQL constructor");
	}
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto child = NestedValueExpression(child_types[i].second, children ? &(*children)[i] : nullptr);
		auto name = function == "struct_pack" ? child_types[i].first : Identifier();
		child->SetAlias(name);
		arguments.emplace_back(name, std::move(child));
	}
	unique_ptr<ParsedExpression> result =
	    make_uniq<FunctionExpression>(QualifiedName("system", "main", Identifier(function)), std::move(arguments));
	return type.HasAlias() ? CastTo(type, std::move(result)) : std::move(result);
}

unique_ptr<ParsedExpression> ConstantExpression::FromValue(const Value &value) {
	auto &type = value.type();
	if (type.IsAggregateState()) {
		auto storage_type = type.WithAlias("").WithExtensionInfo(nullptr);
		Vector source(value, count_t(1));
		Vector storage(storage_type, 1);
		storage.Reinterpret(source);
		auto result = ExportAggregateFunction::StateToSQL(type, FromValue(storage.GetValue(0)));
		if (!result) {
			throw NotImplementedException("Aggregate state SQL parameters are not representable");
		}
		return result;
	}
	if (type.id() == LogicalTypeId::VARCHAR && !StringType::GetCollation(type).empty()) {
		auto result = FromValue(value.WithType(LogicalType::VARCHAR));
		if (type.HasAlias()) {
			result = CastTo(type, std::move(result));
		}
		return make_uniq<CollateExpression>(StringType::GetCollation(type), std::move(result));
	}
	if (type.id() == LogicalTypeId::TYPE) {
		if (value.IsNull()) {
			return CastTo(type, Null());
		}
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(FromValue(Value(TypeValue::GetType(value))));
		return ValueFunction("get_type", std::move(arguments));
	}
	if (type.id() == LogicalTypeId::GEOMETRY) {
		auto result = GeometryExpression(value);
		return type.HasAlias() ? CastTo(type, std::move(result)) : std::move(result);
	}
	if (TypeVisitor::Contains(type, [](const LogicalType &child) {
		    return child.id() == LogicalTypeId::ENUM && EnumType::GetSize(child) == 0;
	    })) {
		throw NotImplementedException("The nested value type has no SQL constructor");
	}
	if (RequiresTypeWitness(type) || type.id() == LogicalTypeId::TUPLE) {
		return NestedValueExpression(type, value);
	}
	if (!value.IsNull() && type.id() == LogicalTypeId::INTERVAL) {
		auto result = IntervalExpression(IntervalValue::Get(value));
		return type.HasAlias() ? CastTo(type, std::move(result)) : std::move(result);
	}
	if (value.IsNull()) {
		if (type.id() == LogicalTypeId::SQLNULL) {
			return Null();
		}
		return CastTo(type, Null());
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return LiteralOrCast(Literal::Boolean(value.GetValue<bool>()), value);
	case LogicalTypeId::VARCHAR:
		return LiteralOrCast(Literal::String(StringValue::Get(value)), value);
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto dbl = value.GetValue<double>();
		if (dbl == 0 && std::signbit(dbl)) {
			return StringCast(value);
		}
		if (Value::IsNan(dbl)) {
			return StringCast(value);
		}
		if (!Value::DoubleIsFinite(dbl)) {
			// an out-of-range number literal is the SQL spelling of infinity
			return LiteralOrCast(Literal::Number(dbl < 0 ? "-1e1000" : "1e1000"), value);
		}
		return LiteralOrCast(Literal::Number(value.ToString()), value);
	}
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DECIMAL:
		return LiteralOrCast(Literal::Number(value.ToString()), value);
	case LogicalTypeId::BLOB: {
		auto &bytes = StringValue::Get(value);
		string hex;
		hex.reserve(bytes.size() * 2);
		for (auto byte : bytes) {
			hex += StringUtil::Format("%02X", static_cast<uint8_t>(byte));
		}
		return LiteralOrCast(Literal::Hex(std::move(hex)), value);
	}
	case LogicalTypeId::BIT:
		return LiteralOrCast(Literal::Bit(value.ToString()), value);
	case LogicalTypeId::STRUCT:
		return StructExpression(value);
	case LogicalTypeId::VARIANT: {
		Vector vector(value, count_t(1));
		VariantIterator iterator(vector);
		if (HasUnsupportedVariantKeys(iterator.Root(0))) {
			throw NotImplementedException("VARIANT object keys cannot be represented by a struct literal");
		}
		auto payload = VariantValue::GetValue(value);
		return CastTo(type, CastTo(payload.type(), FromValue(payload)));
	}
	case LogicalTypeId::LIST: {
		auto &children = ListValue::GetChildren(value);
		if (children.empty()) {
			return CastTo(type, ListValueExpression({}));
		}
		return ListValueExpression(ChildExpressions(children));
	}
	case LogicalTypeId::ARRAY: {
		auto &children = ArrayValue::GetChildren(value);
		if (children.empty()) {
			return CastTo(type, ListValueExpression({}));
		}
		return CastTo(type, make_uniq<FunctionExpression>(QualifiedName("system", "main", "array_value"),
		                                                  ChildExpressions(children)));
	}
	case LogicalTypeId::MAP:
		return MapExpression(value);
	case LogicalTypeId::POINTER:
		return FromLiteral(Literal::Pointer(value.GetPointer()));
	case LogicalTypeId::UNION: {
		auto tag = UnionValue::GetTag(value);
		auto &name = UnionType::GetMemberName(type, tag);
		return CastTo(type, NamedArgument(name, UnionValue::GetValue(value), "union_value"));
	}
	default:
		return StringCast(value);
	}
}

} // namespace duckdb
