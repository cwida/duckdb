#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/exception.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

void ArrowTableSchema::AddColumn(idx_t index, shared_ptr<ArrowType> type, const string &name) {
	D_ASSERT(arrow_convert_data.find(index) == arrow_convert_data.end());
	if (index >= types.size()) {
		types.resize(index + 1);
		column_names.resize(index + 1);
	}
	types[index] = type->GetDuckType(true);
	column_names[index] = name;
	arrow_convert_data.emplace(std::make_pair(index, std::move(type)));
}

const arrow_column_map_t &ArrowTableSchema::GetColumns() const {
	return arrow_convert_data;
}
vector<LogicalType> &ArrowTableSchema::GetTypes() {
	return types;
}

vector<string> &ArrowTableSchema::GetNames() {
	return column_names;
}
void ArrowType::SetDictionary(unique_ptr<ArrowType> dictionary) {
	D_ASSERT(!this->dictionary_type);
	dictionary_type = std::move(dictionary);
}

bool ArrowType::HasDictionary() const {
	return dictionary_type != nullptr;
}

const ArrowType &ArrowType::GetDictionary() const {
	D_ASSERT(dictionary_type);
	return *dictionary_type;
}

void ArrowType::SetRunEndEncoded() {
	D_ASSERT(type_info);
	D_ASSERT(type_info->type == ArrowTypeInfoType::STRUCT);
	auto &struct_info = type_info->Cast<ArrowStructInfo>();
	D_ASSERT(struct_info.ChildCount() == 2);

	auto actual_type = struct_info.GetChild(1).GetDuckType();
	// Override the duckdb type to the actual type
	type = actual_type;
	run_end_encoded = true;
}

bool ArrowType::RunEndEncoded() const {
	return run_end_encoded;
}

void ArrowType::ThrowIfInvalid() const {
	if (type.id() == LogicalTypeId::INVALID) {
		if (not_implemented) {
			throw NotImplementedException(error_message);
		}
		throw InvalidInputException(error_message);
	}
}

