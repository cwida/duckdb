#include "duckdb/common/arrow/arrow_type_extension.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/function/table/arrow/arrow_type_info.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"

namespace duckdb {

ArrowTypeExtension::ArrowTypeExtension(string extension_name, string arrow_format,
                                       shared_ptr<ArrowTypeExtensionData> type)
    : extension_metadata(std::move(extension_name), {}, {}, std::move(arrow_format)), type_extension(std::move(type)) {
}

ArrowTypeExtension::ArrowTypeExtension(ArrowExtensionMetadata &extension_metadata, unique_ptr<ArrowType> type)
    : extension_metadata(extension_metadata) {
	type_extension = make_shared_ptr<ArrowTypeExtensionData>(type->GetDuckType());
}

ArrowExtensionMetadata::ArrowExtensionMetadata(string extension_name, string vendor_name, string type_name,
                                               string arrow_format)
    : extension_name(std::move(extension_name)), vendor_name(std::move(vendor_name)), type_name(std::move(type_name)),
      arrow_format(std::move(arrow_format)) {
}

hash_t ArrowExtensionMetadata::GetHash() const {
	const auto h_extension = Hash(extension_name.c_str());
	const auto h_vendor = Hash(vendor_name.c_str());
	const auto h_type = Hash(type_name.c_str());
	// Most arrow extensions are unique on the extension name
	// However we use arrow.opaque as all the non-canonical extensions, hence we do a hash-aroo of all.
	return CombineHash(h_extension, CombineHash(h_vendor, h_type));
}

TypeInfo::TypeInfo() : type() {
}

TypeInfo::TypeInfo(const LogicalType &type_p) : alias(type_p.GetAlias()), type(type_p.id()) {
}

TypeInfo::TypeInfo(string alias) : alias(std::move(alias)), type(LogicalTypeId::ANY) {
}

hash_t TypeInfo::GetHash() const {
	const auto h_type_id = Hash(type);
	const auto h_alias = Hash(alias.c_str());
	return CombineHash(h_type_id, h_alias);
}

bool TypeInfo::operator==(const TypeInfo &other) const {
	return alias == other.alias && type == other.type;
}

string ArrowExtensionMetadata::ToString() const {
	std::ostringstream info;
	info << "Extension Name: " << extension_name << "\n";
	if (!vendor_name.empty()) {
		info << "Vendor: " << vendor_name << "\n";
	}
	if (!type_name.empty()) {
		info << "Type: " << type_name << "\n";
	}
	if (!arrow_format.empty()) {
		info << "Format: " << arrow_format << "\n";
	}
	return info.str();
}

string ArrowExtensionMetadata::GetExtensionName() const {
	return extension_name;
}

string ArrowExtensionMetadata::GetVendorName() const {
	return vendor_name;
}

string ArrowExtensionMetadata::GetTypeName() const {
	return type_name;
}

string ArrowExtensionMetadata::GetArrowFormat() const {
	return arrow_format;
}

void ArrowExtensionMetadata::SetArrowFormat(string arrow_format_p) {
	arrow_format = std::move(arrow_format_p);
}

bool ArrowExtensionMetadata::IsCanonical() const {
	D_ASSERT((!vendor_name.empty() && !type_name.empty()) || (vendor_name.empty() && type_name.empty()));
	return vendor_name.empty();
}

bool ArrowExtensionMetadata::operator==(const ArrowExtensionMetadata &other) const {
	return extension_name == other.extension_name && type_name == other.type_name && vendor_name == other.vendor_name;
}

ArrowTypeExtension::ArrowTypeExtension(string vendor_name, string type_name, string arrow_format,
                                       shared_ptr<ArrowTypeExtensionData> type)
    : extension_metadata(ArrowExtensionMetadata::ARROW_EXTENSION_NON_CANONICAL, std::move(vendor_name),
                         std::move(type_name), std::move(arrow_format)),
      type_extension(std::move(type)) {
}

ArrowTypeExtension::ArrowTypeExtension(string extension_name, populate_arrow_schema_t populate_arrow_schema,
                                       get_type_t get_type, shared_ptr<ArrowTypeExtensionData> type)
    : populate_arrow_schema(populate_arrow_schema), get_type(get_type),
      extension_metadata(std::move(extension_name), {}, {}, {}), type_extension(std::move(type)) {
}

ArrowTypeExtension::ArrowTypeExtension(string vendor_name, string type_name,
                                       populate_arrow_schema_t populate_arrow_schema, get_type_t get_type,
                                       shared_ptr<ArrowTypeExtensionData> type, cast_arrow_duck_t arrow_to_duckdb,
                                       cast_duck_arrow_t duckdb_to_arrow)
    : populate_arrow_schema(populate_arrow_schema), get_type(get_type),
      extension_metadata(ArrowExtensionMetadata::ARROW_EXTENSION_NON_CANONICAL, std::move(vendor_name),
                         std::move(type_name), {}),
      type_extension(std::move(type)) {
	type_extension->arrow_to_duckdb = arrow_to_duckdb;
	type_extension->duckdb_to_arrow = duckdb_to_arrow;
}

ArrowExtensionMetadata ArrowTypeExtension::GetInfo() const {
	return extension_metadata;
}

unique_ptr<ArrowType> ArrowTypeExtension::GetType(ClientContext &context, const ArrowSchema &schema,
                                                  const ArrowSchemaMetadata &schema_metadata) const {
	if (get_type) {
		return get_type(context, schema, schema_metadata);
	}
	// FIXME: THis is not good
	auto duckdb_type = type_extension->GetDuckDBType();
	return make_uniq<ArrowType>(duckdb_type);
}

shared_ptr<ArrowTypeExtensionData> ArrowTypeExtension::GetTypeExtension() const {
	return type_extension;
}

LogicalTypeId ArrowTypeExtension::GetLogicalTypeId() const {
	return type_extension->GetDuckDBType().id();
}

LogicalType ArrowTypeExtension::GetLogicalType() const {
	return type_extension->GetDuckDBType();
}

bool ArrowTypeExtension::HasType() const {
	return type_extension.get() != nullptr;
}

void ArrowTypeExtension::PopulateArrowSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &child,
                                             const LogicalType &duckdb_type, ClientContext &context,
                                             const ArrowTypeExtension &extension) {
	if (extension.populate_arrow_schema) {
		extension.populate_arrow_schema(root_holder, child, duckdb_type, context, extension);
		return;
	}

	auto format = make_unsafe_uniq_array<char>(extension.extension_metadata.GetArrowFormat().size() + 1);
	idx_t i = 0;
	for (const auto &c : extension.extension_metadata.GetArrowFormat()) {
		format[i++] = c;
	}
	format[i++] = '\0';
	// We do the default way of populating the schema
	root_holder.extension_format.emplace_back(std::move(format));

	child.format = root_holder.extension_format.back().get();
	ArrowSchemaMetadata schema_metadata;
	if (extension.extension_metadata.IsCanonical()) {
		schema_metadata = ArrowSchemaMetadata::ArrowCanonicalType(extension.extension_metadata.GetExtensionName());
	} else {
		schema_metadata = ArrowSchemaMetadata::NonCanonicalType(extension.extension_metadata.GetTypeName(),
		                                                        extension.extension_metadata.GetVendorName());
	}
	root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
	child.metadata = root_holder.metadata_info.back().get();
}

