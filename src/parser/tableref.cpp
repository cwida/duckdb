#include "duckdb/parser/tableref.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/parser/tableref/list.hpp"

namespace duckdb {

string TableRef::ToString() const {
	return string();
}

bool TableRef::Equals(const TableRef *other) const {
	return other && type == other->type && alias == other->alias &&
	       SampleOptions::Equals(sample.get(), other->sample.get());
}

void TableRef::Serialize(Serializer &serializer) {
	serializer.Write<TableReferenceType>(type);
	serializer.WriteString(alias);
	serializer.WriteOptional(sample);
}

//! Deserializes a blob back into an TableRef
unique_ptr<TableRef> TableRef::Deserialize(Deserializer &source) {
	auto type = source.Read<TableReferenceType>();
	auto alias = source.Read<string>();
	auto sample = source.ReadOptional<SampleOptions>();
	unique_ptr<TableRef> result;
	switch (type) {
	case TableReferenceType::BASE_TABLE:
		result = BaseTableRef::Deserialize(source);
		break;
	case TableReferenceType::CROSS_PRODUCT:
		result = CrossProductRef::Deserialize(source);
		break;
	case TableReferenceType::JOIN:
		result = JoinRef::Deserialize(source);
		break;
	case TableReferenceType::SUBQUERY:
		result = SubqueryRef::Deserialize(source);
		break;
	case TableReferenceType::TABLE_FUNCTION:
		result = TableFunctionRef::Deserialize(source);
		break;
	case TableReferenceType::EMPTY:
		result = EmptyTableRef::Deserialize(source);
		break;
	case TableReferenceType::EXPRESSION_LIST:
		result = ExpressionListRef::Deserialize(source);
		break;
	case TableReferenceType::CTE:
	case TableReferenceType::INVALID:
		throw InternalException("Unsupported type for TableRef::Deserialize");
	}
	result->alias = alias;
	result->sample = move(sample);
	return result;
}

void TableRef::CopyProperties(TableRef &target) const {
	D_ASSERT(type == target.type);
	target.alias = alias;
	target.query_location = query_location;
	target.sample = sample ? sample->Copy() : nullptr;
}

void TableRef::Print() {
	Printer::Print(ToString());
}

} // namespace duckdb
