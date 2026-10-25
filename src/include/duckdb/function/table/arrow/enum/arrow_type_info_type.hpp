#pragma once

#include <cstdint>

namespace duckdb {

enum class ArrowTypeInfoType : uint8_t { LIST, STRUCT, DATE_TIME, STRING, ARRAY, DECIMAL, UNION };

} // namespace duckdb
