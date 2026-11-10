#include "duckdb/parser/tableref/delimgetref.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

string DelimGetRef::ToString() const {
	return "";
}

bool DelimGetRef::Equals(const TableRef &other) const {
	return TableRef::Equals(other);
}

unique_ptr<TableRef> DelimGetRef::Copy() {
	auto copy = make_uniq<DelimGetRef>(types);
	CopyProperties(*copy);
	return std::move(copy);
}

void DelimGetRef::Serialize(Serializer &serializer) const {
	TableRef::Serialize(serializer);
	serializer.WriteProperty<vector<LogicalType>>(105, "chunk_types", types);
}

unique_ptr<TableRef> DelimGetRef::Deserialize(Deserializer &deserializer) {
	vector<LogicalType> types = deserializer.ReadProperty<vector<LogicalType>>(105, "chunk_types");
	auto result = duckdb::unique_ptr<DelimGetRef>(new DelimGetRef(types));
	return std::move(result);
}

} // namespace duckdb
