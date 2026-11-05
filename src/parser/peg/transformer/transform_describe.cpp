#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SelectStatement> PEGTransformerFactory::TransformDescribeStatement(PEGTransformer &transformer,
                                                                              unique_ptr<QueryNode> child) {
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(child);
	return select_statement;
}

// Wrap a ShowRef as the query "SELECT * FROM <showref>".
static unique_ptr<QueryNode> WrapShowRef(unique_ptr<ShowRef> showref) {
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(showref);
	return std::move(select_node);
}

// Configure `showref` as the "list all tables" form (bare SHOW/DESCRIBE and SHOW ALL TABLES). The
// __show_tables_expanded name is recognized by the binder (see bind_showref.cpp).
static void SetShowAllTables(ShowRef &showref) {
	showref.SetTableName("__show_tables_expanded");
	showref.show_type = ShowType::SHOW_SPECIAL;
}

//! Build the "describe the columns of this query" QueryNode. Shared by "DESCRIBE/SUMMARIZE (query)" and the
//! deprecated "SHOW (query)".
static unique_ptr<QueryNode> BuildDescribeSelect(ShowType show_type, unique_ptr<SelectStatement> select_statement) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = show_type;
	showref->query = std::move(select_statement->node);
	return WrapShowRef(std::move(showref));
}

unique_ptr<QueryNode>
PEGTransformerFactory::TransformDescribeSelect(PEGTransformer &transformer, const ShowType &describe_or_summarize,
                                               unique_ptr<SelectStatement> select_statement_internal) {
	return BuildDescribeSelect(describe_or_summarize, std::move(select_statement_internal));
}

unique_ptr<QueryNode>
PEGTransformerFactory::TransformShowDeprecatedSelect(PEGTransformer &transformer, const ShowType &show_rule,
                                                     unique_ptr<SelectStatement> select_statement_internal) {
	return BuildDescribeSelect(show_rule, std::move(select_statement_internal));
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowTables(PEGTransformer &transformer,
                                                                 const ShowType &show_or_describe,
                                                                 const QualifiedName &qualified_name) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = ShowType::SHOW_FROM;
	if (!IsInvalidCatalog(qualified_name.Catalog())) {
		throw ParserException("Expected \"SHOW TABLES FROM database\", \"SHOW TABLES FROM schema\", or "
		                      "\"SHOW TABLES FROM database.schema\"");
	}
	if (IsInvalidSchema(qualified_name.Schema())) {
		showref->SetSchemaName(qualified_name.Name());
	} else {
		showref->SetCatalogName(qualified_name.Schema());
		showref->SetSchemaName(qualified_name.Name());
	}
	return WrapShowRef(std::move(showref));
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowAllTables(PEGTransformer &transformer,
                                                                    const ShowType &show_or_describe,
                                                                    const bool &has_result) {
	auto showref = make_uniq<ShowRef>();
	SetShowAllTables(*showref);
	return WrapShowRef(std::move(showref));
}

// The special MySQL-inherited forms - "[SHOW|DESCRIBE] DATABASES|SCHEMAS|TABLES|VARIABLES" - which the binder
// dispatches to a dedicated pragma. Returns true (and configures `showref`) if `relation` is one of them.
static bool TrySetSpecialShowForm(ShowRef &showref, const BaseTableRef &relation) {
	if (!IsInvalidSchema(relation.GetQualifiedName().Schema())) {
		// A qualified name is never a special form.
		return false;
	}

	auto name = StringUtil::Lower(relation.Table().GetIdentifierName());
	if (name != "databases" && name != "tables" && name != "schemas" && name != "variables") {
		return false;
	}

	showref.SetTableName(Identifier("\"" + name + "\""));
	showref.show_type = ShowType::SHOW_SPECIAL;
	return true;
}

// Describe the columns of `relation` by binding it as "SELECT * FROM <relation>".
static void SetDescribeQuery(ShowRef &showref, unique_ptr<BaseTableRef> relation) {
	auto query = make_uniq<SelectNode>();
	query->select_list.push_back(make_uniq<StarExpression>());
	query->from_table = std::move(relation);
	showref.query = std::move(query);
}

// True when nothing follows the keyword (bare "SHOW" / "DESCRIBE" / "SUMMARIZE").
static bool HasNoTarget(const DescribeTarget &target) {
	return !target.is_table_name && !target.table_ref;
}

//! Build the ShowRef for "SHOW <target>". SHOW is settings-first: a bare name is offered to the binder as a setting
//! (falling back to describing a table); a qualified name or the special forms describe/list directly.
static unique_ptr<ShowRef> BuildShowByName(ShowType show_type, optional<DescribeTarget> show_target) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = show_type;

	if (!show_target || HasNoTarget(*show_target)) {
		// Bare "SHOW" lists all tables.
		SetShowAllTables(*showref);
		return showref;
	}

	auto target = std::move(*show_target);
	if (target.is_table_name) {
		// "SHOW 'literal'": the string names the table directly.
		showref->SetTableName(target.table_name);
		return showref;
	}

	if (TrySetSpecialShowForm(*showref, *target.table_ref)) {
		// "SHOW DATABASES" and friends.
		return showref;
	}

	if (!IsInvalidSchema(target.table_ref->GetQualifiedName().Schema())) {
		// "SHOW cat.tbl" (deprecated): a qualified name can't be a setting, so record it and describe the table via a
		// query. The recorded name lets the binder report a table (not a setting) error if it does not exist.
		showref->qualified_name = target.table_ref->GetQualifiedName();
		SetDescribeQuery(*showref, std::move(target.table_ref));
		return showref;
	}

	// Bareword "SHOW name" is settings-first: name the table so the binder tries a setting, then (deprecated) falls
	// back to describing a table with this name.
	showref->SetTableName(target.table_ref->Table());
	return showref;
}

