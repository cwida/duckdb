#include "duckdb/function/cast_rules.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

//! The target type determines the preferred implicit casts
static int64_t TargetTypeCost(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BIGINT:
		return 101;
	case LogicalTypeId::INTEGER:
		return 102;
	case LogicalTypeId::HUGEINT:
		return 103;
	case LogicalTypeId::DOUBLE:
		return 104;
	case LogicalTypeId::DECIMAL:
		return 105;
	case LogicalTypeId::BIGNUM:
		return 106;
	case LogicalTypeId::TIMESTAMP_NS:
		return 119;
	case LogicalTypeId::TIMESTAMP:
		return 120;
	case LogicalTypeId::TIMESTAMP_MS:
		return 121;
	case LogicalTypeId::TIMESTAMP_SEC:
		return 122;
	case LogicalTypeId::TIMESTAMP_TZ:
		return 123;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return 124;
	case LogicalTypeId::VARCHAR:
		return 149;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	case LogicalTypeId::UNION:
	case LogicalTypeId::ARRAY:
		return 160;
	case LogicalTypeId::ANY:
		return int64_t(AnyType::GetCastScore(type));
	case LogicalTypeId::GEOGRAPHY:
		// Cost slightly higher than GEOMETRY (default) so that untyped NULL/literal arguments prefer the
		// GEOMETRY overload of functions overloaded on both, instead of being ambiguous.
		return 111;
	case LogicalTypeId::TEMPLATE:
		// we can cast anything to a template type, but prefer to cast to anything else!
		return 1000000;
	default:
		return 110;
	}
}

