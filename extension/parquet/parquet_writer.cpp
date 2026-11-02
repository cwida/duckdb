#include "parquet_writer.hpp"

#include <functional>
#include <limits>
#include <vector>

#include "parquet_crypto.hpp"
#include "parquet_decimal_utils.hpp"
#include "parquet_shredding.hpp"
#include "parquet_timestamp.hpp"
#include "resizable_buffer.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "writer/variant_column_writer.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/types/column/column_data_collection_iterators.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/original/std/memory.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_geometry.hpp"
#include "parquet_statistics.hpp"
#include "thrift/TBase.h"
#include "thrift/protocol/TCompactProtocol.h"
#include "thrift/protocol/TProtocol.h"
#include "thrift/protocol/TVirtualProtocol.h"
#include "thrift/transport/TTransport.h"

namespace duckdb {

using namespace duckdb_apache::thrift;            // NOLINT
using namespace duckdb_apache::thrift::protocol;  // NOLINT
using namespace duckdb_apache::thrift::transport; // NOLINT

using duckdb_parquet::CompressionCodec;
using duckdb_parquet::ConvertedType;
using duckdb_parquet::Encoding;
using duckdb_parquet::FieldRepetitionType;
using duckdb_parquet::FileCryptoMetaData;
using duckdb_parquet::FileMetaData;
using duckdb_parquet::PageHeader;
using duckdb_parquet::PageType;
using ParquetRowGroup = duckdb_parquet::RowGroup;
using duckdb_parquet::Type;

class MyTransport : public TTransport {
public:
	explicit MyTransport(WriteStream &serializer) : serializer(serializer) {
	}

	bool isOpen() const override {
		return true;
	}

	void open() override {
	}

	void close() override {
	}

	void write_virt(const uint8_t *buf, uint32_t len) override {
		serializer.WriteData(const_data_ptr_cast(buf), len);
	}

private:
	WriteStream &serializer;
};

class ParquetPreparedWriteBuffer : public AsyncWriteBuffer {
public:
	explicit ParquetPreparedWriteBuffer(unique_ptr<MemoryStream> stream_p) : stream(std::move(stream_p)) {
		D_ASSERT(stream);
		data = stream->GetData();
		size = stream->GetPosition();
	}

	data_ptr_t Ptr() override {
		return data;
	}

	idx_t Size() const override {
		return size;
	}

private:
	unique_ptr<MemoryStream> stream;
	data_ptr_t data;
	idx_t size;
};

bool ParquetWriter::TryGetParquetType(const LogicalType &duckdb_type, optional_ptr<Type::type> parquet_type_ptr,
                                      bool write_timestamp_as_int96) {
	Type::type parquet_type;
	switch (duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		parquet_type = Type::BOOLEAN;
		break;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::DATE:
		parquet_type = Type::INT32;
		break;
	case LogicalTypeId::BIGINT:
		parquet_type = Type::INT64;
		break;
	case LogicalTypeId::FLOAT:
		parquet_type = Type::FLOAT;
		break;
	case LogicalTypeId::DOUBLE:
		parquet_type = Type::DOUBLE;
		break;
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::HUGEINT:
		parquet_type = Type::DOUBLE;
		break;
	case LogicalTypeId::ENUM:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::GEOMETRY:
		parquet_type = Type::BYTE_ARRAY;
		break;
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
		parquet_type = Type::INT64;
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		parquet_type = write_timestamp_as_int96 ? Type::INT96 : Type::INT64;
		break;
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		parquet_type = Type::INT32;
		break;
	case LogicalTypeId::UBIGINT:
		parquet_type = Type::INT64;
		break;
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID:
		parquet_type = Type::FIXED_LEN_BYTE_ARRAY;
		break;
	case LogicalTypeId::SQLNULL:
		parquet_type = Type::INT32;
		break;
	case LogicalTypeId::DECIMAL:
		switch (duckdb_type.InternalType()) {
		case PhysicalType::INT16:
		case PhysicalType::INT32:
			parquet_type = Type::INT32;
			break;
		case PhysicalType::INT64:
			parquet_type = Type::INT64;
			break;
		case PhysicalType::INT128:
			parquet_type = Type::FIXED_LEN_BYTE_ARRAY;
			break;
		default:
			throw InternalException("Unsupported internal decimal type");
		}
		break;
	default:
		// Anything that is not supported
		return false;
	}
	if (parquet_type_ptr) {
		*parquet_type_ptr = parquet_type;
	}
	return true;
}

Type::type ParquetWriter::DuckDBTypeToParquetType(const LogicalType &duckdb_type, bool write_timestamp_as_int96) {
	Type::type result;
	if (TryGetParquetType(duckdb_type, &result, write_timestamp_as_int96)) {
		return result;
	}
	throw NotImplementedException("Unimplemented type for Parquet \"%s\"", duckdb_type.ToString());
}

static bool GetTimestampIsAdjustedToUTC(const LogicalTypeId type_id, const TimeStampIsAdjustedToUTC setting) {
	switch (setting) {
	case TimeStampIsAdjustedToUTC::AUTO:
		return type_id == LogicalTypeId::TIMESTAMP_TZ || type_id == LogicalTypeId::TIMESTAMP_TZ_NS;
	case TimeStampIsAdjustedToUTC::ALWAYS_TRUE:
		return true;
	case TimeStampIsAdjustedToUTC::ALWAYS_FALSE:
		return false;
	default:
		throw InternalException("Unhandled TimeStampIsAdjustedToUTC setting");
	}
}

void ParquetWriter::SetSchemaProperties(const LogicalType &duckdb_type, duckdb_parquet::SchemaElement &schema_ele,
                                        bool allow_geometry, ClientContext &context, bool write_timestamp_as_int96,
                                        TimeStampIsAdjustedToUTC timestamp_is_adjusted_to_utc) {
	if (duckdb_type.IsJSONType()) {
		schema_ele.converted_type = ConvertedType::JSON;
		schema_ele.__isset.converted_type = true;
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__set_JSON(duckdb_parquet::JsonType());
		return;
	}
	switch (duckdb_type.id()) {
	case LogicalTypeId::TINYINT:
		schema_ele.converted_type = ConvertedType::INT_8;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::SMALLINT:
		schema_ele.converted_type = ConvertedType::INT_16;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::INTEGER:
		schema_ele.converted_type = ConvertedType::INT_32;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::BIGINT:
		schema_ele.converted_type = ConvertedType::INT_64;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::UTINYINT:
		schema_ele.converted_type = ConvertedType::UINT_8;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::USMALLINT:
		schema_ele.converted_type = ConvertedType::UINT_16;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::UINTEGER:
		schema_ele.converted_type = ConvertedType::UINT_32;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::UBIGINT:
		schema_ele.converted_type = ConvertedType::UINT_64;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::DATE:
		schema_ele.converted_type = ConvertedType::DATE;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIME:
		schema_ele.converted_type = ConvertedType::TIME_MICROS;
		schema_ele.__isset.converted_type = true;
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.TIME = true;
		schema_ele.logicalType.TIME.isAdjustedToUTC = (duckdb_type.id() == LogicalTypeId::TIME_TZ);
		schema_ele.logicalType.TIME.unit.__isset.MICROS = true;
		break;
	case LogicalTypeId::TIME_NS:
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.TIME = true;
		schema_ele.logicalType.TIME.isAdjustedToUTC = false;
		schema_ele.logicalType.TIME.unit.__isset.NANOS = true;
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
		if (write_timestamp_as_int96) {
			return;
		}
		schema_ele.converted_type = ConvertedType::TIMESTAMP_MICROS;
		schema_ele.__isset.converted_type = true;
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.TIMESTAMP = true;
		schema_ele.logicalType.TIMESTAMP.isAdjustedToUTC =
		    GetTimestampIsAdjustedToUTC(duckdb_type.id(), timestamp_is_adjusted_to_utc);
		schema_ele.logicalType.TIMESTAMP.unit.__isset.MICROS = true;
		break;
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		if (write_timestamp_as_int96) {
			return;
		}
		schema_ele.__isset.converted_type = false;
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.TIMESTAMP = true;
		schema_ele.logicalType.TIMESTAMP.isAdjustedToUTC =
		    GetTimestampIsAdjustedToUTC(duckdb_type.id(), timestamp_is_adjusted_to_utc);
		schema_ele.logicalType.TIMESTAMP.unit.__isset.NANOS = true;
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		if (write_timestamp_as_int96) {
			return;
		}
		schema_ele.converted_type = ConvertedType::TIMESTAMP_MILLIS;
		schema_ele.__isset.converted_type = true;
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.TIMESTAMP = true;
		schema_ele.logicalType.TIMESTAMP.isAdjustedToUTC =
		    GetTimestampIsAdjustedToUTC(duckdb_type.id(), timestamp_is_adjusted_to_utc);
		schema_ele.logicalType.TIMESTAMP.unit.__isset.MILLIS = true;
		break;
	case LogicalTypeId::ENUM:
	case LogicalTypeId::VARCHAR:
		schema_ele.converted_type = ConvertedType::UTF8;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::INTERVAL:
		schema_ele.type_length = 12;
		schema_ele.converted_type = ConvertedType::INTERVAL;
		schema_ele.__isset.type_length = true;
		schema_ele.__isset.converted_type = true;
		break;
	case LogicalTypeId::UUID:
		schema_ele.type_length = 16;
		schema_ele.__isset.type_length = true;
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.UUID = true;
		break;
	case LogicalTypeId::DECIMAL:
		schema_ele.converted_type = ConvertedType::DECIMAL;
		schema_ele.precision = DecimalType::GetWidth(duckdb_type);
		schema_ele.scale = DecimalType::GetScale(duckdb_type);
		schema_ele.__isset.converted_type = true;
		schema_ele.__isset.precision = true;
		schema_ele.__isset.scale = true;
		if (duckdb_type.InternalType() == PhysicalType::INT128) {
			schema_ele.type_length = 16;
			schema_ele.__isset.type_length = true;
		}
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__isset.DECIMAL = true;
		schema_ele.logicalType.DECIMAL.precision = schema_ele.precision;
		schema_ele.logicalType.DECIMAL.scale = schema_ele.scale;
		break;
	case LogicalTypeId::GEOMETRY:
		if (allow_geometry) { // Don't set this if we write GeoParquet V1
			schema_ele.__isset.logicalType = true;
			schema_ele.logicalType.__isset.GEOMETRY = true;
			if (GeoType::HasCRS(duckdb_type)) {
				const auto &crs = GeoType::GetCRS(duckdb_type);

				if (crs.GetType() != CoordinateReferenceSystemType::PROJJSON) {
					// Try to convert to GeoJSON
					const auto lookup =
					    CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::PROJJSON);
					if (lookup) {
						schema_ele.logicalType.GEOMETRY.__isset.crs = true;
						schema_ele.logicalType.GEOMETRY.crs = lookup->GetDefinition();
						break;
					}
				}

				schema_ele.logicalType.GEOMETRY.__isset.crs = true;
				schema_ele.logicalType.GEOMETRY.crs = crs.GetDefinition();
			}
		}
		break;
	case LogicalTypeId::SQLNULL:
		schema_ele.__isset.logicalType = true;
		schema_ele.logicalType.__set_UNKNOWN(duckdb_parquet::NullType());
		break;
	default:
		break;
	}
}

