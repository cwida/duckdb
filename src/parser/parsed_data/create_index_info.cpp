#include "duckdb/parser/parsed_data/create_index_info.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"

namespace duckdb {

CreateIndexInfo::CreateIndexInfo() : CreateInfo(CatalogType::INDEX_ENTRY, Identifier::InvalidSchema()) {
}

CreateIndexInfo::CreateIndexInfo(const duckdb::CreateIndexInfo &info)
    : CreateInfo(CatalogType::INDEX_ENTRY), table(info.table), options(info.options), index_type(info.index_type),
      constraint_type(info.constraint_type), column_ids(info.column_ids), scan_types(info.scan_types),
      names(info.names) {
	SetQualifiedName(info.GetQualifiedName());
}

// only a top-level column reference is qualified with the table name when the index is created, column references
// inside expressions are stored as written (and may be struct fields of a column named like the table)
static void RemoveTableQualification(ParsedExpression &expr, const Identifier &table_name) {
	if (expr.GetExpressionType() != ExpressionType::COLUMN_REF) {
		return;
	}
	auto &col_names = expr.Cast<ColumnRefExpression>().ColumnNamesMutable();
	if (col_names.size() == 2 && col_names[0] == table_name) {
		col_names.erase(col_names.begin());
	}
}

vector<string> CreateIndexInfo::ExpressionsToList() const {
	vector<string> list;

	for (idx_t i = 0; i < parsed_expressions.size(); i++) {
		auto &expr = parsed_expressions[i];
		auto copy = expr->Copy();

		// column references are qualified with the table name, remove it to reproduce the original query
		RemoveTableQualification(*copy, table);
		bool add_parenthesis = true;
		if (copy->GetExpressionType() == ExpressionType::COLUMN_REF) {
			auto &column_ref = copy->Cast<ColumnRefExpression>();
			if (!column_ref.IsQualified()) {
				// Only not qualified references like (col1, col2) don't need parenthesis.
				add_parenthesis = false;
			}
		}

		if (add_parenthesis) {
			list.push_back(StringUtil::Format("(%s)", copy->ToString()));
		} else {
			list.push_back(StringUtil::Format("%s", copy->ToString()));
		}
	}
	return list;
}

string CreateIndexInfo::ExpressionsToString() const {
	auto list = ExpressionsToList();
	return StringUtil::Join(list, ", ");
}

string CreateIndexInfo::ToString() const {
	string result;

	result += "CREATE";
	D_ASSERT(constraint_type == IndexConstraintType::UNIQUE || constraint_type == IndexConstraintType::NONE);
	if (constraint_type == IndexConstraintType::UNIQUE) {
		result += " UNIQUE";
	}
	result += " INDEX ";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		result += "IF NOT EXISTS ";
	}
	result += SQLIdentifier(GetIndexName());
	result += " ON ";
	// the index lives in the same (possibly nested) schema as the table it is created on
	auto table_name = GetQualifiedName().WithName(table);
	if (temporary) {
		table_name.StripCatalog();
	}
	result += table_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (index_type != "ART") {
		result += " USING ";
		result += SQLIdentifier(index_type);
		result += " ";
	}
	result += "(";
	result += ExpressionsToString();
	result += ")";
	if (!options.empty()) {
		result += " WITH (";
		idx_t i = 0;
		for (auto &opt : options) {
			if (i > 0) {
				result += ", ";
			}
			result += SQLIdentifier(opt.first);
			if (!opt.second.IsNull()) {
				result += " = " + opt.second.ToSQLString();
			}
			i++;
		}
		result += " )";
	}
	result += ";";
	return result;
}

unique_ptr<CreateInfo> CreateIndexInfo::Copy() const {
	auto result = make_uniq<CreateIndexInfo>(*this);
	CopyProperties(*result);

	for (auto &expr : expressions) {
		result->expressions.push_back(expr->Copy());
	}
	for (auto &expr : parsed_expressions) {
		result->parsed_expressions.push_back(expr->Copy());
	}
	return std::move(result);
}

} // namespace duckdb