void DBConfig::RegisterArrowExtension(const ArrowTypeExtension &extension) const {
	lock_guard<mutex> l(arrow_extensions->lock);
	auto extension_info = extension.GetInfo();
	if (arrow_extensions->type_extensions.find(extension_info) != arrow_extensions->type_extensions.end()) {
		throw NotImplementedException("Arrow Extension with configuration %s is already registered",
		                              extension_info.ToString());
	}
	arrow_extensions->type_extensions[extension_info] = extension;
	if (extension.HasType()) {
		const TypeInfo type_info(extension.GetLogicalType());
		arrow_extensions->type_to_info[type_info].push_back(extension_info);
		return;
	}
	const TypeInfo type_info(extension.GetInfo().GetExtensionName());
	arrow_extensions->type_to_info[type_info].push_back(extension_info);
}

ArrowTypeExtension GetArrowExtensionInternal(
    unordered_map<ArrowExtensionMetadata, ArrowTypeExtension, HashArrowTypeExtension> &type_extensions,
    ArrowExtensionMetadata info) {
	if (type_extensions.find(info) == type_extensions.end()) {
		auto og_info = info;
		info.SetArrowFormat("");
		if (type_extensions.find(info) == type_extensions.end()) {
			auto format = og_info.GetArrowFormat();
			auto type = ArrowType::GetTypeFromFormat(format);
			return ArrowTypeExtension(og_info, std::move(type));
		}
	}
	return type_extensions[info];
}
ArrowTypeExtension DBConfig::GetArrowExtension(ArrowExtensionMetadata info) const {
	lock_guard<mutex> l(arrow_extensions->lock);
	return GetArrowExtensionInternal(arrow_extensions->type_extensions, std::move(info));
}