uint32_t ParquetWriter::Write(const duckdb_apache::thrift::TBase &object) {
	if (options.encryption_config) {
		return ParquetCrypto::Write(object, *protocol, options.encryption_config->GetFooterKey(), *encryption_util);
	} else {
		return object.write(protocol.get());
	}
}

uint32_t ParquetWriter::WriteData(const const_data_ptr_t buffer, const uint32_t buffer_size) {
	if (options.encryption_config) {
		return ParquetCrypto::WriteData(*protocol, buffer, buffer_size, options.encryption_config->GetFooterKey(),
		                                *encryption_util);
	} else {
		protocol->getTransport()->write(buffer, buffer_size);
		return buffer_size;
	}
}

unique_ptr<AsyncWriteBuffer> ParquetWriter::PrepareWrite(const duckdb_apache::thrift::TBase &object) {
	auto stream = make_uniq<MemoryStream>(BufferAllocator::Get(context));
	TCompactProtocolFactoryT<MyTransport> tproto_factory;
	auto stream_protocol = tproto_factory.getProtocol(duckdb_base_std::make_shared<MyTransport>(*stream));
	if (options.encryption_config) {
		ParquetCrypto::Write(object, *stream_protocol, options.encryption_config->GetFooterKey(), *encryption_util);
	} else {
		object.write(stream_protocol.get());
	}
	return make_uniq<ParquetPreparedWriteBuffer>(std::move(stream));
}

unique_ptr<AsyncWriteBuffer> ParquetWriter::PrepareWriteData(unique_ptr<AsyncWriteBuffer> buffer) {
	if (!options.encryption_config) {
		return buffer;
	}

	auto required_capacity =
	    buffer->Size() + ParquetCrypto::LENGTH_BYTES + ParquetCrypto::NONCE_BYTES + ParquetCrypto::TAG_BYTES;
	auto stream = make_uniq<MemoryStream>(BufferAllocator::Get(context), NextPowerOfTwo(required_capacity));
	TCompactProtocolFactoryT<MyTransport> tproto_factory;
	auto stream_protocol = tproto_factory.getProtocol(duckdb_base_std::make_shared<MyTransport>(*stream));
	ParquetCrypto::WriteData(*stream_protocol, buffer->Ptr(), NumericCast<uint32_t>(buffer->Size()),
	                         options.encryption_config->GetFooterKey(), *encryption_util);
	return make_uniq<ParquetPreparedWriteBuffer>(std::move(stream));
}

uint32_t ParquetWriter::WriteData(unique_ptr<AsyncWriteBuffer> buffer) {
	auto buffer_size = NumericCast<uint32_t>(buffer->Size());
	writer->WriteData(std::move(buffer));
	return buffer_size;
}

static void VerifyUniqueNames(const vector<string> &names) {
#ifdef DEBUG
	unordered_set<string> name_set;
	name_set.reserve(names.size());
	for (auto &column : names) {
		auto res = name_set.insert(column);
		D_ASSERT(res.second == true);
	}
	// If there would be duplicates, these sizes would differ
	D_ASSERT(name_set.size() == names.size());
#endif
}

struct ColumnStatsUnifier {
	virtual ~ColumnStatsUnifier() = default;

	string column_name;
	string global_min;
	string global_max;
	//! Only set by the 'metadata' of the VARIANT column
	string variant_type;
	idx_t null_count = 0;
	idx_t num_values = 0;
	bool all_min_max_set = true;
	bool all_nulls_set = true;
	bool min_is_set = false;
	bool max_is_set = false;
	bool min_is_exact = true;
	bool max_is_exact = true;
	idx_t column_size_bytes = 0;
	bool can_have_nan = false;
	bool has_nan = false;
	idx_t nan_count = 0;

