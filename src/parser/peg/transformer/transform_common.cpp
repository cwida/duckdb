#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/parser/expression/type_expression.hpp"

namespace duckdb {

Identifier PEGTransformerFactory::TransformAnalyzeKeyword(PEGTransformer &transformer) {
	return Identifier("analyze");
}

string PEGTransformerFactory::TransformIdentifierOrKeyword(PEGTransformer &transformer, ParseResult &parse_result) {
	if (parse_result.type == ParseResultType::IDENTIFIER) {
		return parse_result.Cast<IdentifierParseResult>().identifier.GetIdentifierName();
	}
	if (parse_result.type == ParseResultType::KEYWORD) {
		return parse_result.Cast<KeywordParseResult>().keyword;
	}
	if (parse_result.type == ParseResultType::CHOICE) {
		auto &choice_pr = parse_result.Cast<ChoiceParseResult>();
		return TransformIdentifierOrKeyword(transformer, choice_pr.GetResult());
	}
	if (parse_result.type == ParseResultType::LIST) {
		auto &list_pr = parse_result.Cast<ListParseResult>();
		for (auto child : list_pr.GetChildren()) {
			if (child.get().type == ParseResultType::LIST &&
			    child.get().Cast<ListParseResult>().GetChildren().empty()) {
				continue;
			}
			if (child.get().type == ParseResultType::CHOICE) {
				auto &choice_result = child.get().Cast<ChoiceParseResult>().GetResult();
				return TransformIdentifierOrKeyword(transformer, choice_result);
			}
			if (child.get().type == ParseResultType::LIST) {
				return TransformIdentifierOrKeyword(transformer, child.get());
			}
			if (child.get().type == ParseResultType::IDENTIFIER) {
				return child.get().Cast<IdentifierParseResult>().identifier.GetIdentifierName();
			}
			if (child.get().type == ParseResultType::KEYWORD) {
				return child.get().Cast<KeywordParseResult>().keyword;
			}
			throw InternalException("Unexpected IdentifierOrKeyword type encountered %s.",
			                        ParseResultToString(child.get().type));
		}
	}
	throw ParserException("Unexpected ParseResult type in identifier transformation.");
}

LogicalType PEGTransformerFactory::TransformType(PEGTransformer &transformer,
                                                 unique_ptr<ParsedExpression> type_variations,
                                                 const optional<vector<int64_t>> &array_bounds) {
	return LogicalType::UNBOUND(TransformNestedType(transformer, std::move(type_variations), array_bounds));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformNestedType(PEGTransformer &transformer,
                                                                        unique_ptr<ParsedExpression> type_variations,
                                                                        const optional<vector<int64_t>> &array_bounds) {
	auto array_depth_guard = transformer.StackCheck(array_bounds ? array_bounds->size() : 0);
	auto type = std::move(type_variations);
	if (array_bounds) {
		for (auto array_size : *array_bounds) {
			vector<unique_ptr<ParsedExpression>> children_types;
			children_types.push_back(std::move(type));

			if (array_size < 0) {
				type = make_uniq<TypeExpression>(Identifier("list"), std::move(children_types));
			} else {
				children_types.push_back(ConstantExpression::Integer(array_size));
				type = make_uniq<TypeExpression>(Identifier("array"), std::move(children_types));
			}
		}
	}
	return type;
}

int64_t PEGTransformerFactory::TransformArrayKeyword(PEGTransformer &transformer) {
	return -1;
}

int64_t PEGTransformerFactory::TransformArrayKeywordWithBounds(PEGTransformer &transformer,
                                                               const int64_t &square_brackets_array) {
	return square_brackets_array;
}

int64_t PEGTransformerFactory::TransformSquareBracketsArray(PEGTransformer &transformer,
                                                            optional<unique_ptr<ParsedExpression>> expression) {
	if (!expression) {
		// Empty array so we return -1 to signify it's a list
		return -1;
	}
	auto &array_size = *expression;
	if (array_size->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("Expected a constant number as array size");
	}
	auto &const_number = array_size->Cast<ConstantExpression>();
	int64_t number_val;
	if (!const_number.GetLiteral().TryGetInt64(number_val)) {
		throw BinderException("Expected an integer as array bound instead of %s",
		                      const_number.GetLiteral().ToValue().ToString());
	}
	if (number_val < 0) {
		throw ParserException("Array size must be greater than 0");
	}
	return number_val;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformTimeType(PEGTransformer &transformer, const LogicalTypeId &time_or_timestamp,
                                         optional<vector<unique_ptr<ParsedExpression>>> type_modifiers,
                                         const optional<bool> &time_zone) {
	auto type = time_or_timestamp;
	vector<unique_ptr<ParsedExpression>> modifiers;
	if (type_modifiers) {
		modifiers = std::move(*type_modifiers);
	}
	auto with_timezone = time_zone && *time_zone;
	if (type == LogicalTypeId::TIME) {
		if (!modifiers.empty()) {
			throw ParserException("Type TIME does not allow any modifiers");
		}
		if (with_timezone) {
			return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIME_TZ)),
			                                 vector<unique_ptr<ParsedExpression>> {});
		}
		return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIME)),
		                                 vector<unique_ptr<ParsedExpression>> {});
	}
	if (type == LogicalTypeId::TIMESTAMP) {
		if (modifiers.empty()) {
			if (with_timezone) {
				return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIMESTAMP_TZ)),
				                                 vector<unique_ptr<ParsedExpression>> {});
			}
			return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIMESTAMP)),
			                                 vector<unique_ptr<ParsedExpression>> {});
		}
		if (with_timezone) {
			throw ParserException("TIMESTAMP WITH TIME ZONE does not take a precision");
		}
		if (modifiers.size() > 1) {
			throw ParserException("TIMESTAMP only supports a single modifier");
		}
		if (modifiers[0]->GetExpressionClass() != ExpressionClass::CONSTANT) {
			throw ParserException("Expected a constant expression for timestamp precision");
		}
		auto &precision_literal = modifiers[0]->Cast<ConstantExpression>().GetLiteral();
		if (precision_literal.IsNull()) {
			throw ParserException("TIMESTAMP precision cannot be NULL");
		}
		int64_t timestamp_precision;
		if (!precision_literal.TryGetInt64(timestamp_precision)) {
			throw ParserException("TIMESTAMP precision must be an integral type");
		}
		if (timestamp_precision > 10) {
			throw ParserException("TIMESTAMP only supports until nano-second precision (9)");
		}
		if (timestamp_precision < 0) {
			throw ParserException("TIMESTAMP precision should be between 0 and 10 (inclusive)");
		}
		if (timestamp_precision == 0) {
			return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIMESTAMP_S)),
			                                 vector<unique_ptr<ParsedExpression>> {});
		}
		if (timestamp_precision <= 3) {
			return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIMESTAMP_MS)),
			                                 vector<unique_ptr<ParsedExpression>> {});
		}
		if (timestamp_precision <= 6) {
			// Corresponds to microseconds, which is the default TIMESTAMP
			return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIMESTAMP)),
			                                 vector<unique_ptr<ParsedExpression>> {});
		}
		return make_uniq<TypeExpression>(Identifier(EnumUtil::ToString(LogicalType::TIMESTAMP_NS)),
		                                 vector<unique_ptr<ParsedExpression>> {});
	}
	throw ParserException("Unexpected time type encountered");
}

