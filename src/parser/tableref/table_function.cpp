#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

TableFunctionRef::TableFunctionRef() : TableRef(TableReferenceType::TABLE_FUNCTION) {
}

string TableFunctionRef::ToString() const {
	auto result = function->ToString();
	if (with_ordinality == OrdinalityType::WITH_ORDINALITY) {
		result += " WITH ORDINALITY";
	}
	return BaseToString(result, column_name_alias);
}

bool TableFunctionRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<TableFunctionRef>();
	return bind_info == other.bind_info && function->Equals(*other.function);
}

const unique_ptr<ParsedExpression> &TableFunctionRef::SerializableFunction() const {
	if (bind_info) {
		throw NotImplementedException("Cannot serialize a table function with process-local bind input");
	}
	return function;
}

unique_ptr<TableRef> TableFunctionRef::Copy() {
	auto copy = make_uniq<TableFunctionRef>();

	copy->function = function->Copy();
	copy->bind_info = bind_info;
	copy->column_name_alias = column_name_alias;
	copy->with_ordinality = with_ordinality;
	CopyProperties(*copy);

	return std::move(copy);
}

} // namespace duckdb
