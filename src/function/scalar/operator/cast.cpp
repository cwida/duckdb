#include "duckdb/function/scalar/operator_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/legacy_bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

static BoundCastInfo BindCastScalarFunction(ClientContext &context, const LogicalType &source,
                                            const LogicalType &target) {
	auto &cast_functions = DBConfig::GetConfig(context).GetCastFunctions();
	GetCastFunctionInput input(context);
	return cast_functions.GetCastFunction(source, target, input);
}

static BoundCastInfo BindDefaultCastScalarFunction(const LogicalType &source, const LogicalType &target) {
	CastFunctionSet cast_functions;
	GetCastFunctionInput input;
	return cast_functions.GetCastFunction(source, target, input);
}

//! Casts to UNION types must run even for constant NULL inputs (the union member tag is set even for NULL values),
//! so we disable the executor's constant-NULL short-circuit for those. For all other targets casts propagate NULLs
//! normally, and using default NULL handling keeps Expression::PropagatesNullValues() accurate for the optimizer.
template <class FUNC>
static void SetCastNullHandling(FUNC &function, const LogicalType &target_type) {
	if (target_type.id() == LogicalTypeId::UNION) {
		function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	}
}

//! Whether the cast can throw a runtime error - either because the types allow for it, or because no cast exists
//! between the types at all, in which case the cast throws for every non-NULL value
static bool BoundCastCanThrow(const BoundCastInfo &bound_cast, const LogicalType &source_type,
                              const LogicalType &target_type, bool try_cast) {
	if (bound_cast.IsNopCast()) {
		return false; // The cast does not do anything
	}
	if (bound_cast.IsNullCast()) {
		// No cast exists between these types - it throws for every non-NULL value, unless this is a try_cast
		return !try_cast;
	}
	return BoundCastExpression::CastCanThrow(source_type, target_type, try_cast);
}

struct CastFunctionData : public FunctionData {
	CastFunctionData(LogicalType source_type_p, LogicalType target_type_p, BoundCastInfo bound_cast_p, bool try_cast_p,
	                 bool is_default_cast_p)
	    : source_type(std::move(source_type_p)), target_type(std::move(target_type_p)),
	      bound_cast(std::move(bound_cast_p)), try_cast(try_cast_p), is_default_cast(is_default_cast_p) {
	}

	LogicalType source_type;
	LogicalType target_type;
	BoundCastInfo bound_cast;
	bool try_cast;
	bool is_default_cast;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CastFunctionData>(source_type, target_type, bound_cast.Copy(), try_cast, is_default_cast);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CastFunctionData>();
		return source_type == other.source_type && target_type == other.target_type && try_cast == other.try_cast &&
		       is_default_cast == other.is_default_cast && bound_cast.Equals(other.bound_cast);
	}
};

void CastFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &cast_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &cast_data = cast_expr.BindInfo()->Cast<CastFunctionData>();
	auto lstate = ExecuteFunctionState::GetFunctionState(state);

	auto &source = args.data[0];
	idx_t count = args.size();

	// Constant inputs are handled by the generic function executor (cardinality is reduced to 1). NULL handling is
	// marked SPECIAL_HANDLING so that constant NULL inputs still reach the cast (required for e.g. UNION targets).
	string error_message;
	auto error_ref = cast_data.try_cast ? &error_message : nullptr;
	CastParameters parameters(cast_data.bound_cast.GetCastData(), false, error_ref, lstate);
	parameters.query_location = cast_expr.GetQueryLocation();
	parameters.cast_source = &BoundCastExpression::Child(cast_expr);
	parameters.cast_target = cast_expr;
	cast_data.bound_cast.Cast(source, result, count, parameters);
}

static unique_ptr<FunctionLocalState> CastInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                         FunctionData *bind_data) {
	auto &cast_data = bind_data->Cast<CastFunctionData>();
	if (!cast_data.bound_cast.HasInitLocalState()) {
		return nullptr;
	}
	auto context_ptr = state.root.executor->HasContext() ? &state.root.executor->GetContext() : nullptr;
	CastLocalStateParameters parameters(context_ptr, cast_data.bound_cast.GetCastData());
	return cast_data.bound_cast.InitLocalState(parameters);
}

static unique_ptr<FunctionData> BindCastFun(BindScalarFunctionInput &input) {
	throw InvalidInputException("Cast function cannot be called directly");
}

