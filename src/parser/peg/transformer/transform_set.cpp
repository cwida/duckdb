#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/default_expression.hpp"

namespace duckdb {

static SetScope GetSettingScope(const optional<SetScope> &setting_scope) {
	return setting_scope ? *setting_scope : SetScope::AUTOMATIC;
}

// ResetStatement <- 'RESET' ResetTarget
unique_ptr<SQLStatement> PEGTransformerFactory::TransformResetStatement(PEGTransformer &transformer,
                                                                        unique_ptr<SQLStatement> reset_target) {
	return reset_target;
}

// ResetTimeZone <- SettingScope? 'TIME' 'ZONE'
unique_ptr<SQLStatement> PEGTransformerFactory::TransformResetTimeZone(PEGTransformer &transformer,
                                                                       const optional<SetScope> &setting_scope) {
	if (setting_scope && *setting_scope == SetScope::LOCAL) {
		throw NotImplementedException("RESET LOCAL is not implemented.");
	}
	return make_uniq<ResetVariableStatement>("timezone", GetSettingScope(setting_scope));
}

// ResetVariableOrSetting <- SetVariableOrSetting
unique_ptr<SQLStatement>
PEGTransformerFactory::TransformResetVariableOrSetting(PEGTransformer &transformer,
                                                       const SettingInfo &set_variable_or_setting) {
	if (set_variable_or_setting.scope == SetScope::LOCAL) {
		throw NotImplementedException("RESET LOCAL is not implemented.");
	}
	return make_uniq<ResetVariableStatement>(set_variable_or_setting.name, set_variable_or_setting.scope);
}

// SetAssignment <- VariableAssign VariableList
vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSetAssignment(PEGTransformer &transformer,
                                              vector<unique_ptr<ParsedExpression>> variable_list) {
	return variable_list;
}

// SetSetting <- SettingScope? SettingName
SettingInfo PEGTransformerFactory::TransformSetSetting(PEGTransformer &transformer,
                                                       const optional<SetScope> &setting_scope,
                                                       const Identifier &setting_name) {
	SettingInfo result;
	result.name = setting_name;
	if (setting_scope) {
		result.scope = *setting_scope;
	}
	return result;
}

// SetKeywordStatement <- 'SET' SetAssignmentOrTimeZone
unique_ptr<SQLStatement>
PEGTransformerFactory::TransformSetKeywordStatement(PEGTransformer &transformer,
                                                    unique_ptr<SetStatement> set_assignment_or_time_zone) {
	return std::move(set_assignment_or_time_zone);
}

// VariableAssignment <- '$' Identifier ':=' Expression
unique_ptr<SQLStatement> PEGTransformerFactory::TransformVariableAssignment(PEGTransformer &transformer,
                                                                            const Identifier &identifier,
                                                                            unique_ptr<ParsedExpression> expression) {
	return make_uniq<SetVariableStatement>(identifier, std::move(expression), SetScope::VARIABLE);
}

// SetSchema <- SettingScope? 'SCHEMA' StringLiteral
unique_ptr<SetStatement> PEGTransformerFactory::TransformSetSchema(PEGTransformer &transformer,
                                                                   const optional<SetScope> &setting_scope,
                                                                   const string &string_literal) {
	if (setting_scope && *setting_scope == SetScope::LOCAL) {
		throw NotImplementedException("SET LOCAL is not implemented.");
	}
	auto value = ConstantExpression::String(string_literal);
	return make_uniq<SetVariableStatement>("schema", std::move(value), GetSettingScope(setting_scope));
}

// ZoneLocal <- 'LOCAL'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneLocal(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

// ZoneDefault <- 'DEFAULT'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneDefault(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

// ZoneStringLiteral <- StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneStringLiteral(PEGTransformer &transformer,
                                                                               const string &string_literal) {
	return ConstantExpression::String(string_literal);
}