	unique_ptr<GeometryStatsData> geo_stats;

	virtual void UnifyGeoStats(const GeometryStatsData &other) {
	}

	virtual void UnifyMinMax(const string &new_min, const string &new_max) = 0;
	virtual string StatsToString(const string &stats) = 0;
};

class ParquetStatsAccumulator {
public:
	struct NestedColumnStats {
		string column_name;
		idx_t null_count = 0;
		idx_t num_values = 0;
	};

	vector<unique_ptr<ColumnStatsUnifier>> stats_unifiers;
	map<idx_t, NestedColumnStats> nested_stats;
};

ParquetWriteTransformData::ParquetWriteTransformData(ClientContext &context, const vector<LogicalType> &types,
                                                     vector<unique_ptr<Expression>> expressions_p)
    : buffer(context, types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR), types(types),
      expressions(std::move(expressions_p)), executor(context, expressions) {
	chunk.Initialize(buffer.GetAllocator(), this->types);
}

bool ParquetWriteTransformData::MatchesTypes(const vector<LogicalType> &other_types) const {
	return types == other_types;
}

//! TODO: this doesnt work.. the ParquetWriteTransformData is shared with all threads, the method is stateful, but has
//! no locks Either every local state needs its own copy of this or we need a lock so its used by one thread at a time..
//! The former has my preference
ColumnDataCollection &ParquetWriteTransformData::ApplyTransform(ColumnDataCollection &input) {
	buffer.Reset();
	for (auto &input_chunk : input.Chunks()) {
		chunk.Reset();
		executor.Execute(input_chunk, chunk);
		buffer.Append(chunk);
	}
	return buffer;
}

ParquetWriter::ParquetWriter(ClientContext &context, FileSystem &fs, ParquetWriterOptions &&options_p,
                             const vector<pair<string, string>> &kv_metadata)
    : context(context), options(std::move(options_p)), metadata_arena(BufferAllocator::Get(context)),
      file_meta_data(metadata_arena) {
	// initialize the file writer
	writer = make_uniq<AsyncFileWriter>(context, fs, options.file_name.c_str(), options.open_flags);

	if (options.encryption_config) {
		// Get the encryption util
		encryption_util = context.db->GetEncryptionUtil(false);
		// encrypted parquet files start with the string "PARE"
		writer->WriteData(const_data_ptr_cast("PARE"), 4);
		// we only support this one for now, not "AES_GCM_CTR_V1"
		file_meta_data.encryption_algorithm.__isset.AES_GCM_V1 = true;
	} else {
		// parquet files start with the string "PAR1"
		writer->WriteData(const_data_ptr_cast("PAR1"), 4);
	}

	TCompactProtocolFactoryT<MyTransport> tproto_factory;
	protocol = tproto_factory.getProtocol(duckdb_base_std::make_shared<MyTransport>(*writer));

	file_meta_data.num_rows = 0;
	file_meta_data.version = UnsafeNumericCast<int32_t>(options.parquet_version);

	file_meta_data.__isset.created_by = true;
	file_meta_data.created_by =
	    StringUtil::Format("DuckDB version %s (build %s)", DuckDB::LibraryVersion(), DuckDB::SourceID());

	for (auto &kv_pair : kv_metadata) {
		file_meta_data.key_value_metadata.emplace_back();
		auto &kv = file_meta_data.key_value_metadata.back();
		kv.__set_key(kv_pair.first);
		kv.__set_value(kv_pair.second);
		file_meta_data.__isset.key_value_metadata = true;
	}

	if (options.enable_bloom_filters) {
		for (const auto &type : options.sql_types) {
			if (!TypeVisitor::Contains(type, LogicalTypeId::INTERVAL)) {
				continue;
			}
			file_meta_data.key_value_metadata.emplace_back();
			auto &kv = file_meta_data.key_value_metadata.back();
			kv.__set_key(ParquetStatisticsUtils::INTERVAL_BLOOM_FILTER_KEY);
			kv.__set_value(ParquetStatisticsUtils::INTERVAL_BLOOM_FILTER_VALUE);
			// Readers require this capability before probing the additional normalized hashes.
			file_meta_data.__isset.key_value_metadata = true;
			break;
		}
	}

	InitializeColumnWriters();
}

void ParquetWriter::InitializeColumnWriters() {
	auto &unique_names = options.column_names;
	VerifyUniqueNames(unique_names);

	auto &types = options.sql_types;

	// V1 GeoParquet stores geometries as blobs, no logical type
	auto allow_geometry = options.geoparquet_version != GeoParquetVersion::V1;

	// construct the column writers
	column_writers.clear();
	D_ASSERT(options.sql_types.size() == unique_names.size());
	for (idx_t i = 0; i < options.sql_types.size(); i++) {
		vector<Identifier> path_in_schema;
		const bool can_have_nulls = options.not_null_columns.empty() || !options.not_null_columns[i];
		column_writers.push_back(ColumnWriter::CreateWriterRecursive(
		    context, *this, path_in_schema, types[i], Identifier(unique_names[i]), allow_geometry, &options.field_ids,
		    &options.shredding_types, 0, 1, can_have_nulls));
	}
}

ParquetWriter::~ParquetWriter() {
}

void ParquetWriter::AnalyzeSchema(ColumnDataCollection &buffer, vector<unique_ptr<ColumnWriter>> &column_writers) {
	D_ASSERT(buffer.ColumnCount() == column_writers.size());
	vector<unique_ptr<ParquetAnalyzeSchemaState>> states;
	bool needs_analyze = false;
	lock_guard<mutex> glock(lock);

	vector<column_t> column_ids;
	for (idx_t i = 0; i < column_writers.size(); i++) {
		auto &writer = column_writers[i];
		auto state = writer->AnalyzeSchemaInit();
		if (state) {
			needs_analyze = true;
			states.push_back(std::move(state));
			column_ids.push_back(i);
		} else {
			states.push_back(nullptr);
		}
	}

	if (!needs_analyze) {
		return;
	}

	for (auto &chunk : buffer.Chunks(column_ids)) {
		// the column writers assume flat vectors, scans can return compact representations
		chunk.Flatten();
		idx_t index = 0;
		for (idx_t i = 0; i < column_writers.size(); i++) {
			auto &state = states[i];
			if (!state) {
				continue;
			}
			auto &writer = column_writers[i];
			writer->AnalyzeSchema(*state, chunk.data[index++], chunk.size());
		}
	}

	for (idx_t i = 0; i < column_writers.size(); i++) {
		auto &writer = column_writers[i];
		auto &state = states[i];
		if (!state) {
			continue;
		}
		writer->AnalyzeSchemaFinalize(*state);
	}
}

PreparedParquetLayout ParquetWriter::ExportPreparedLayout(ArenaAllocator &arena) const {
	PreparedParquetLayout result(arena);
	result.schema = file_meta_data.schema;
	for (auto &column_writer : column_writers) {
		ShreddingType column_shredding_type;
		if (!column_writer->TryExportPreparedShreddingType(column_shredding_type)) {
			continue;
		}
		result.shredding_types.AddChild(Identifier(column_writer->Schema().name), std::move(column_shredding_type));
	}
	return result;
}

#ifdef DEBUG
static idx_t CountLeafColumnWritersRecursiveVerification(const ColumnWriter &writer) {
	if (writer.ChildWriters().empty()) {
		return 1;
	}
	idx_t result = 0;
	for (auto &child_writer : writer.ChildWriters()) {
		result += CountLeafColumnWritersRecursiveVerification(*child_writer);
	}
	return result;
}