bool PEGTransformerFactory::TransformTimeZone(PEGTransformer &transformer, const bool &with_or_without) {
	return with_or_without;
}

bool PEGTransformerFactory::TransformWithRule(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformWithoutRule(PEGTransformer &transformer) {
	return false;
}

LogicalTypeId PEGTransformerFactory::TransformTimeTypeId(PEGTransformer &transformer) {
	return LogicalTypeId::TIME;
}

LogicalTypeId PEGTransformerFactory::TransformTimestampTypeId(PEGTransformer &transformer) {
	return LogicalTypeId::TIMESTAMP;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformSimpleNumericType(PEGTransformer &transformer,
                                                                               const string &child) {
	return make_uniq<TypeExpression>(Identifier(child), vector<unique_ptr<ParsedExpression>> {});
}

string PEGTransformerFactory::TransformIntType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::INTEGER);
}

string PEGTransformerFactory::TransformIntegerType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::INTEGER);
}

string PEGTransformerFactory::TransformSmallintType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::SMALLINT);
}

string PEGTransformerFactory::TransformBigintType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::BIGINT);
}

string PEGTransformerFactory::TransformRealType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::FLOAT);
}

string PEGTransformerFactory::TransformBooleanType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::BOOLEAN);
}

string PEGTransformerFactory::TransformDoubleType(PEGTransformer &transformer) {
	return LogicalTypeIdToString(LogicalTypeId::DOUBLE);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformFloatType(PEGTransformer &transformer,
                                          optional<unique_ptr<ParsedExpression>> number_literal) {
	if (!number_literal) {
		return make_uniq<TypeExpression>(Identifier("FLOAT"), vector<unique_ptr<ParsedExpression>> {});
	}
	vector<unique_ptr<ParsedExpression>> modifiers;
	modifiers.push_back(std::move(number_literal.value()));
	return make_uniq<TypeExpression>(Identifier("FLOAT"), std::move(modifiers));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformDecimalType(PEGTransformer &transformer,
                                            optional<vector<unique_ptr<ParsedExpression>>> type_modifiers) {
	vector<unique_ptr<ParsedExpression>> modifiers;
	if (type_modifiers) {
		modifiers = std::move(*type_modifiers);
	}
	return make_uniq<TypeExpression>(Identifier("DECIMAL"), std::move(modifiers));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformDecType(PEGTransformer &transformer,
                                        optional<vector<unique_ptr<ParsedExpression>>> type_modifiers) {
	return TransformDecimalType(transformer, std::move(type_modifiers));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformNumericModType(PEGTransformer &transformer,
                                               optional<vector<unique_ptr<ParsedExpression>>> type_modifiers) {
	return TransformDecimalType(transformer, std::move(type_modifiers));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformTypeModifiers(PEGTransformer &transformer,
                                              optional<vector<unique_ptr<ParsedExpression>>> expression) {
	if (!expression) {
		return vector<unique_ptr<ParsedExpression>> {};
	}
	for (auto &expr : *expression) {
		if (expr->GetExpressionClass() != ExpressionClass::CONSTANT) {
			throw ParserException("Expected a constant as type modifier");
		}
	}
	return std::move(*expression);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformCharacterSimpleType(PEGTransformer &transformer,
                                                    optional<vector<unique_ptr<ParsedExpression>>> type_modifiers) {
	vector<unique_ptr<ParsedExpression>> modifiers;
	if (type_modifiers) {
		modifiers = std::move(*type_modifiers);
	}
	return make_uniq<TypeExpression>(Identifier("VARCHAR"), std::move(modifiers));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformQualifiedSimpleType(PEGTransformer &transformer,
                                                    const QualifiedName &qualified_type_name,
                                                    optional<vector<unique_ptr<ParsedExpression>>> type_modifiers) {
	vector<unique_ptr<ParsedExpression>> modifiers;
	if (type_modifiers) {
		modifiers = std::move(*type_modifiers);
	}
	return make_uniq<TypeExpression>(qualified_type_name, std::move(modifiers));
}

QualifiedName PEGTransformerFactory::TransformTypeNameAsQualifiedName(PEGTransformer &transformer,
                                                                      const Identifier &type_name) {
	QualifiedName result(type_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformSchemaReservedTypeName(PEGTransformer &transformer,
                                                                     const Identifier &schema_qualification,
                                                                     const Identifier &reserved_type_name) {
	QualifiedName result({schema_qualification}, reserved_type_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformCatalogReservedSchemaTypeName(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const vector<Identifier> &reserved_schema_qualification, const Identifier &reserved_type_name) {
	vector<Identifier> qualification;
	qualification.push_back(catalog_qualification);
	for (auto &schema : reserved_schema_qualification) {
		qualification.push_back(schema);
	}
	return QualifiedName(std::move(qualification), reserved_type_name);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformMapType(PEGTransformer &transformer,
                                        optional<vector<unique_ptr<ParsedExpression>>> nested_type) {
	vector<unique_ptr<ParsedExpression>> map_children;
	if (nested_type) {
		map_children = std::move(*nested_type);
	}
	return make_uniq<TypeExpression>(Identifier("MAP"), std::move(map_children));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformTupleType(PEGTransformer &transformer,
                                          vector<unique_ptr<ParsedExpression>> nested_type) {
	return make_uniq<TypeExpression>(Identifier("TUPLE"), std::move(nested_type));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformRowType(
    PEGTransformer &transformer, optional<vector<pair<Identifier, unique_ptr<ParsedExpression>>>> col_id_type_list) {
	vector<unique_ptr<ParsedExpression>> struct_children;
	if (col_id_type_list) {
		for (auto &child : *col_id_type_list) {
			child.second->SetAlias(child.first);
			struct_children.push_back(std::move(child.second));
		}
	}
	return make_uniq<TypeExpression>(Identifier("STRUCT"), std::move(struct_children));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformGeometryType(PEGTransformer &transformer,
                                             optional<unique_ptr<ParsedExpression>> expression) {
	if (!expression) {
		return make_uniq<TypeExpression>(Identifier("GEOMETRY"), vector<unique_ptr<ParsedExpression>> {});
	}
	auto geo_modifier = std::move(*expression);
	vector<unique_ptr<ParsedExpression>> geo_children;
	if (geo_modifier->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("Expected a constant as type modifier");
	}
	geo_children.push_back(std::move(geo_modifier));
	return make_uniq<TypeExpression>(Identifier("GEOMETRY"), std::move(geo_children));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformVariantType(PEGTransformer &transformer) {
	return make_uniq<TypeExpression>(Identifier("VARIANT"), vector<unique_ptr<ParsedExpression>> {});
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformUnionType(PEGTransformer &transformer,
                                          vector<pair<Identifier, unique_ptr<ParsedExpression>>> col_id_type_list) {
	vector<unique_ptr<ParsedExpression>> union_children;
	for (auto &colid : col_id_type_list) {
		colid.second->SetAlias(colid.first);
		union_children.push_back(std::move(colid.second));
	}
	return make_uniq<TypeExpression>(Identifier("UNION"), std::move(union_children));
}

vector<pair<Identifier, unique_ptr<ParsedExpression>>>
PEGTransformerFactory::TransformColIdTypeList(PEGTransformer &transformer,
                                              vector<pair<Identifier, unique_ptr<ParsedExpression>>> col_id_type) {
	return col_id_type;
}

pair<Identifier, unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformColIdType(PEGTransformer &transformer, const Identifier &col_id,
                                          unique_ptr<ParsedExpression> nested_type) {
	return make_pair(Identifier(col_id), std::move(nested_type));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformBitType(
    PEGTransformer &transformer, const bool &has_result,
    optional<vector<unique_ptr<ParsedExpression>>> expression) { // NOLINT(performance-unnecessary-value-param)
	return make_uniq<TypeExpression>(Identifier("BIT"), vector<unique_ptr<ParsedExpression>> {});
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformIntervalWithoutSpecifier(PEGTransformer &transformer) {
	return make_uniq<TypeExpression>(Identifier("INTERVAL"), vector<unique_ptr<ParsedExpression>> {});
}

DatePartSpecifier PEGTransformerFactory::TransformIntervalToIntervalAsType(PEGTransformer &transformer,
                                                                           ParseResult &parse_result) {
	return DatePartSpecifier::INVALID;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformIntervalWithRangeSpecifier(PEGTransformer &transformer,
                                                           const DatePartSpecifier &interval_to_interval_as_type) {
	return make_uniq<TypeExpression>(Identifier("INTERVAL"), vector<unique_ptr<ParsedExpression>> {});
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformIntervalWithSimpleSpecifier(PEGTransformer &transformer,
                                                            const DatePartSpecifier &interval) {
	return make_uniq<TypeExpression>(Identifier("INTERVAL"), vector<unique_ptr<ParsedExpression>> {});
}

DatePartSpecifier PEGTransformerFactory::TransformYearKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::YEAR;
}

DatePartSpecifier PEGTransformerFactory::TransformMonthKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::MONTH;
}

DatePartSpecifier PEGTransformerFactory::TransformDayKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::DAY;
}

DatePartSpecifier PEGTransformerFactory::TransformHourKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::HOUR;
}

DatePartSpecifier PEGTransformerFactory::TransformMinuteKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::MINUTE;
}

DatePartSpecifier PEGTransformerFactory::TransformSecondKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::SECOND;
}

DatePartSpecifier PEGTransformerFactory::TransformMillisecondKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::MILLISECONDS;
}

DatePartSpecifier PEGTransformerFactory::TransformMicrosecondKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::MICROSECONDS;
}

DatePartSpecifier PEGTransformerFactory::TransformWeekKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::WEEK;
}

DatePartSpecifier PEGTransformerFactory::TransformQuarterKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::QUARTER;
}

DatePartSpecifier PEGTransformerFactory::TransformDecadeKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::DECADE;
}

DatePartSpecifier PEGTransformerFactory::TransformCenturyKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::CENTURY;
}

DatePartSpecifier PEGTransformerFactory::TransformMillenniumKeyword(PEGTransformer &transformer) {
	return DatePartSpecifier::MILLENNIUM;
}

static DatePartSpecifier UnsupportedIntervalRange(DatePartSpecifier first_interval, DatePartSpecifier second_interval) {
	throw ParserException("%s TO %s is not supported", EnumUtil::ToString(first_interval),
	                      EnumUtil::ToString(second_interval));
}

DatePartSpecifier PEGTransformerFactory::TransformYearToMonth(PEGTransformer &transformer,
                                                              const DatePartSpecifier &year_keyword,
                                                              const DatePartSpecifier &month_keyword) {
	return UnsupportedIntervalRange(year_keyword, month_keyword);
}

DatePartSpecifier PEGTransformerFactory::TransformDayToHour(PEGTransformer &transformer,
                                                            const DatePartSpecifier &day_keyword,
                                                            const DatePartSpecifier &hour_keyword) {
	return UnsupportedIntervalRange(day_keyword, hour_keyword);
}

DatePartSpecifier PEGTransformerFactory::TransformDayToMinute(PEGTransformer &transformer,
                                                              const DatePartSpecifier &day_keyword,
                                                              const DatePartSpecifier &minute_keyword) {
	return UnsupportedIntervalRange(day_keyword, minute_keyword);
}

DatePartSpecifier PEGTransformerFactory::TransformDayToSecond(PEGTransformer &transformer,
                                                              const DatePartSpecifier &day_keyword,
                                                              const DatePartSpecifier &second_keyword) {
	return UnsupportedIntervalRange(day_keyword, second_keyword);
}

DatePartSpecifier PEGTransformerFactory::TransformHourToMinute(PEGTransformer &transformer,
                                                               const DatePartSpecifier &hour_keyword,
                                                               const DatePartSpecifier &minute_keyword) {
	return UnsupportedIntervalRange(hour_keyword, minute_keyword);
}

DatePartSpecifier PEGTransformerFactory::TransformHourToSecond(PEGTransformer &transformer,
                                                               const DatePartSpecifier &hour_keyword,
                                                               const DatePartSpecifier &second_keyword) {
	return UnsupportedIntervalRange(hour_keyword, second_keyword);
}

DatePartSpecifier PEGTransformerFactory::TransformMinuteToSecond(PEGTransformer &transformer,
                                                                 const DatePartSpecifier &minute_keyword,
                                                                 const DatePartSpecifier &second_keyword) {
	return UnsupportedIntervalRange(minute_keyword, second_keyword);
}

// NumberLiteral <- < [+-]?[0-9]*([.][0-9]*)? >
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformNumberLiteral(PEGTransformer &transformer,
                                                                           ParseResult &parse_result) {
	auto &literal_pr = parse_result.Cast<NumberParseResult>();
	return ConstantExpression::Number(literal_pr.number);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformSetofType(PEGTransformer &transformer,
                                                                       unique_ptr<ParsedExpression> nested_type) {
	return nested_type;
}

// StringLiteral <- '\'' [^\']* '\''
string PEGTransformerFactory::TransformStringLiteral(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &string_literal_pr = parse_result.Cast<StringLiteralParseResult>();
	return string_literal_pr.result;
}

Identifier PEGTransformerFactory::TransformConstraintName(PEGTransformer &transformer,
                                                          const Identifier &col_id_or_string) {
	return Identifier(col_id_or_string);
}
} // namespace duckdb
