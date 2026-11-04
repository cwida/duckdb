#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/unordered_set.hpp"
namespace duckdb {

bool TypeExpression::IsSQLType(LogicalTypeId id) {
	static const auto admitted_ids = [] {
		// TYPE has SQL syntax but is not included in AllTypes.
		unordered_set<LogicalTypeId> ids {LogicalTypeId::SQLNULL, LogicalTypeId::TYPE};
		for (auto &type : LogicalType::AllTypes()) {
			ids.insert(type.id());
		}
		return ids;
	}();
	return admitted_ids.count(id) != 0;
}

bool TypeExpression::CanRepresent(const LogicalType &type) {
	return type.IsComplete() && !TypeVisitor::Contains(type, [](const LogicalType &child) {
		       const bool empty_tuple = child.id() == LogicalTypeId::TUPLE && StructType::GetChildCount(child) == 0;
		       const bool empty_enum = child.id() == LogicalTypeId::ENUM && EnumType::GetSize(child) == 0;
		       return !IsSQLType(child.id()) || empty_tuple || empty_enum;
	       });
}

TypeExpression::TypeExpression(QualifiedName qualified_name_p, vector<unique_ptr<ParsedExpression>> children_p)
    : ParsedExpression(ExpressionType::TYPE, ExpressionClass::TYPE), qualified_name(std::move(qualified_name_p)),
      children(std::move(children_p)) {
	D_ASSERT(!qualified_name.Name().empty());
}

TypeExpression::TypeExpression(Identifier type_name, vector<unique_ptr<ParsedExpression>> children)
    : TypeExpression(QualifiedName(std::move(type_name)), std::move(children)) {
}

TypeExpression::TypeExpression(const string &type_name, vector<unique_ptr<ParsedExpression>> children)
    : TypeExpression(Identifier(type_name), std::move(children)) {
}

TypeExpression::TypeExpression() : ParsedExpression(ExpressionType::TYPE, ExpressionClass::TYPE) {
}

struct StackItem {
	optional_ptr<const TypeExpression> node;
	string text;
};

static void PushChild(vector<StackItem> &stack, const ParsedExpression &child) {
	if (child.GetExpressionClass() == ExpressionClass::TYPE) {
		stack.push_back(StackItem {&child.Cast<TypeExpression>(), ""});
	} else {
		stack.push_back(StackItem {nullptr, child.ToString()});
	}
}

string TypeExpression::ToString() const {
	string output;
	vector<StackItem> stack;
	stack.push_back(StackItem {this, ""});

	while (!stack.empty()) {
		auto item = std::move(stack.back());
		stack.pop_back();

		if (!item.node) {
			output += item.text;
		} else {
			auto &node = *item.node;
			auto &type_name = node.qualified_name.Name();
			auto qual = node.qualified_name.QualificationToString();
			auto &params = node.children;

			// LIST and ARRAY have special syntax
			if (qual.empty() && type_name == "LIST" && params.size() == 1) {
				stack.push_back(StackItem {nullptr, "[]"});
				PushChild(stack, *params[0]);
			} else if (qual.empty() && type_name == "ARRAY" && params.size() == 2) {
				stack.push_back(StackItem {nullptr, "]"});
				PushChild(stack, *params[1]);
				stack.push_back(StackItem {nullptr, "["});
				PushChild(stack, *params[0]);
			} else if (qual.empty() && (type_name == "STRUCT" || type_name == "UNION")) {
				// STRUCT and UNION share the NAME(alias type, ...) syntax
				string head = type_name == "STRUCT" ? "STRUCT" : "UNION";
				if (params.empty()) {
					stack.push_back(StackItem {nullptr, head});
					continue;
				}
				stack.push_back(StackItem {nullptr, ")"});
				for (idx_t i = params.size(); i > 0; i--) {
					auto &curr_param = *params[i - 1];
					PushChild(stack, curr_param);
					stack.push_back(StackItem {nullptr, SQLIdentifier(curr_param.GetAlias()) + " "});
					if (i > 1) {
						stack.push_back(StackItem {nullptr, ", "});
					}
				}
				stack.push_back(StackItem {nullptr, head + "("});
			} else if (qual.empty() && type_name == "MAP" && params.size() == 2) {
				stack.push_back(StackItem {nullptr, ")"});
				PushChild(stack, *params[1]);
				stack.push_back(StackItem {nullptr, ", "});
				PushChild(stack, *params[0]);
				stack.push_back(StackItem {nullptr, "MAP("});
			} else if (qual.empty() && type_name == "VARCHAR" && !params.empty() && params.back()->HasAlias() &&
			           params.back()->GetAlias() == "collation") {
				// the constructor form parses in every type position, a COLLATE clause only in column definitions
				auto &collate_expr = params.back()->Cast<ConstantExpression>();
				stack.push_back(
				    StackItem {nullptr, StringUtil::Format("VARCHAR(%s)",
				                                           SQLString(collate_expr.GetLiteral().ToValue().ToString()))});
			} else if (qual.empty() && type_name == "INTERVAL" && !params.empty()) {
				// We ignore interval types parameters.
				stack.push_back(StackItem {nullptr, "INTERVAL"});
			} else {
				string result = qual;
				auto type_id = TransformStringToLogicalTypeId(type_name.GetIdentifierName());
				if (type_id != LogicalTypeId::UNBOUND && type_id != LogicalTypeId::SQLNULL) {
					// Built-in type name
					result += type_name.GetIdentifierName();
				} else {
					result += SQLIdentifier(type_name);
				}
				if (params.empty()) {
					stack.push_back(StackItem {nullptr, result});
					continue;
				}
				stack.push_back(StackItem {nullptr, ")"});
				for (idx_t i = params.size(); i > 0; i--) {
					PushChild(stack, *params[i - 1]);
					if (i > 1) {
						stack.push_back(StackItem {nullptr, ", "});
					}
				}
				stack.push_back(StackItem {nullptr, result + "("});
			}
		}
	}
	return output;
}

//===--------------------------------------------------------------------===//
// LogicalType -> TypeExpression
//===--------------------------------------------------------------------===//

namespace {

unique_ptr<ParsedExpression> TypeChild(const LogicalType &type, const Identifier &name) {
	auto child = TypeExpression::FromLogicalType(type);
	if (!name.empty()) {
		child->SetAlias(name);
	}
	return std::move(child);
}

// type parameters are the literal atoms the parser would produce for the type's SQL spelling
unique_ptr<ParsedExpression> LiteralChild(unique_ptr<ConstantExpression> child, const char *label = nullptr) {
	if (label) {
		child->SetAlias(Identifier(label));
	}
	return std::move(child);
}

//! The name a built-in type is spelled with. Must be a name DefaultTypeGenerator knows.
const char *BuiltinTypeName(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case LogicalTypeId::TINYINT:
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::INTEGER:
		return "INTEGER";
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::HUGEINT:
		return "HUGEINT";
	case LogicalTypeId::UTINYINT:
		return "UTINYINT";
	case LogicalTypeId::USMALLINT:
		return "USMALLINT";
	case LogicalTypeId::UINTEGER:
		return "UINTEGER";
	case LogicalTypeId::UBIGINT:
		return "UBIGINT";
	case LogicalTypeId::UHUGEINT:
		return "UHUGEINT";
	case LogicalTypeId::FLOAT:
		return "FLOAT";
	case LogicalTypeId::DOUBLE:
		return "DOUBLE";
	case LogicalTypeId::BIGNUM:
		return "BIGNUM";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME";
	case LogicalTypeId::TIME_NS:
		return "TIME_NS";
	case LogicalTypeId::TIME_TZ:
		return "TIMETZ";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP_US";
	case LogicalTypeId::TIMESTAMP_SEC:
		return "TIMESTAMP_S";
	case LogicalTypeId::TIMESTAMP_MS:
		return "TIMESTAMP_MS";
	case LogicalTypeId::TIMESTAMP_NS:
		return "TIMESTAMP_NS";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "TIMESTAMPTZ";
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return "TIMESTAMPTZ_NS";
	case LogicalTypeId::INTERVAL:
		return "INTERVAL";
	case LogicalTypeId::VARCHAR:
		return "VARCHAR";
	case LogicalTypeId::BLOB:
		return "BLOB";
	case LogicalTypeId::BIT:
		return "BIT";
	case LogicalTypeId::UUID:
		return "UUID";
	case LogicalTypeId::SQLNULL:
		return "NULL";
	case LogicalTypeId::TYPE:
		return "TYPE";
	case LogicalTypeId::VARIANT:
		return "VARIANT";
	case LogicalTypeId::GEOMETRY:
		return "GEOMETRY";
	case LogicalTypeId::GEOGRAPHY:
		return "GEOGRAPHY";
	case LogicalTypeId::DECIMAL:
		return "DECIMAL";
	case LogicalTypeId::ENUM:
		return "ENUM";
	case LogicalTypeId::LIST:
		return "LIST";
	case LogicalTypeId::ARRAY:
		return "ARRAY";
	case LogicalTypeId::STRUCT:
		return "STRUCT";
	case LogicalTypeId::TUPLE:
		return "TUPLE";
	case LogicalTypeId::MAP:
		return "MAP";
	case LogicalTypeId::UNION:
		return "UNION";
	default:
		return nullptr;
	}
}

} // namespace