idx_t ParquetWriter::LeafColumnWriterCounts() const {
	idx_t result = 0;
	for (auto &column_writer : column_writers) {
		result += CountLeafColumnWritersRecursiveVerification(*column_writer);
	}
	return result;
}
#endif

void ParquetWriter::VerifyPreparedRowGroup(const PreparedRowGroup &prepared) const {
#ifdef DEBUG
	D_ASSERT(prepared.row_group.columns.size() == LeafColumnWriterCounts());
	D_ASSERT(prepared.states.size() == column_writers.size());
#endif
}

void ParquetWriter::InitializePreprocessing(unique_ptr<ParquetWriteTransformData> &transform_data) {
	vector<LogicalType> transformed_types;
	for (idx_t col_idx = 0; col_idx < column_writers.size(); col_idx++) {
		auto &column_writer = *column_writers[col_idx];
		auto &original_type = options.sql_types[col_idx];
		if (!column_writer.HasTransform()) {
			transformed_types.push_back(original_type);
			continue;
		}
		transformed_types.push_back(column_writer.TransformedType());
	}
	if (transform_data && transform_data->MatchesTypes(transformed_types)) {
		return;
	}

	vector<unique_ptr<Expression>> transform_expressions;
	for (idx_t col_idx = 0; col_idx < column_writers.size(); col_idx++) {
		auto &column_writer = *column_writers[col_idx];
		auto expr = make_uniq<BoundReferenceExpression>(options.sql_types[col_idx], col_idx);
		if (!column_writer.HasTransform()) {
			transform_expressions.push_back(std::move(expr));
			continue;
		}
		transform_expressions.push_back(column_writer.TransformExpression(std::move(expr)));
	}
	transform_data = make_uniq<ParquetWriteTransformData>(context, transformed_types, std::move(transform_expressions));
}

void ParquetWriter::PrepareRowGroup(ColumnDataCollection &raw_buffer, PreparedRowGroup &result,
                                    unique_ptr<ParquetWriteTransformData> &transform_data) {
	AnalyzeSchema(raw_buffer, column_writers);

	bool requires_transform = false;
	for (auto &col_writer_p : column_writers) {
		auto &col_writer = *col_writer_p;

		if (col_writer.HasTransform()) {
			requires_transform = true;
			break;
		}
	}

	reference<ColumnDataCollection> buffer_ref(raw_buffer);
	if (requires_transform) {
		InitializePreprocessing(transform_data);
		buffer_ref = transform_data->ApplyTransform(raw_buffer);
	}
	auto &buffer = buffer_ref.get();

	// We write 8 columns at a time so that iterating over ColumnDataCollection is more efficient
	static constexpr idx_t COLUMNS_PER_PASS = 8;

	// We want these to be buffer-managed
	D_ASSERT(buffer.GetAllocatorType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);

	// set up a new row group for this chunk collection
	auto &row_group = result.row_group;
	row_group.num_rows = NumericCast<int64_t>(buffer.Count());
	row_group.__isset.file_offset = true;

	InitializeSchemaElements();
	{
		lock_guard<mutex> glock(lock);
		result.layout = ExportPreparedLayout(*result.arena);
	}

	auto &states = result.states;
	// iterate over each of the columns of the chunk collection and write them
	D_ASSERT(buffer.ColumnCount() == column_writers.size());
	for (idx_t col_idx = 0; col_idx < buffer.ColumnCount(); col_idx += COLUMNS_PER_PASS) {
		const auto next = MinValue<idx_t>(buffer.ColumnCount() - col_idx, COLUMNS_PER_PASS);
		vector<column_t> column_ids;
		vector<reference<ColumnWriter>> col_writers;
		vector<unique_ptr<ColumnWriterState>> write_states;
		for (idx_t i = 0; i < next; i++) {
			column_ids.emplace_back(col_idx + i);
			col_writers.emplace_back(*column_writers[column_ids.back()]);
			write_states.emplace_back(col_writers.back().get().InitializeWriteState(row_group));
		}

		for (auto &chunk : buffer.Chunks(column_ids)) {
			// the column writers assume flat vectors, scans can return compact representations
			chunk.Flatten();
			for (idx_t i = 0; i < next; i++) {
				if (col_writers[i].get().HasAnalyze()) {
					col_writers[i].get().Analyze(*write_states[i], nullptr, chunk.data[i], chunk.size());
				}
			}
		}

		for (idx_t i = 0; i < next; i++) {
			if (col_writers[i].get().HasAnalyze()) {
				col_writers[i].get().FinalizeAnalyze(*write_states[i]);
			}
		}

		// Reserving these once at the start really pays off
		for (auto &write_state : write_states) {
			write_state->definition_levels.reserve(buffer.Count());
		}

		for (auto &chunk : buffer.Chunks({column_ids})) {
			// the column writers assume flat vectors, scans can return compact representations
			chunk.Flatten();
			for (idx_t i = 0; i < next; i++) {
				col_writers[i].get().Prepare(*write_states[i], nullptr, chunk.data[i], chunk.size(), true);
			}
		}

		for (idx_t i = 0; i < next; i++) {
			col_writers[i].get().BeginWrite(*write_states[i]);
		}

		for (auto &chunk : buffer.Chunks({column_ids})) {
			// the column writers assume flat vectors, scans can return compact representations
			chunk.Flatten();
			for (idx_t i = 0; i < next; i++) {
				col_writers[i].get().Write(*write_states[i], chunk.data[i], chunk.size());
			}
		}

		for (idx_t i = 0; i < next; i++) {
			col_writers[i].get().PrepareWrite(*write_states[i]);
		}

		for (auto &write_state : write_states) {
			states.push_back(std::move(write_state));
		}
	}
}

// Validation code adapted from Impala
static void ValidateOffsetInFile(const string &filename, idx_t col_idx, idx_t file_length, idx_t offset,
                                 const string &offset_name) {
	if (offset >= file_length) {
		// LCOV_EXCL_START
		throw IOException("File '%s': metadata is corrupt. Column %d has invalid "
		                  "%s (offset=%llu file_size=%llu).",
		                  filename, col_idx, offset_name, offset, file_length);
		// LCOV_EXCL_STOP
	}
}

static void ValidateColumnOffsets(const string &filename, idx_t file_length, const ParquetRowGroup &row_group) {
	for (idx_t i = 0; i < row_group.columns.size(); ++i) {
		const auto &col_chunk = row_group.columns[i];
		ValidateOffsetInFile(filename, i, file_length, col_chunk.meta_data.data_page_offset, "data page offset");
		auto col_start = NumericCast<idx_t>(col_chunk.meta_data.data_page_offset);
		// The file format requires that if a dictionary page exists, it be before data pages.
		if (col_chunk.meta_data.__isset.dictionary_page_offset) {
			ValidateOffsetInFile(filename, i, file_length, col_chunk.meta_data.dictionary_page_offset,
			                     "dictionary page offset");
			if (NumericCast<idx_t>(col_chunk.meta_data.dictionary_page_offset) >= col_start) {
				// LCOV_EXCL_START
				throw IOException("Parquet file '%s': metadata is corrupt. Dictionary "
				                  "page (offset=%llu) must come before any data pages (offset=%llu).",
				                  filename, col_chunk.meta_data.dictionary_page_offset, col_start);
				// LCOV_EXCL_STOP
			}
			col_start = col_chunk.meta_data.dictionary_page_offset;
		}
		auto col_len = NumericCast<idx_t>(col_chunk.meta_data.total_compressed_size);
		auto col_end = col_start + col_len;
		if (col_end <= 0 || col_end > file_length) {
			// LCOV_EXCL_START
			throw IOException("Parquet file '%s': metadata is corrupt. Column %llu has "
			                  "invalid column offsets (offset=%llu, size=%llu, file_size=%llu).",
			                  filename, i, col_start, col_len, file_length);
			// LCOV_EXCL_STOP
		}
	}
}

