#include "duckdb/parser/peg/ast/sequence_option.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateSequenceStmt(
    PEGTransformer &transformer, const optional<bool> &if_not_exists, const QualifiedName &qualified_name,
    optional<vector<pair<string, unique_ptr<SequenceOption>>>> sequence_option) {
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateSequenceInfo>();
	info->SetQualifiedName(qualified_name);
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	case_insensitive_map_t<unique_ptr<SequenceOption>> sequence_options;
	if (sequence_option) {
		for (auto &seq_option : *sequence_option) {
			if (sequence_options.find(seq_option.first) != sequence_options.end()) {
				auto seq_option_capital = StringUtil::Lower(seq_option.first);
				seq_option_capital[0] = StringUtil::CharacterToUpper(seq_option_capital[0]);
				throw ParserException("%s should be passed at most once", seq_option_capital);
			}
			sequence_options.insert(std::move(seq_option));
		}
	}
	bool no_min = false;
	bool no_max = false;
	if (sequence_options.find("nominvalue") != sequence_options.end()) {
		no_min = true;
		sequence_options.erase("nominvalue");
	}
	if (no_min && sequence_options.find("minvalue") != sequence_options.end()) {
		throw ParserException("Minvalue should be passed at most once");
	}
	if (sequence_options.find("nomaxvalue") != sequence_options.end()) {
		no_max = true;
		sequence_options.erase("nomaxvalue");
	}
	if (no_max && sequence_options.find("maxvalue") != sequence_options.end()) {
		throw ParserException("Maxvalue should be passed at most once");
	}
	bool has_start_value = false;
	bool min_value_set = false;
	bool max_value_set = false;

	for (auto &option : sequence_options) {
		if (option.first == "increment") {
			auto seq_val_option = unique_ptr_cast<SequenceOption, ValueSequenceOption>(std::move(option.second));
			if (seq_val_option->value.IsNull()) {
				throw ParserException("INCREMENT must not be NULL");
			}
			info->increment = seq_val_option->value.GetValue<int64_t>();
			if (info->increment == 0) {
				throw ParserException("Increment must not be zero");
			} else if (info->increment < 0) {
				if (!min_value_set) {
					info->min_value = NumericLimits<int64_t>::Minimum();
				}
				if (!max_value_set) {
					info->max_value = -1;
				}
			} else {
				if (!max_value_set) {
					info->max_value = NumericLimits<int64_t>::Maximum();
				}
				if (!min_value_set) {
					info->min_value = 1;
				}
			}
		} else if (option.first == "minvalue") {
			if (no_min) {
				continue;
			}
			auto seq_val_option = unique_ptr_cast<SequenceOption, ValueSequenceOption>(std::move(option.second));
			if (seq_val_option->value.IsNull()) {
				throw ParserException("MINVALUE must not be NULL");
			}
			info->min_value = seq_val_option->value.GetValue<int64_t>();
			min_value_set = true;
		} else if (option.first == "maxvalue") {
			if (no_max) {
				continue;
			}
			auto seq_val_option = unique_ptr_cast<SequenceOption, ValueSequenceOption>(std::move(option.second));
			if (seq_val_option->value.IsNull()) {
				throw ParserException("MAXVALUE must not be NULL");
			}
			info->max_value = seq_val_option->value.GetValue<int64_t>();
			max_value_set = true;
		} else if (option.first == "start") {
			auto seq_val_option = unique_ptr_cast<SequenceOption, ValueSequenceOption>(std::move(option.second));
			if (seq_val_option->value.IsNull()) {
				throw ParserException("START value must not be NULL");
			}
			info->start_value = seq_val_option->value.GetValue<int64_t>();
			has_start_value = true;
		} else if (option.first == "cycle") {
			auto seq_val_option = unique_ptr_cast<SequenceOption, ValueSequenceOption>(std::move(option.second));
			info->cycle = seq_val_option->value.GetValue<bool>();
		} else {
			throw ParserException("Unrecognized option \"%s\" for CREATE SEQUENCE", option.first);
		}
	}
	if (!has_start_value) {
		if (info->increment < 0) {
			info->start_value = info->max_value;
		} else {
			info->start_value = info->min_value;
		}
	}
	if (info->max_value <= info->min_value) {
		throw ParserException("MINVALUE (%lld) must be less than MAXVALUE (%lld)", info->min_value, info->max_value);
	}
	if (info->start_value < info->min_value) {
		throw ParserException("START value (%lld) cannot be less than MINVALUE (%lld)", info->start_value,
		                      info->min_value);
	}
	if (info->start_value > info->max_value) {
		throw ParserException("START value (%lld) cannot be greater than MAXVALUE (%lld)", info->start_value,
		                      info->max_value);
	}
	result->info = std::move(info);
	return result;
}

pair<string, unique_ptr<SequenceOption>> PEGTransformerFactory::TransformSeqCycle(PEGTransformer &transformer) {
	return make_pair("cycle", make_uniq<ValueSequenceOption>(SequenceInfo::SEQ_CYCLE, Value(true)));
}

pair<string, unique_ptr<SequenceOption>> PEGTransformerFactory::TransformSeqNoCycle(PEGTransformer &transformer) {
	return make_pair("cycle", make_uniq<ValueSequenceOption>(SequenceInfo::SEQ_CYCLE, Value(false)));
}

