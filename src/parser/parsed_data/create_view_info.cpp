#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

namespace duckdb {

CreateViewInfo::CreateViewInfo() : CreateInfo(CatalogType::VIEW_ENTRY, Identifier::InvalidSchema()) {
}
CreateViewInfo::CreateViewInfo(const QualifiedName &view_name)
    : CreateInfo(CatalogType::VIEW_ENTRY, view_name.Schema(), view_name.Catalog()) {
	SetViewName(view_name.Name());
}

CreateViewInfo::CreateViewInfo(SchemaCatalogEntry &schema, const Identifier &view_name)
    : CreateViewInfo(schema.GetQualifiedName(view_name)) {
}

string CreateViewInfo::ToString() const {
	string result = GetCreatePrefix(security_type == ViewSecurityType::SECURE_VIEW ? "SECURE VIEW" : "VIEW");
	result += QualifiedNameToString();
	if (!aliases.empty()) {
		result += " (";
		result +=
		    StringUtil::Join(aliases, aliases.size(), ", ", [](const Identifier &name) { return SQLIdentifier(name); });
		result += ")";
	}
	if (binding_mode == CreateViewBindingMode::SKIP_BINDING) {
		result += " WITH (DEFER_BINDING)";
	}
	result += " AS ";
	result += query->ToString();
	result += ";";
	return result;
}

unique_ptr<CreateInfo> CreateViewInfo::Copy() const {
	auto result = make_uniq<CreateViewInfo>(GetQualifiedName());
	CopyProperties(*result);
	result->aliases = aliases;
	result->types = types;
	result->names = names;
	result->column_comments_map = column_comments_map;
	result->binding_mode = binding_mode;
	result->security_type = security_type;
	result->query = unique_ptr_cast<SQLStatement, SelectStatement>(query->Copy());
	return std::move(result);
}

unique_ptr<SelectStatement> CreateViewInfo::ParseSelect(Parser &parser, const string &sql) {
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException(
		    "Failed to create view from SQL string - \"%s\" - statement did not contain a single SELECT statement",
		    sql);
	}
	D_ASSERT(parser.statements.size() == 1 && parser.statements[0]->type == StatementType::SELECT_STATEMENT);
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

unique_ptr<CreateViewInfo> CreateViewInfo::FromSelect(Parser &parser, unique_ptr<CreateViewInfo> info) {
	D_ASSERT(info);
	D_ASSERT(!info->GetViewName().empty());
	D_ASSERT(!info->sql.empty());
	D_ASSERT(!info->query);

	info->query = ParseSelect(parser, info->sql);
	return info;
}

unique_ptr<CreateViewInfo> CreateViewInfo::FromCreateView(ClientContext &context, SchemaCatalogEntry &schema,
                                                          const string &sql) {
	D_ASSERT(!sql.empty());

	// parse the SQL statement
	Parser parser(context);
	parser.ParseQuery(sql);

	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::CREATE_STATEMENT) {
		throw BinderException(
		    "Failed to create view from SQL string - \"%s\" - statement did not contain a single CREATE VIEW statement",
		    sql);
	}
	auto &create_statement = parser.statements[0]->Cast<CreateStatement>();
	if (create_statement.info->type != CatalogType::VIEW_ENTRY) {
		throw BinderException(
		    "Failed to create view from SQL string - \"%s\" - view did not contain a CREATE VIEW statement", sql);
	}

	auto result = unique_ptr_cast<CreateInfo, CreateViewInfo>(std::move(create_statement.info));
	result->SetQualifiedName(
	    QualifiedName(schema.ParentCatalog().GetName(), schema.name, result->GetQualifiedName().Name()));

	auto view_binder = Binder::CreateBinder(context);
	view_binder->BindCreateViewInfo(*result);

	return result;
}

vector<Value> CreateViewInfo::GetColumnCommentsList() const {
	if (column_comments_map.empty()) {
		return vector<Value>();
	}
	if (names.empty()) {
		throw InternalException(
		    "Attempting to serialize column comments using the legacy format, but view is not bound");
	}
	vector<Value> result;
	result.resize(names.size());
	for (auto &entry : column_comments_map) {
		auto it = std::find_if(names.begin(), names.end(), [&](const Identifier &n) { return entry.first == n; });
		if (it == names.end()) {
			throw InternalException("While serializing comments for view %s - did not find column %s in list of names",
			                        GetViewName(), entry.first);
		}
		result[NumericCast<idx_t>(it - names.begin())] = entry.second;
	}
	return result;
}

CreateViewInfo::CreateViewInfo(vector<Identifier> names_p, vector<Value> comments,
                               identifier_map_t<Value> column_comments_p)
    : CreateInfo(CatalogType::VIEW_ENTRY, Identifier::InvalidSchema()), names(std::move(names_p)),
      column_comments_map(std::move(column_comments_p)) {
	if (comments.empty()) {
		return;
	}
	if (!column_comments_map.empty()) {
		throw SerializationException("Either column_comments or column_comments_map should be provided, not both");
	}
	for (idx_t i = 0; i < comments.size(); i++) {
		if (comments[i].IsNull()) {
			continue;
		}
		column_comments_map[names[i]] = std::move(comments[i]);
	}
}

} // namespace duckdb
