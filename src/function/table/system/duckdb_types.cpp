#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

struct DuckDBTypesData : public GlobalTableFunctionState {
	DuckDBTypesData() : offset(0), constructor_offset(0) {
	}

	vector<reference<TypeCatalogEntry>> entries;
	idx_t offset;
	//! Every type emits one row per constructor overload, like duckdb_functions() does for functions
	idx_t constructor_offset;
	unordered_set<int64_t> oids;
};

static unique_ptr<FunctionData> DuckDBTypesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("type_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("type_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("type_size");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("logical_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	// NUMERIC, STRING, DATETIME, BOOLEAN, COMPOSITE, USER
	names.emplace_back("type_category");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("extension_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("labels");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("parameters");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("parameter_types");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("varargs");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBTypesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTypesData>();
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TYPE_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry.Cast<TypeCatalogEntry>()); });
	};
	return std::move(result);
}

void DuckDBTypesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBTypesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// database_name, VARCHAR
	auto &database_name = output.data[0];
	// database_oid, BIGINT
	auto &database_oid = output.data[1];
	// schema_name, VARCHAR
	auto &schema_name = output.data[2];
	// schema_oid, BIGINT
	auto &schema_oid = output.data[3];
	// type_oid, BIGINT
	auto &type_oid = output.data[4];
	// type_name, VARCHAR
	auto &type_name = output.data[5];
	// type_size, BIGINT
	auto &type_size = output.data[6];
	// logical_type, VARCHAR
	auto &logical_type = output.data[7];
	// type_category, VARCHAR
	auto &type_category = output.data[8];
	// comment, VARCHAR
	auto &comment = output.data[9];
	// tags, MAP
	auto &tags = output.data[10];
	// internal, BOOLEAN
	auto &internal = output.data[11];
	// extension_name, VARCHAR
	auto &extension_name = output.data[12];
	// labels, VARCHAR[]
	auto &labels_vec = output.data[13];
	// parameters, VARCHAR[]
	auto &parameters = output.data[14];
	// parameter_types, VARCHAR[]
	auto &parameter_types = output.data[15];
	// varargs, VARCHAR
	auto &varargs = output.data[16];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &type_entry = data.entries[data.offset].get();
		auto &type = type_entry.user_type;
		auto &constructor = *type_entry.constructors.GetFunctionByOffset(data.constructor_offset++);
		if (data.constructor_offset >= type_entry.constructors.functions.size()) {
			data.offset++;
			data.constructor_offset = 0;
		}

		database_name.Append(Value(type_entry.catalog.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(type_entry.catalog.GetOid())));
		schema_name.Append(Value(type_entry.schema.name));
		schema_oid.Append(Value::BIGINT(NumericCast<int64_t>(type_entry.schema.oid)));
		int64_t oid;
		if (type_entry.internal) {
			oid = NumericCast<int64_t>(type.id());
		} else {
			oid = NumericCast<int64_t>(type_entry.oid);
		}
		Value oid_val;
		if (data.oids.find(oid) == data.oids.end()) {
			data.oids.insert(oid);
			oid_val = Value::BIGINT(oid);
		} else {
			oid_val = Value();
		}
		type_oid.Append(oid_val);
		type_name.Append(Value(type_entry.name));
		auto internal_type = type.InternalType();
		type_size.Append(internal_type == PhysicalType::INVALID
		                     ? Value()
		                     : Value::BIGINT(NumericCast<int64_t>(GetTypeIdSize(internal_type))));
		logical_type.Append(Value(EnumUtil::ToString(type.id())));
		string category;
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
		case LogicalTypeId::HUGEINT:
		case LogicalTypeId::UHUGEINT:
			category = "NUMERIC";
			break;
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIME_NS:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::INTERVAL:
		case LogicalTypeId::TIME_TZ:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			category = "DATETIME";
			break;
		case LogicalTypeId::CHAR:
		case LogicalTypeId::VARCHAR:
			category = "STRING";
			break;
		case LogicalTypeId::BOOLEAN:
			category = "BOOLEAN";
			break;
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::LIST:
		case LogicalTypeId::ARRAY:
		case LogicalTypeId::MAP:
		case LogicalTypeId::UNION:
			category = "COMPOSITE";
			break;
		case LogicalTypeId::VARIANT:
			category = "VARIANT";
			break;
		default:
			break;
		}
		type_category.Append(category.empty() ? Value() : Value(category));
		comment.Append(Value(type_entry.comment));
		tags.Append(Value::MAP(type_entry.tags));
		internal.Append(Value::BOOLEAN(type_entry.internal));
		extension_name.Append(type_entry.extension_name.empty() ? Value() : Value(type_entry.extension_name));
		if (type.id() == LogicalTypeId::ENUM && type.HasParameters()) {
			auto enum_data = FlatVector::GetData<string_t>(EnumType::GetValuesInsertOrder(type));
			idx_t size = EnumType::GetSize(type);

			vector<Value> labels;
			for (idx_t i = 0; i < size; i++) {
				labels.emplace_back(enum_data[i]);
			}

			labels_vec.Append(Value::LIST(LogicalType::VARCHAR, labels));
		} else {
			labels_vec.Append(Value());
		}

		auto &signature = constructor.GetSignature();
		vector<Value> parameter_names;
		vector<Value> parameter_type_names;
		for (auto &param : signature.GetParameters()) {
			if (param.IsVariadic()) {
				continue;
			}
			parameter_names.emplace_back(param.GetName());
			parameter_type_names.emplace_back(param.GetType().ToString());
		}
		parameters.Append(Value::LIST(LogicalType::VARCHAR, std::move(parameter_names)));
		parameter_types.Append(Value::LIST(LogicalType::VARCHAR, std::move(parameter_type_names)));
		auto variadic = signature.GetArgs();
		if (!variadic) {
			variadic = signature.GetKwargs();
		}
		// the legacy "varargs" column cannot tell "*args" and "**kwargs" apart
		varargs.Append(variadic ? Value(variadic->GetType().ToString()) : Value());

		count++;
	}
}

void DuckDBTypesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_types", {}, DuckDBTypesFunction, DuckDBTypesBind, DuckDBTypesInit));
}

} // namespace duckdb
