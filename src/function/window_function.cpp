#include "duckdb/function/window_function.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

unique_ptr<BoundWindowExpression> WindowFunction::Bind(ClientContext &context) const {
	vector<unique_ptr<Expression>> arguments;
	return Bind(context, std::move(arguments));
}

unique_ptr<BoundWindowExpression> WindowFunction::Bind(ClientContext &context,
                                                       vector<unique_ptr<Expression>> arguments) const {
	FunctionBinder func_binder(context);
	vector<LogicalType> order_types;
	vector<LogicalType> arg_order_types;

	// the function does not come from a function set - copy it into a definition of its own
	return func_binder.BindWindowFunction(make_shared_ptr<WindowFunction>(*this), std::move(arguments), {}, order_types,
	                                      arg_order_types);
}

BoundWindowFunction::BoundWindowFunction(const WindowFunction &base)
    // the function does not come from a function set - copy it into a definition of its own
    : BoundWindowFunction(make_shared_ptr<WindowFunction>(base)) {
}

BoundWindowFunction::BoundWindowFunction(shared_ptr<const WindowFunction> base_p)
    : window_enum(base_p->window_enum), definition(std::move(base_p)) {
	auto &base = *definition;
	qualified_name = base.GetQualifiedName();
	extra_info = base.extra_info;
	return_type = base.GetReturnType();
	callbacks = base.GetCallbacks();
	properties = base.GetProperties();
	function_info = base.GetFunctionInfo();

	// Try to default bind the function, to fill in any missing information in the BoundScalarFunction (e.g. from the
	// "bind" callback)
	for (auto &param : base.GetSignature().GetParameters()) {
		if (!param.IsVariadic()) {
			arguments.push_back(param.GetType());
		}
	}
	positional_arguments = arguments.size();
	logical_arguments = arguments;
	logical_return_type = return_type;
}

bool BoundWindowFunction::operator==(const BoundWindowFunction &rhs) const {
	return window_enum == rhs.window_enum && arguments == rhs.arguments &&
	       positional_arguments == rhs.positional_arguments && named_arguments == rhs.named_arguments &&
	       return_type == rhs.return_type;
}

bool BoundWindowFunction::operator!=(const BoundWindowFunction &rhs) const {
	return !(*this == rhs);
}

BindWindowFunctionInput::BindWindowFunctionInput(ClientContext &context_p, BoundWindowFunction &bound_function_p,
                                                 vector<unique_ptr<Expression>> &arguments_p,
                                                 OptionalOrderTypes order_types_p, OptionalOrderTypes arg_order_types_p)
    : BindFunctionInput(context_p, bound_function_p, bound_function_p.GetDefinition()->GetSignature(), arguments_p),
      bound_function(bound_function_p), order_types(order_types_p), arg_order_types(arg_order_types_p) {
}

} // namespace duckdb