static int64_t ImplicitCastTinyint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastSmallint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastInteger(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastBigint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastUTinyint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastUSmallint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastUInteger(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastUBigint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastFloat(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::DOUBLE:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastDouble(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastDecimal(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastHugeint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastUhugeint(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BIGNUM:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastDate(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastEnum(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::VARCHAR:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastTimestampSec(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastTimestampMS(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastTimestampNS(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::TIMESTAMP:
		// we allow casting ALL timestamps, including nanosecond ones, to TimestampNS
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastTimestamp(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::TIMESTAMP_NS:
		return TargetTypeCost(to);
	case LogicalTypeId::TIMESTAMP_TZ:
		return TargetTypeCost(to);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastBignum(const LogicalType &to) {
	switch (to.id()) {
	case LogicalTypeId::DOUBLE:
		return TargetTypeCost(to);
	default:
		return -1;
	}
}

static int64_t ImplicitCastVariant(const LogicalType &to) {
	return TargetTypeCost(to);
}

bool LogicalTypeIsValid(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::UNION:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::DECIMAL:
		// these types are only valid with auxiliary info
		if (!type.HasParameters()) {
			return false;
		}
		break;
	default:
		break;
	}
	switch (type.id()) {
	case LogicalTypeId::ANY:
	case LogicalTypeId::INVALID:
	case LogicalTypeId::UNKNOWN:
		return false;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto child_count = StructType::GetChildCount(type);
		for (idx_t i = 0; i < child_count; i++) {
			if (!LogicalTypeIsValid(StructType::GetChildType(type, i))) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::UNION: {
		auto member_count = UnionType::GetMemberCount(type);
		for (idx_t i = 0; i < member_count; i++) {
			if (!LogicalTypeIsValid(UnionType::GetMemberType(type, i))) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return LogicalTypeIsValid(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return LogicalTypeIsValid(ArrayType::GetChildType(type));
	default:
		return true;
	}
}

int64_t ImplicitCastToUnionMember(const LogicalType &from, const LogicalType &to) {
	// check that the union type is fully resolved.
	if (!to.HasParameters()) {
		return -1;
	}
	// check if the union contains something castable from the source type
	// in which case the least expensive (most specific) cast should be used
	bool found = false;
	auto cost = NumericLimits<int64_t>::Maximum();
	for (idx_t i = 0; i < UnionType::GetMemberCount(to); i++) {
		auto target_member = UnionType::GetMemberType(to, i);
		auto target_cost = CastRules::ImplicitCast(from, target_member);
		if (target_cost != -1) {
			found = true;
			cost = MinValue(cost, target_cost);
		}
	}
	return found ? cost : -1;
}

int64_t CastRules::ImplicitCast(const LogicalType &from, const LogicalType &to) {
	if (from.id() == LogicalTypeId::SQLNULL && to.id() == LogicalTypeId::TEMPLATE) {
		// Prefer the TEMPLATE type for NULL casts, as it is the most generic
		return 5;
	}
	if (from.id() == LogicalTypeId::SQLNULL || to.id() == LogicalTypeId::ANY || to.id() == LogicalTypeId::TEMPLATE) {
		// NULL expression can be cast to anything
		return TargetTypeCost(to);
	}
	if (from.id() == LogicalTypeId::ANY && to.IsTemplated()) {
		// This can happen when changing a function from using ANY to using TEMPLATE.
		return TargetTypeCost(to);
	}
	if (from.id() == LogicalTypeId::UNKNOWN) {
		// parameter expression can be cast to anything for no cost
		return 0;
	}
	if (from.id() == LogicalTypeId::STRING_LITERAL) {
		// string literals can be cast to any type for low cost as long as the type is valid
		// i.e. we cannot cast to LIST(ANY) as we don't know what "ANY" should be
		// we cannot cast to DECIMAL without precision/width specified
		if (!LogicalTypeIsValid(to)) {
			return -1;
		}
		if (to.id() == LogicalTypeId::VARCHAR && to.GetAlias().empty()) {
			return 1;
		}
		if (to.id() == LogicalTypeId::GEOGRAPHY) {
			// Prefer GEOMETRY over GEOGRAPHY for untyped string literals (see TargetTypeCost), so functions
			// overloaded on both resolve unambiguously.
			return 21;
		}
		return 20;
	}
	if (from.id() == LogicalTypeId::INTEGER_LITERAL) {
		// the integer literal has an underlying type - this type always matches
		if (IntegerLiteral::GetType(from).id() == to.id()) {
			return 0;
		}
		// integer literals can be cast to any other integer type for a low cost, but only if the literal fits
		if (IntegerLiteral::FitsInType(from, to)) {
			// to avoid ties we prefer BIGINT, INT, ...
			auto target_cost = TargetTypeCost(to);
			if (target_cost < 100) {
				throw InternalException("Integer literal implicit cast - TargetTypeCost should be >= 100");
			}
			return target_cost - 90;
		}
		// in any other case we use the casting rules of the preferred type of the literal
		return CastRules::ImplicitCast(IntegerLiteral::GetType(from), to);
	}

	if (from.GetAlias() != to.GetAlias()) {
		// if aliases are different, an implicit cast is not possible

		// Special case: Anything can be cast to a union if the source type is a member of the union
		if (to.id() == LogicalTypeId::UNION) {
			return ImplicitCastToUnionMember(from, to);
		}

		return -1;
	}
	if (from.id() == LogicalTypeId::LIST && to.id() == LogicalTypeId::LIST) {
		// Lists can be cast if their child types can be cast
		auto child_cost = ImplicitCast(ListType::GetChildType(from), ListType::GetChildType(to));
		if (child_cost >= 1) {
			// subtract one from the cost because we prefer LIST[X] -> LIST[VARCHAR] over LIST[X] -> VARCHAR
			child_cost--;
		}
		return child_cost;
	}
	if (from.id() == LogicalTypeId::ARRAY && to.id() == LogicalTypeId::ARRAY) {
		// Arrays can be cast if their child types can be cast and the source and target has the same size
		// or the target type has a unknown (any) size.
		auto from_size = ArrayType::GetSize(from);
		auto to_size = ArrayType::GetSize(to);
		auto to_is_any_size = ArrayType::IsAnySize(to);
		if (from_size == to_size || to_is_any_size) {
			auto child_cost = ImplicitCast(ArrayType::GetChildType(from), ArrayType::GetChildType(to));
			if (child_cost >= 100) {
				// subtract one from the cost because we prefer ARRAY[X] -> ARRAY[VARCHAR] over ARRAY[X] -> VARCHAR
				child_cost--;
			}
			return child_cost;
		}
		return -1; // Not possible if the sizes are different
	}
	if (from.id() == LogicalTypeId::ARRAY && to.id() == LogicalTypeId::LIST) {
		// Arrays can be cast to lists for the cost of casting the child type
		auto child_cost = ImplicitCast(ArrayType::GetChildType(from), ListType::GetChildType(to));
		if (child_cost < 0) {
			return -1;
		}
		// add 1 because we prefer ARRAY->ARRAY casts over ARRAY->LIST casts
		return child_cost + 1;
	}
	if (from.id() == LogicalTypeId::LIST && (to.id() == LogicalTypeId::ARRAY && !ArrayType::IsAnySize(to))) {
		// Lists can be cast to arrays for the cost of casting the child type, if the target size is known
		// there is no way for us to resolve the size at bind-time without inspecting the list values.
		// TODO: if we can access the expression we could resolve the size if the list is constant.
		return ImplicitCast(ListType::GetChildType(from), ArrayType::GetChildType(to));
	}
	if (from.id() == LogicalTypeId::UNION && to.id() == LogicalTypeId::UNION) {
		// Check that the target union type is fully resolved.
		if (!to.HasParameters()) {
			// If not, try anyway and let the actual cast logic handle it.
			// This is to allow passing unions into functions that take a generic union type (without specifying member
			// types) as an argument.
			return 0;
		}
		// Unions can be cast if the source tags are a subset of the target tags
		// in which case the most expensive cost is used
		int64_t cost = -1;
		for (idx_t from_member_idx = 0; from_member_idx < UnionType::GetMemberCount(from); from_member_idx++) {
			auto &from_member_name = UnionType::GetMemberName(from, from_member_idx);

			bool found = false;
			for (idx_t to_member_idx = 0; to_member_idx < UnionType::GetMemberCount(to); to_member_idx++) {
				auto &to_member_name = UnionType::GetMemberName(to, to_member_idx);

				if (from_member_name == to_member_name) {
					auto &from_member_type = UnionType::GetMemberType(from, from_member_idx);
					auto &to_member_type = UnionType::GetMemberType(to, to_member_idx);

					auto child_cost = ImplicitCast(from_member_type, to_member_type);
					cost = MaxValue(cost, child_cost);
					found = true;
					break;
				}
			}
			if (!found) {
				return -1;
			}
		}
		// If not found, try casting source to member of target (below)
		if (cost != -1) {
			return cost;
		}
	}
	if (StructType::IsStruct(from) && StructType::IsStruct(to)) {
		if (!to.HasParameters()) {
			// If this struct is not fully resolved, we'll leave it to the actual cast logic to handle it.
			return 0;
		}

		auto &source_children = StructType::GetChildTypes(from);
		auto &target_children = StructType::GetChildTypes(to);

		if (source_children.size() != target_children.size()) {
			// different number of children: not possible
			return -1;
		}

		if (source_children.empty()) {
			// both are empty - empty STRUCT and empty TUPLE are trivially inter-castable
			return 0;
		}

		auto target_is_unnamed = StructType::IsUnnamed(to);
		auto source_is_unnamed = StructType::IsUnnamed(from);
		auto named_struct_cast = !source_is_unnamed && !target_is_unnamed;

		int64_t cost = -1;
		if (named_struct_cast) {
			// Collect the target members in a map for easy lookup
			identifier_map_t<idx_t> target_members;
			for (idx_t target_idx = 0; target_idx < target_children.size(); target_idx++) {
				auto &target_name = target_children[target_idx].first;
				if (target_members.find(target_name) != target_members.end()) {
					// duplicate name in target struct
					return -1;
				}
				target_members[target_name] = target_idx;
			}
			// Match the source members to the target members by name
			for (idx_t source_idx = 0; source_idx < source_children.size(); source_idx++) {
				auto &source_child = source_children[source_idx];
				auto entry = target_members.find(source_child.first);
				if (entry == target_members.end()) {
					// element in source struct was not found in target struct
					return -1;
				}
				auto target_idx = entry->second;
				target_members.erase(entry);
				auto child_cost = ImplicitCast(source_child.second, target_children[target_idx].second);
				if (child_cost == -1) {
					return -1;
				}
				cost = MaxValue(cost, child_cost);
			}
		} else {
			// Match the source members to the target members by position
			for (idx_t i = 0; i < source_children.size(); i++) {
				auto &source_child = source_children[i];
				auto &target_child = target_children[i];
				auto child_cost = ImplicitCast(source_child.second, target_child.second);
				if (child_cost == -1) {
					return -1;
				}
				cost = MaxValue(cost, child_cost);
			}
		}
		return cost;
	}

	if (from.id() == to.id()) {
		// arguments match: do nothing
		return 0;
	}
	// Special case: Anything can be cast to a union if the source type is a member of the union
	if (to.id() == LogicalTypeId::UNION) {
		return ImplicitCastToUnionMember(from, to);
	}

	switch (from.id()) {
	case LogicalTypeId::TINYINT:
		return ImplicitCastTinyint(to);
	case LogicalTypeId::SMALLINT:
		return ImplicitCastSmallint(to);
	case LogicalTypeId::INTEGER:
		return ImplicitCastInteger(to);
	case LogicalTypeId::BIGINT:
		return ImplicitCastBigint(to);
	case LogicalTypeId::UTINYINT:
		return ImplicitCastUTinyint(to);
	case LogicalTypeId::USMALLINT:
		return ImplicitCastUSmallint(to);
	case LogicalTypeId::UINTEGER:
		return ImplicitCastUInteger(to);
	case LogicalTypeId::UBIGINT:
		return ImplicitCastUBigint(to);
	case LogicalTypeId::HUGEINT:
		return ImplicitCastHugeint(to);
	case LogicalTypeId::UHUGEINT:
		return ImplicitCastUhugeint(to);
	case LogicalTypeId::FLOAT:
		return ImplicitCastFloat(to);
	case LogicalTypeId::DOUBLE:
		return ImplicitCastDouble(to);
	case LogicalTypeId::DATE:
		return ImplicitCastDate(to);
	case LogicalTypeId::DECIMAL:
		return ImplicitCastDecimal(to);
	case LogicalTypeId::ENUM:
		return ImplicitCastEnum(to);
	case LogicalTypeId::TIMESTAMP_SEC:
		return ImplicitCastTimestampSec(to);
	case LogicalTypeId::TIMESTAMP_MS:
		return ImplicitCastTimestampMS(to);
	case LogicalTypeId::TIMESTAMP_NS:
		return ImplicitCastTimestampNS(to);
	case LogicalTypeId::TIMESTAMP:
		return ImplicitCastTimestamp(to);
	case LogicalTypeId::BIGNUM:
		return ImplicitCastBignum(to);
	case LogicalTypeId::VARIANT:
		return ImplicitCastVariant(to);
	default:
		return -1;
	}
}

} // namespace duckdb