void ParquetWriter::FlushRowGroup(PreparedRowGroup &prepared) {
	auto batch_guard = writer->StartBatch();
	{
		lock_guard<mutex> glock(lock);
		auto &row_group = prepared.row_group;
		auto &states = prepared.states;
		if (states.empty()) {
			throw InternalException("Attempting to flush a row group with no rows");
		}
		InitializeSchemaFromPreparedRowGroup(prepared);
		VerifyPreparedRowGroup(prepared);
		row_group.file_offset = NumericCast<int64_t>(writer->GetTotalWritten());
		for (idx_t col_idx = 0; col_idx < states.size(); col_idx++) {
			const auto &col_writer = column_writers[col_idx];
			auto write_state = std::move(states[col_idx]);
			col_writer->FinalizeWrite(*write_state);
		}
		// let's make sure all offsets are ay-okay
		ValidateColumnOffsets(options.file_name, writer->GetTotalWritten(), row_group);

		row_group.total_compressed_size = NumericCast<int64_t>(writer->GetTotalWritten()) - row_group.file_offset;
		row_group.__isset.total_compressed_size = true;

		if (options.encryption_config) {
			const auto row_group_ordinal = file_meta_data.row_groups.size();
			if (row_group_ordinal > std::numeric_limits<int16_t>::max()) {
				throw InvalidInputException("RowGroup ordinal exceeds 32767 when encryption enabled");
			}
			row_group.ordinal = NumericCast<int16_t>(row_group_ordinal);
			row_group.__isset.ordinal = true;
		}

		// append the row group to the file metadata
		file_meta_data.row_groups.push_back(row_group);
		file_meta_data.num_rows += row_group.num_rows;
	}
	batch_guard.Finish();
}

void ParquetWriter::Flush(ColumnDataCollection &buffer, unique_ptr<ParquetWriteTransformData> &transform_data) {
	if (buffer.Count() == 0) {
		return;
	}

	PreparedRowGroup prepared_row_group(BufferAllocator::Get(context));
	PrepareRowGroup(buffer, prepared_row_group, transform_data);
	buffer.Reset();

	FlushRowGroup(prepared_row_group);
}

template <class T>
struct BaseNumericStatsUnifier : public ColumnStatsUnifier {
	void UnifyMinMax(const string &new_min, const string &new_max) override {
		if (new_min.size() != sizeof(T) || new_max.size() != sizeof(T)) {
			throw InternalException("Incorrect size for stats in UnifyMinMax");
		}
		if (!min_is_set) {
			global_min = new_min;
			min_is_set = true;
		} else {
			auto min_val = Load<T>(const_data_ptr_cast(new_min.data()));
			auto global_min_val = Load<T>(const_data_ptr_cast(global_min.data()));
			if (LessThan::Operation(min_val, global_min_val)) {
				global_min = new_min;
			}
		}
		if (!max_is_set) {
			global_max = new_max;
			max_is_set = true;
		} else {
			auto max_val = Load<T>(const_data_ptr_cast(new_max.data()));
			auto global_max_val = Load<T>(const_data_ptr_cast(global_max.data()));
			if (GreaterThan::Operation(max_val, global_max_val)) {
				global_max = new_max;
			}
		}
	}
};

template <class T>
struct NumericStatsUnifier : public BaseNumericStatsUnifier<T> {
	string StatsToString(const string &stats) override {
		if (stats.empty()) {
			return string();
		}
		return Value::CreateValue<T>(Load<T>(const_data_ptr_cast(stats.data()))).ToString();
	}
};

struct TimeTZStatsUnifier : public BaseNumericStatsUnifier<int64_t> {
	string StatsToString(const string &stats) override {
		if (stats.empty()) {
			return string();
		}
		return Value::TIMETZ(ParquetIntToTimeTZ(Load<int64_t>(const_data_ptr_cast(stats.data())))).ToString();
	}
};

template <class T>
struct DecimalStatsUnifier : public NumericStatsUnifier<T> {
	DecimalStatsUnifier(uint8_t width, uint8_t scale) : width(width), scale(scale) {
	}

	uint8_t width;
	uint8_t scale;

	string StatsToString(const string &stats) override {
		if (stats.empty()) {
			return string();
		}
		auto stats_data = const_data_ptr_cast(stats.data());
		if (sizeof(T) == sizeof(hugeint_t)) {
			auto schema = ParquetColumnSchema(); // schema unused for FLBA/hugeint_t
			auto numeric_val = ParquetDecimalUtils::ReadDecimalValue<hugeint_t>(stats_data, stats.size(), schema);
			return Value::DECIMAL(numeric_val, width, scale).ToString();
		} else {
			return Value::DECIMAL(Load<T>(stats_data), width, scale).ToString();
		}
	}

	void UnifyMinMax(const string &new_min, const string &new_max) override {
		if (sizeof(T) != sizeof(hugeint_t)) {
			// INT32/INT64-backed decimals are little-endian; the base compare is correct.
			BaseNumericStatsUnifier<T>::UnifyMinMax(new_min, new_max);
		} else {
			// FLBA decimal stats are big-endian two's complement (most significant byte
			// first), so a native little-endian Load would compare the wrong value; decode
			// the bytes into a hugeint_t before comparing.
			auto decode = [](const string &stats) {
				auto schema = ParquetColumnSchema(); // schema unused for FLBA/hugeint_t
				return ParquetDecimalUtils::ReadDecimalValue<hugeint_t>(const_data_ptr_cast(stats.data()), stats.size(),
				                                                        schema);
			};

			if (!this->min_is_set) {
				this->global_min = new_min;
				this->min_is_set = true;
			} else if (LessThan::Operation(decode(new_min), decode(this->global_min))) {
				this->global_min = new_min;
			}

			if (!this->max_is_set) {
				this->global_max = new_max;
				this->max_is_set = true;
			} else if (GreaterThan::Operation(decode(new_max), decode(this->global_max))) {
				this->global_max = new_max;
			}
		}
	}
};

struct BaseStringStatsUnifier : public ColumnStatsUnifier {
	void UnifyMinMax(const string &new_min, const string &new_max) override {
		if (!min_is_set) {
			global_min = new_min;
			min_is_set = true;
		} else {
			if (LessThan::Operation(string_t(new_min), string_t(global_min))) {
				global_min = new_min;
			}
		}
		if (!max_is_set) {
			global_max = new_max;
			max_is_set = true;
		} else {
			if (GreaterThan::Operation(string_t(new_max), string_t(global_max))) {
				global_max = new_max;
			}
		}
	}
};

struct StringStatsUnifier : public BaseStringStatsUnifier {
	string StatsToString(const string &stats) override {
		return stats;
	}
};