unique_ptr<ArrowType> ArrowType::GetTypeFromFormat(string &format) {
	if (format == "n") {
		return make_uniq<ArrowType>(LogicalType::SQLNULL);
	} else if (format == "b") {
		return make_uniq<ArrowType>(LogicalType::BOOLEAN);
	} else if (format == "c") {
		return make_uniq<ArrowType>(LogicalType::TINYINT);
	} else if (format == "s") {
		return make_uniq<ArrowType>(LogicalType::SMALLINT);
	} else if (format == "i") {
		return make_uniq<ArrowType>(LogicalType::INTEGER);
	} else if (format == "l") {
		return make_uniq<ArrowType>(LogicalType::BIGINT);
	} else if (format == "C") {
		return make_uniq<ArrowType>(LogicalType::UTINYINT);
	} else if (format == "S") {
		return make_uniq<ArrowType>(LogicalType::USMALLINT);
	} else if (format == "I") {
		return make_uniq<ArrowType>(LogicalType::UINTEGER);
	} else if (format == "L") {
		return make_uniq<ArrowType>(LogicalType::UBIGINT);
	} else if (format == "f") {
		return make_uniq<ArrowType>(LogicalType::FLOAT);
	} else if (format == "g") {
		return make_uniq<ArrowType>(LogicalType::DOUBLE);
	} else if (format[0] == 'd') { //! this can be either decimal128 or decimal 256 (e.g., d:38,0)
		auto extra_info = StringUtil::Split(format, ':');
		if (extra_info.size() != 2) {
			throw InvalidInputException(
			    "Decimal format of Arrow object is incomplete, it is missing the scale and width. Current format: %s",
			    format);
		}
		auto parameters = StringUtil::Split(extra_info[1], ",");
		// Parameters must always be 2 or 3 values (i.e., width, scale and an optional bit-width)
		if (parameters.size() != 2 && parameters.size() != 3) {
			throw InvalidInputException(
			    "Decimal format of Arrow object is incomplete, it is missing the scale or width. Current format: %s",
			    format);
		}
		uint64_t width = std::stoull(parameters[0]);
		uint64_t scale = std::stoull(parameters[1]);
		uint64_t bitwidth = 128;
		if (parameters.size() == 3) {
			// We have a bit-width defined
			bitwidth = std::stoull(parameters[2]);
		}
		if (width > 38 || bitwidth > 128) {
			throw NotImplementedException("Unsupported Internal Arrow Type for Decimal %s", format);
		}
		switch (bitwidth) {
		case 32:
			return make_uniq<ArrowType>(LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale)),
			                            make_uniq<ArrowDecimalInfo>(DecimalBitWidth::DECIMAL_32));
		case 64:
			return make_uniq<ArrowType>(LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale)),
			                            make_uniq<ArrowDecimalInfo>(DecimalBitWidth::DECIMAL_64));
		case 128:
			return make_uniq<ArrowType>(LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale)),
			                            make_uniq<ArrowDecimalInfo>(DecimalBitWidth::DECIMAL_128));
		case 256:
			return make_uniq<ArrowType>(LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale)),
			                            make_uniq<ArrowDecimalInfo>(DecimalBitWidth::DECIMAL_256));
		default:
			throw NotImplementedException("Unsupported bit-width value of %d  for Arrow Decimal type", bitwidth);
		}
	} else if (format == "u") {
		return make_uniq<ArrowType>(LogicalType::VARCHAR, make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL));
	} else if (format == "U") {
		return make_uniq<ArrowType>(LogicalType::VARCHAR,
		                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE));
	} else if (format == "vu") {
		return make_uniq<ArrowType>(LogicalType::VARCHAR, make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW));
	} else if (format == "tsn:") {
		return make_uniq<ArrowType>(LogicalTypeId::TIMESTAMP_NS);
	} else if (format == "tsu:") {
		return make_uniq<ArrowType>(LogicalTypeId::TIMESTAMP);
	} else if (format == "tsm:") {
		return make_uniq<ArrowType>(LogicalTypeId::TIMESTAMP_MS);
	} else if (format == "tss:") {
		return make_uniq<ArrowType>(LogicalTypeId::TIMESTAMP_SEC);
	} else if (format == "tdD") {
		return make_uniq<ArrowType>(LogicalType::DATE, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::DAYS));
	} else if (format == "tdm") {
		return make_uniq<ArrowType>(LogicalType::DATE, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MILLISECONDS));
	} else if (format == "tts") {
		return make_uniq<ArrowType>(LogicalType::TIME, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::SECONDS));
	} else if (format == "ttm") {
		return make_uniq<ArrowType>(LogicalType::TIME, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MILLISECONDS));
	} else if (format == "ttu") {
		return make_uniq<ArrowType>(LogicalType::TIME, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MICROSECONDS));
	} else if (format == "ttn") {
		return make_uniq<ArrowType>(LogicalType::TIME_NS, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::NANOSECONDS));
	} else if (format == "tDs") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::SECONDS));
	} else if (format == "tDm") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL,
		                            make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MILLISECONDS));
	} else if (format == "tDu") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL,
		                            make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MICROSECONDS));
	} else if (format == "tDn") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL,
		                            make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::NANOSECONDS));
	} else if (format == "tiD") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::DAYS));
	} else if (format == "tiM") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL, make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MONTHS));
	} else if (format == "tin") {
		return make_uniq<ArrowType>(LogicalType::INTERVAL,
		                            make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MONTH_DAY_NANO));
	} else if (format == "z") {
		auto type_info = make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL);
		return make_uniq<ArrowType>(LogicalType::BLOB, std::move(type_info));
	} else if (format == "Z") {
		auto type_info = make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE);
		return make_uniq<ArrowType>(LogicalType::BLOB, std::move(type_info));
	} else if (format == "vz") {
		auto type_info = make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW);
		return make_uniq<ArrowType>(LogicalType::BLOB, std::move(type_info));
	} else if (format[0] == 'w') {
		// Arrow C Data Interface spec: fixed-size binary is "w:NN", colon always at position 1
		if (format.size() <= 2 || format[1] != ':') {
			throw InvalidInputException("Invalid Arrow fixed-size binary format string: \"%s\"", format);
		}
		string parameters = format.substr(2);
		auto fixed_size = NumericCast<idx_t>(std::stoi(parameters));
		auto type_info = make_uniq<ArrowStringInfo>(fixed_size);
		return make_uniq<ArrowType>(LogicalType::BLOB, std::move(type_info));
	} else if (format[0] == 't' && format[1] == 's') {
		// Timestamp with Timezone
		// TODO right now we just get the UTC value. We probably want to support this properly in the future
		unique_ptr<ArrowTypeInfo> type_info;
		if (format[2] == 'n') {
			type_info = make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::NANOSECONDS);
			return make_uniq<ArrowType>(LogicalType::TIMESTAMP_TZ_NS, std::move(type_info));
		} else if (format[2] == 'u') {
			type_info = make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MICROSECONDS);
		} else if (format[2] == 'm') {
			type_info = make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::MILLISECONDS);
		} else if (format[2] == 's') {
			type_info = make_uniq<ArrowDateTimeInfo>(ArrowDateTimeType::SECONDS);
		} else {
			throw NotImplementedException(" Timestamptz precision of not accepted");
		}
		return make_uniq<ArrowType>(LogicalType::TIMESTAMP_TZ, std::move(type_info));
	}
	return nullptr;
}

