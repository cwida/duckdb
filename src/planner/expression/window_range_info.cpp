#include "duckdb/planner/expression/window_range_info.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

static bool HasDirectRangeArguments(const BoundScalarFunction &function) {
	const auto &definition = function.GetDefinition();
	if (!definition || function.HasBindExpressionCallback() || definition->HasUnbindCallback()) {
		return false;
	}
	auto &properties = definition->GetProperties();
	return !properties.GetCaptureArgumentAliases() && !properties.RequiresExpressionNames();
}

bool WindowRangeCast::Capture(const Expression &expression, const Expression &operand, vector<WindowRangeCast> &casts) {
	optional_ptr<const Expression> current = expression;
	// the operand is the expression itself when it is still in place, or a copy of it when the function binder may
	// have folded it (a subquery can neither be copied nor compared, it is always matched by identity)
	while (current.get() != &operand && !current->Equals(operand)) {
		if (operand.IsFoldable() && current->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			// the function binder folded the constant operand to its value
			return true;
		}
		if (!BoundCastExpression::IsCast(*current)) {
			return false;
		}
		auto &cast = current->Cast<BoundFunctionExpression>();
		if (!BoundCastExpression::HasValidBindData(cast)) {
			return false;
		}
		casts.push_back({BoundCastExpression::SourceType(cast), cast.GetReturnType(),
		                 BoundCastExpression::IsTryCast(cast), BoundCastExpression::IsDefaultCast(cast)});
		current = &BoundCastExpression::Child(cast);
	}
	return true;
}

optional_ptr<const Expression> WindowRangeCast::Match(const Expression &expression,
                                                      const vector<WindowRangeCast> &casts) {
	optional_ptr<const Expression> current = expression;
	for (auto &origin : casts) {
		if (!BoundCastExpression::IsCast(*current)) {
			return nullptr;
		}
		auto &cast = current->Cast<BoundFunctionExpression>();
		if (!BoundCastExpression::HasValidBindData(cast) ||
		    !BoundCastExpression::SourceType(cast).EqualsIncludingCollation(origin.source_type) ||
		    !cast.GetReturnType().EqualsIncludingCollation(origin.target_type) ||
		    BoundCastExpression::IsTryCast(cast) != origin.try_cast ||
		    BoundCastExpression::IsDefaultCast(cast) != origin.default_cast) {
			return nullptr;
		}
		current = &BoundCastExpression::Child(cast);
	}
	return current;
}

unique_ptr<WindowRangeBoundary> WindowRangeBoundary::Capture(const Expression &expression, const Expression &order,
                                                             const Expression &offset) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &call = expression.Cast<BoundFunctionExpression>();
	auto &function = call.Function();
	const auto &definition = function.GetDefinition();
	if (!HasDirectRangeArguments(function) || call.GetChildren().size() != 2) {
		return nullptr;
	}
	auto result = make_uniq<WindowRangeBoundary>();
	vector<WindowRangeCast> offset_casts;
	if (!WindowRangeCast::Capture(*call.GetChildren()[0], order, result->order_casts) ||
	    !WindowRangeCast::Capture(*call.GetChildren()[1], offset, offset_casts)) {
		return nullptr;
	}
	result->function_name = definition->GetQualifiedName();
	result->arguments = function.GetLogicalArguments();
	result->return_type = function.GetLogicalReturnType();
	result->order_type = call.GetChildren()[0]->GetReturnType();
	result->offset_type = call.GetChildren()[1]->GetReturnType();
	return result;
}

optional_ptr<const Expression> WindowRangeBoundary::Match(const Expression &expression, const Expression &order) const {
	auto endpoint = WindowRangeCast::Match(expression, result_casts);
	if (!endpoint || endpoint->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &call = endpoint->Cast<BoundFunctionExpression>();
	auto &function = call.Function();
	const auto &definition = function.GetDefinition();
	if (!HasDirectRangeArguments(function) || call.GetChildren().size() != 2) {
		return nullptr;
	}
	const bool matches_signature = definition->GetQualifiedName() == function_name &&
	                               function.GetLogicalArguments() == arguments &&
	                               function.GetLogicalReturnType() == return_type;
	const bool matches_types = call.GetReturnType().EqualsIncludingCollation(return_type) &&
	                           call.GetChildren()[0]->GetReturnType().EqualsIncludingCollation(order_type) &&
	                           call.GetChildren()[1]->GetReturnType().EqualsIncludingCollation(offset_type);
	if (!matches_signature || !matches_types) {
		return nullptr;
	}
	auto input_order = WindowRangeCast::Match(*call.GetChildren()[0], order_casts);
	if (!input_order || !Expression::Equals(*input_order, order)) {
		return nullptr;
	}
	return call.GetChildren()[1].get();
}

void WindowRangeCast::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "source_type", source_type);
	serializer.WriteProperty(101, "target_type", target_type);
	serializer.WriteProperty(102, "try_cast", try_cast);
	serializer.WriteProperty(103, "default_cast", default_cast);
}

WindowRangeCast WindowRangeCast::Deserialize(Deserializer &deserializer) {
	WindowRangeCast result;
	deserializer.ReadProperty(100, "source_type", result.source_type);
	deserializer.ReadProperty(101, "target_type", result.target_type);
	deserializer.ReadProperty(102, "try_cast", result.try_cast);
	deserializer.ReadProperty(103, "default_cast", result.default_cast);
	return result;
}

void WindowRangeBoundary::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "function_name", function_name);
	serializer.WriteProperty(101, "arguments", arguments);
	serializer.WriteProperty(102, "return_type", return_type);
	serializer.WriteProperty(103, "order_type", order_type);
	serializer.WriteProperty(104, "offset_type", offset_type);
	serializer.WriteProperty(105, "order_casts", order_casts);
	serializer.WriteProperty(106, "result_casts", result_casts);
	serializer.WriteProperty(107, "boundary", boundary);
	serializer.WriteProperty(108, "direction", direction);
}

unique_ptr<WindowRangeBoundary> WindowRangeBoundary::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<WindowRangeBoundary>();
	deserializer.ReadProperty(100, "function_name", result->function_name);
	deserializer.ReadProperty(101, "arguments", result->arguments);
	deserializer.ReadProperty(102, "return_type", result->return_type);
	deserializer.ReadProperty(103, "order_type", result->order_type);
	deserializer.ReadProperty(104, "offset_type", result->offset_type);
	deserializer.ReadProperty(105, "order_casts", result->order_casts);
	deserializer.ReadProperty(106, "result_casts", result->result_casts);
	deserializer.ReadProperty(107, "boundary", result->boundary);
	deserializer.ReadProperty(108, "direction", result->direction);
	return result;
}
} // namespace duckdb