//! Build the ShowRef for "DESCRIBE <target>" / "SUMMARIZE <target>", which always describe a relation or query.
static unique_ptr<ShowRef> BuildDescribeByName(ShowType show_type, optional<DescribeTarget> describe_target) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = show_type;

	if (!describe_target || HasNoTarget(*describe_target)) {
		// Bare "DESCRIBE" lists all tables; "SUMMARIZE" requires a target.
		if (show_type == ShowType::SUMMARY) {
			throw ParserException("Expected table name with SUMMARIZE");
		}
		SetShowAllTables(*showref);
		return showref;
	}

	auto target = std::move(*describe_target);
	if (target.is_table_name) {
		// "DESCRIBE 'literal'": the string names the table directly.
		showref->SetTableName(target.table_name);
		return showref;
	}

	if (TrySetSpecialShowForm(*showref, *target.table_ref)) {
		// "DESCRIBE DATABASES" and friends.
		return showref;
	}

	// Describe the (possibly qualified) relation's columns via a query.
	SetDescribeQuery(*showref, std::move(target.table_ref));
	return showref;
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowByName(PEGTransformer &transformer, const ShowType &show_rule,
                                                                 optional<DescribeTarget> show_target) {
	return WrapShowRef(BuildShowByName(show_rule, std::move(show_target)));
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformDescribeByName(PEGTransformer &transformer,
                                                                     const ShowType &describe_or_summarize,
                                                                     optional<DescribeTarget> describe_target) {
	return WrapShowRef(BuildDescribeByName(describe_or_summarize, std::move(describe_target)));
}

DescribeTarget PEGTransformerFactory::TransformShowSettingName(PEGTransformer &transformer,
                                                               const Identifier &setting_name) {
	// A bare "SHOW name" - the name is an unqualified relation/setting; BuildShowRef tries it as a setting first.
	DescribeTarget result;
	auto table_ref = make_uniq<BaseTableRef>();
	table_ref->SetTable(setting_name);
	result.table_ref = std::move(table_ref);
	return result;
}

DescribeTarget
PEGTransformerFactory::TransformShowDeprecatedQualifiedTableName(PEGTransformer &transformer,
                                                                 unique_ptr<BaseTableRef> qualified_table_name) {
	// A qualified "SHOW cat.tbl" always describes a table.
	DescribeTarget result;
	result.table_ref = std::move(qualified_table_name);
	return result;
}

DescribeTarget PEGTransformerFactory::TransformDescribeBaseTableName(PEGTransformer &transformer,
                                                                     unique_ptr<BaseTableRef> base_table_name) {
	DescribeTarget result;
	result.table_ref = std::move(base_table_name);
	return result;
}

DescribeTarget PEGTransformerFactory::TransformDescribeStringLiteral(PEGTransformer &transformer,
                                                                     const string &string_literal) {
	if (string_literal.empty()) {
		throw ParserException("Expected a table name, but got an empty string");
	}
	DescribeTarget result;
	result.is_table_name = true;
	result.table_name = Identifier(string_literal);
	return result;
}

ShowType PEGTransformerFactory::TransformSummarizeRule(PEGTransformer &transformer) {
	return ShowType::SUMMARY;
}

ShowType PEGTransformerFactory::TransformSummarize(PEGTransformer &transformer, const ShowType &summarize_rule) {
	return summarize_rule;
}

ShowType PEGTransformerFactory::TransformShowRule(PEGTransformer &transformer) {
	return ShowType::SHOW;
}

ShowType PEGTransformerFactory::TransformDescribeLongRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

ShowType PEGTransformerFactory::TransformDescRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

} // namespace duckdb
