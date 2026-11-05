#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/statement/export_statement.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformExportStatement(PEGTransformer &transformer, const optional<string> &export_source,
                                                const string &string_literal,
                                                const optional<vector<GenericCopyOption>> &export_options) {
	auto info = make_uniq<CopyInfo>();
	info->file_path = string_literal;
	info->format = "csv";
	info->is_from = false;

	if (export_options) {
		for (const auto &option : *export_options) {
			if (option.name == "format") {
				if (option.expression) {
					throw ParserException(
					    "Unsupported parameter type for FORMAT: expected e.g. FORMAT 'csv', 'parquet'");
				}
				if (option.children.empty()) {
					throw ParserException("FORMAT requires a parameter, e.g. FORMAT 'csv' or FORMAT 'parquet'");
				}
				info->format = option.children[0].GetValue<string>();
				info->is_format_auto_detected = false;
			} else if (option.expression) {
				info->parsed_options[option.name] = option.expression->Copy();
			} else {
				info->options[option.name] = option.children;
			}
		}
	}

	auto result = make_uniq<ExportStatement>(std::move(info));
	if (export_source) {
		result->database = *export_source;
	}
	return std::move(result);
}

string PEGTransformerFactory::TransformExportSource(PEGTransformer &transformer, const Identifier &catalog_name) {
	return catalog_name.GetIdentifierName();
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformImportStatement(PEGTransformer &transformer,
                                                                         const string &string_literal) {
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "import_database";
	result->info->parameters.emplace_back(ConstantExpression::String(string_literal));
	return std::move(result);
}

} // namespace duckdb
