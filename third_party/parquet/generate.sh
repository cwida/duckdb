#!/bin/bash
rm -rf gen-cpp
thrift --gen "cpp:moveable_types,no_default_operators=true" parquet.thrift
cp gen-cpp/* .
sed -i .bak -e "s/std::vector/duckdb::vector/" parquet_types.*
sed -i .bak -e "s/namespace parquet/namespace duckdb_parquet/" parquet_types.*
sed -i .bak -e 's/namespace duckdb_parquet {/#include "windows_compatibility.h"\nnamespace apache = duckdb_apache;\n\nnamespace duckdb_parquet {/' parquet_types.h
sed -i .bak '/namespace duckdb_parquet {/a\
\
template <class ENUM>\
static typename ENUM::type SafeEnumCast(const std::map<int, const char*> &values_to_names, const int &ecast) {\
  if (values_to_names.find(ecast) == values_to_names.end()) {\
    throw duckdb_apache::thrift::protocol::TProtocolException(duckdb_apache::thrift::protocol::TProtocolException::INVALID_DATA);\
  }\
  return static_cast<typename ENUM::type>(ecast);\
}\
' parquet_types.cpp
sed -i .bak -e 's/static_cast<Type::type>(/SafeEnumCast<Type>(_Type_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<ConvertedType::type>(/SafeEnumCast<ConvertedType>(_ConvertedType_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<FieldRepetitionType::type>(/SafeEnumCast<FieldRepetitionType>(_FieldRepetitionType_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<EdgeInterpolationAlgorithm::type>(/SafeEnumCast<EdgeInterpolationAlgorithm>(_EdgeInterpolationAlgorithm_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<Encoding::type>(/SafeEnumCast<Encoding>(_Encoding_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<CompressionCodec::type>(/SafeEnumCast<CompressionCodec>(_CompressionCodec_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<PageType::type>(/SafeEnumCast<PageType>(_PageType_VALUES_TO_NAMES, /' parquet_types.*
sed -i .bak -e 's/static_cast<BoundaryOrder::type>(/SafeEnumCast<BoundaryOrder>(_BoundaryOrder_VALUES_TO_NAMES, /' parquet_types.*
python3 arena_transform.py parquet_types.h parquet_types.cpp
rm *.bak
rm -rf gen-cpp