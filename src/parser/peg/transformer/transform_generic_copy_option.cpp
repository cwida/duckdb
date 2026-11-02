#include "duckdb/parser/peg/ast/generic_copy_option.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

vector<GenericCopyOption>
PEGTransformerFactory::TransformGenericCopyOptionList(PEGTransformer &transformer,
                                                      const vector<GenericCopyOption> &generic_copy_option) {
	identifier_set_t option_names;
	for (const auto &option : generic_copy_option) {
		if (option_names.find(option.name) != option_names.end()) {
			throw ParserException("Unexpected duplicate option %s", option.name);
		}
		option_names.insert(option.name);
	}
	return generic_copy_option;
}

vector<GenericCopyOption>
PEGTransformerFactory::TransformCopyGenericOptionList(PEGTransformer &transformer,
                                                      const vector<GenericCopyOption> &copy_generic_option) {
	return copy_generic_option;
}

static void SetGenericCopyOptionExpression(GenericCopyOption &copy_option, unique_ptr<ParsedExpression> expression) {
	if (expression->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		copy_option.children.push_back(expression->Cast<ConstantExpression>().GetLiteral().ToValue());
	} else if (expression->GetExpressionType() == ExpressionType::COLUMN_REF) {
		copy_option.children.push_back(Value(expression->Cast<ColumnRefExpression>().GetColumnName()));
	} else if (expression->GetExpressionType() == ExpressionType::PLACEHOLDER) {
		auto &op_expr = expression->Cast<OperatorExpression>();
		for (auto &child : op_expr.GetChildren()) {
			if (child->GetExpressionClass() == ExpressionClass::CONSTANT) {
				copy_option.children.push_back(child->Cast<ConstantExpression>().GetLiteral().ToValue());
			} else if (child->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				copy_option.children.push_back(Value(child->Cast<ColumnRefExpression>().GetColumnName()));
			} else {
				throw InternalException("Unexpected expression type %s encountered for GenericCopyOption",
				                        ExpressionClassToString(child->GetExpressionClass()));
			}
		}
	} else if (expression->GetExpressionType() == ExpressionType::FUNCTION ||
	           expression->GetExpressionType() == ExpressionType::VALUE_PARAMETER) {
		copy_option.expression = std::move(expression);
	} else if (expression->GetExpressionType() == ExpressionType::STAR) {
		copy_option.children.push_back(Value("*"));
	} else if (expression->GetExpressionType() == ExpressionType::OPERATOR_CAST) {
		auto &cast_expr = expression->Cast<CastExpression>();
		if (cast_expr.Child().GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &literal = cast_expr.Child().Cast<ConstantExpression>().GetLiteral();
			if (literal.kind == LiteralKind::STRING && literal.text == "t") {
				copy_option.children.push_back(Value(true));
			} else if (literal.kind == LiteralKind::STRING && literal.text == "f") {
				copy_option.children.push_back(Value(false));
			} else {
				copy_option.expression = std::move(expression);
			}
		} else {
			copy_option.expression = std::move(expression);
		}
	} else {
		throw NotImplementedException("Unrecognized expression type %s",
		                              ExpressionTypeToString(expression->GetExpressionType()));
	}
}

static unique_ptr<ParsedExpression> CreateRowFunction(vector<unique_ptr<ParsedExpression>> &&children) {
	return make_uniq<FunctionExpression>("row", std::move(children));
}

static unique_ptr<ParsedExpression> CreateOrderByRowFunction(const vector<OrderByNode> &orders) {
	vector<unique_ptr<ParsedExpression>> children;
	children.reserve(orders.size());
	for (auto &order : orders) {
		children.push_back(ConstantExpression::String(order.ToString()));
	}
	return CreateRowFunction(std::move(children));
}

static unique_ptr<ParsedExpression> CreateExpressionRowFunction(vector<OrderByNode> &orders) {
	vector<unique_ptr<ParsedExpression>> children;
	children.reserve(orders.size());
	for (auto &order : orders) {
		children.push_back(std::move(order.expression));
	}
	return CreateRowFunction(std::move(children));
}