static unique_ptr<BaseStatistics> CastPropagateStatistics(ClientContext &context, FunctionStatisticsInput &input) {
	D_ASSERT(input.child_stats.size() == 1);
	return BoundCastExpression::PropagateStatistics(input.expr, input.child_stats[0], context);
}

static string CastToString(FunctionToStringInput &input) {
	auto &cast_data = input.bind_data->Cast<CastFunctionData>();
	string prefix = cast_data.try_cast ? "TRY_CAST(" : "CAST(";
	return prefix + input.GetChild(0).GetName() + " AS " + cast_data.target_type.ToString() + ")";
}

static ExpressionType CastGetExpressionType(FunctionToStringInput &input) {
	return ExpressionType::OPERATOR_CAST;
}

static unique_ptr<Expression> CastLegacySerializeCallback(FunctionToStringInput &input) {
	auto &cast_data = input.bind_data->Cast<CastFunctionData>();
	if (cast_data.is_default_cast) {
		throw NotImplementedException("Default casts cannot be represented by the legacy cast format");
	}
	return make_uniq<LegacyBoundCastExpression>(input.GetChild(0).Copy(), cast_data.target_type, cast_data.try_cast);
}

static void CastFunctionSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                                  const BoundScalarFunction &function) {
	auto &bind_data = bind_data_p->Cast<CastFunctionData>();
	serializer.WriteProperty(100, "try_cast", bind_data.try_cast);
	serializer.WriteProperty(101, "is_default_cast", bind_data.is_default_cast);
}

static unique_ptr<FunctionData> CastFunctionDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	auto try_cast = deserializer.ReadProperty<bool>(100, "try_cast");
	auto is_default_cast = deserializer.ReadPropertyWithDefault<bool>(101, "is_default_cast");
	auto &context = deserializer.Get<ClientContext &>();
	// the target type is the return type of the function (set by the function serializer)
	auto target_type = deserializer.Get<const LogicalType &>();
	auto source_type = function.GetArguments()[0];
	auto bound_cast = is_default_cast ? BindDefaultCastScalarFunction(source_type, target_type)
	                                  : BindCastScalarFunction(context, source_type, target_type);
	if (BoundCastCanThrow(bound_cast, source_type, target_type, try_cast)) {
		function.SetErrorMode(FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	}
	function.SetStability(bound_cast.GetStability());
	SetCastNullHandling(function, target_type);
	if (is_default_cast || !CastFunctionSet::Get(context).CanOverrideDefaultCast(source_type, target_type)) {
		function.SetBindingSensitivity(FunctionBindingSensitivity::CONTEXT_INSENSITIVE);
	}
	return make_uniq<CastFunctionData>(source_type, target_type, std::move(bound_cast), try_cast, is_default_cast);
}

ScalarFunction CastFun::GetFunction() {
	ScalarFunction cast_fun("__cast", {}, LogicalType::ANY, CastFunction, BindCastFun);
	cast_fun.GetSignature().AddParameter("value", LogicalType::ANY);
	cast_fun.SetToStringCallback(CastToString);
	cast_fun.SetGetExpressionTypeCallback(CastGetExpressionType);
	cast_fun.SetLegacySerializeCallback(CastLegacySerializeCallback);
	cast_fun.SetSerializeCallback(CastFunctionSerialize);
	cast_fun.SetDeserializeCallback(CastFunctionDeserialize);
	cast_fun.SetInitStateCallback(CastInitLocalState);
	cast_fun.SetStatisticsCallback(CastPropagateStatistics);
	return cast_fun;
}