struct BlobStatsUnifier : public BaseStringStatsUnifier {
	string StatsToString(const string &stats) override {
		// convert blobs to hexadecimal
		auto data = const_data_ptr_cast(stats.c_str());
		auto len = stats.size();
		string result;
		result.reserve(len * 2);
		for (idx_t i = 0; i < len; i++) {
			auto byte_a = data[i] >> 4;
			auto byte_b = data[i] & 0x0F;
			result += Blob::HEX_TABLE[byte_a];
			result += Blob::HEX_TABLE[byte_b];
		}
		return result;
	}
};

struct GeoStatsUnifier : public ColumnStatsUnifier {
	void UnifyGeoStats(const GeometryStatsData &other) override {
		if (geo_stats) {
			geo_stats->Merge(other);
		} else {
			// Make copy
			geo_stats = make_uniq<GeometryStatsData>();
			geo_stats->extent = other.extent;
			geo_stats->types = other.types;
		}
	}

	void UnifyMinMax(const string &new_min, const string &new_max) override {
		// Do nothing
	}

	string StatsToString(const string &stats) override {
		if (!geo_stats) {
			return string();
		}

		const auto &bbox = geo_stats->extent;
		const auto &types = geo_stats->types;

		const auto bbox_value = Value::STRUCT({{"xmin", bbox.x_min},
		                                       {"xmax", bbox.x_max},
		                                       {"ymin", bbox.y_min},
		                                       {"ymax", bbox.y_max},
		                                       {"zmin", bbox.z_min},
		                                       {"zmax", bbox.z_max},
		                                       {"mmin", bbox.m_min},
		                                       {"mmax", bbox.m_max}});

		vector<Value> type_strings;
		for (const auto &type : types.ToString(true)) {
			type_strings.push_back(Value(StringUtil::Lower(type)));
		}

		return Value::STRUCT({{"bbox", bbox_value}, {"types", Value::LIST(type_strings)}}).ToString();
	}
};

struct UUIDStatsUnifier : public BaseStringStatsUnifier {
	string StatsToString(const string &stats) override {
		if (stats.size() != 16) {
			return string();
		}
		auto data_ptr = const_data_ptr_cast(stats.c_str());
		static char const UUID_DIGITS[] = "0123456789abcdef";
		string result;
		// UUID format is XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXX
		// i.e. dashes are at bytes 4, 6, 8, 10
		for (idx_t i = 0; i < 16; i++) {
			if (i == 4 || i == 6 || i == 8 || i == 10) {
				result += "-";
			}
			result += UUID_DIGITS[data_ptr[i] >> 4];
			result += UUID_DIGITS[data_ptr[i] & 0xf];
		}
		return result;
	}
};
struct NullStatsUnifier : public ColumnStatsUnifier {
	void UnifyMinMax(const string &new_min, const string &new_max) override {
	}

	string StatsToString(const string &stats) override {
		return string();
	}
};

static unique_ptr<ColumnStatsUnifier> GetBaseStatsUnifier(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return make_uniq<NumericStatsUnifier<int8_t>>();
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
		return make_uniq<NumericStatsUnifier<int32_t>>();
	case LogicalTypeId::DATE:
		return make_uniq<NumericStatsUnifier<date_t>>();
	case LogicalTypeId::BIGINT:
		return make_uniq<NumericStatsUnifier<int64_t>>();
	case LogicalTypeId::TIME:
		return make_uniq<NumericStatsUnifier<dtime_t>>();
	case LogicalTypeId::TIME_NS:
		return make_uniq<NumericStatsUnifier<dtime_ns_t>>();
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP:
		return make_uniq<NumericStatsUnifier<timestamp_t>>();
	case LogicalTypeId::TIMESTAMP_TZ:
		return make_uniq<NumericStatsUnifier<timestamp_tz_t>>();
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return make_uniq<NumericStatsUnifier<timestamp_tz_ns_t>>();
	case LogicalTypeId::TIMESTAMP_MS:
		return make_uniq<NumericStatsUnifier<timestamp_ms_t>>();
	case LogicalTypeId::TIMESTAMP_NS:
		return make_uniq<NumericStatsUnifier<timestamp_ns_t>>();
	case LogicalTypeId::TIME_TZ:
		return make_uniq<TimeTZStatsUnifier>();
	case LogicalTypeId::UINTEGER:
		return make_uniq<NumericStatsUnifier<uint32_t>>();
	case LogicalTypeId::UBIGINT:
		return make_uniq<NumericStatsUnifier<uint64_t>>();
	case LogicalTypeId::FLOAT:
		return make_uniq<NumericStatsUnifier<float>>();
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DOUBLE:
		return make_uniq<NumericStatsUnifier<double>>();
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
		case PhysicalType::INT32:
			return make_uniq<DecimalStatsUnifier<int32_t>>(width, scale);
		case PhysicalType::INT64:
			return make_uniq<DecimalStatsUnifier<int64_t>>(width, scale);
		case PhysicalType::INT128:
			return make_uniq<DecimalStatsUnifier<hugeint_t>>(width, scale);
		default:
			return make_uniq<NullStatsUnifier>();
		}
	}
	case LogicalTypeId::BLOB:
		return make_uniq<BlobStatsUnifier>();
	case LogicalTypeId::GEOMETRY:
		return make_uniq<GeoStatsUnifier>();
	case LogicalTypeId::VARCHAR:
		return make_uniq<StringStatsUnifier>();
	case LogicalTypeId::UUID:
		return make_uniq<UUIDStatsUnifier>();
	case LogicalTypeId::INTERVAL:;
	case LogicalTypeId::ENUM:
	default:
		return make_uniq<NullStatsUnifier>();
	}
}

static bool IsVariantMetadataField(const ColumnWriter &writer) {
	if (!writer.parent) {
		//! Not a nested column
		return false;
	}
	auto &parent = *writer.parent;
	if (parent.Type().id() != LogicalTypeId::VARIANT) {
		//! (direct) parent is not a VARIANT
		return false;
	}
	auto &name = writer.Schema().name;
	return name == "metadata";
}

static void GetStatsUnifier(const ColumnWriter &column_writer, ParquetStatsAccumulator &accumulator,
                            string base_name = string()) {
	auto &schema = column_writer.Schema();
	if (schema.repetition_type != duckdb_parquet::FieldRepetitionType::REPEATED) {
		if (!base_name.empty()) {
			base_name += ".";
		}
		base_name += SQLQuotedIdentifier(schema.name);
	}

	auto &children = column_writer.ChildWriters();
	if (children.empty()) {
		auto unifier = GetBaseStatsUnifier(schema.type);
		unifier->column_name = std::move(base_name);

		if (IsVariantMetadataField(column_writer)) {
			//! Stamp the 'metadata' field of the VARIANT with the internal layout of the VARIANT
			auto &variant_writer = column_writer.parent->Cast<VariantColumnWriter>();
			unifier->variant_type = variant_writer.TransformedType().ToString();
		}
		accumulator.stats_unifiers.push_back(std::move(unifier));
		return;
	}
	if (schema.repetition_type != duckdb_parquet::FieldRepetitionType::REPEATED) {
		accumulator.nested_stats[column_writer.SchemaIndex()].column_name = base_name;
	}
	for (auto &child_writer : children) {
		GetStatsUnifier(*child_writer, accumulator, base_name);
	}
}

void ParquetWriter::FlushNestedColumnStats(idx_t schema_idx, idx_t null_count, idx_t num_values) {
	if (!written_stats) {
		return;
	}
	auto entry = stats_accumulator->nested_stats.find(schema_idx);
	if (entry != stats_accumulator->nested_stats.end()) {
		entry->second.null_count += null_count;
		entry->second.num_values += num_values;
	}
}

