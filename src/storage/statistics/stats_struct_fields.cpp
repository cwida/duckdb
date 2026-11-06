#include "duckdb/storage/statistics/stats_struct_fields.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

Value StatsStructFields::Plain(const Value &value) {
	if (value.type().id() != LogicalTypeId::VARIANT || value.IsNull()) {
		return value;
	}
	return VariantValue::GetValue(value);
}

StatsStructFields::StatsStructFields(const Value &input, string path_p) : value(Plain(input)), path(std::move(path_p)) {
	if (value.IsNull()) {
		Throw("expected a struct of statistics, got NULL");
	}
	if (value.type().id() != LogicalTypeId::STRUCT) {
		Throw(StringUtil::Format("expected a struct of statistics, got a value of type %s", value.type().ToString()));
	}
	read.resize(StructType::GetChildCount(value.type()), false);
}

optional_ptr<const Value> StatsStructFields::Get(const string &name) {
	auto &child_types = StructType::GetChildTypes(value.type());
	for (idx_t i = 0; i < child_types.size(); i++) {
		if (child_types[i].first == name) {
			return FieldValue(i);
		}
	}
	return nullptr;
}

bool StatsStructFields::TryGet(const string &name, const LogicalType &type, Value &result) {
	auto field = Get(name);
	if (!field) {
		return false;
	}
	auto plain = Plain(*field);
	string error;
	auto cast = plain.DefaultTryCastAs(type, &error, true);
	if (!cast) {
		Throw(name, StringUtil::Format("the value '%s' of type %s does not convert to %s", plain.ToString(),
		                               plain.type().ToString(), type.ToString()));
	}
	result = std::move(*cast);
	return true;
}

idx_t StatsStructFields::FieldCount() const {
	return read.size();
}

const string &StatsStructFields::FieldName(idx_t index) const {
	return StructType::GetChildTypes(value.type())[index].first.GetIdentifierName();
}

optional_ptr<const Value> StatsStructFields::FieldValue(idx_t index) {
	read[index] = true;
	auto &child = StructValue::GetChildren(value)[index];
	if (child.IsNull()) {
		return nullptr;
	}
	return child;
}

LogicalType StatsStructFields::FieldType(idx_t index) const {
	return Plain(StructValue::GetChildren(value)[index]).type();
}

const string &StatsStructFields::Path() const {
	return path;
}

string StatsStructFields::FieldPath(const string &name) const {
	return path.empty() ? name : path + "." + name;
}

void StatsStructFields::Throw(const string &problem) const {
	if (path.empty()) {
		throw InvalidInputException("statistics: %s", problem);
	}
	throw InvalidInputException("statistics field '%s': %s", path, problem);
}

void StatsStructFields::Throw(const string &name, const string &problem) const {
	throw InvalidInputException("statistics field '%s': %s", FieldPath(name), problem);
}

void StatsStructFields::VerifyAllRead(const LogicalType &type) const {
	for (idx_t i = 0; i < read.size(); i++) {
		if (!read[i]) {
			Throw(FieldName(i), StringUtil::Format("statistics of type %s have no such part", type.ToString()));
		}
	}
}

} // namespace duckdb
