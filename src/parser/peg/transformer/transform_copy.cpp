#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/copy_database_statement.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCopyStatement(PEGTransformer &transformer,
                                                                       unique_ptr<SQLStatement> copy_variations) {
	return copy_variations;
}

void SetCopyOptions(unique_ptr<CopyInfo> &info, vector<GenericCopyOption> &options) {
	identifier_set_t option_names;
	for (auto &option : options) {
		if (option.name == "PARTITIONED_BY") {
			option.name = "partition_by";
		}
		if (option_names.find(option.name) != option_names.end()) {
			throw ParserException("Unexpected duplicate option %s", option.name);
		}
		option_names.insert(option.name);
		if (option.name == "PARTITION_BY" || option.name == "FORCE_QUOTE" || option.name == "FORCE_NOT_NULL" ||
		    option.name == "FORCE_NULL") {
			if (option.expression) {
				info->parsed_options[option.name] = std::move(option.expression);
			} else {
				if (option.children.empty()) {
					throw BinderException("%s expects a column list or * as parameter", option.name);
				}
				vector<unique_ptr<ParsedExpression>> func_children;
				for (const auto &partition : option.children) {
					func_children.push_back(make_uniq<ColumnRefExpression>(partition.GetValue<Identifier>()));
				}
				auto row_func = make_uniq<FunctionExpression>("row", std::move(func_children));
				info->parsed_options[option.name] = std::move(row_func);
			}
		} else if (option.name == "HEADER" || option.name == "ESCAPE") {
			if (option.expression) {
				info->parsed_options[option.name] = std::move(option.expression);
			} else if (option.children.empty()) {
				info->parsed_options[option.name] = nullptr;
			} else {
				info->parsed_options[option.name] = ConstantExpression::FromValue(option.children[0]);
			}
		} else if (option.name == "NULL" || option.name == "NULLSTR") {
			if (option.children.empty()) {
				info->parsed_options[option.name] = std::move(option.expression);
			} else {
				info->parsed_options[option.name] = ConstantExpression::FromValue(option.children[0]);
			}
		} else {
			if (option.expression) {
				info->parsed_options[option.name] = std::move(option.expression);
			} else {
				info->options[option.name] = option.children;
			}
		}
	}
	auto format_option = info->options.find("format");
	if (format_option != info->options.end()) {
		if (format_option->second.empty()) {
			throw ParserException("Unsupported parameter type for FORMAT: expected e.g. FORMAT 'csv', 'parquet'");
		}
		info->format = format_option->second[0].GetValue<string>();
		info->is_format_auto_detected = false;
		info->options.erase(format_option);
	}
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCopySelect(
    PEGTransformer &transformer, unique_ptr<SelectStatement> select_statement_internal,
    unique_ptr<ParsedExpression> copy_file_name, const optional<vector<GenericCopyOption>> &copy_options) {
	auto result = make_uniq<CopyStatement>();
	auto info = make_uniq<CopyInfo>();
	info->is_from = false;
	if (copy_file_name->GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &const_expr = copy_file_name->Cast<ConstantExpression>();
		info->file_path = const_expr.GetLiteral().ToValue().GetValue<string>();
	} else {
		info->file_path_expression = std::move(copy_file_name);
	}
	if (copy_options) {
		auto options = *copy_options;
		SetCopyOptions(info, options);
	}
	info->select_statement = std::move(select_statement_internal->node);
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformCopyFromDatabaseWithFlag(PEGTransformer &transformer, const Identifier &col_id,
                                                         const Identifier &col_id_1,
                                                         const CopyDatabaseType &copy_database_flag) {
	return make_uniq<CopyDatabaseStatement>(Identifier(col_id), Identifier(col_id_1), copy_database_flag);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCopyFromDatabaseWithoutFlag(PEGTransformer &transformer,
                                                                                     const Identifier &col_id,
                                                                                     const Identifier &col_id_1) {
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "copy_database";
	result->info->parameters.emplace_back(ConstantExpression::String(col_id.GetIdentifierName()));
	result->info->parameters.emplace_back(ConstantExpression::String(col_id_1.GetIdentifierName()));
	return std::move(result);
}

CopyDatabaseType PEGTransformerFactory::TransformCopyDatabaseFlag(PEGTransformer &transformer,
                                                                  const CopyDatabaseType &schema_or_data) {
	return schema_or_data;
}

string PEGTransformerFactory::ExtractFormat(const string &file_path) {
	auto format = StringUtil::Lower(file_path);
	if (StringUtil::EndsWith(format, CompressionExtensionFromType(FileCompressionType::GZIP))) {
		format = format.substr(0, format.size() - 3);
	} else if (StringUtil::EndsWith(format, CompressionExtensionFromType(FileCompressionType::ZSTD))) {
		format = format.substr(0, format.size() - 4);
	}
	size_t dot_pos = format.rfind('.');
	if (dot_pos == std::string::npos || dot_pos == format.length() - 1) {
		// No format found
		return "";
	}
	return format.substr(dot_pos + 1);
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformCopyTable(PEGTransformer &transformer, unique_ptr<BaseTableRef> base_table_name,
                                          const optional<vector<string>> &insert_column_list, const bool &from_or_to,
                                          unique_ptr<ParsedExpression> copy_file_name,
                                          const optional<GenericCopyOption> &legacy_option,
                                          const optional<vector<GenericCopyOption>> &copy_options) {
	auto result = make_uniq<CopyStatement>();
	auto info = make_uniq<CopyInfo>();

	info->SetQualifiedName(base_table_name->GetQualifiedName());
	if (insert_column_list) {
		info->select_list = StringsToIdentifiers(*insert_column_list);
	}
	info->is_from = from_or_to;
	if (copy_file_name->GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &const_expr = copy_file_name->Cast<ConstantExpression>();
		info->file_path = const_expr.GetLiteral().ToValue().GetValue<string>();
	} else {
		info->file_path_expression = std::move(copy_file_name);
	}
	info->format = ExtractFormat(info->file_path);

	vector<GenericCopyOption> generic_options;

	if (legacy_option) {
		generic_options.push_back(*legacy_option);
	}

	if (copy_options) {
		generic_options.insert(generic_options.end(), copy_options->begin(), copy_options->end());
	}

	if (!generic_options.empty()) {
		SetCopyOptions(info, generic_options);
	}

	result->info = std::move(info);
	return std::move(result);
}

bool PEGTransformerFactory::TransformCopyFrom(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformCopyTo(PEGTransformer &transformer) {
	return false;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCopyFileNameStringLiteral(PEGTransformer &transformer,
                                                                                       const string &string_literal) {
	return ConstantExpression::String(string_literal);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCopyFileNameIdentifier(PEGTransformer &transformer,
                                                                                    const Identifier &identifier) {
	if (identifier == "stdin") {
		return ConstantExpression::String("/dev/stdin");
	}
	if (identifier == "stdout") {
		return ConstantExpression::String("/dev/stdout");
	}
	return ConstantExpression::String(identifier.GetIdentifierName());
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformCopyFileNameIdentifierColId(PEGTransformer &transformer,
                                                            const Identifier &identifier_col_id) {
	return ConstantExpression::String(identifier_col_id.GetIdentifierName());
}

Identifier PEGTransformerFactory::TransformIdentifierColId(PEGTransformer &transformer, const Identifier &identifier,
                                                           const vector<Identifier> &copy_file_name_suffix) {
	string result = identifier.GetIdentifierName();
	for (const auto &suffix : copy_file_name_suffix) {
		result += ".";
		result += suffix.GetIdentifierName();
	}
	return Identifier(result);
}

GenericCopyOption PEGTransformerFactory::TransformCopyLegacyOption(PEGTransformer &transformer, const bool &has_result,
                                                                   const string &delimiter) {
	return GenericCopyOption("delimiter", delimiter);
}

vector<GenericCopyOption>
PEGTransformerFactory::TransformCopyOptions(PEGTransformer &transformer, const bool &has_result,
                                            const vector<GenericCopyOption> &copy_option_list) {
	return copy_option_list;
}

vector<GenericCopyOption> PEGTransformerFactory::TransformSpecializedOptionList(
    PEGTransformer &transformer, const GenericCopyOption &specialized_option,
    const optional<vector<GenericCopyOption>> &specialized_option_tail) {
	vector<GenericCopyOption> result {specialized_option};
	if (specialized_option_tail) {
		result.insert(result.end(), specialized_option_tail->begin(), specialized_option_tail->end());
	}
	return result;
}

GenericCopyOption PEGTransformerFactory::TransformSpecializedOptionTail(PEGTransformer &transformer,
                                                                        const bool &has_result,
                                                                        const GenericCopyOption &specialized_option) {
	return specialized_option;
}

GenericCopyOption PEGTransformerFactory::TransformEncodingOption(PEGTransformer &transformer,
                                                                 const string &string_literal) {
	return GenericCopyOption("encoding", string_literal);
}

GenericCopyOption PEGTransformerFactory::TransformForceQuoteOption(PEGTransformer &transformer,
                                                                   const optional<bool> &force_quote,
                                                                   const vector<string> &star_symbol_column_list) {
	string func_name = force_quote ? "force_quote" : "quote";
	auto result = GenericCopyOption();
	result.name = Identifier(func_name);
	if (star_symbol_column_list.empty()) {
		result.expression = make_uniq<StarExpression>();
		return result;
	}
	for (auto &col : star_symbol_column_list) {
		result.children.push_back(Value(col));
	}
	return result;
}

GenericCopyOption PEGTransformerFactory::TransformQuoteAsOption(PEGTransformer &transformer, const bool &has_result,
                                                                const string &string_literal) {
	return GenericCopyOption("quote", string_literal);
}

GenericCopyOption PEGTransformerFactory::TransformForceNullOption(PEGTransformer &transformer,
                                                                  const optional<bool> &force_not_null,
                                                                  const vector<string> &column_list) {
	auto result = GenericCopyOption();
	result.name = force_not_null ? "force_not_null" : "force_null";
	for (auto &col : column_list) {
		result.children.push_back(Value(col));
	}
	return result;
}

GenericCopyOption PEGTransformerFactory::TransformPartitionByOption(PEGTransformer &transformer,
                                                                    const vector<string> &partition_by_column_list) {
	auto result = GenericCopyOption();
	result.name = "partition_by";
	if (partition_by_column_list.empty()) {
		result.expression = make_uniq<StarExpression>();
		return result;
	}
	for (auto &col : partition_by_column_list) {
		result.children.push_back(Value(col));
	}
	return result;
}

vector<string> PEGTransformerFactory::TransformStarPartitionByColumnList(PEGTransformer &transformer) {
	return {};
}

vector<string> PEGTransformerFactory::TransformParenthesizedPartitionByColumnList(PEGTransformer &transformer,
                                                                                  const vector<string> &column_list) {
	return column_list;
}

vector<string> PEGTransformerFactory::TransformSinglePartitionByColumnList(PEGTransformer &transformer,
                                                                           const Identifier &col_id) {
	return {col_id.GetIdentifierName()};
}

GenericCopyOption PEGTransformerFactory::TransformNullAsOption(PEGTransformer &transformer, const bool &has_result,
                                                               const string &string_literal) {
	return GenericCopyOption("null", string_literal);
}

GenericCopyOption PEGTransformerFactory::TransformDelimiterAsOption(PEGTransformer &transformer, const bool &has_result,
                                                                    const string &string_literal) {
	return GenericCopyOption("delimiter", string_literal);
}

GenericCopyOption PEGTransformerFactory::TransformEscapeAsOption(PEGTransformer &transformer, const bool &has_result,
                                                                 const string &string_literal) {
	return GenericCopyOption("escape", string_literal);
}

GenericCopyOption PEGTransformerFactory::TransformBinaryOption(PEGTransformer &transformer) {
	return GenericCopyOption("format", Value("binary"));
}

GenericCopyOption PEGTransformerFactory::TransformFreezeOption(PEGTransformer &transformer) {
	return GenericCopyOption("freeze", Value());
}

GenericCopyOption PEGTransformerFactory::TransformOidsOption(PEGTransformer &transformer) {
	return GenericCopyOption("oids", Value());
}

GenericCopyOption PEGTransformerFactory::TransformCsvOption(PEGTransformer &transformer) {
	return GenericCopyOption("format", Value("csv"));
}

GenericCopyOption PEGTransformerFactory::TransformHeaderOption(PEGTransformer &transformer) {
	return GenericCopyOption("header", Value(true));
}

CopyDatabaseType PEGTransformerFactory::TransformCopySchema(PEGTransformer &transformer) {
	return CopyDatabaseType::COPY_SCHEMA;
}

CopyDatabaseType PEGTransformerFactory::TransformCopyData(PEGTransformer &transformer) {
	return CopyDatabaseType::COPY_DATA;
}

bool PEGTransformerFactory::TransformForceQuote(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformForceNotNull(PEGTransformer &transformer) {
	return true;
}

} // namespace duckdb
