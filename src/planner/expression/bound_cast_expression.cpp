#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

enum class CastBindSource : uint8_t { CONTEXT, DEFAULT };

static unique_ptr<Expression> AddCastExpressionInternal(unique_ptr<Expression> expr, const LogicalType &target_type,
                                                        BoundCastInfo bound_cast, bool try_cast,
                                                        CastBindSource bind_source) {
	if (ExpressionBinder::GetExpressionReturnType(*expr) == target_type) {
		return expr;
	}
	auto &expr_type = expr->GetReturnType();
	if (target_type.id() == LogicalTypeId::LIST && expr_type.id() == LogicalTypeId::LIST) {
		auto &target_list = ListType::GetChildType(target_type);
		auto &expr_list = ListType::GetChildType(expr_type);
		if (target_list.id() == LogicalTypeId::ANY || expr_list == target_list) {
			return expr;
		}
	}
	if (bind_source == CastBindSource::DEFAULT) {
		return BoundCastExpression::CreateDefault(std::move(expr), target_type, std::move(bound_cast), try_cast);
	}
	return BoundCastExpression::Create(std::move(expr), target_type, std::move(bound_cast), try_cast);
}

static unique_ptr<Expression> AddCastToTypeInternal(unique_ptr<Expression> expr, const LogicalType &target_type,
                                                    CastFunctionSet &cast_functions, GetCastFunctionInput &get_input,
                                                    bool try_cast, CastBindSource bind_source) {
	D_ASSERT(expr);
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_PARAMETER) {
		auto &parameter = expr->Cast<BoundParameterExpression>();
		if (!target_type.IsValid()) {
			// invalidate the parameter
			parameter.ParameterData()->return_type = LogicalType::INVALID;
			parameter.SetReturnType(target_type);
			return expr;
		}
		if (parameter.ParameterData()->return_type.id() == LogicalTypeId::INVALID) {
			// we don't know the type of this parameter
			parameter.SetReturnType(target_type);
			return expr;
		}
		if (parameter.ParameterData()->return_type.id() == LogicalTypeId::UNKNOWN) {
			// prepared statement parameter cast - but there is no type, convert the type
			parameter.ParameterData()->return_type = target_type;
			parameter.SetReturnType(target_type);
			return expr;
		}
		// prepared statement parameter already has a type
		if (parameter.ParameterData()->return_type == target_type) {
			// this type! we are done
			parameter.SetReturnType(parameter.ParameterData()->return_type);
			return expr;
		}
		// If this occurrence's own return_type still matches parameter_data->return_type, the
		// parameter was pinned by an inner cast on this same occurrence (e.g. CAST(CAST($1 AS A) AS B)).
		// Add a regular cast on top instead of invalidating.
		if (parameter.GetReturnType() == parameter.ParameterData()->return_type) {
			auto cast_function = cast_functions.GetCastFunction(parameter.GetReturnType(), target_type, get_input);
			return AddCastExpressionInternal(std::move(expr), target_type, std::move(cast_function), try_cast,
			                                 bind_source);
		}
		// invalidate the type
		parameter.ParameterData()->return_type = LogicalType::INVALID;
		parameter.SetReturnType(target_type);
		return expr;
	} else if (expr->GetExpressionClass() == ExpressionClass::BOUND_DEFAULT) {
		D_ASSERT(target_type.IsValid());
		auto &def = expr->Cast<BoundDefaultExpression>();
		def.SetReturnType(target_type);
	}
	if (!target_type.IsValid()) {
		return expr;
	}

	auto cast_function = cast_functions.GetCastFunction(expr->GetReturnType(), target_type, get_input);
	return AddCastExpressionInternal(std::move(expr), target_type, std::move(cast_function), try_cast, bind_source);
}

