#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/vector/string_vector.hpp"

namespace duckdb {

template <class T>
struct EnumTypeInfoTemplated : public EnumTypeInfo {
	explicit EnumTypeInfoTemplated(const Vector &values_insert_order_p, idx_t size_p)
	    : EnumTypeInfo(values_insert_order_p, size_p) {
		D_ASSERT(values_insert_order_p.GetType().InternalType() == PhysicalType::VARCHAR);

		UnifiedVectorFormat vdata;
		values_insert_order.ToUnifiedFormat(vdata);

		auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
		for (idx_t i = 0; i < size_p; i++) {
			auto idx = vdata.sel->get_index(i);
			if (!vdata.validity.RowIsValid(idx)) {
				InternalException::Throw("Attempted to create ENUM type with NULL value");
			}
			if (values.count(data[idx]) > 0) {
				ThrowDuplicateValue(data[idx]);
			}
			values[data[idx]] = UnsafeNumericCast<T>(i);
		}
	}

	static unique_ptr<EnumTypeInfoTemplated> Deserialize(Deserializer &deserializer, uint32_t size) {
		Vector values_insert_order(LogicalType::VARCHAR, size);
		auto strings = FlatVector::ScatterWriter<string_t>(values_insert_order);

		idx_t read_count = 0;
		deserializer.ReadList(201, "values", [&](Deserializer::List &list, idx_t i) {
			if (i >= size) {
				ThrowCorruptedIndex(i, size);
			}
			strings[i] = list.ReadElement<string>();
			read_count++;
		});
		if (read_count != size) {
			ThrowCorruptedCount(size, read_count);
		}
		return make_uniq<EnumTypeInfoTemplated>(values_insert_order, size);
	}

	const string_map_t<T> &GetValues() const {
		return values;
	}

	EnumTypeInfoTemplated(const EnumTypeInfoTemplated &) = delete;
	EnumTypeInfoTemplated &operator=(const EnumTypeInfoTemplated &) = delete;

private:
	string_map_t<T> values;
};

} // namespace duckdb