ArrowTypeExtension DBConfig::GetArrowExtension(const LogicalType &type) const {
	lock_guard<mutex> l(arrow_extensions->lock);
	TypeInfo type_info(type);
	if (!arrow_extensions->type_to_info[type_info].empty()) {
		return GetArrowExtensionInternal(arrow_extensions->type_extensions,
		                                 arrow_extensions->type_to_info[type_info].front());
	}
	type_info.type = LogicalTypeId::ANY;
	return GetArrowExtensionInternal(arrow_extensions->type_extensions,
	                                 arrow_extensions->type_to_info[type_info].front());
}

bool DBConfig::HasArrowExtension(const LogicalType &type) const {
	lock_guard<mutex> l(arrow_extensions->lock);
	TypeInfo type_info(type);
	if (!arrow_extensions->type_to_info[type_info].empty()) {
		return true;
	}
	type_info.type = LogicalTypeId::ANY;
	return !arrow_extensions->type_to_info[type_info].empty();
}

bool DBConfig::HasArrowExtension(ArrowExtensionMetadata info) const {
	lock_guard<mutex> l(arrow_extensions->lock);
	auto type_extensions = arrow_extensions->type_extensions;

	if (type_extensions.find(info) != type_extensions.end()) {
		return true;
	}

	auto og_info = info;
	info.SetArrowFormat("");
	if (type_extensions.find(info) != type_extensions.end()) {
		return true;
	}

	return false;
}

struct ArrowJson {
	static unique_ptr<ArrowType> GetType(ClientContext &context, const ArrowSchema &schema,
	                                     const ArrowSchemaMetadata &schema_metadata) {
		const auto format = string(schema.format);
		if (format == "u") {
			return make_uniq<ArrowType>(LogicalType::JSON(), make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL));
		} else if (format == "U") {
			return make_uniq<ArrowType>(LogicalType::JSON(),
			                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE));
		} else if (format == "vu") {
			return make_uniq<ArrowType>(LogicalType::JSON(), make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW));
		}
		throw InvalidInputException("Arrow extension type \"%s\" not supported for arrow.json", format.c_str());
	}

	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		const ArrowSchemaMetadata schema_metadata =
		    ArrowSchemaMetadata::ArrowCanonicalType(extension.GetInfo().GetExtensionName());
		root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
		schema.metadata = root_holder.metadata_info.back().get();
		const auto options = context.GetClientProperties();
		// view layout only when string_view + >= 1.4; declare it to match.
		if (options.produce_arrow_string_view && options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			schema.format = "vu";
		} else {
			if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
				schema.format = "U";
			} else {
				schema.format = "u";
			}
		}
	}
};

struct ArrowBit {
	static unique_ptr<ArrowType> GetType(ClientContext &context, const ArrowSchema &schema,
	                                     const ArrowSchemaMetadata &schema_metadata) {
		const auto format = string(schema.format);
		if (format == "z") {
			return make_uniq<ArrowType>(LogicalType::BIT, make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL));
		} else if (format == "Z") {
			return make_uniq<ArrowType>(LogicalType::BIT,
			                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE));
		} else if (format == "vz") {
			return make_uniq<ArrowType>(LogicalType::BIT, make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW));
		}
		throw InvalidInputException("Arrow extension type \"%s\" not supported for BIT type", format.c_str());
	}

	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		const ArrowSchemaMetadata schema_metadata = ArrowSchemaMetadata::NonCanonicalType(
		    extension.GetInfo().GetTypeName(), extension.GetInfo().GetVendorName());
		root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
		schema.metadata = root_holder.metadata_info.back().get();
		const auto options = context.GetClientProperties();
		if (options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			// >= 1.4 appends the binary view (4-buffer) layout; declare it to match.
			schema.format = "vz";
		} else if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
			schema.format = "Z";
		} else {
			schema.format = "z";
		}
	}
};