unique_ptr<Expression> BoundCastExpression::AddDefaultCastToType(unique_ptr<Expression> expr,
                                                                 const LogicalType &target_type, bool try_cast) {
	CastFunctionSet default_set;
	GetCastFunctionInput get_input;
	get_input.query_location = expr->GetQueryLocation();
	return AddCastToTypeInternal(std::move(expr), target_type, default_set, get_input, try_cast,
	                             CastBindSource::DEFAULT);
}

unique_ptr<Expression> BoundCastExpression::AddCastToType(ClientContext &context, unique_ptr<Expression> expr,
                                                          const LogicalType &target_type, bool try_cast) {
	auto &cast_functions = DBConfig::GetConfig(context).GetCastFunctions();
	GetCastFunctionInput get_input(context);
	get_input.query_location = expr->GetQueryLocation();
	return AddCastToTypeInternal(std::move(expr), target_type, cast_functions, get_input, try_cast,
	                             CastBindSource::CONTEXT);
}

unique_ptr<Expression> BoundCastExpression::AddArrayCastToList(ClientContext &context, unique_ptr<Expression> expr) {
	if (expr->GetReturnType().id() != LogicalTypeId::ARRAY) {
		return expr;
	}
	auto &child_type = ArrayType::GetChildType(expr->GetReturnType());
	return BoundCastExpression::AddCastToType(context, std::move(expr), LogicalType::LIST(child_type));
}

