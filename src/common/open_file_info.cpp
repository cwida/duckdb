#include "duckdb/common/open_file_info.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/parser_exception.hpp"

#include <algorithm>

namespace duckdb {

OpenFileInfo OpenFileInfo::FromValue(const Value &input, const Identifier &function_name) {
	if (input.IsNull()) {
		throw ParserException("%s reader cannot take NULL input as parameter", function_name);
	}
	if (input.type().id() == LogicalTypeId::VARCHAR) {
		return OpenFileInfo(StringValue::Get(input));
	}
	if (input.type().id() == LogicalTypeId::VARIANT) {
		// a VARIANT lets every file carry its own set of open options - unpack it to its logical value
		// a variant never unpacks to another variant, so this recurses at most once
		return FromValue(VariantValue::GetValue(input), function_name);
	}
	if (input.type().id() != LogicalTypeId::STRUCT) {
		throw ParserException("%s reader can only take a list of strings, structs or variants as a parameter",
		                      function_name);
	}
	// a file specified as a struct holds the path in the "filename" field - every other field is an open option
	auto &child_types = StructType::GetChildTypes(input.type());
	auto &children = StructValue::GetChildren(input);
	OpenFileInfo result;
	auto extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	bool found_path = false;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		auto &name = child_types[child_idx].first;
		auto &child = children[child_idx];
		if (name == PATH_FIELD) {
			if (child.IsNull() || child.type().id() != LogicalTypeId::VARCHAR) {
				throw ParserException("%s reader requires the \"%s\" field of a file struct to be a non-NULL VARCHAR",
				                      function_name, PATH_FIELD);
			}
			result.path = StringValue::Get(child);
			found_path = true;
			continue;
		}
		if (child.IsNull()) {
			// a NULL option is an option that was not specified - a list of structs is typed by unifying the
			// structs of its entries, which fills the options an entry did not specify with NULL
			continue;
		}
		extended_info->options[name.GetIdentifierName()] = child;
	}
	if (!found_path) {
		throw ParserException("%s reader requires a file struct to have a \"%s\" field holding the path of the file",
		                      function_name, PATH_FIELD);
	}
	result.extended_info = std::move(extended_info);
	return result;
}

Value OpenFileInfo::ToValue() const {
	if (!extended_info || extended_info->options.empty()) {
		return Value(path);
	}
	// the options are kept unordered - sort them, so the same file always becomes the same value
	vector<string> names;
	for (auto &option : extended_info->options) {
		names.push_back(option.first);
	}
	std::sort(names.begin(), names.end());
	child_list_t<Value> children;
	children.emplace_back(PATH_FIELD, Value(path));
	for (auto &name : names) {
		children.emplace_back(name, extended_info->options.at(name));
	}
	return Value::STRUCT(std::move(children));
}

template <>
bool ExtendedOpenFileInfo::TryGetOption(const string &name, bool &result) const {
	auto entry = options.find(name);
	if (entry == options.end()) {
		return false;
	}
	auto &value = entry->second;
	string error_message;
	auto bool_value = value.DefaultTryCastAs(LogicalType::BOOLEAN, &error_message);
	if (!bool_value || bool_value->IsNull()) {
		throw InvalidInputException(
		    "Invalid value for file option \"%s\" - expected a BOOLEAN, but \"%s\" was provided", name,
		    value.ToString());
	}
	result = BooleanValue::Get(*bool_value);
	return true;
}

template <>
bool ExtendedOpenFileInfo::TryGetOption(const string &name, string &result) const {
	auto entry = options.find(name);
	if (entry == options.end()) {
		return false;
	}
	auto &value = entry->second;
	if (value.IsNull()) {
		throw InvalidInputException(
		    "Invalid value for file option \"%s\" - expected a VARCHAR, but \"%s\" was provided", name,
		    value.ToString());
	}
	// VARCHAR and BLOB are read back as-is - casting a BLOB to VARCHAR would escape the bytes it holds
	if (value.type().InternalType() == PhysicalType::VARCHAR) {
		result = StringValue::Get(value);
		return true;
	}
	result = value.ToString();
	return true;
}

template <>
bool ExtendedOpenFileInfo::TryGetOption(const string &name, idx_t &result) const {
	auto entry = options.find(name);
	if (entry == options.end()) {
		return false;
	}
	auto &value = entry->second;
	string error_message;
	auto ubigint_value = value.DefaultTryCastAs(LogicalType::UBIGINT, &error_message);
	if (!ubigint_value || ubigint_value->IsNull()) {
		throw InvalidInputException(
		    "Invalid value for file option \"%s\" - expected a UBIGINT, but \"%s\" was provided", name,
		    value.ToString());
	}
	result = UBigIntValue::Get(*ubigint_value);
	return true;
}

template <>
bool ExtendedOpenFileInfo::TryGetOption(const string &name, timestamp_t &result) const {
	auto entry = options.find(name);
	if (entry == options.end()) {
		return false;
	}
	auto &value = entry->second;
	string error_message;
	auto timestamp_value = value.DefaultTryCastAs(LogicalType::TIMESTAMP, &error_message);
	if (!timestamp_value || timestamp_value->IsNull()) {
		throw InvalidInputException(
		    "Invalid value for file option \"%s\" - expected a TIMESTAMP, but \"%s\" was provided", name,
		    value.ToString());
	}
	result = TimestampValue::Get(*timestamp_value);
	return true;
}

} // namespace duckdb
