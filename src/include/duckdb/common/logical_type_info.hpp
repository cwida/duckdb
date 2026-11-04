//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/logical_type_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/types/geometry_crs.hpp"

namespace duckdb {

class ParsedExpression;

struct DecimalTypeInfo : public LogicalTypeInfo {
	DecimalTypeInfo(uint8_t width_p, uint8_t scale_p);

	uint8_t width;
	uint8_t scale;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	DecimalTypeInfo();
};

struct StringTypeInfo : public LogicalTypeInfo {
	explicit StringTypeInfo(string collation_p);

	string collation;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	StringTypeInfo();
};

struct ListTypeInfo : public LogicalTypeInfo {
	explicit ListTypeInfo(LogicalType child_type_p);

	LogicalType child_type;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	ListTypeInfo();
};

struct StructTypeInfo : public LogicalTypeInfo {
	explicit StructTypeInfo(child_list_t<LogicalType> child_types_p);

	child_list_t<LogicalType> child_types;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &deserializer);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	StructTypeInfo();
};

struct LegacyAggregateStateTypeInfo : public LogicalTypeInfo {
public:
	void Serialize(Serializer &serializer) const override;
	// Legacy deserialize method kept only for compatibility with old database files
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);

	static unique_ptr<LogicalTypeInfo> LegacyDeserialize();

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	LegacyAggregateStateTypeInfo();
};

// If this type is primarily stored in the catalog or not. Enums from Pandas/Factors are not in the catalog.
enum EnumDictType : uint8_t { INVALID = 0, VECTOR_DICT = 1 };

struct EnumTypeInfo : public LogicalTypeInfo {
	explicit EnumTypeInfo(const Vector &values_insert_order_p, idx_t dict_size_p);
	EnumTypeInfo(const EnumTypeInfo &) = delete;
	EnumTypeInfo &operator=(const EnumTypeInfo &) = delete;

public:
	const EnumDictType &GetEnumDictType() const;
	const Vector &GetValuesInsertOrder() const;
	const idx_t &GetDictSize() const;
	static PhysicalType DictType(idx_t size);
	[[noreturn]] static void ThrowDuplicateValue(const string_t &value);
	[[noreturn]] static void ThrowCorruptedIndex(idx_t index, idx_t size);
	[[noreturn]] static void ThrowCorruptedCount(idx_t size, idx_t read_count);

	static LogicalType CreateType(const Vector &ordered_data, idx_t size);
	static unique_ptr<LogicalTypeInfo> CreateTypeInfo(const Vector &ordered_data, idx_t size);

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	// Equalities are only used in enums with different catalog entries
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

	Vector values_insert_order;

private:
	EnumDictType dict_type;
	idx_t dict_size;
};

struct ArrayTypeInfo : public LogicalTypeInfo {
	LogicalType child_type;
	uint32_t size;
	explicit ArrayTypeInfo(LogicalType child_type_p, uint32_t size_p);

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &reader);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;
};

struct AnyTypeInfo : public LogicalTypeInfo {
	AnyTypeInfo(LogicalType target_type, idx_t cast_score);

	LogicalType target_type;
	idx_t cast_score;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	AnyTypeInfo();
};

struct IntegerLiteralTypeInfo : public LogicalTypeInfo {
	explicit IntegerLiteralTypeInfo(Value constant_value);

	Value constant_value;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	IntegerLiteralTypeInfo();
};

struct TemplateTypeInfo : public LogicalTypeInfo {
	explicit TemplateTypeInfo(string name_p);

	// The name of the template, e.g. `T`, or `KEY_TYPE`. Used to distinguish between different template types within
	// the same function. The binder tries to resolve all templates with the same name to the same concrete type.
	string name;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;
	TemplateTypeInfo();
};

struct GeoTypeInfo : public LogicalTypeInfo {
public:
	GeoTypeInfo();

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

	// The Coordinate Reference System associated with this geometry type
	CoordinateReferenceSystem crs;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;
};

struct UnboundTypeInfo : public LogicalTypeInfo {
	explicit UnboundTypeInfo(unique_ptr<ParsedExpression> expr_p);

	unique_ptr<ParsedExpression> expr;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	unique_ptr<LogicalTypeInfo> Copy() const override;

protected:
	bool EqualsInternal(const LogicalTypeInfo *other_p) const override;

private:
	UnboundTypeInfo();
};

} // namespace duckdb