//! Whether a cast between nested types is invertible: the structure has to stay the same, and all child casts have to
//! be invertible
static bool NestedCastIsInvertible(const LogicalType &source_type, const LogicalType &target_type) {
	if (source_type.id() != target_type.id()) {
		return false;
	}
	switch (source_type.id()) {
	case LogicalTypeId::LIST:
		return BoundCastExpression::CastIsInvertible(ListType::GetChildType(source_type),
		                                             ListType::GetChildType(target_type));
	case LogicalTypeId::ARRAY:
		if (ArrayType::IsAnySize(source_type) || ArrayType::IsAnySize(target_type) ||
		    ArrayType::GetSize(source_type) != ArrayType::GetSize(target_type)) {
			return false;
		}
		return BoundCastExpression::CastIsInvertible(ArrayType::GetChildType(source_type),
		                                             ArrayType::GetChildType(target_type));
	case LogicalTypeId::MAP:
		return BoundCastExpression::CastIsInvertible(MapType::KeyType(source_type), MapType::KeyType(target_type)) &&
		       BoundCastExpression::CastIsInvertible(MapType::ValueType(source_type), MapType::ValueType(target_type));
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		// adding or removing fields is not invertible
		auto &source_children = StructType::GetChildTypes(source_type);
		auto &target_children = StructType::GetChildTypes(target_type);
		if (source_children.size() != target_children.size()) {
			return false;
		}
		for (idx_t child_idx = 0; child_idx < source_children.size(); child_idx++) {
			if (source_children[child_idx].first != target_children[child_idx].first ||
			    !BoundCastExpression::CastIsInvertible(source_children[child_idx].second,
			                                           target_children[child_idx].second)) {
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

bool BoundCastExpression::CastIsInvertible(const LogicalType &source_type, const LogicalType &target_type) {
	D_ASSERT(source_type.IsValid() && target_type.IsValid());
	if (source_type.id() == LogicalTypeId::BOOLEAN || target_type.id() == LogicalTypeId::BOOLEAN) {
		return false;
	}
	if (source_type.id() == LogicalTypeId::FLOAT || target_type.id() == LogicalTypeId::FLOAT) {
		return false;
	}
	if (source_type.id() == LogicalTypeId::DOUBLE || target_type.id() == LogicalTypeId::DOUBLE) {
		return false;
	}
	if (source_type.id() == LogicalTypeId::VARIANT || target_type.id() == LogicalTypeId::VARIANT) {
		return false;
	}
	if (source_type.IsNested() || target_type.IsNested()) {
		return NestedCastIsInvertible(source_type, target_type);
	}
	if (source_type.id() == LogicalTypeId::ENUM && target_type.id() == LogicalTypeId::ENUM) {
		return false;
	}
	if (source_type.id() == LogicalTypeId::DECIMAL || target_type.id() == LogicalTypeId::DECIMAL) {
		uint32_t source_width, target_width;
		uint32_t source_scale, target_scale;
		// cast to or from decimal
		// cast is only invertible if the cast is strictly widening
		if (!source_type.GetDecimalProperties(source_width, source_scale)) {
			return false;
		}
		if (!target_type.GetDecimalProperties(target_width, target_scale)) {
			return false;
		}
		if (target_scale < source_scale) {
			return false;
		}
		if (target_type.IsUnsigned()) {
			// decimals can be negative
			return false;
		}
		// the target needs to fit all digits before the decimal point
		// integral types do not fit all numbers of their decimal width, e.g. SMALLINT (width 5) does not fit 99999
		auto source_digits = source_width - source_scale;
		auto target_digits = target_width - target_scale;
		if (source_type.id() == LogicalTypeId::HUGEINT || source_type.id() == LogicalTypeId::UHUGEINT) {
			// (u)hugeint values can have up to 39 digits
			source_digits++;
		}
		if (target_type.id() != LogicalTypeId::DECIMAL) {
			target_digits--;
		}
		return source_digits <= target_digits;
	}
	switch (source_type.id()) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		switch (target_type.id()) {
			// see types.hpp to see timestamp ranking
		case LogicalTypeId::TIMESTAMP:
			return source_type.id() <= LogicalTypeId::TIMESTAMP;
		case LogicalTypeId::TIMESTAMP_SEC:
			return source_type.id() <= LogicalTypeId::TIMESTAMP_SEC;
		case LogicalTypeId::TIMESTAMP_MS:
			return source_type.id() <= LogicalTypeId::TIMESTAMP_MS;
		case LogicalTypeId::TIMESTAMP_NS:
			return source_type.id() <= LogicalTypeId::TIMESTAMP_NS;
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIME_TZ:
			return false;
		case LogicalTypeId::TIMESTAMP_TZ:
			return source_type.id() == LogicalTypeId::TIMESTAMP_TZ;
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return LogicalTypeId::TIMESTAMP_TZ <= source_type.id() &&
			       source_type.id() <= LogicalTypeId::TIMESTAMP_TZ_NS;
		default:
			break;
		}
		break;
	case LogicalTypeId::TIME_NS:
		// a cast to microseconds rounds the nanoseconds
		if (target_type.id() == LogicalTypeId::TIME) {
			return false;
		}
		break;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BIT:
	case LogicalTypeId::TIME_TZ:
		return false;
	default:
		break;
	}
	if (target_type.id() == LogicalTypeId::VARCHAR) {
		switch (source_type.id()) {
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIME_NS:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIME_TZ:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return true;
		default:
			return false;
		}
	}
	if (source_type.IsSigned() && target_type.IsUnsigned()) {
		return false;
	}
	if (source_type.IsIntegral() && target_type.IsIntegral()) {
		// a narrowing integer cast is not invertible: a source value outside the target's range does not
		// survive the cast, so comparing in the target type is not the same as comparing in the source type
		auto source_size = GetTypeIdSize(source_type.InternalType());
		auto target_size = GetTypeIdSize(target_type.InternalType());
		if (source_type.IsUnsigned() && target_type.IsSigned()) {
			// the sign bit costs a byte of range, so equal widths are not enough
			return target_size > source_size;
		}
		return target_size >= source_size;
	}
	return true;
}

static bool IsExactNumeric(const LogicalType &type) {
	return type.IsIntegral() || type.id() == LogicalTypeId::DECIMAL;
}

//! Whether an integral or DECIMAL source fits in the digits10 of the floating target, so that distinct source values
//! stay distinct floating values
static bool IsLosslessExactNumericToFloatingCast(const LogicalType &source_type, const LogicalType &target_type) {
	uint32_t width, scale;
	if (!IsExactNumeric(source_type) || !source_type.GetDecimalProperties(width, scale)) {
		return false;
	}
	switch (target_type.id()) {
	case LogicalTypeId::DOUBLE:
		return width <= std::numeric_limits<double>::digits10;
	case LogicalTypeId::FLOAT:
		return width <= std::numeric_limits<float>::digits10;
	default:
		return false;
	}
}

bool BoundCastExpression::IsLosslessIntegralToFloatingCast(const LogicalType &source_type,
                                                           const LogicalType &target_type) {
	return source_type.IsIntegral() && IsLosslessExactNumericToFloatingCast(source_type, target_type);
}

//! Whether every value of the type has a unique VARCHAR representation that casts back to the same value
static bool HasCanonicalVarcharRepresentation(const LogicalType &type) {
	if (type.IsIntegral()) {
		return true;
	}
	// unlike CastIsInvertible, TZ types are excluded: equal TIME_TZ values with different offsets render differently,
	// and the rendering of TIMESTAMP_TZ depends on the TimeZone setting
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::UUID:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

bool BoundCastExpression::CastIsInvertibleForEquality(const LogicalType &source_type, const LogicalType &target_type) {
	if (target_type.id() == LogicalTypeId::VARCHAR) {
		// a collation can make distinct strings equal
		return HasCanonicalVarcharRepresentation(source_type) && StringType::GetCollation(target_type).empty();
	}
	if (IsExactNumeric(source_type) && IsExactNumeric(target_type)) {
		return CastIsInvertible(source_type, target_type);
	}
	if (IsLosslessExactNumericToFloatingCast(source_type, target_type)) {
		return true;
	}
	switch (source_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return target_type.IsIntegral();
	case LogicalTypeId::FLOAT:
		return target_type.id() == LogicalTypeId::DOUBLE;
	default:
		return false;
	}
}

optional<Value> BoundCastExpression::TryInvertConstant(ClientContext &context, const BoundFunctionExpression &cast_expr,
                                                       const Value &constant) {
	auto source_constant = constant.TryCastAs(context, SourceType(cast_expr), nullptr, true);
	if (!source_constant) {
		return nullopt;
	}
	auto roundtrip = source_constant->TryCastAs(context, TargetType(cast_expr), nullptr, true);
	if (!roundtrip || !Value::NotDistinctFrom(*roundtrip, constant)) {
		return nullopt;
	}
	return source_constant;
}

//! The child type of a LIST or ARRAY
static const LogicalType &ListOrArrayChildType(const LogicalType &type) {
	return type.id() == LogicalTypeId::ARRAY ? ArrayType::GetChildType(type) : ListType::GetChildType(type);
}

//! Whether a cast of a nested type can throw a runtime error
//! Nested casts are executed on the child types, so they throw if any of the child casts throws, or if the
//! structure of the source does not fit into the structure of the target
static bool NestedCastCanThrow(const LogicalType &source_type, const LogicalType &target_type) {
	// Casts to and from UNION validate the union tags, which can throw
	if (source_type.id() == LogicalTypeId::UNION || target_type.id() == LogicalTypeId::UNION) {
		return true;
	}
	switch (source_type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		if (target_type.id() == LogicalTypeId::ARRAY) {
			// Casting into a fixed-size ARRAY throws whenever the source length differs
			if (source_type.id() != LogicalTypeId::ARRAY || ArrayType::IsAnySize(source_type) ||
			    ArrayType::IsAnySize(target_type) ||
			    ArrayType::GetSize(source_type) != ArrayType::GetSize(target_type)) {
				return true;
			}
		} else if (target_type.id() != LogicalTypeId::LIST) {
			return true;
		}
		return BoundCastExpression::CastCanThrow(ListOrArrayChildType(source_type), ListOrArrayChildType(target_type),
		                                         false);
	// TUPLE is an unnamed STRUCT - same physical layout, positional child matching, and the same shared struct
	// cast operators, so it can throw exactly when a STRUCT of the same children can.
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::STRUCT: {
		if (target_type.id() != LogicalTypeId::STRUCT && target_type.id() != LogicalTypeId::TUPLE) {
			return true;
		}
		// Pair the fields as the cast executes them: positionally for unnamed structs, by name otherwise.
		// Unmatched target fields become NULL and unmatched source fields are dropped, which does not throw.
		auto mapping = StructBoundCastData::GetFieldMapping(source_type, target_type);
		if (mapping.kind != StructCastFieldMapping::MatchKind::VALID) {
			// Binding the cast fails, which throws when the cast is bound lazily
			return true;
		}
		auto &source_children = StructType::GetChildTypes(source_type);
		auto &target_children = StructType::GetChildTypes(target_type);
		for (idx_t i = 0; i < mapping.source_indexes.size(); i++) {
			if (BoundCastExpression::CastCanThrow(source_children[mapping.source_indexes[i]].second,
			                                      target_children[mapping.target_indexes[i]].second, false)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::MAP: {
		if (target_type.id() != LogicalTypeId::MAP) {
			return true;
		}
		auto &source_key = MapType::KeyType(source_type);
		auto &target_key = MapType::KeyType(target_type);
		// Map keys must stay unique: a key cast that can map distinct keys onto the same key (is not invertible)
		// throws the duplicate-key error even when none of the child casts throws
		if (source_key != target_key && !BoundCastExpression::CastIsInvertible(source_key, target_key)) {
			return true;
		}
		// Map keys cannot be NULL, so a failing key cast throws even when the value cast would default to NULL
		return BoundCastExpression::CastCanThrow(source_key, target_key, false) ||
		       BoundCastExpression::CastCanThrow(MapType::ValueType(source_type), MapType::ValueType(target_type),
		                                         false);
	}
	default:
		return true;
	}
}

//! The decimal digits that every value of an integer type fits in, e.g. 5 for SMALLINT: no five-digit DECIMAL
//! overflows for 32767 or -32768. 0 for other types, and for HUGEINT and UHUGEINT, whose 39-digit values no DECIMAL
//! holds
static uint8_t IntegerTypeDecimalDigits(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 3;
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		return 5;
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		return 10;
	case LogicalTypeId::BIGINT:
		return 19;
	case LogicalTypeId::UBIGINT:
		return 20;
	default:
		return 0;
	}
}

//! Whether a DECIMAL target holds every value of the source type, so that the cast cannot overflow
static bool DecimalHoldsEveryValue(const LogicalType &source_type, const LogicalType &target_type) {
	D_ASSERT(target_type.id() == LogicalTypeId::DECIMAL);
	const auto target_integral_digits = DecimalType::GetWidth(target_type) - DecimalType::GetScale(target_type);
	if (source_type.id() == LogicalTypeId::DECIMAL) {
		// The target needs the digits of the source on both sides of the decimal point: scaling up multiplies by a
		// power of ten and keeps every digit, but scaling down rounds, which can carry into one more integral digit
		// (9.99 -> DECIMAL(2,1) rounds to 10.0)
		const auto source_integral_digits = DecimalType::GetWidth(source_type) - DecimalType::GetScale(source_type);
		return DecimalType::GetScale(target_type) >= DecimalType::GetScale(source_type) &&
		       target_integral_digits >= source_integral_digits;
	}
	// An integer fits when its digits fit before the decimal point: the cast checks the value against 10^(width-scale)
	const auto source_digits = IntegerTypeDecimalDigits(source_type);
	return source_digits > 0 && target_integral_digits >= source_digits;
}

bool BoundCastExpression::CastCanThrow(const LogicalType &source_type, const LogicalType &target_type, bool try_cast) {
	if (source_type == target_type) {
		return false;
	}
	// Converting VARIANT can hit its traversal depth limit, regardless of the target type or TRY_CAST.
	if (source_type.id() == LogicalTypeId::VARIANT) {
		return true;
	}
	// Casts involving custom types are implemented by extensions, so we cannot reason about them - they can throw
	// even for a try_cast, e.g. casting JSON into a MAP throws when a key fails to cast, since map keys cannot be NULL
	if (source_type.HasAlias() || target_type.HasAlias()) {
		return true;
	}
	if (try_cast) {
		// try_cast turns conversion errors into NULL values instead of throwing
		return false;
	}
	// Every value has a string representation, so casting to VARCHAR never throws. VARCHAR is not the maximum
	// of the coercion lattice (you do not implicitly widen to it), so the max-type check below would miss this.
	if (target_type.id() == LogicalTypeId::VARCHAR) {
		return false;
	}
	// A geometry -> geometry cast only changes CRS metadata (incompatible CRSs are rejected at bind time)
	if (source_type.id() == LogicalTypeId::GEOMETRY && target_type.id() == LogicalTypeId::GEOMETRY) {
		return false;
	}
	// Casting from a string requires parsing it, which throws whenever the string is not a valid representation of
	// the target type. Types that sit above VARCHAR in the coercion lattice (e.g. UUID, BLOB, BIT, nested types)
	// would otherwise be considered safe by the max-type check below.
	// ENUM values are cast through their string value, so casting from an ENUM parses a string as well
	if (source_type.id() == LogicalTypeId::VARCHAR || source_type.id() == LogicalTypeId::ENUM) {
		return true;
	}
	// BIGNUM has arbitrary precision, so its values do not necessarily fit in any other type, e.g. BIGNUM -> DOUBLE
	// overflows for large values, even though BIGNUM implicitly casts to DOUBLE
	if (source_type.id() == LogicalTypeId::BIGNUM) {
		return true;
	}
	// Floating point values that are not finite (NaN, inf) cannot be represented as a BIGNUM
	if (target_type.id() == LogicalTypeId::BIGNUM &&
	    (source_type.id() == LogicalTypeId::FLOAT || source_type.id() == LogicalTypeId::DOUBLE)) {
		return true;
	}
	// Casts of nested types are executed on the child types - recurse into them
	if (source_type.IsNested() || target_type.IsNested()) {
		return NestedCastCanThrow(source_type, target_type);
	}
	// Casting to DECIMAL overflows whenever the value does not fit in the width and scale, e.g. 100 -> DECIMAL(38,37),
	// unless the target holds every value of the source type: a DECIMAL that keeps the source's integral and fractional
	// digits, or an integer type whose digits fit before the decimal point. The max-type check below cannot decide
	// this, since the maximum DECIMAL of two types is capped at the maximum width, e.g. HUGEINT -> DECIMAL(38,0)
	if (target_type.id() == LogicalTypeId::DECIMAL) {
		return !DecimalHoldsEveryValue(source_type, target_type);
	}
	// Casting to a TIMESTAMP overflows for values outside of the timestamp range, even when widening the type,
	// e.g. a DATE far in the future does not fit in a TIMESTAMP
	if (LogicalType::TypeIsTimestamp(target_type) || target_type.id() == LogicalTypeId::TIMESTAMP_TZ_NS) {
		return true;
	}
	// A cast only succeeds for every input if the target type can represent every value of the source type,
	// i.e. if the target is the maximum of the two types. Narrowing casts can overflow, and casts between types
	// that have no ordering in the lattice can fail to convert - or not be implemented at all, e.g. INTEGER -> DATE
	LogicalType max_type;
	if (!LogicalType::DefaultTryGetMaxLogicalTypeUnchecked(source_type, target_type, max_type)) {
		return true;
	}
	return max_type != target_type;
}

} // namespace duckdb
