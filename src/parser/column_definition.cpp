#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

ColumnDefinition::ColumnDefinition(Identifier name_p, LogicalType type_p)
    : name(std::move(name_p)), type(std::move(type_p)) {
}

ColumnDefinition::ColumnDefinition(Identifier name_p, LogicalType type_p, unique_ptr<ParsedExpression> expression,
                                   TableColumnType category)
    : name(std::move(name_p)), type(std::move(type_p)), category(category), expression(std::move(expression)) {
}

ColumnDefinition ColumnDefinition::Copy() const {
	ColumnDefinition copy(name, type);
	copy.oid = oid;
	copy.storage_oid = storage_oid;
	copy.expression = expression ? expression->Copy() : nullptr;
	copy.compression_type = compression_type;
	copy.category = category;
	copy.comment = comment;
	copy.tags = tags;
	return copy;
}

const ParsedExpression &ColumnDefinition::DefaultValue() const {
	if (!HasDefaultValue()) {
		if (Generated()) {
			throw InternalException("Calling DefaultValue() on a generated column");
		}
		throw InternalException("DefaultValue() called on a column without a default value");
	}
	return *expression;
}

bool ColumnDefinition::HasDefaultValue() const {
	if (Generated()) {
		return false;
	}
	return expression != nullptr;
}

void ColumnDefinition::SetDefaultValue(unique_ptr<ParsedExpression> default_value) {
	if (Generated()) {
		throw InternalException("Calling SetDefaultValue() on a generated column");
	}
	this->expression = std::move(default_value);
}

const LogicalType &ColumnDefinition::Type() const {
	return type;
}

LogicalType &ColumnDefinition::TypeMutable() {
	return type;
}

void ColumnDefinition::SetType(const LogicalType &type) {
	this->type = type;
}

const Identifier &ColumnDefinition::Name() const {
	return name;
}
void ColumnDefinition::SetName(const Identifier &name) {
	this->name = name;
}

const Value &ColumnDefinition::Comment() const {
	return comment;
}

void ColumnDefinition::SetComment(const Value &comment) {
	this->comment = comment;
}

const InsertionOrderPreservingMap<string> &ColumnDefinition::Tags() const {
	return tags;
}

void ColumnDefinition::SetTags(InsertionOrderPreservingMap<string> new_tags) {
	this->tags = std::move(new_tags);
}

const duckdb::CompressionType &ColumnDefinition::CompressionType() const {
	return compression_type;
}

void ColumnDefinition::SetCompressionType(duckdb::CompressionType compression_type) {
	this->compression_type = compression_type;
}

const storage_t &ColumnDefinition::StorageOid() const {
	return storage_oid;
}

LogicalIndex ColumnDefinition::Logical() const {
	return LogicalIndex(oid);
}

PhysicalIndex ColumnDefinition::Physical() const {
	return PhysicalIndex(storage_oid);
}

void ColumnDefinition::SetStorageOid(storage_t storage_oid) {
	this->storage_oid = storage_oid;
}

const column_t &ColumnDefinition::Oid() const {
	return oid;
}

void ColumnDefinition::SetOid(column_t oid) {
	this->oid = oid;
}

const TableColumnType &ColumnDefinition::Category() const {
	return category;
}

bool ColumnDefinition::Generated() const {
	return category == TableColumnType::GENERATED;
}

string ColumnDefinition::ToSQLString() const {
	string result = SQLIdentifier(Name()) + " ";
	auto &column_type = Type();
	if (column_type.id() != LogicalTypeId::ANY) {
		result += column_type.ToString();
	}
	if (Generated()) {
		reference<const ParsedExpression> generated_expression = GeneratedExpression();
		if (column_type.id() != LogicalTypeId::ANY) {
			// We artificially add a cast if the type is specified, need to strip it
			auto &expr = generated_expression.get();
			D_ASSERT(expr.GetExpressionType() == ExpressionType::OPERATOR_CAST);
			auto &cast_expr = expr.Cast<CastExpression>();
			generated_expression = cast_expr.Child();
		}
		result += " GENERATED ALWAYS AS(" + generated_expression.get().ToString() + ")";
	} else if (HasDefaultValue()) {
		result += " DEFAULT(" + DefaultValue().ToString() + ")";
	}
	if (CompressionType() != CompressionType::COMPRESSION_AUTO) {
		result += " USING COMPRESSION " + CompressionTypeToString(CompressionType());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Generated Columns (VIRTUAL)
//===--------------------------------------------------------------------===//

static void VerifyColumnRefs(const ParsedExpression &expr) {
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(expr, [&](const ColumnRefExpression &column_ref) {
		if (column_ref.IsQualified()) {
			throw ParserException(
			    "Qualified (tbl.name) column references are not allowed inside of generated column expressions");
		}
	});
}

static void InnerGetListOfDependencies(ParsedExpression &expr, vector<string> &dependencies) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto columnref = expr.Cast<ColumnRefExpression>();
		auto &name = columnref.GetColumnName();
		dependencies.emplace_back(name);
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (expr.GetExpressionType() == ExpressionType::LAMBDA) {
			throw NotImplementedException("Lambda functions are currently not supported in generated columns.");
		}
		InnerGetListOfDependencies((ParsedExpression &)child, dependencies);
	});
}

void ColumnDefinition::GetListOfDependencies(vector<string> &dependencies) const {
	D_ASSERT(Generated());
	InnerGetListOfDependencies(*expression, dependencies);
}

Identifier ColumnDefinition::GetName() const {
	return name;
}

LogicalType ColumnDefinition::GetType() const {
	return type;
}

void ColumnDefinition::SetGeneratedExpression(unique_ptr<ParsedExpression> new_expr) {
	category = TableColumnType::GENERATED;

	if (new_expr->HasSubquery()) {
		throw ParserException("Expression of generated column %s contains a subquery, which isn't allowed", name);
	}

	VerifyColumnRefs(*new_expr);
	if (type.id() == LogicalTypeId::ANY) {
		expression = std::move(new_expr);
		return;
	}
	// Always wrap the expression in a cast, that way we can always update the cast when we change the type
	// Except if the type is LogicalType::ANY (no type specified)
	expression = make_uniq_base<ParsedExpression, CastExpression>(type, std::move(new_expr));
}

void ColumnDefinition::ChangeGeneratedExpressionType(const LogicalType &type) {
	D_ASSERT(Generated());
	// First time the type is set, add a cast around the expression
	D_ASSERT(this->type.id() == LogicalTypeId::ANY);
	expression = make_uniq_base<ParsedExpression, CastExpression>(type, std::move(expression));
	// Every generated expression should be wrapped in a cast on creation
	// D_ASSERT(generated_expression->type == ExpressionType::OPERATOR_CAST);
	// auto &cast_expr = generated_expression->Cast<CastExpression>();
	// auto base_expr = std::move(cast_expr.child);
	// generated_expression = make_uniq_base<ParsedExpression, CastExpression>(type, std::move(base_expr));
}

const ParsedExpression &ColumnDefinition::GeneratedExpression() const {
	D_ASSERT(Generated());
	return *expression;
}

ParsedExpression &ColumnDefinition::GeneratedExpressionMutable() {
	D_ASSERT(Generated());
	return *expression;
}

} // namespace duckdb