void ParquetWriter::FlushColumnStats(idx_t col_idx, duckdb_parquet::ColumnChunk &column,
                                     optional_ptr<ColumnWriterStatistics> writer_stats) {
	if (!written_stats) {
		return;
	}

	// push the stats of this column into the unifier
	auto &stats_unifier = stats_accumulator->stats_unifiers[col_idx];
	bool has_nan = false;
	if (writer_stats) {
		stats_unifier->can_have_nan = writer_stats->CanHaveNaN();
		has_nan = writer_stats->HasNaN();
		// this is called once per row group: accumulate, so NaNs in earlier row groups are not lost
		stats_unifier->has_nan = stats_unifier->has_nan || has_nan;
		stats_unifier->nan_count += writer_stats->GetNaNCount();
	}
	if (column.meta_data.__isset.statistics) {
		if (has_nan && writer_stats->HasStats()) {
			// if we have NaN values we have not written the min/max to the Parquet file
			// BUT we can return them as part of RETURN STATS by fetching them from the stats directly
			stats_unifier->UnifyMinMax(writer_stats->GetMin(), writer_stats->GetMax());
			stats_unifier->min_is_exact = stats_unifier->min_is_exact && writer_stats->MinIsExact();
			stats_unifier->max_is_exact = stats_unifier->max_is_exact && writer_stats->MaxIsExact();
		} else if (column.meta_data.statistics.__isset.min_value && column.meta_data.statistics.__isset.max_value) {
			stats_unifier->UnifyMinMax(column.meta_data.statistics.min_value, column.meta_data.statistics.max_value);
			stats_unifier->min_is_exact = stats_unifier->min_is_exact &&
			                              column.meta_data.statistics.__isset.is_min_value_exact &&
			                              column.meta_data.statistics.is_min_value_exact;
			stats_unifier->max_is_exact = stats_unifier->max_is_exact &&
			                              column.meta_data.statistics.__isset.is_max_value_exact &&
			                              column.meta_data.statistics.is_max_value_exact;
		} else {
			stats_unifier->all_min_max_set = false;
		}
		if (column.meta_data.statistics.__isset.null_count) {
			stats_unifier->null_count += column.meta_data.statistics.null_count;
		} else {
			stats_unifier->all_nulls_set = false;
		}
		if (writer_stats && writer_stats->HasGeoStats()) {
			stats_unifier->UnifyGeoStats(*writer_stats->GetGeoStats());
		}
		stats_unifier->column_size_bytes += column.meta_data.total_compressed_size;
		stats_unifier->num_values += column.meta_data.num_values;
	}
}

void ParquetWriter::GatherWrittenStatistics() {
	written_stats->row_count = file_meta_data.num_rows;

	for (auto &entry : stats_accumulator->nested_stats) {
		auto &stats = entry.second;
		case_insensitive_map_t<Value> column_stats;
		column_stats["null_count"] = Value::UBIGINT(stats.null_count);
		column_stats["num_values"] = Value::UBIGINT(stats.num_values);
		written_stats->column_statistics.emplace(stats.column_name, std::move(column_stats));
	}

	// finalize the min/max values and write to column stats
	for (idx_t c = 0; c < stats_accumulator->stats_unifiers.size(); c++) {
		auto &stats_unifier = stats_accumulator->stats_unifiers[c];
		case_insensitive_map_t<Value> column_stats;
		column_stats["column_size_bytes"] = Value::UBIGINT(stats_unifier->column_size_bytes);
		column_stats["num_values"] = Value::UBIGINT(stats_unifier->num_values);
		if (stats_unifier->all_min_max_set) {
			auto min_value = stats_unifier->StatsToString(stats_unifier->global_min);
			auto max_value = stats_unifier->StatsToString(stats_unifier->global_max);
			if (stats_unifier->min_is_set) {
				column_stats["min"] = min_value;
				column_stats["min_is_exact"] = Value::BOOLEAN(stats_unifier->min_is_exact);
			}
			if (stats_unifier->max_is_set) {
				column_stats["max"] = max_value;
				column_stats["max_is_exact"] = Value::BOOLEAN(stats_unifier->max_is_exact);
			}
		}
		if (!stats_unifier->variant_type.empty()) {
			column_stats["variant_type"] = Value(stats_unifier->variant_type);
		}
		if (stats_unifier->all_nulls_set) {
			column_stats["null_count"] = Value::UBIGINT(stats_unifier->null_count);
		}
		if (stats_unifier->can_have_nan) {
			column_stats["has_nan"] = Value::BOOLEAN(stats_unifier->has_nan);
			column_stats["nan_count"] = Value::UBIGINT(stats_unifier->nan_count);
		}
		if (stats_unifier->geo_stats) {
			const auto &bbox = stats_unifier->geo_stats->extent;
			const auto &types = stats_unifier->geo_stats->types;

			if (bbox.HasXY()) {
				column_stats["bbox_xmin"] = Value::DOUBLE(bbox.x_min);
				column_stats["bbox_xmax"] = Value::DOUBLE(bbox.x_max);
				column_stats["bbox_ymin"] = Value::DOUBLE(bbox.y_min);
				column_stats["bbox_ymax"] = Value::DOUBLE(bbox.y_max);

				if (bbox.HasZ()) {
					column_stats["bbox_zmin"] = Value::DOUBLE(bbox.z_min);
					column_stats["bbox_zmax"] = Value::DOUBLE(bbox.z_max);
				}

				if (bbox.HasM()) {
					column_stats["bbox_mmin"] = Value::DOUBLE(bbox.m_min);
					column_stats["bbox_mmax"] = Value::DOUBLE(bbox.m_max);
				}
			}

			if (!types.IsEmpty()) {
				vector<Value> type_strings;
				for (const auto &type : types.ToString(true)) {
					type_strings.push_back(Value(StringUtil::Lower(type)));
				}
				column_stats["geo_types"] = Value::LIST(type_strings);
			}
		}
		written_stats->column_statistics.emplace(stats_unifier->column_name, std::move(column_stats));
	}
}

void ParquetWriter::InitializeSchemaElements() {
	//! Populate the schema elements of the parquet file we're writing
	lock_guard<mutex> glock(lock);
	if (!file_meta_data.schema.empty()) {
		return;
	}
	auto unique_columns = InitializeColumnWriterSchemaIndices();
	InitializeStatsUnifiers();
	InitializeColumnOrders(unique_columns);
}

idx_t ParquetWriter::InitializeColumnWriterSchemaIndices() {
	// populate root schema object
	file_meta_data.schema.resize(1);
	file_meta_data.schema[0].name = "duckdb_schema";
	file_meta_data.schema[0].num_children = NumericCast<int32_t>(options.sql_types.size());
	file_meta_data.schema[0].__isset.num_children = true;
	file_meta_data.schema[0].repetition_type = duckdb_parquet::FieldRepetitionType::REQUIRED;
	file_meta_data.schema[0].__isset.repetition_type = true;

	idx_t unique_columns = 0;
	for (auto &column_writer : column_writers) {
		unique_columns += column_writer->FinalizeSchema(file_meta_data.schema);
	}
	return unique_columns;
}

void ParquetWriter::InitializeColumnOrders(idx_t unique_columns) {
	if (file_meta_data.__isset.column_orders) {
		return;
	}
	duckdb_parquet::ColumnOrder column_order;
	column_order.__set_TYPE_ORDER(duckdb_parquet::TypeDefinedOrder());
	file_meta_data.column_orders.resize(unique_columns, column_order);
	file_meta_data.__isset.column_orders = true;
}