unique_ptr<ArrowType> ArrowType::GetTypeFromFormat(ClientContext &context, ArrowSchema &schema, string &format) {
	auto type = GetTypeFromFormat(format);

	if (type) {
		return type;
	}
	if (format == "+l") {
		return CreateListType(context, *schema.children[0], ArrowVariableSizeType::NORMAL, false);
	} else if (format == "+L") {
		return CreateListType(context, *schema.children[0], ArrowVariableSizeType::SUPER_SIZE, false);
	} else if (format == "+vl") {
		return CreateListType(context, *schema.children[0], ArrowVariableSizeType::NORMAL, true);
	} else if (format == "+vL") {
		return CreateListType(context, *schema.children[0], ArrowVariableSizeType::SUPER_SIZE, true);
	} else if (format[0] == '+' && format[1] == 'w') {
		// Arrow C Data Interface spec: fixed-size list is "+w:NN", colon always at position 2
		if (format.size() <= 3 || format[2] != ':') {
			throw InvalidInputException("Invalid Arrow fixed-size list format string: \"%s\"", format);
		}
		std::string parameters = format.substr(3);
		auto fixed_size = NumericCast<idx_t>(std::stoi(parameters));
		auto child_type = GetArrowLogicalType(context, *schema.children[0]);

		auto array_type = LogicalType::ARRAY(child_type->GetDuckType(), fixed_size);
		auto type_info = make_uniq<ArrowArrayInfo>(std::move(child_type), fixed_size);
		return make_uniq<ArrowType>(array_type, std::move(type_info));
	} else if (format == "+s") {
		child_list_t<LogicalType> child_types;
		vector<shared_ptr<ArrowType>> children;
		for (idx_t type_idx = 0; type_idx < static_cast<idx_t>(schema.n_children); type_idx++) {
			children.emplace_back(GetArrowLogicalType(context, *schema.children[type_idx]));
			child_types.emplace_back(schema.children[type_idx]->name, children.back()->GetDuckType());
		}
		auto type_info = make_uniq<ArrowStructInfo>(std::move(children));
		auto struct_type = make_uniq<ArrowType>(LogicalType::STRUCT(std::move(child_types)), std::move(type_info));
		return struct_type;
	} else if (format[0] == '+' && format[1] == 'u') {
		if (format[2] != 's') {
			throw NotImplementedException("Unsupported Internal Arrow Type: \"%c\" Union", format[2]);
		}
		D_ASSERT(format[3] == ':');

		std::string prefix = "+us:";
		// TODO: what are these type ids actually for?
		auto type_ids = StringUtil::Split(format.substr(prefix.size()), ',');

		child_list_t<LogicalType> members;
		vector<shared_ptr<ArrowType>> children;
		if (schema.n_children == 0) {
			throw InvalidInputException("Attempted to convert a UNION with no fields to DuckDB which is not supported");
		}
		for (idx_t type_idx = 0; type_idx < static_cast<idx_t>(schema.n_children); type_idx++) {
			auto type = schema.children[type_idx];

			children.emplace_back(GetArrowLogicalType(context, *type));
			members.emplace_back(type->name, children.back()->GetDuckType());
		}

		auto type_info = make_uniq<ArrowStructInfo>(std::move(children));
		auto union_type = make_uniq<ArrowType>(LogicalType::UNION(members), std::move(type_info));
		return union_type;
	} else if (format == "+r") {
		child_list_t<LogicalType> members;
		vector<shared_ptr<ArrowType>> children;
		idx_t n_children = static_cast<idx_t>(schema.n_children);
		D_ASSERT(n_children == 2);
		D_ASSERT(string(schema.children[0]->name) == "run_ends");
		D_ASSERT(string(schema.children[1]->name) == "values");
		for (idx_t i = 0; i < n_children; i++) {
			auto type = schema.children[i];
			children.emplace_back(GetArrowLogicalType(context, *type));
			members.emplace_back(type->name, children.back()->GetDuckType());
		}

		auto type_info = make_uniq<ArrowStructInfo>(std::move(children));
		auto struct_type = make_uniq<ArrowType>(LogicalType::STRUCT(members), std::move(type_info));
		struct_type->SetRunEndEncoded();
		return struct_type;
	} else if (format == "+m") {
		auto &arrow_struct_type = *schema.children[0];
		D_ASSERT(arrow_struct_type.n_children == 2);
		auto key_type = GetArrowLogicalType(context, *arrow_struct_type.children[0]);
		auto value_type = GetArrowLogicalType(context, *arrow_struct_type.children[1]);
		child_list_t<LogicalType> key_value;
		key_value.emplace_back(std::make_pair("key", key_type->GetDuckType()));
		key_value.emplace_back(std::make_pair("value", value_type->GetDuckType()));

		auto map_type = LogicalType::MAP(key_type->GetDuckType(), value_type->GetDuckType());
		vector<shared_ptr<ArrowType>> children;
		children.reserve(2);
		children.push_back(std::move(key_type));
		children.push_back(std::move(value_type));
		auto inner_struct = make_uniq<ArrowType>(LogicalType::STRUCT(std::move(key_value)),
		                                         make_uniq<ArrowStructInfo>(std::move(children)));
		auto map_type_info = ArrowListInfo::List(std::move(inner_struct), ArrowVariableSizeType::NORMAL);
		return make_uniq<ArrowType>(map_type, std::move(map_type_info));
	}
	throw NotImplementedException("Unsupported Internal Arrow Type %s", format);
}

