#include "reader/decimal_column_reader.hpp"

#include <memory>
#include <cmath>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "parquet_types.h"

namespace duckdb {
class ParquetReader;
struct hugeint_t;

template <bool FIXED>
static unique_ptr<ColumnReader> CreateDecimalReaderInternal(const ParquetReader &reader,
                                                            const ParquetColumnSchema &schema) {
	switch (schema.type.InternalType()) {
	case PhysicalType::INT16:
		return make_uniq<DecimalColumnReader<int16_t, FIXED>>(reader, schema);
	case PhysicalType::INT32:
		return make_uniq<DecimalColumnReader<int32_t, FIXED>>(reader, schema);
	case PhysicalType::INT64:
		return make_uniq<DecimalColumnReader<int64_t, FIXED>>(reader, schema);
	case PhysicalType::INT128:
		return make_uniq<DecimalColumnReader<hugeint_t, FIXED>>(reader, schema);
	case PhysicalType::DOUBLE:
		return make_uniq<DecimalColumnReader<double, FIXED>>(reader, schema);
	default:
		throw InternalException("Unrecognized type for Decimal");
	}
}

template <>
double ParquetDecimalUtils::ReadDecimalValue(const_data_ptr_t pointer, idx_t size,
                                             const ParquetColumnSchema &schema_ele) {
	double res = 0;
	if (size == 0) {
		// empty byte array - value is zero, and there is no sign byte to read
		return res;
	}
	bool positive = (*pointer & 0x80) == 0;
	for (idx_t i = 0; i < size; i += sizeof(uint64_t)) {
		// the trailing chunk can hold fewer than 8 bytes - scale by the bytes actually consumed
		auto byte_size = MinValue<idx_t>(sizeof(uint64_t), size - i);
		uint64_t input = 0;
		for (idx_t k = 0; k < byte_size; k++) {
			auto byte = pointer[i + k];
			input = (input << 8) | static_cast<uint64_t>(positive ? byte : byte ^ 0xFF);
		}
		res = std::ldexp(res, static_cast<int>(8 * byte_size));
		res += static_cast<double>(input);
	}
	if (!positive) {
		res += 1;
		res /= pow(10, schema_ele.type_scale);
		return -res;
	}
	res /= pow(10, schema_ele.type_scale);
	return res;
}

unique_ptr<ColumnReader> ParquetDecimalUtils::CreateReader(const ParquetReader &reader,
                                                           const ParquetColumnSchema &schema) {
	if (schema.parquet_type == Type::FIXED_LEN_BYTE_ARRAY) {
		return CreateDecimalReaderInternal<true>(reader, schema);
	} else {
		return CreateDecimalReaderInternal<false>(reader, schema);
	}
}

} // namespace duckdb