struct ArrowBignum {
	static unique_ptr<ArrowType> GetType(ClientContext &context, const ArrowSchema &schema,
	                                     const ArrowSchemaMetadata &schema_metadata) {
		const auto format = string(schema.format);
		if (format == "z") {
			return make_uniq<ArrowType>(LogicalType::BIGNUM, make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL));
		} else if (format == "Z") {
			return make_uniq<ArrowType>(LogicalType::BIGNUM,
			                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE));
		} else if (format == "vz") {
			return make_uniq<ArrowType>(LogicalType::BIGNUM, make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW));
		}
		throw InvalidInputException("Arrow extension type \"%s\" not supported for Bignum", format.c_str());
	}

	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		const ArrowSchemaMetadata schema_metadata = ArrowSchemaMetadata::NonCanonicalType(
		    extension.GetInfo().GetTypeName(), extension.GetInfo().GetVendorName());
		root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
		schema.metadata = root_holder.metadata_info.back().get();
		const auto options = context.GetClientProperties();
		if (options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			// >= 1.4 appends the binary view (4-buffer) layout; declare it to match.
			schema.format = "vz";
		} else if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
			schema.format = "Z";
		} else {
			schema.format = "z";
		}
	}
};

struct ArrowBool8 {
	static void ArrowToDuck(ClientContext &context, Vector &source, Vector &result, idx_t count) {
		auto source_ptr = FlatVector::GetData<int8_t>(source);
		auto result_data = FlatVector::Writer<bool>(result, count);
		for (idx_t i = 0; i < count; i++) {
			result_data.WriteValue(source_ptr[i]);
		}
	}
	static void DuckToArrow(ClientContext &context, const Vector &source, Vector &result, idx_t count) {
		auto entries = source.Values<bool>();
		auto result_data = FlatVector::Writer<int8_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			auto entry = entries[i];
			if (entry.IsValid()) {
				result_data.WriteValue(static_cast<int8_t>(entry.GetValue()));
			} else {
				result_data.WriteNull();
			}
		}
	}
};