unique_ptr<ArrowType> ArrowType::CreateListType(ClientContext &context, ArrowSchema &child,
                                                ArrowVariableSizeType size_type, bool view) {
	auto child_type = GetArrowLogicalType(context, child);

	unique_ptr<ArrowTypeInfo> type_info;
	auto type = LogicalType::LIST(child_type->GetDuckType());
	if (view) {
		type_info = ArrowListInfo::ListView(std::move(child_type), size_type);
	} else {
		type_info = ArrowListInfo::List(std::move(child_type), size_type);
	}
	return make_uniq<ArrowType>(type, std::move(type_info));
}

LogicalType ArrowType::GetDuckType(bool use_dictionary) const {
	if (use_dictionary && dictionary_type) {
		return dictionary_type->GetDuckType();
	}
	if (!use_dictionary) {
		return type;
	}
	if (HasExtension()) {
		// A registered extension type's type info describes the extension's STORAGE tree, which need not
		// mirror the logical type's children — reconstructing through it would produce a hybrid type.
		// The logical type is authoritative here; any dictionaries inside the storage are handled by the
		// storage-side conversion, not by the output type.
		return type;
	}
	// Dictionaries can exist in arbitrarily nested schemas
	// have to reconstruct the type
	auto id = type.id();
	switch (id) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto &struct_info = type_info->Cast<ArrowStructInfo>();
		child_list_t<LogicalType> new_children;
		for (idx_t i = 0; i < struct_info.ChildCount(); i++) {
			auto &child = struct_info.GetChild(i);
			auto &child_name = StructType::GetChildName(type, i);
			new_children.emplace_back(std::make_pair(child_name, child.GetDuckType(true)));
		}
		if (id == LogicalTypeId::TUPLE) {
			return LogicalType::TUPLE(std::move(new_children));
		}
		return LogicalType::STRUCT(std::move(new_children));
	}
	case LogicalTypeId::LIST: {
		auto &list_info = type_info->Cast<ArrowListInfo>();
		auto &child = list_info.GetChild();
		return LogicalType::LIST(child.GetDuckType(true));
	}
	case LogicalTypeId::MAP: {
		auto &list_info = type_info->Cast<ArrowListInfo>();
		auto &struct_child = list_info.GetChild();
		auto struct_type = struct_child.GetDuckType(true);
		return LogicalType::MAP(StructType::GetChildType(struct_type, 0), StructType::GetChildType(struct_type, 1));
	}
	case LogicalTypeId::UNION: {
		auto &union_info = type_info->Cast<ArrowStructInfo>();
		child_list_t<LogicalType> new_children;
		for (idx_t i = 0; i < union_info.ChildCount(); i++) {
			auto &child = union_info.GetChild(i);
			auto &child_name = UnionType::GetMemberName(type, i);
			new_children.emplace_back(std::make_pair(child_name, child.GetDuckType(true)));
		}
		return LogicalType::UNION(std::move(new_children));
	}
	default: {
		return type;
	}
	}
}

