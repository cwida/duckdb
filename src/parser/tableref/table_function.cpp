#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/serializer.hpp"

namespace duckdb {

string TableFunctionRef::ToString() const {
	return function->ToString();
}

bool TableFunctionRef::Equals(const TableRef *other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto other = (TableFunctionRef *)other_p;
	return function->Equals(other->function.get());
}

void TableFunctionRef::Serialize(Serializer &serializer) {
	TableRef::Serialize(serializer);
	function->Serialize(serializer);
	serializer.WriteString(alias);
	serializer.WriteStringVector(column_name_alias);
}

unique_ptr<TableRef> TableFunctionRef::Deserialize(Deserializer &source) {
	auto result = make_unique<TableFunctionRef>();

	result->function = ParsedExpression::Deserialize(source);
	result->alias = source.Read<string>();
	source.ReadStringVector(result->column_name_alias);

	return move(result);
}

unique_ptr<TableRef> TableFunctionRef::Copy() {
	auto copy = make_unique<TableFunctionRef>();

	copy->function = function->Copy();
	copy->column_name_alias = column_name_alias;
	CopyProperties(*copy);

	return move(copy);
}

} // namespace duckdb