struct ArrowGeometry {
	static unique_ptr<ArrowType> GetType(ClientContext &context, const ArrowSchema &schema,
	                                     const ArrowSchemaMetadata &schema_metadata) {
		// Validate extension metadata. This metadata also contains a CRS, which we drop
		// because the GEOMETRY type does not implement a CRS at the type level (yet).
		const auto extension_metadata = schema_metadata.GetOption(ArrowSchemaMetadata::ARROW_METADATA_KEY);

		unique_ptr<CoordinateReferenceSystem> duckdb_crs;

		if (!extension_metadata.empty()) {
			JSONParseError error;
			auto doc = JSONDocument::TryParse(extension_metadata.data(), extension_metadata.size(), error);
			if (!doc) {
				throw SerializationException("Invalid JSON in GeoArrow metadata");
			}

			auto val = doc->GetRoot();
			if (!val.IsObject()) {
				throw SerializationException("Invalid GeoArrow metadata: not a JSON object");
			}

			auto edges = val.GetMember("edges");
			if (edges.IsString() && edges.GetString() != "planar") {
				throw NotImplementedException("Can't import non-planar edges");
			}

			// Pick out the CRS if present
			auto crs = val.GetMember("crs");
			if (crs.IsString()) {
				duckdb_crs = CoordinateReferenceSystem::TryIdentify(context, crs.GetString());
			} else if (crs.IsObject()) {
				// Stringify the object
				duckdb_crs = CoordinateReferenceSystem::TryIdentify(context, crs.ToString(JSONWriteFlags::NONE));
			}
		}

		// Create the geometry type, with or without CRS
		auto geo_type = duckdb_crs ? LogicalType::GEOMETRY(*duckdb_crs) : LogicalType::GEOMETRY();

		const auto format = string(schema.format);
		if (format == "z") {
			return make_uniq<ArrowType>(std::move(geo_type), make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL));
		}
		if (format == "Z") {
			return make_uniq<ArrowType>(std::move(geo_type),
			                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE));
		}
		if (format == "vz") {
			return make_uniq<ArrowType>(std::move(geo_type), make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW));
		}
		throw InvalidInputException("Arrow extension type \"%s\" not supported for geoarrow.wkb", format.c_str());
	}

	static void WriteCRS(JSONWriter &writer, JSONMutableValue &root, const CoordinateReferenceSystem &crs,
	                     ClientContext &context) {
		// Try to convert to preferred formats, in order
		auto converted = CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::PROJJSON);
		if (!converted) {
			converted = CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::WKT2_2019);
		}
		if (!converted) {
			converted = CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::AUTH_CODE);
		}
		if (!converted) {
			converted = CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::SRID);
		}
		if (!converted) {
			converted = nullptr;
		}

		const auto &crs_def = converted ? converted->GetDefinition() : crs.GetDefinition();
		const auto &crs_type = converted ? converted->GetType() : crs.GetType();

		switch (crs_type) {
		case CoordinateReferenceSystemType::PROJJSON: {
			JSONParseError error;
			auto projjson_doc = JSONDocument::TryParse(crs_def.c_str(), crs_def.size(), error);
			if (projjson_doc) {
				root.AddString("crs_type", "projjson");
				root.Add("crs", writer.CreateCopy(projjson_doc->GetRoot()));
			} else {
				throw SerializationException("Could not parse PROJJSON CRS for GeoArrow metadata");
			}
		} break;
		case CoordinateReferenceSystemType::AUTH_CODE: {
			root.AddString("crs_type", "authority_code");
			root.AddString("crs", crs_def);
		} break;
		case CoordinateReferenceSystemType::SRID: {
			root.AddString("crs_type", "srid");
			root.AddString("crs", crs_def);
		} break;
		case CoordinateReferenceSystemType::WKT2_2019: {
			root.AddString("crs_type", "wkt2:2019");
			root.AddString("crs", crs_def);
		} break;
		default:
			throw SerializationException("Could not serialize CRS of type %d for GeoArrow metadata",
			                             static_cast<int>(crs.GetType()));
		}
	}

	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		ArrowSchemaMetadata schema_metadata;

		schema_metadata.AddOption(ArrowSchemaMetadata::ARROW_EXTENSION_NAME, "geoarrow.wkb");

		// Make a CRS entry if the type has a CRS
		JSONWriter writer;
		auto root = writer.CreateObject();
		writer.SetRoot(root);

		if (GeoType::HasCRS(type)) {
			WriteCRS(writer, root, GeoType::GetCRS(type), context);
		}

		schema_metadata.AddOption(ArrowSchemaMetadata::ARROW_METADATA_KEY, writer.ToString(JSONWriteFlags::NONE));

		root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
		schema.metadata = root_holder.metadata_info.back().get();

		const auto options = context.GetClientProperties();
		if (options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			// >= 1.4 appends the binary view (4-buffer) layout; declare it to match.
			schema.format = "vz";
		} else if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
			schema.format = "Z";
		} else {
			schema.format = "z";
		}
	}

	static void ArrowToDuck(ClientContext &, Vector &source, Vector &result, idx_t count) {
		Geometry::FromBinary(source, result, count, true);
	}

	static void DuckToArrow(ClientContext &context, const Vector &source, Vector &result, idx_t count) {
		Geometry::ToBinary(source, result);
	}
};

struct ArrowVariant {
	//! VARIANT travels as the canonical `arrow.parquet.variant` extension: the Variant spec's binary
	//! encoding in a struct<metadata: binary, value: binary> storage type. The values convert through
	//! ParquetVariantConversion's encode/decode directly — never through the binder, since a conversion
	//! can run without a valid transaction (e.g. duckdb_result_arrow_array over a materialized result).

	static LogicalType StorageType() {
		child_list_t<LogicalType> children;
		children.emplace_back("metadata", LogicalType::BLOB);
		children.emplace_back("value", LogicalType::BLOB);
		return LogicalType::STRUCT(std::move(children));
	}

	//! Declares the storage schema: struct<metadata: binary, value: binary>, tagged with the canonical
	//! extension name.
	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		const auto schema_metadata = ArrowSchemaMetadata::ArrowCanonicalType(extension.GetInfo().GetExtensionName());
		root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
		schema.metadata = root_holder.metadata_info.back().get();