void ParquetWriter::InitializeStatsUnifiers() {
	if (!written_stats || !stats_accumulator->stats_unifiers.empty()) {
		return;
	}
	for (auto &column_writer : column_writers) {
		GetStatsUnifier(*column_writer, *stats_accumulator);
	}
}

static bool HasPreparedShreddingTypes(const ShreddingType &shredding_types) {
	return shredding_types.set || !shredding_types.children.types->empty();
}

void ParquetWriter::InitializeSchemaFromPreparedRowGroup(const PreparedRowGroup &prepared) {
	auto &layout = prepared.layout;
	if (file_meta_data.schema.empty()) {
		if (layout.schema.empty()) {
			throw InternalException("Prepared Parquet row group is missing schema");
		}
		if (HasPreparedShreddingTypes(layout.shredding_types)) {
			options.shredding_types = layout.shredding_types.Copy();
			InitializeColumnWriters();
		}
		auto unique_columns = InitializeColumnWriterSchemaIndices();
		D_ASSERT(unique_columns == prepared.row_group.columns.size());
		D_ASSERT(file_meta_data.schema.size() == layout.schema.size());
		file_meta_data.schema = layout.schema;
		InitializeColumnOrders(unique_columns);
	}
	InitializeStatsUnifiers();
}

void ParquetWriter::Finalize() {
	InitializeSchemaElements();

	// dump the bloom filters right before footer, not if stuff is encrypted

	for (auto &bloom_filter_entry : bloom_filters) {
		D_ASSERT(!options.encryption_config);
		// write nonsense bloom filter header
		duckdb_parquet::BloomFilterHeader filter_header;
		auto bloom_filter_bytes = bloom_filter_entry.bloom_filter->Get();
		filter_header.numBytes = NumericCast<int32_t>(bloom_filter_bytes->len);
		filter_header.algorithm.__set_BLOCK(duckdb_parquet::SplitBlockAlgorithm());
		filter_header.compression.__set_UNCOMPRESSED(duckdb_parquet::Uncompressed());
		filter_header.hash.__set_XXHASH(duckdb_parquet::XxHash());

		// set metadata flags
		auto &column_chunk =
		    file_meta_data.row_groups[bloom_filter_entry.row_group_idx].columns[bloom_filter_entry.column_idx];

		column_chunk.meta_data.__isset.bloom_filter_offset = true;
		column_chunk.meta_data.bloom_filter_offset = NumericCast<int64_t>(writer->GetTotalWritten());

		auto bloom_filter_header_size = Write(filter_header);
		// write actual data
		WriteData(bloom_filter_bytes->ptr, bloom_filter_bytes->len);

		column_chunk.meta_data.__isset.bloom_filter_length = true;
		column_chunk.meta_data.bloom_filter_length =
		    NumericCast<int32_t>(bloom_filter_header_size + bloom_filter_bytes->len);
	}

	// the page index goes right in front of the footer, after the bloom filters, so that it is part of the tail of the
	// file a reader fetches together with the footer: first the ColumnIndex of every column chunk, then the
	// OffsetIndex of every column chunk, their location is recorded in the ColumnChunk
	for (auto &page_index : page_indexes) {
		D_ASSERT(!options.encryption_config);
		if (!page_index.has_column_index) {
			continue;
		}
		auto &column_chunk = file_meta_data.row_groups[page_index.row_group_idx].columns[page_index.column_idx];
		column_chunk.column_index_offset = NumericCast<int64_t>(writer->GetTotalWritten());
		column_chunk.__isset.column_index_offset = true;
		column_chunk.column_index_length = NumericCast<int32_t>(Write(page_index.column_index));
		column_chunk.__isset.column_index_length = true;
	}
	for (auto &page_index : page_indexes) {
		auto &column_chunk = file_meta_data.row_groups[page_index.row_group_idx].columns[page_index.column_idx];
		column_chunk.offset_index_offset = NumericCast<int64_t>(writer->GetTotalWritten());
		column_chunk.__isset.offset_index_offset = true;
		column_chunk.offset_index_length = NumericCast<int32_t>(Write(page_index.offset_index));
		column_chunk.__isset.offset_index_length = true;
	}
	page_indexes.clear();

	const auto metadata_start_offset = writer->GetTotalWritten();
	if (options.encryption_config) {
		// Crypto metadata is written unencrypted
		FileCryptoMetaData crypto_metadata(metadata_arena);
		duckdb_parquet::AesGcmV1 aes_gcm_v1(metadata_arena);
		duckdb_parquet::EncryptionAlgorithm alg(metadata_arena);
		alg.__set_AES_GCM_V1(aes_gcm_v1);
		crypto_metadata.__set_encryption_algorithm(alg);
		crypto_metadata.write(protocol.get());
	}

	// Add geoparquet metadata to the file metadata
	if (geoparquet_data && GeoParquetFileMetadata::IsGeoParquetConversionEnabled(context) &&
	    GetGeoParquetVersion() != GeoParquetVersion::NONE) {
		geoparquet_data->Write(file_meta_data);
	}

	Write(file_meta_data);

	auto footer_size = NumericCast<uint32_t>(writer->GetTotalWritten() - metadata_start_offset);
	writer->Write<uint32_t>(footer_size);

	if (options.encryption_config) {
		// encrypted parquet files also end with the string "PARE"
		writer->WriteData(const_data_ptr_cast("PARE"), 4);
	} else {
		// parquet files also end with the string "PAR1"
		writer->WriteData(const_data_ptr_cast("PAR1"), 4);
	}
	if (written_stats) {
		// gather written statistics from the metadata
		GatherWrittenStatistics();
		written_stats->file_size_bytes = writer->GetTotalWritten();
		written_stats->footer_size_bytes = Value::UBIGINT(footer_size);
		written_stats->extra_info["row_group_count"] = Value::UBIGINT(NumberOfRowGroups());
	}

	// flush to disk
	writer->Close();
	writer.reset();
}

GeoParquetFileMetadata &ParquetWriter::GetGeoParquetData() {
	if (!geoparquet_data) {
		geoparquet_data = make_uniq<GeoParquetFileMetadata>(GetGeoParquetVersion());
	}
	return *geoparquet_data;
}

void ParquetWriter::BufferPageIndex(idx_t col_idx, ParquetPageIndexEntry page_index) {
	if (options.encryption_config) {
		// the page index of an encrypted file has to be encrypted as well - not supported (yet)
		return;
	}
	page_index.column_idx = col_idx;
	page_index.row_group_idx = file_meta_data.row_groups.size();
	page_indexes.push_back(std::move(page_index));
}

void ParquetWriter::BufferBloomFilter(idx_t col_idx, unique_ptr<ParquetBloomFilter> bloom_filter) {
	if (options.encryption_config) {
		return;
	}
	ParquetBloomFilterEntry new_entry;
	new_entry.bloom_filter = std::move(bloom_filter);
	new_entry.column_idx = col_idx;
	new_entry.row_group_idx = file_meta_data.row_groups.size();
	bloom_filters.push_back(std::move(new_entry));
}

void ParquetWriter::SetWrittenStatistics(CopyFunctionFileStatistics &written_stats_p) {
	written_stats = written_stats_p;
	stats_accumulator = make_uniq<ParquetStatsAccumulator>();
	//! NOTE: the actual accumulators for the writers are created after FinalizeSchema() is called
}

ParquetWriteGlobalState::ParquetWriteGlobalState() = default;

} // namespace duckdb