static GenericCopyOption BuildGenericCopyOption(const Identifier &generic_copy_option_name,
                                                optional<GenericCopyOptionValue> generic_copy_option_value) {
	GenericCopyOption copy_option;
	copy_option.name = Identifier(StringUtil::Lower(generic_copy_option_name.GetIdentifierName()));
	const bool has_value = generic_copy_option_value && generic_copy_option_value->has_value;
	if (!has_value && copy_option.name == "PARTITION_PATH") {
		throw ParserException("PARTITION_PATH expects an expression");
	}
	if (!has_value) {
		return copy_option;
	}

	if (copy_option.name == "PARTITION_PATH") {
		// kept as-is, as it is bound against the partition columns
		if (!generic_copy_option_value->is_order_list) {
			copy_option.expression = std::move(generic_copy_option_value->expression);
			return copy_option;
		}
		// an expression wrapped in () will trigger this case
		auto &orders = generic_copy_option_value->order_list;
		if (orders.size() != 1 || orders[0].type != OrderType::ORDER_DEFAULT ||
		    orders[0].null_order != OrderByNullType::ORDER_DEFAULT) {
			throw ParserException("PARTITION_PATH expects a single expression");
		}
		copy_option.expression = std::move(orders[0].expression);
		return copy_option;
	}

	if (generic_copy_option_value->is_order_list) {
		auto &orders = generic_copy_option_value->order_list;
		bool has_order_modifier = false;
		for (auto &order : orders) {
			if (order.type != OrderType::ORDER_DEFAULT || order.null_order != OrderByNullType::ORDER_DEFAULT) {
				has_order_modifier = true;
				break;
			}
		}

		if (copy_option.name == "ORDER_BY") {
			copy_option.expression = CreateOrderByRowFunction(orders);
		} else if (has_order_modifier) {
			throw ParserException("ORDER BY modifiers are only supported in the ORDER BY option");
		} else if (orders.size() == 1) {
			SetGenericCopyOptionExpression(copy_option, std::move(orders[0].expression));
		} else {
			copy_option.expression = CreateExpressionRowFunction(generic_copy_option_value->order_list);
		}
	} else {
		SetGenericCopyOptionExpression(copy_option, std::move(generic_copy_option_value->expression));
	}
	return copy_option;
}

GenericCopyOption
PEGTransformerFactory::TransformGenericCopyOption(PEGTransformer &transformer, const Identifier &copy_option_name,
                                                  optional<GenericCopyOptionValue> generic_copy_option_value) {
	return BuildGenericCopyOption(copy_option_name, std::move(generic_copy_option_value));
}

GenericCopyOption
PEGTransformerFactory::TransformOrderByCopyOption(PEGTransformer &transformer,
                                                  optional<GenericCopyOptionValue> generic_copy_option_value) {
	return BuildGenericCopyOption(Identifier("order_by"), std::move(generic_copy_option_value));
}

GenericCopyOption
PEGTransformerFactory::TransformPartitionedByCopyOption(PEGTransformer &transformer,
                                                        optional<GenericCopyOptionValue> generic_copy_option_value) {
	return BuildGenericCopyOption(Identifier("partition_by"), std::move(generic_copy_option_value));
}

GenericCopyOptionValue PEGTransformerFactory::TransformGenericCopyOptionOrderList(
    PEGTransformer &transformer, vector<OrderByNode> generic_copy_option_parenthesized_expression_list) {
	GenericCopyOptionValue result;
	result.has_value = true;
	result.is_order_list = true;
	result.order_list = std::move(generic_copy_option_parenthesized_expression_list);
	return result;
}

GenericCopyOptionValue
PEGTransformerFactory::TransformGenericCopyOptionExpression(PEGTransformer &transformer,
                                                            unique_ptr<ParsedExpression> expression) {
	GenericCopyOptionValue result;
	result.has_value = true;
	result.expression = std::move(expression);
	return result;
}

vector<OrderByNode> PEGTransformerFactory::TransformGenericCopyOptionParenthesizedExpressionList(
    PEGTransformer &transformer, vector<OrderByNode> order_by_expression_list) {
	return order_by_expression_list;
}

void PEGTransformerFactory::SplitGenericOptions(const vector<GenericCopyOption> &options_in,
                                                case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_options,
                                                unordered_map<string, Value> &options, const char *statement_name) {
	for (const auto &option : options_in) {
		if (option.expression) {
			parsed_options[option.name.GetIdentifierName()] = option.expression->Copy();
			continue;
		}
		if (option.children.empty()) {
			options[option.name.GetIdentifierName()] = Value(true);
		} else if (option.children.size() == 1) {
			if (option.children[0].IsNull()) {
				throw BinderException("NULL is not supported as a valid option for %s option %s", statement_name,
				                      option.name);
			}
			options[option.name.GetIdentifierName()] = option.children[0];
		} else {
			throw ParserException("Option %s can only have one argument", option.name);
		}
	}
}

void PEGTransformerFactory::CollectGenericOptions(const vector<GenericCopyOption> &options_in,
                                                  case_insensitive_map_t<unique_ptr<ParsedExpression>> &options,
                                                  const char *statement_name) {
	for (const auto &option : options_in) {
		auto name = option.name.GetIdentifierName();
		if (option.expression) {
			options[name] = option.expression->Copy();
			continue;
		}
		if (option.children.empty()) {
			// Bare flag (e.g. `(SPOT)`) binds to boolean true, as in SplitGenericOptions.
			options[name] = ConstantExpression::Boolean(true);
		} else if (option.children.size() == 1) {
			if (option.children[0].IsNull()) {
				throw BinderException("NULL is not supported as a valid option for %s option %s", statement_name,
				                      option.name);
			}
			options[name] = ConstantExpression::FromValue(option.children[0]);
		} else {
			throw ParserException("Option %s can only have one argument", option.name);
		}
	}
}

} // namespace duckdb