		auto release_child = [](ArrowSchema *child) {
			child->release = nullptr;
		};

		schema.format = "+s";
		schema.n_children = 2;
		root_holder.nested_children.emplace_back();
		root_holder.nested_children.back().resize(2);
		root_holder.nested_children_ptr.emplace_back();
		root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
		root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[1]);
		schema.children = root_holder.nested_children_ptr.back().data();

		// The appender picks the binary layout from the session settings — declare the same one, or the
		// schema and the produced buffers disagree (same rule as SetArrowFormat's BLOB case).
		const auto options = context.GetClientProperties();
		const char *binary_format;
		if (options.arrow_output_version >= ArrowFormatVersion::V1_4) {
			binary_format = "vz";
		} else if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
			binary_format = "Z";
		} else {
			binary_format = "z";
		}

		const char *child_names[] = {"metadata", "value"};
		for (idx_t i = 0; i < 2; i++) {
			auto &child = *schema.children[i];
			child.format = binary_format;
			child.name = child_names[i];
			// the spec requires `metadata` non-nullable; `value` stays nullable (it may be absent per row
			// once shredding exists)
			child.flags = i == 0 ? 0 : ARROW_FLAG_NULLABLE;
			child.release = release_child;
		}
	}

	//! Maps a tagged schema back: the VARIANT logical type, with type info describing the storage struct
	//! for the reader's buffer walk. The spec allows the fields in ANY order, in any binary layout
	//! (binary / large binary / view), and dictionary- or run-end-encoded — so the fields are resolved by
	//! NAME and the children are parsed from the actual schema. The resolved order is recorded on a
	//! per-column ArrowTypeExtensionData whose internal type is the storage struct in the schema's OWN
	//! field order (the buffer walk is positional), so the conversion can normalize by name.
	static unique_ptr<ArrowType> GetType(ClientContext &context, const ArrowSchema &schema,
	                                     const ArrowSchemaMetadata &schema_metadata) {
		if (!schema.format || string(schema.format) != "+s") {
			throw InvalidInputException("arrow.parquet.variant column must have a struct storage type, got format '%s'",
			                            schema.format ? schema.format : "(none)");
		}
		idx_t metadata_idx = DConstants::INVALID_INDEX;
		idx_t value_idx = DConstants::INVALID_INDEX;
		for (idx_t i = 0; i < NumericCast<idx_t>(schema.n_children); i++) {
			const string name = schema.children[i]->name ? schema.children[i]->name : "";
			if (name == "metadata" && metadata_idx == DConstants::INVALID_INDEX) {
				metadata_idx = i;
			} else if (name == "value" && value_idx == DConstants::INVALID_INDEX) {
				value_idx = i;
			} else if (name == "typed_value") {
				// TODO: support shredded variants — the value has to be re-assembled from typed_value
				// (and, when both are present, merged with the residual value field).
				throw NotImplementedException(
				    "arrow.parquet.variant column with a 'typed_value' field (a shredded variant) is not supported "
				    "yet");
			} else {
				throw InvalidInputException(
				    "arrow.parquet.variant column has an unexpected or duplicate field '%s' (expected 'metadata' and "
				    "'value')",
				    name);
			}
		}
		if (metadata_idx == DConstants::INVALID_INDEX || value_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException(
			    "arrow.parquet.variant column must have a 'metadata' and a 'value' field, got %lld children",
			    schema.n_children);
		}
		vector<shared_ptr<ArrowType>> children;
		child_list_t<LogicalType> storage_children;
		for (idx_t i = 0; i < NumericCast<idx_t>(schema.n_children); i++) {
			// GetArrowLogicalType, not GetTypeFromSchema: a dictionary-encoded child carries its value
			// type in schema.dictionary, which only this entry point resolves.
			children.push_back(ArrowType::GetArrowLogicalType(context, *schema.children[i]));
			// Both fields are binary by the spec. Without this check a non-binary field would be
			// declared BLOB storage here and then reinterpreted as bytes by the decode.
			const auto child_type = children.back()->GetDuckType(true);
			if (child_type.id() != LogicalTypeId::BLOB) {
				throw InvalidInputException("arrow.parquet.variant field '%s' must be a binary type, got %s",
				                            schema.children[i]->name, child_type.ToString());
			}
			storage_children.emplace_back(schema.children[i]->name, LogicalType::BLOB);
		}
		auto result = make_uniq<ArrowType>(LogicalType::VARIANT(), make_uniq<ArrowStructInfo>(std::move(children)));
		result->extension_data = make_shared_ptr<ArrowTypeExtensionData>(
		    LogicalType::VARIANT(), LogicalType::STRUCT(std::move(storage_children)), ArrowToDuck, DuckToArrow);
		return result;
	}

	//! DuckDB -> Arrow: convert VARIANT values into the storage struct via the Parquet Variant encode
	//! (the result vector declares no typed_value child, so the result is the unshredded
	//! struct<metadata, value>).
	static void DuckToArrow(ClientContext &, const Vector &source, Vector &result, idx_t count) {
		Vector transformed(StorageType(), count);
		ParquetVariantConversion::ToParquetVariant(source, count, transformed);

		// The transform encodes SQL NULL as a Variant-null VALUE; Arrow needs the source's top-level validity.
		UnifiedVectorFormat source_format;
		source.ToUnifiedFormat(source_format);
		bool has_nulls = false;
		for (idx_t i = 0; i < count; i++) {
			if (!source_format.validity.RowIsValid(source_format.sel->get_index(i)) ||
			    FlatVector::IsNull(transformed, i)) {
				FlatVector::SetNull(result, i, true);
				has_nulls = true;
			}
		}

		// Move the encoded data over child-wise so `result` (the extension's declared storage type)
		// shares the encode's buffers where possible.
		auto &result_entries = StructVector::GetEntries(result);
		auto &transformed_entries = StructVector::GetEntries(transformed);
		if (!has_nulls) {
			result_entries[0].Reference(transformed_entries[0]);
			result_entries[1].Reference(transformed_entries[1]);
			return;
		}
		// `metadata` is declared non-nullable, as the spec requires — so a NULL row's child slots carry
		// the minimal valid encoding (v1 empty-dictionary metadata + a Variant null value) instead of the
		// child-level NULLs a strict consumer would reject. Copy into the result's OWN children rather
		// than patching buffers shared with the transform's output.
		static constexpr const char MINIMAL_METADATA[] = "\x01\x00\x00";
		static constexpr const char VARIANT_NULL_VALUE[] = "\x00";
		transformed_entries[0].Flatten();
		transformed_entries[1].Flatten();
		auto src_metadata = FlatVector::GetData<string_t>(transformed_entries[0]);
		auto src_value = FlatVector::GetData<string_t>(transformed_entries[1]);
		auto &metadata_entry = result_entries[0];
		auto &value_entry = result_entries[1];
		auto dst_metadata = FlatVector::GetDataMutable<string_t>(metadata_entry);
		auto dst_value = FlatVector::GetDataMutable<string_t>(value_entry);
		for (idx_t i = 0; i < count; i++) {
			if (FlatVector::IsNull(result, i)) {
				dst_metadata[i] = string_t(MINIMAL_METADATA, 3);
				dst_value[i] = string_t(VARIANT_NULL_VALUE, 1);
			} else {
				dst_metadata[i] = StringVector::AddStringOrBlob(metadata_entry, src_metadata[i]);
				dst_value[i] = StringVector::AddStringOrBlob(value_entry, src_value[i]);
			}
		}
		FlatVector::ValidityMutable(metadata_entry).SetAllValid(count);
		FlatVector::ValidityMutable(value_entry).SetAllValid(count);
	}

	//! Arrow -> DuckDB: concatenate each row's metadata and value bytes (the self-delimiting Variant
	//! binary form) and decode through the Parquet Variant binary decode.
	static void ArrowToDuck(ClientContext &, Vector &source, Vector &result, idx_t count) {
		source.Flatten();
		// The storage vector's type records the incoming schema's own field order (see GetType,
		// which already established that these are exactly the two fields) - so only their order
		// is open here, the spec allowing them either way round.
		auto &source_children = StructType::GetChildTypes(source.GetType());
		const idx_t metadata_idx = source_children[0].first == "metadata" ? 0 : 1;
		const idx_t value_idx = 1 - metadata_idx;
		auto &entries = StructVector::GetEntries(source);
		Vector &metadata = entries[metadata_idx];
		Vector &value = entries[value_idx];
		metadata.Flatten();
		value.Flatten();

		Vector blob(LogicalType::BLOB, count);
		auto blob_data = FlatVector::GetDataMutable<string_t>(blob);
		auto metadata_data = FlatVector::GetData<string_t>(metadata);
		auto value_data = FlatVector::GetData<string_t>(value);
		for (idx_t i = 0; i < count; i++) {
			if (FlatVector::IsNull(source, i) || FlatVector::IsNull(metadata, i) || FlatVector::IsNull(value, i)) {
				// The decode reads the blob's validity and answers a Variant null for an invalid
				// row, so the slot itself is never read.
				FlatVector::SetNull(blob, i, true);
				continue;
			}
			auto &metadata_bytes = metadata_data[i];
			auto &value_bytes = value_data[i];
			auto total_size = metadata_bytes.GetSize() + value_bytes.GetSize();
			auto target = StringVector::EmptyString(blob, total_size);
			auto target_ptr = target.GetDataWriteable();
			memcpy(target_ptr, metadata_bytes.GetData(), metadata_bytes.GetSize());
			memcpy(target_ptr + metadata_bytes.GetSize(), value_bytes.GetData(), value_bytes.GetSize());
			target.Finalize();
			blob_data[i] = target;
		}

		ParquetVariantConversion::ConvertBinary(blob, result, count);
	}
};