// ZoneIdentifier <- Identifier
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneIdentifier(PEGTransformer &transformer,
                                                                            const Identifier &identifier) {
	return ConstantExpression::String(identifier.GetIdentifierName());
}

// SetTimeZone <- SettingScope? 'TIME' 'ZONE' ZoneValue
unique_ptr<SetStatement> PEGTransformerFactory::TransformSetTimeZone(PEGTransformer &transformer,
                                                                     const optional<SetScope> &setting_scope,
                                                                     unique_ptr<ParsedExpression> zone_value) {
	if (setting_scope && *setting_scope == SetScope::LOCAL) {
		throw NotImplementedException("SET LOCAL is not implemented.");
	}
	auto scope = GetSettingScope(setting_scope);
	if (zone_value->GetExpressionClass() == ExpressionClass::DEFAULT) {
		return make_uniq<ResetVariableStatement>("timezone", scope);
	}
	return make_uniq<SetVariableStatement>("timezone", std::move(zone_value), scope);
}

// SetVariable <- VariableScope Identifier
SettingInfo PEGTransformerFactory::TransformSetVariable(PEGTransformer &transformer, const SetScope &variable_scope,
                                                        const Identifier &identifier) {
	SettingInfo result;
	result.name = identifier;
	result.scope = variable_scope;
	return result;
}

// StandardAssignment <- SetVariableOrSetting SetAssignment
unique_ptr<SetStatement>
PEGTransformerFactory::TransformStandardAssignment(PEGTransformer &transformer,
                                                   const SettingInfo &set_variable_or_setting,
                                                   vector<unique_ptr<ParsedExpression>> set_assignment) {
	if (set_variable_or_setting.scope == SetScope::LOCAL) {
		throw NotImplementedException("SET LOCAL is not implemented.");
	}
	if (set_assignment.size() > 1) {
		throw ParserException("SET can only contain a single value");
	}
	auto value = std::move(set_assignment[0]);
	if (value->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		// SET value cannot be a column reference
		auto &col_ref = value->Cast<ColumnRefExpression>();
		if (col_ref.IsQualified()) {
			value = ConstantExpression::String(col_ref.ToString());
		} else {
			value = ConstantExpression::String(col_ref.GetColumnName().GetIdentifierName());
		}
	} else if (value->GetExpressionClass() == ExpressionClass::DEFAULT) {
		return make_uniq<ResetVariableStatement>(set_variable_or_setting.name, set_variable_or_setting.scope);
	}
	return make_uniq<SetVariableStatement>(set_variable_or_setting.name, std::move(value),
	                                       set_variable_or_setting.scope);
}

// VariableList <- List(Expression)
vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformVariableList(PEGTransformer &transformer,
                                             vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

// VariableScope <- 'VARIABLE'
SetScope PEGTransformerFactory::TransformVariableScope(PEGTransformer &transformer) {
	return SetScope::VARIABLE;
}

// LocalScope <- 'LOCAL'
SetScope PEGTransformerFactory::TransformLocalScope(PEGTransformer &transformer) {
	return SetScope::LOCAL;
}

// SessionScope <- 'SESSION'
SetScope PEGTransformerFactory::TransformSessionScope(PEGTransformer &transformer) {
	return SetScope::SESSION;
}

// GlobalScope <- 'GLOBAL'
SetScope PEGTransformerFactory::TransformGlobalScope(PEGTransformer &transformer) {
	return SetScope::GLOBAL;
}

// ZoneIntervalWithInterval <- 'INTERVAL' StringLiteral Interval?
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformZoneIntervalWithInterval(PEGTransformer &transformer, const string &string_literal,
                                                         const optional<DatePartSpecifier> &interval) {
	auto expr = ConstantExpression::String(string_literal);
	return make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
}

// ZoneIntervalWithPrecision <- 'INTERVAL' Parens(NumberLiteral) StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneIntervalWithPrecision(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal, const string &string_literal) {
	auto expr = ConstantExpression::String(string_literal);
	return make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
}

} // namespace duckdb
