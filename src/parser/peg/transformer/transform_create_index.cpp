#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateIndexStmt(
    PEGTransformer &transformer, const optional<bool> &unique_index, const bool &concurrently,
    const optional<bool> &if_not_exists, const optional<Identifier> &index_name,
    unique_ptr<BaseTableRef> base_table_name, const optional<Identifier> &index_type,
    vector<unique_ptr<ParsedExpression>> index_element,
    optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list,
    optional<unique_ptr<ParsedExpression>> where_clause) {
	auto result = make_uniq<CreateStatement>();
	auto index_info = make_uniq<CreateIndexInfo>();
	index_info->constraint_type = unique_index ? IndexConstraintType::UNIQUE : IndexConstraintType::NONE;
	index_info->on_conflict =
	    if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	if (!index_name) {
		throw NotImplementedException("Please provide an index name, e.g., CREATE INDEX my_name ...");
	}
	index_info->table = base_table_name->Table();
	// the index lives in the same (possibly nested) schema as the table it is created on
	index_info->SetQualifiedName(base_table_name->GetQualifiedName().WithName(*index_name));
	index_info->index_type = index_type ? index_type->GetIdentifierName() : "ART";
	for (auto &expr : index_element) {
		if (!index_type && expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &column_ref = expr->Cast<ColumnRefExpression>();
			if (!column_ref.IsQualified()) {
				auto column_name = column_ref.GetColumnName();
				expr = make_uniq<ColumnRefExpression>(std::move(column_name), base_table_name->Table());
			}
		}
		if (expr->GetExpressionType() == ExpressionType::COLLATE) {
			throw NotImplementedException("Index with collation not supported yet!");
		}
		index_info->expressions.push_back(expr->Copy());
		index_info->parsed_expressions.push_back(std::move(expr));
	}
	if (where_clause) {
		throw NotImplementedException("Creating partial indexes is not supported currently");
	}
	if (with_list) {
		for (auto &option_entry : *with_list) {
			if (option_entry.second->GetExpressionClass() != ExpressionClass::CONSTANT) {
				throw InvalidInputException("Create index option must be a constant value");
			}
			index_info->options[option_entry.first] =
			    option_entry.second->Cast<ConstantExpression>().GetLiteral().ToValue();
		}
	}
	result->info = std::move(index_info);
	return result;
}

string PEGTransformerFactory::TransformDottedColLabel(PEGTransformer &transformer, const string &col_label,
                                                      const optional<vector<string>> &dot_col_label) {
	vector<string> parts {col_label};
	if (dot_col_label) {
		parts.insert(parts.end(), dot_col_label->begin(), dot_col_label->end());
	}
	return StringUtil::Join(parts, ".");
}

Identifier PEGTransformerFactory::TransformIndexType(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformIndexElement(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
                                             const optional<OrderType> &desc_or_asc,
                                             const optional<OrderByNullType> &nulls_first_or_last) {
	// TODO(Dtenwolde): We currently ignore desc_or_asc and nulls_first_or_last
	return expression;
}

bool PEGTransformerFactory::TransformUniqueIndex(PEGTransformer &transformer) {
	return true;
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformWithList(PEGTransformer &transformer,
                                         case_insensitive_map_t<unique_ptr<ParsedExpression>> rel_option_or_oids) {
	return rel_option_or_oids;
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformRelOptionList(PEGTransformer &transformer,
                                              vector<pair<Identifier, unique_ptr<ParsedExpression>>> rel_option) {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> result;
	for (auto &option : rel_option) {
		auto &name = option.first.GetIdentifierName();
		if (result.find(name) != result.end()) {
			throw ParserException("Duplicate option \"%s\" in WITH clause", name);
		}
		result.insert({name, std::move(option.second)});
	}
	return result;
}

// Oids <- WithOrWithoutOids 'OIDS'
case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformOids(PEGTransformer &transformer, const bool &with_or_without_oids) {
	throw NotImplementedException("OIDS for index are not yet implemented.");
}

// WithOids <- 'WITH'
bool PEGTransformerFactory::TransformWithOids(PEGTransformer &transformer) {
	return true;
}

// WithoutOids <- 'WITHOUT'
bool PEGTransformerFactory::TransformWithoutOids(PEGTransformer &transformer) {
	return false;
}

Identifier PEGTransformerFactory::TransformRelOptionName(PEGTransformer &transformer, const string &child) {
	return Identifier(child);
}

pair<Identifier, unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformRelOption(PEGTransformer &transformer, const Identifier &rel_option_name,
                                          optional<unique_ptr<ParsedExpression>> rel_option_argument_opt) {
	if (!rel_option_argument_opt) {
		return {rel_option_name, ConstantExpression::Null()};
	}
	return {rel_option_name, std::move(*rel_option_argument_opt)};
}

// RelOptionArgumentOpt <- '=' DefArg
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRelOptionArgumentOpt(PEGTransformer &transformer,
                                                     unique_ptr<ParsedExpression> def_arg) {
	return def_arg;
}

// DefArgNull <- NullLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefArgNull(PEGTransformer &transformer,
                                                                        const Value &null_literal) {
	return ConstantExpression::Null();
}

// DefArgKeyword <- ReservedKeyword
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefArgKeyword(PEGTransformer &transformer,
                                                                           const string &reserved_keyword) {
	return ConstantExpression::String(reserved_keyword);
}

// DefArgStringLiteral <- StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefArgStringLiteral(PEGTransformer &transformer,
                                                                                 const string &string_literal) {
	return ConstantExpression::String(string_literal);
}

// NoneLiteral <- 'NONE'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformNoneLiteral(PEGTransformer &transformer) {
	return ConstantExpression::Null();
}

} // namespace duckdb
