#include "catch.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/function/table/read_csv.hpp"

namespace duckdb {

TEST_CASE("CSV sample sizes survive serialization", "[serialization]") {
	const vector<idx_t> sample_sizes = {1,
	                                    1000,
	                                    2048,
	                                    2049,
	                                    3000,
	                                    CSVReaderOptions::default_sample_size,
	                                    NumericCast<idx_t>(NumericLimits<int64_t>::Maximum()),
	                                    NumericLimits<idx_t>::Maximum()};
	for (const auto serialize_defaults : {false, true}) {
		for (const auto sample_size : sample_sizes) {
			CSVReaderOptions options;
			options.SetSampleSize(sample_size);
			options.auto_detect = false;
			SerializedCSVReaderOptions input(options, MultiFileOptions());
			MemoryStream stream(Allocator::DefaultAllocator());
			SerializationOptions serialization_options;
			serialization_options.serialize_default_values = serialize_defaults;
			BinarySerializer::Serialize(input, stream, serialization_options);
			stream.Rewind();
			BinaryDeserializer deserializer(stream);
			deserializer.Begin();
			auto output = SerializedCSVReaderOptions::Deserialize(deserializer);
			deserializer.End();
			REQUIRE(output.options.GetSampleSize() == sample_size);
			REQUIRE_FALSE(output.options.auto_detect);
		}
	}
}

} // namespace duckdb