unique_ptr<TypeExpression> TypeExpression::FromLogicalType(const LogicalType &type) {
	if (type.IsUnbound()) {
		auto &expr = UnboundType::GetTypeExpression(type);
		if (expr->GetExpressionClass() != ExpressionClass::TYPE) {
			throw InternalException("Unbound type does not wrap a type expression");
		}
		return unique_ptr_cast<ParsedExpression, TypeExpression>(expr->Copy());
	}

	vector<unique_ptr<ParsedExpression>> children;

	// A type that carries an alias is a user-defined type: name it, and let the binder resolve it again.
	auto alias = type.GetAlias();
	if (!alias.empty()) {
		if (type.HasExtensionInfo()) {
			for (auto &modifier : type.GetExtensionInfo()->modifiers) {
				children.push_back(ConstantExpression::FromValue(modifier.value));
			}
		}
		return make_uniq<TypeExpression>(Identifier(alias), std::move(children));
	}

	auto name = BuiltinTypeName(type.id());
	if (!name) {
		// only the internal placeholder ids (ANY, UNKNOWN, INVALID, ...) have no name, and none of them is a
		// type a user can name in the first place
		throw InternalException("Type '%s' cannot be expressed as a type expression", type.ToString());
	}

	switch (type.id()) {
	case LogicalTypeId::DECIMAL:
		children.push_back(LiteralChild(ConstantExpression::Integer(DecimalType::GetWidth(type))));
		children.push_back(LiteralChild(ConstantExpression::Integer(DecimalType::GetScale(type))));
		break;
	case LogicalTypeId::VARCHAR: {
		auto collation = StringType::GetCollation(type);
		if (!collation.empty()) {
			children.push_back(LiteralChild(ConstantExpression::String(collation), "collation"));
		}
		break;
	}
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY:
		if (GeoType::HasCRS(type)) {
			children.push_back(LiteralChild(ConstantExpression::String(GeoType::GetCRS(type).GetDefinition())));
		}
		break;
	case LogicalTypeId::ENUM:
		for (idx_t i = 0; i < EnumType::GetSize(type); i++) {
			children.push_back(LiteralChild(ConstantExpression::String(EnumType::GetString(type, i).GetString())));
		}
		break;
	case LogicalTypeId::LIST:
		children.push_back(TypeChild(ListType::GetChildType(type), Identifier()));
		break;
	case LogicalTypeId::ARRAY:
		children.push_back(TypeChild(ArrayType::GetChildType(type), Identifier()));
		if (!ArrayType::IsAnySize(type)) {
			children.push_back(
			    LiteralChild(ConstantExpression::Integer(NumericCast<int64_t>(ArrayType::GetSize(type)))));
		}
		break;
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			children.push_back(TypeChild(child.second, child.first));
		}
		break;
	case LogicalTypeId::TUPLE:
		for (auto &child : StructType::GetChildTypes(type)) {
			children.push_back(TypeChild(child.second, Identifier()));
		}
		break;
	case LogicalTypeId::MAP:
		children.push_back(TypeChild(MapType::KeyType(type), Identifier()));
		children.push_back(TypeChild(MapType::ValueType(type), Identifier()));
		break;
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			children.push_back(TypeChild(UnionType::GetMemberType(type, i), UnionType::GetMemberName(type, i)));
		}
		break;
	default:
		break;
	}

	return make_uniq<TypeExpression>(Identifier(name), std::move(children));
}

void TypeExpression::Verify() const {
	D_ASSERT(!qualified_name.Name().empty());
}

} // namespace duckdb
