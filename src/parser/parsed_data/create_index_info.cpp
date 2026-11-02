#include "duckdb/parser/parsed_data/create_index_info.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

CreateIndexInfo::CreateIndexInfo() : CreateInfo(CatalogType::INDEX_ENTRY, Identifier::InvalidSchema()) {
}

CreateIndexInfo::CreateIndexInfo(const duckdb::CreateIndexInfo &info)
    : CreateInfo(CatalogType::INDEX_ENTRY), table(info.table), options(info.options), index_type(info.index_type),
      constraint_type(info.constraint_type), column_ids(info.column_ids), scan_types(info.scan_types),
      names(info.names) {
	SetQualifiedName(info.GetQualifiedName());
}

static void RemoveTableQualificationRecursive(unique_ptr<ParsedExpression> &root_expr, const Identifier &table_name) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    *root_expr, [&](ColumnRefExpression &col_ref) {
		    auto &col_names = col_ref.ColumnNamesMutable();
		    // the table qualifier is the component directly before the column name
		    if (col_ref.IsQualified() && col_names[col_names.size() - 2] == table_name) {
			    col_names.erase(col_names.begin());
		    }
	    });
}

vector<string> CreateIndexInfo::ExpressionsToList() const {
	vector<string> list;

	for (idx_t i = 0; i < parsed_expressions.size(); i++) {
		auto &expr = parsed_expressions[i];
		auto copy = expr->Copy();

		// Column reference expressions are qualified with the table name.
		// We need to remove them to reproduce the original query.
		RemoveTableQualificationRecursive(copy, table);
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
