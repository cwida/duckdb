#include "duckdb/function/scalar/generic_functions.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

namespace {

//! The level a barrier was given by filter pushdown, see ExpressionBarrier::Wrap. A barrier bound from SQL has none
struct BarrierBindData : public FunctionData {
	LogicalType return_type;
	bool has_level = false;
	idx_t level = 0;

	explicit BarrierBindData(LogicalType return_type_p) : return_type(std::move(return_type_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<BarrierBindData>(return_type);
		result->has_level = has_level;
		result->level = level;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<BarrierBindData>();
		return return_type == other.return_type && has_level == other.has_level && level == other.level;
	}
	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const BoundScalarFunction &function) {
		D_ASSERT(bind_data);
		auto &info = bind_data->Cast<BarrierBindData>();
		serializer.WriteProperty(100, "return_type", info.return_type);
		serializer.WritePropertyWithDefault(101, "has_level", info.has_level);
		serializer.WritePropertyWithDefault(102, "level", info.level);
	}
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &bound_function) {
		auto result = make_uniq<BarrierBindData>(deserializer.ReadProperty<LogicalType>(100, "return_type"));
		result->has_level = deserializer.ReadPropertyWithDefault<bool>(101, "has_level");
		result->level = deserializer.ReadPropertyWithDefault<idx_t>(102, "level");
		bound_function.SetReturnType(result->return_type);
		return std::move(result);
	}
};

void BarrierFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

unique_ptr<FunctionData> BarrierBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	bound_function.SetReturnType(arguments[0]->GetReturnType());
	return make_uniq<BarrierBindData>(bound_function.GetReturnType());
}

unique_ptr<BaseStatistics> BarrierStats(ClientContext &context, FunctionStatisticsInput &input) {
	// the barrier returns its argument unchanged - the statistics of the child carry over
	return input.child_stats[0].ToUnique();
}

ScalarFunction GetBarrierFunction(const LogicalType &type) {
	ScalarFunction fun(BarrierFun::Name, {}, type, BarrierFunction);
	fun.GetSignature().AddParameter("arg", type);
	fun.SetBindCallback(BarrierBind);
	fun.SetStatisticsCallback(BarrierStats);
	fun.SetSerializeCallback(BarrierBindData::Serialize);
	fun.SetDeserializeCallback(BarrierBindData::Deserialize);
	// default null handling replaces the call with a NULL constant when the argument is typed as NULL - that would
	// swallow the very expression the barrier exists to preserve, e.g. __internal_barrier(error('boom'))
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// the barrier itself never throws, but marking it fallible keeps the expression out of every optimization that
	// evaluates an expression on more rows than it would otherwise see (dictionary execution, filter reordering)
	fun.SetFallible();
	return fun;
}

optional_ptr<const BarrierBindData> GetBarrierData(const BoundFunctionExpression &barrier) {
	if (!barrier.BindInfo()) {
		return nullptr;
	}
	return &barrier.BindInfo()->Cast<BarrierBindData>();
}

optional_ptr<BarrierBindData> GetBarrierDataMutable(BoundFunctionExpression &barrier) {
	if (!barrier.BindInfoMutable()) {
		barrier.BindInfoMutable() = make_uniq<BarrierBindData>(barrier.GetReturnType());
	}
	return &barrier.BindInfoMutable()->Cast<BarrierBindData>();
}

} // namespace

ScalarFunction BarrierFun::GetFunction() {
	return GetBarrierFunction(LogicalType::ANY);
}

bool ExpressionBarrier::IsBarrier(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	return expr.Cast<BoundFunctionExpression>().Function().GetName() == BarrierFun::Name;
}

bool ExpressionBarrier::Contains(const Expression &expr) {
	if (IsBarrier(expr)) {
		return true;
	}
	bool contains = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { contains |= Contains(child); });
	return contains;
}

bool ExpressionBarrier::Required(const Expression &expr) {
	return expr.CanThrow() || expr.IsVolatile() || expr.HasSubquery();
}

unique_ptr<Expression> ExpressionBarrier::Wrap(unique_ptr<Expression> expr) {
	auto return_type = expr->GetReturnType();
	auto function = GetBarrierFunction(return_type);
	auto bind_data = make_uniq<BarrierBindData>(return_type);

	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(expr));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments),
	                                          std::move(bind_data));
}

unique_ptr<Expression> ExpressionBarrier::Wrap(unique_ptr<Expression> expr, idx_t level) {
	auto result = Wrap(std::move(expr));
	auto data = GetBarrierDataMutable(result->Cast<BoundFunctionExpression>());
	data->has_level = true;
	data->level = level;
	return result;
}

void ExpressionBarrier::SetLevel(Expression &expr, idx_t level) {
	if (IsBarrier(expr)) {
		auto data = GetBarrierDataMutable(expr.Cast<BoundFunctionExpression>());
		// a barrier written into a query or a view body has no level of its own and gets the level of the filter it
		// appears in. a barrier that has one keeps it: the level is where the expression originated
		if (!data->has_level) {
			data->has_level = true;
			data->level = level;
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { SetLevel(child, level); });
}

idx_t ExpressionBarrier::Level(const Expression &expr) {
	idx_t level = NumericLimits<idx_t>::Maximum();
	if (IsBarrier(expr)) {
		auto data = GetBarrierData(expr.Cast<BoundFunctionExpression>());
		level = data && data->has_level ? data->level : 0;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (Contains(child)) {
			level = MinValue(level, Level(child));
		}
	});
	return level == NumericLimits<idx_t>::Maximum() ? 0 : level;
}

} // namespace duckdb
