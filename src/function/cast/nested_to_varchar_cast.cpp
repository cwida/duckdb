#include "duckdb/function/cast/nested_to_varchar_cast.hpp"

#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {

const bool NestedToVarcharCast::LOOKUP_TABLE[256] = {
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false,
    true, // "
    false, false, false, false,
    true, // '
    true, // (
    true, // )
    false, false,
    true, // ,
    false, false, false, false, false, false, false, false, false, false, false, false, false,
    true, // :
    false, false,
    true, // =
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    true, // [
    true, // backslash
    true, // ]
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    true, // {
    false,
    true, // }
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false};

namespace {

struct QuotedChildCastData : public BoundCastData {
	QuotedChildCastData(BoundCastInfo child_cast_p, LogicalType child_type_p)
	    : child_cast(std::move(child_cast_p)), child_type(std::move(child_type_p)) {
	}

	BoundCastInfo child_cast;
	LogicalType child_type;

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<QuotedChildCastData>(child_cast.Copy(), child_type);
	}
};

unique_ptr<FunctionLocalState> InitQuotedChildLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<QuotedChildCastData>();
	if (!cast_data.child_cast.HasInitLocalState()) {
		return nullptr;
	}
	CastLocalStateParameters child_parameters(parameters, cast_data.child_cast.GetCastData());
	return cast_data.child_cast.InitLocalState(child_parameters);
}

//! Marks the rows that are written as they are: NULL values, and nested values (a nested UNION member, or a VARIANT
//! OBJECT or ARRAY)
void GetUnquotedRows(const Vector &source, idx_t count, vector<bool> &is_unquoted) {
	is_unquoted.assign(count, true);
	auto &type = source.GetType();
	if (type.id() == LogicalTypeId::UNION) {
		vector<unique_ptr<VectorValidityIterator>> member_validity;
		for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(type); member_idx++) {
			member_validity.push_back(make_uniq<VectorValidityIterator>(UnionVector::GetMember(source, member_idx)));
		}
		auto tags = UnionVector::GetTags(source).Values<union_tag_t>();
		for (idx_t i = 0; i < count; i++) {
			auto tag = tags[i];
			if (!tag.IsValid()) {
				continue;
			}
			auto member_idx = tag.GetValue();
			is_unquoted[i] =
			    UnionType::GetMemberType(type, member_idx).IsNested() || !member_validity[member_idx]->IsValid(i);
		}
		return;
	}
	D_ASSERT(type.id() == LogicalTypeId::VARIANT);
	VectorIterator<VectorVariantType> variants(source);
	for (idx_t i = 0; i < count; i++) {
		if (!variants.RowIsValid(i)) {
			continue;
		}
		auto value = variants[i];
		if (value.IsNull() || value.IsMissing()) {
			continue;
		}
		auto type_id = value.GetTypeId();
		is_unquoted[i] = type_id == VariantLogicalType::OBJECT || type_id == VariantLogicalType::ARRAY;
	}
}

bool QuotedChildCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<QuotedChildCastData>();
	Vector varchar_values(LogicalType::VARCHAR, count);
	CastParameters child_parameters(parameters, cast_data.child_cast.GetCastData(), parameters.local_state);
	auto all_ok = cast_data.child_cast.Cast(source, varchar_values, count, child_parameters);

	vector<bool> is_unquoted;
	GetUnquotedRows(source, count, is_unquoted);

	auto values = varchar_values.Values<string_t>();
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto entry = values[i];
		if (!entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto &value = entry.GetValue();
		if (is_unquoted[i]) {
			writer.WriteValue(value);
			continue;
		}
		bool needs_quotes;
		auto length = VectorCastHelpers::CalculateEscapedStringLength<false>(value, needs_quotes);
		auto &result_str = writer.WriteEmptyString(length);
		VectorCastHelpers::WriteEscapedString<false>(result_str.GetDataWriteable(), value, needs_quotes);
		result_str.Finalize();
	}
	return all_ok;
}

} // namespace

void NestedToVarcharCast::WrapChildCast(BoundCastInfo &child_cast, const LogicalType &child_type) {
	if (child_type.id() != LogicalTypeId::UNION && child_type.id() != LogicalTypeId::VARIANT) {
		return;
	}
	auto stability = child_cast.GetStability();
	child_cast = BoundCastInfo(QuotedChildCast, make_uniq<QuotedChildCastData>(std::move(child_cast), child_type),
	                           InitQuotedChildLocalState);
	child_cast.SetStability(stability);
}

} // namespace duckdb
