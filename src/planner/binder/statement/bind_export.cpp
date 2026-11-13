#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/statement/export_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_export.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/parser/parsed_data/exported_table_data.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"

#include <algorithm>

namespace duckdb {

//! Sanitizes a string to have only low case chars and underscores
string SanitizeExportIdentifier(const Identifier &str) {
	// Copy the original string to result
	string result(str.GetIdentifierName());

	for (idx_t i = 0; i < result.length(); ++i) {
		auto c = result[i];
		if (c >= 'a' && c <= 'z') {
			// If it is lower case just continue
			continue;
		}

		if (c >= 'A' && c <= 'Z') {
			// To lowercase
			result[i] = NumericCast<char>(tolower(c));
		} else {
			// Substitute to underscore
			result[i] = '_';
		}
	}

	return result;
}

bool ReferencedTableIsOrdered(const TableCatalogEntry &referencing_table, const Identifier &referenced_table,
                              catalog_entry_vector_t &ordered) {
	for (auto &entry : ordered) {
		auto &table_entry = entry.get().Cast<TableCatalogEntry>();
		if (&table_entry.ParentSchema() == &referencing_table.ParentSchema() && table_entry.name == referenced_table) {
			// The referenced table is already ordered
			return true;
		}
	}
	return false;
}

void ScanForeignKeyTable(catalog_entry_vector_t &ordered, catalog_entry_vector_t &unordered,
                         bool move_primary_keys_only) {
	catalog_entry_vector_t remaining;

	for (auto &entry : unordered) {
		auto &table_entry = entry.get().Cast<TableCatalogEntry>();
		bool move_to_ordered = true;
		auto &constraints = table_entry.GetConstraints();

		for (auto &cond : constraints) {
			if (cond->type != ConstraintType::FOREIGN_KEY) {
				continue;
			}
			auto &fk = cond->Cast<ForeignKeyConstraint>();
			if (fk.info.type != ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
				continue;
			}

			if (move_primary_keys_only) {
				// This table references a table, don't move it yet
				move_to_ordered = false;
				break;
			} else if (!ReferencedTableIsOrdered(table_entry, fk.info.table, ordered)) {
				// The table that it references isn't ordered yet
				move_to_ordered = false;
				break;
			}
		}

		if (move_to_ordered) {
			ordered.push_back(table_entry);
		} else {
			remaining.push_back(table_entry);
		}
	}
	unordered = remaining;
}

void ReorderTableEntries(catalog_entry_vector_t &tables) {
	catalog_entry_vector_t ordered;
	catalog_entry_vector_t unordered = tables;
	// First only move the tables that don't have any dependencies
	ScanForeignKeyTable(ordered, unordered, true);
	while (!unordered.empty()) {
		// Now we will start moving tables that have foreign key constraints
		// if the tables they reference are already moved
		ScanForeignKeyTable(ordered, unordered, false);
	}
	tables = ordered;
}

string CreateFileName(const string &id_suffix, TableCatalogEntry &table, const string &extension) {
	auto name = SanitizeExportIdentifier(table.name);
	if (table.schema.name == DEFAULT_SCHEMA) {
		return StringUtil::Format("%s%s.%s", name, id_suffix, extension);
	}
	auto schema = SanitizeExportIdentifier(table.schema.name);
	return StringUtil::Format("%s_%s%s.%s", schema, name, id_suffix, extension);
}

static unique_ptr<QueryNode> CreateSelectStatement(CopyStatement &stmt, child_list_t<LogicalType> &select_list) {
	auto ref = make_uniq<BaseTableRef>();
	ref->SetQualifiedName(stmt.info->GetQualifiedName());

	auto statement = make_uniq<SelectNode>();
	statement->from_table = std::move(ref);

	vector<unique_ptr<ParsedExpression>> expressions;
	for (auto &col : select_list) {
		auto expression = make_uniq_base<ParsedExpression, ColumnRefExpression>(col.first);
		expressions.push_back(std::move(expression));
	}

	statement->select_list = std::move(expressions);
	return std::move(statement);
}

unique_ptr<LogicalOperator> Binder::UnionOperators(vector<unique_ptr<LogicalOperator>> nodes, idx_t column_count,
                                                   TableIndex table_index) {
	if (nodes.empty()) {
		return nullptr;
	}
	if (nodes.size() == 1) {
		return std::move(nodes[0]);
	}
	if (!table_index.IsValid()) {
		table_index = GenerateTableIndex();
	}
	return make_uniq<LogicalSetOperation>(table_index, column_count, std::move(nodes),
	                                      LogicalOperatorType::LOGICAL_UNION, true, false);
}

BoundStatement Binder::Bind(ExportStatement &stmt) {
	// COPY TO a file
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	BindCopyFormat(*stmt.info);
	stmt.info->format = StringUtil::Lower(stmt.info->format);

	// lookup the format in the catalog
	auto &copy_function = Catalog::GetEntry<CopyFunctionCatalogEntry>(
	    context,
	    QualifiedName(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), Identifier(stmt.info->format)));
	// bind copy options
	BindCopyOptions(*stmt.info, copy_function.function);
	if (!copy_function.function.copy_to_bind && !copy_function.function.plan) {
		throw NotImplementedException("COPY TO is not supported for FORMAT \"%s\"", stmt.info->format);
	}