//===--------------------------------------------------------------------===//
// BoundCastExpression
//===--------------------------------------------------------------------===//
static unique_ptr<Expression> CreateCastExpression(unique_ptr<Expression> child, const LogicalType &target_type,
                                                   BoundCastInfo bound_cast, bool try_cast, bool is_default_cast) {
	auto source_type = child->GetReturnType();
	auto query_location = child->GetQueryLocation();

	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));

	auto can_throw = BoundCastCanThrow(bound_cast, source_type, target_type, try_cast);
	auto stability = bound_cast.GetStability();
	auto function_data =
	    make_uniq<CastFunctionData>(source_type, target_type, std::move(bound_cast), try_cast, is_default_cast);

	auto scalar_function = CastFun::GetFunction();
	scalar_function.SetReturnType(target_type);
	if (can_throw) {
		scalar_function.SetErrorMode(FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	}
	scalar_function.SetStability(stability);
	SetCastNullHandling(scalar_function, target_type);
	if (is_default_cast) {
		// a default cast is determined by the written source and target types alone,
		// a registered cast may capture context in its bind data
		scalar_function.SetBindingSensitivity(FunctionBindingSensitivity::CONTEXT_INSENSITIVE);
	}

	BoundScalarFunction bound_function(scalar_function);
	bound_function.GetArguments() = {source_type};

	auto result = make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(children),
	                                                 std::move(function_data), true);
	result->SetQueryLocation(query_location);
	return std::move(result);
}

unique_ptr<Expression> BoundCastExpression::Create(unique_ptr<Expression> child, const LogicalType &target_type,
                                                   BoundCastInfo bound_cast, bool try_cast) {
	return CreateCastExpression(std::move(child), target_type, std::move(bound_cast), try_cast, false);
}

unique_ptr<Expression> BoundCastExpression::CreateDefault(unique_ptr<Expression> child, const LogicalType &target_type,
                                                          BoundCastInfo bound_cast, bool try_cast) {
	return CreateCastExpression(std::move(child), target_type, std::move(bound_cast), try_cast, true);
}

bool BoundCastExpression::IsCast(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.GetExpressionType() == ExpressionType::OPERATOR_CAST;
}

const Expression &BoundCastExpression::Child(const BoundFunctionExpression &cast_expr) {
	return *cast_expr.GetChildren()[0];
}

unique_ptr<Expression> &BoundCastExpression::ChildMutable(BoundFunctionExpression &cast_expr) {
	return cast_expr.GetChildrenMutable()[0];
}

const LogicalType &BoundCastExpression::TargetType(const BoundFunctionExpression &cast_expr) {
	return cast_expr.GetReturnType();
}

LogicalType BoundCastExpression::SourceType(const BoundFunctionExpression &cast_expr) {
	return Child(cast_expr).GetReturnType();
}

bool BoundCastExpression::IsTryCast(const BoundFunctionExpression &cast_expr) {
	return cast_expr.BindInfo()->Cast<CastFunctionData>().try_cast;
}

bool BoundCastExpression::HasValidBindData(const BoundFunctionExpression &cast_expr) {
	if (cast_expr.GetChildren().size() != 1 || !cast_expr.GetChildren()[0] || !cast_expr.BindInfo()) {
		return false;
	}
	auto &data = cast_expr.BindInfo()->Cast<CastFunctionData>();
	return data.source_type == cast_expr.GetChildren()[0]->GetReturnType() &&
	       data.target_type == cast_expr.GetReturnType();
}

bool BoundCastExpression::IsDefaultCast(const BoundFunctionExpression &cast_expr) {
	return cast_expr.BindInfo()->Cast<CastFunctionData>().is_default_cast;
}

const BoundCastInfo &BoundCastExpression::GetBoundCast(const BoundFunctionExpression &cast_expr) {
	return cast_expr.BindInfo()->Cast<CastFunctionData>().bound_cast;
}

BoundCastInfo &BoundCastExpression::GetBoundCastMutable(BoundFunctionExpression &cast_expr) {
	return cast_expr.BindInfoMutable()->Cast<CastFunctionData>().bound_cast;
}

unique_ptr<BaseStatistics> BoundCastExpression::PropagateStatistics(BoundFunctionExpression &cast_expr,
                                                                    const BaseStatistics &child_stats,
                                                                    optional_ptr<ClientContext> context) {
	if (!child_stats.CanHaveNoNull() &&
	    cast_expr.Function().GetNullHandling() == FunctionNullHandling::DEFAULT_NULL_HANDLING) {
		return BaseStatistics::FromConstant(Value(cast_expr.GetReturnType())).ToUnique();
	}
	auto &cast_data = cast_expr.BindInfoMutable()->Cast<CastFunctionData>();
	auto result =
	    cast_data.bound_cast.PropagateStatistics(cast_data.source_type, cast_data.target_type, child_stats, context);
	if (cast_data.try_cast && result) {
		result->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	return result;
}

} // namespace duckdb