//! Sequence options take integer constants; anything else used to be rounded or cast silently
static void CheckIntegerSequenceOption(const char *option, const Literal &literal) {
	if (!literal.IsNull() && literal.kind != LiteralKind::INTEGER) {
		throw ParserException("Expected an integer argument for %s, got %s", option, literal.ToString());
	}
}

pair<string, unique_ptr<SequenceOption>>
PEGTransformerFactory::TransformSeqSetIncrement(PEGTransformer &transformer, const bool &has_result,
                                                unique_ptr<ParsedExpression> expression) {
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto func_expr = unique_ptr_cast<ParsedExpression, FunctionExpression>(std::move(expression));
		if (func_expr->FunctionName() != "-") {
			throw InvalidInputException("Expected a minus function instead of %s", func_expr->FunctionName());
		}
		D_ASSERT(!func_expr->GetArguments().empty());
		if (func_expr->GetArguments()[0].GetExpression().GetExpressionClass() != ExpressionClass::CONSTANT) {
			throw InvalidInputException("Expected constant expression as child of minus function");
		}
		auto &literal = func_expr->GetArguments()[0].GetExpression().Cast<ConstantExpression>().GetLiteral();
		if (!literal.IsNumeric()) {
			throw InvalidInputException("Expected constant expression as child of minus function");
		}
		expression = ConstantExpression::FromLiteral(literal.Negate());
	}
	if (expression->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("Expected constant expression.");
	}
	auto &const_expr = expression->Cast<ConstantExpression>();
	CheckIntegerSequenceOption("INCREMENT", const_expr.GetLiteral());
	return make_pair("increment",
	                 make_uniq<ValueSequenceOption>(SequenceInfo::SEQ_INC, const_expr.GetLiteral().ToValue()));
}

pair<string, unique_ptr<SequenceOption>>
PEGTransformerFactory::TransformSeqSetMinMax(PEGTransformer &transformer, const string &seq_min_or_max,
                                             unique_ptr<ParsedExpression> expression) {
	if (expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto func_expr = unique_ptr_cast<ParsedExpression, FunctionExpression>(std::move(expression));
		if (func_expr->FunctionName() != "-") {
			throw InvalidInputException("Expected a minus function instead of %s", func_expr->FunctionName());
		}
		D_ASSERT(!func_expr->GetArguments().empty());
		if (func_expr->GetArguments()[0].GetExpression().GetExpressionClass() != ExpressionClass::CONSTANT) {
			throw InvalidInputException("Expected constant expression as child of minus function");
		}
		auto &literal = func_expr->GetArguments()[0].GetExpression().Cast<ConstantExpression>().GetLiteral();
		if (!literal.IsNumeric()) {
			throw InvalidInputException("Expected constant expression as child of minus function");
		}
		expression = ConstantExpression::FromLiteral(literal.Negate());
	}

	if (expression->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("Expected constant expression.");
	}
	auto &const_expr = expression->Cast<ConstantExpression>();
	CheckIntegerSequenceOption(seq_min_or_max == "minvalue" ? "MINVALUE" : "MAXVALUE", const_expr.GetLiteral());
	auto seq_info = seq_min_or_max == "minvalue" ? SequenceInfo::SEQ_MIN : SequenceInfo::SEQ_MAX;
	return make_pair(seq_min_or_max, make_uniq<ValueSequenceOption>(seq_info, const_expr.GetLiteral().ToValue()));
}

pair<string, unique_ptr<SequenceOption>> PEGTransformerFactory::TransformSeqNoMinMax(PEGTransformer &transformer,
                                                                                     const string &seq_min_or_max) {
	auto seq_info = seq_min_or_max == "minvalue" ? SequenceInfo::SEQ_MIN : SequenceInfo::SEQ_MAX;
	return make_pair("no" + seq_min_or_max, make_uniq<ValueSequenceOption>(seq_info, true));
}

pair<string, unique_ptr<SequenceOption>>
PEGTransformerFactory::TransformSeqStartWith(PEGTransformer &transformer, const bool &has_result,
                                             unique_ptr<ParsedExpression> expression) {
	if (expression->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("Expected constant expression.");
	}
	auto &const_expr = expression->Cast<ConstantExpression>();
	CheckIntegerSequenceOption("START", const_expr.GetLiteral());
	return make_pair("start",
	                 make_uniq<ValueSequenceOption>(SequenceInfo::SEQ_START, const_expr.GetLiteral().ToValue()));
}

pair<string, unique_ptr<SequenceOption>>
PEGTransformerFactory::TransformSeqOwnedBy(PEGTransformer &transformer, const QualifiedName &qualified_name) {
	// Unused by old transformer
	return make_pair("owned", make_uniq<QualifiedSequenceOption>(SequenceInfo::SEQ_OWN, qualified_name));
}

string PEGTransformerFactory::TransformMinValue(PEGTransformer &transformer) {
	return "minvalue";
}

string PEGTransformerFactory::TransformMaxValue(PEGTransformer &transformer) {
	return "maxvalue";
}

} // namespace duckdb