unique_ptr<ArrowType> ArrowType::GetArrowLogicalType(ClientContext &context, ArrowSchema &schema) {
	auto arrow_type = ArrowType::GetTypeFromSchema(context, schema);
	if (schema.dictionary) {
		auto dictionary = GetArrowLogicalType(context, *schema.dictionary);
		arrow_type->SetDictionary(std::move(dictionary));
	}
	return arrow_type;
}

bool ArrowType::HasExtension() const {
	return extension_data.get() != nullptr;
}

ArrowArrayPhysicalType ArrowType::GetPhysicalType() const {
	if (HasDictionary()) {
		return ArrowArrayPhysicalType::DICTIONARY_ENCODED;
	}
	if (RunEndEncoded()) {
		return ArrowArrayPhysicalType::RUN_END_ENCODED;
	}
	return ArrowArrayPhysicalType::DEFAULT;
}

unique_ptr<ArrowType> ArrowType::GetTypeFromSchema(ClientContext &context, ArrowSchema &schema) {
	auto format = string(schema.format);
	// Let's first figure out if this type is an extension type
	ArrowSchemaMetadata schema_metadata(schema.metadata);
	auto &config = DBConfig::GetConfig(context);
	auto arrow_type = GetTypeFromFormat(context, schema, format);
	if (schema_metadata.HasExtension()) {
		auto extension_info = schema_metadata.GetExtensionInfo(string(format));
		if (config.HasArrowExtension(extension_info)) {
			auto extension = config.GetArrowExtension(extension_info);
			arrow_type = extension.GetType(context, schema, schema_metadata);
			if (!arrow_type->extension_data) {
				// GetType may have attached per-column extension data (e.g. to record the incoming
				// schema's field order); fall back to the registered instance otherwise
				arrow_type->extension_data = extension.GetTypeExtension();
			}
		}
	}

	return arrow_type;
}

LogicalType ArrowTypeExtensionData::GetInternalType() const {
	return internal_type;
}

unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>>
ArrowTypeExtensionData::GetExtensionTypes(ClientContext &context, const vector<LogicalType> &duckdb_types) {
	unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_types;
	const auto &db_config = DBConfig::GetConfig(context);
	for (idx_t i = 0; i < duckdb_types.size(); i++) {
		if (db_config.HasArrowExtension(duckdb_types[i])) {
			extension_types.insert({i, db_config.GetArrowExtension(duckdb_types[i]).GetTypeExtension()});
		}
	}
	return extension_types;
}

LogicalType ArrowTypeExtensionData::GetDuckDBType() const {
	return duckdb_type;
}

} // namespace duckdb