void ArrowTypeExtensionSet::Initialize(const DBConfig &config) {
	// Types that are 1:1
	config.RegisterArrowExtension({"arrow.uuid", "w:16", make_shared_ptr<ArrowTypeExtensionData>(LogicalType::UUID)});
	config.RegisterArrowExtension(
	    {"arrow.bool8", "c",
	     make_shared_ptr<ArrowTypeExtensionData>(LogicalType::BOOLEAN, LogicalType::TINYINT, ArrowBool8::ArrowToDuck,
	                                             ArrowBool8::DuckToArrow)});

	config.RegisterArrowExtension(
	    {"DuckDB", "hugeint", "w:16", make_shared_ptr<ArrowTypeExtensionData>(LogicalType::HUGEINT)});
	config.RegisterArrowExtension(
	    {"DuckDB", "uhugeint", "w:16", make_shared_ptr<ArrowTypeExtensionData>(LogicalType::UHUGEINT)});
	config.RegisterArrowExtension(
	    {"DuckDB", "time_tz", "w:8", make_shared_ptr<ArrowTypeExtensionData>(LogicalType::TIME_TZ)});

	config.RegisterArrowExtension(
	    {"geoarrow.wkb", ArrowGeometry::PopulateSchema, ArrowGeometry::GetType,
	     make_shared_ptr<ArrowTypeExtensionData>(LogicalType::GEOMETRY(), LogicalType::BLOB, ArrowGeometry::ArrowToDuck,
	                                             ArrowGeometry::DuckToArrow)});

	// Types that are 1:n
	config.RegisterArrowExtension({"arrow.json", &ArrowJson::PopulateSchema, &ArrowJson::GetType,
	                               make_shared_ptr<ArrowTypeExtensionData>(LogicalType::JSON())});

	config.RegisterArrowExtension({"DuckDB", "bit", &ArrowBit::PopulateSchema, &ArrowBit::GetType,
	                               make_shared_ptr<ArrowTypeExtensionData>(LogicalType::BIT), nullptr, nullptr});

	config.RegisterArrowExtension({"DuckDB", "bignum", &ArrowBignum::PopulateSchema, &ArrowBignum::GetType,
	                               make_shared_ptr<ArrowTypeExtensionData>(LogicalType::BIGNUM), nullptr, nullptr});

	config.RegisterArrowExtension(
	    {"arrow.parquet.variant", &ArrowVariant::PopulateSchema, &ArrowVariant::GetType,
	     make_shared_ptr<ArrowTypeExtensionData>(LogicalType::VARIANT(), ArrowVariant::StorageType(),
	                                             ArrowVariant::ArrowToDuck, ArrowVariant::DuckToArrow)});
}
} // namespace duckdb