	// gather a list of all the tables
	string catalog = stmt.database;
	catalog_entry_vector_t tables;
	auto schemas = Catalog::GetSchemas(context, catalog);
	for (auto &schema : schemas) {
		auto &schema_entry = schema.get();
		if (schema_entry.ParentCatalog().IsTemporaryCatalog()) {
			continue;
		}
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type == CatalogType::TABLE_ENTRY) {
				tables.push_back(entry.Cast<TableCatalogEntry>());
			}
		});
	}

	// reorder tables because of foreign key constraint
	ReorderTableEntries(tables);

	// check for self-referencing foreign keys, which cannot be exported currently, until ALTER TABLE constraints
	// is supported, and we can export additional ALTER TABLEs for the self-references.
	for (auto &t : tables) {
		auto &table = t.get().Cast<TableCatalogEntry>();
		for (auto &constraint : table.GetConstraints()) {
			if (constraint->type != ConstraintType::FOREIGN_KEY) {
				continue;
			}
			auto &fk = constraint->Cast<ForeignKeyConstraint>();
			if (fk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				throw BinderException("Failed to export database: table %s has a self-referencing foreign key "
				                      "constraint which is currently not supported for exporting",
				                      table.name);
			}
		}
	}

	// now generate the COPY statements for each of the tables
	auto &fs = FileSystem::GetFileSystem(context);

	auto exported_tables = make_uniq<BoundExportData>();

	unordered_set<string> table_name_index;
	vector<unique_ptr<LogicalOperator>> export_nodes;
	for (auto &t : tables) {
		auto &table = t.get().Cast<TableCatalogEntry>();
		auto info = make_uniq<CopyInfo>();
		// we copy the options supplied to the EXPORT
		info->format = stmt.info->format;
		info->options = stmt.info->options;
		// set up the file name for the COPY TO

		idx_t id = 0;
		while (true) {
			string id_suffix = id == 0 ? string() : "_" + to_string(id);
			auto name = CreateFileName(id_suffix, table, copy_function.function.extension);
			auto directory = stmt.info->file_path;
			auto full_path = fs.JoinPath(directory, name);
			info->file_path = full_path;
			auto insert_result = table_name_index.insert(info->file_path);
			if (insert_result.second) {
				// this name was not yet taken: take it
				break;
			}
			id++;
		}
		info->is_from = false;
		// carry the full (possibly nested) schema path of the exported table
		info->SetQualifiedName(table.schema.GetQualifiedName(table.name));

		// We can not export generated columns
		child_list_t<LogicalType> select_list;
		// Let's verify if any on these columns have not null constraints
		vector<Identifier> not_null_columns;
		for (auto &constraint : table.GetConstraints()) {
			if (constraint->type == ConstraintType::NOT_NULL) {
				auto &not_null_constraint = constraint->Cast<NotNullConstraint>();
				not_null_columns.emplace_back(table.GetColumn(not_null_constraint.index).GetName().GetIdentifierName());
			}
		}
		for (auto &col : table.GetColumns().Physical()) {
			select_list.emplace_back(std::make_pair(col.Name(), col.Type()));
		}

		ExportedTableData exported_data;
		exported_data.qualified_name = info->GetQualifiedName();

		exported_data.file_path = info->file_path;

		ExportedTableInfo table_info(table, std::move(exported_data), not_null_columns);
		exported_tables->data.push_back(table_info);
		id++;

		// generate the copy statement and bind it
		CopyStatement copy_stmt;
		copy_stmt.info = std::move(info);
		copy_stmt.info->select_statement = CreateSelectStatement(copy_stmt, select_list);

		auto copy_binder = Binder::CreateBinder(context, this);
		auto bound_statement = copy_binder->Bind(copy_stmt, CopyToType::EXPORT_DATABASE);

		auto plan = std::move(bound_statement.plan);

		export_nodes.push_back(std::move(plan));
	}
	auto child_operator = UnionOperators(std::move(export_nodes));

	// try to create the directory, if it doesn't exist yet
	// a bit hacky to do it here, but we need to create the directory BEFORE the copy statements run
	if (!fs.DirectoryExists(stmt.info->file_path)) {
		fs.CreateDirectory(stmt.info->file_path);
	}

	stmt.info->SetQualifiedName(QualifiedName(Identifier(catalog), stmt.info->GetQualifiedName().Schema(),
	                                          stmt.info->GetQualifiedName().Name()));
	// prepare the options for export
	auto &format = stmt.info->format;
	auto &options = stmt.info->options;
	if (format == "csv") {
		// insert default csv options, if not specified
		if (options.find("header") == options.end()) {
			options["header"].push_back(Value::INTEGER(1));
		}
		if (options.find("delimiter") == options.end() && options.find("sep") == options.end() &&
		    options.find("delim") == options.end()) {
			options["delimiter"].push_back(Value(","));
		}
		if (options.find("quote") == options.end()) {
			options["quote"].push_back(Value("\""));
		}
		options.erase("force_quote");
	}
	// for any options that are write-only, use them for writing but don't put them in the COPY statements we generate
	// for reading
	auto &function = copy_function.function;
	if (function.copy_options) {
		auto copy_options = GetFullCopyOptionsList(function, CopyOptionMode::READ_ONLY);
		vector<Identifier> erased_options;
		for (auto &entry : options) {
			if (copy_options.find(entry.first) == copy_options.end()) {
				erased_options.push_back(entry.first);
			}
		}
		for (auto &erased : erased_options) {
			options.erase(erased);
		}
	}

	// create the export node
	auto export_node =
	    make_uniq<LogicalExport>(copy_function.function, std::move(stmt.info), std::move(exported_tables));

	if (child_operator) {
		export_node->children.push_back(std::move(child_operator));
	}

	result.plan = std::move(export_node);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
