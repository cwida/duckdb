#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"

namespace duckdb {

IndexBinder::IndexBinder(Binder &binder, ClientContext &context, optional_ptr<TableCatalogEntry> table,
                         optional_ptr<CreateIndexInfo> info)
    : ExpressionBinder(binder, context), table(table), info(info) {
}

unique_ptr<BoundIndex> IndexBinder::BindIndex(const UnboundIndex &unbound_index) {
	auto &index_type_name = unbound_index.GetIndexType();
	// Do we know the type of this index now?
	auto index_type = context.db->config.GetIndexTypes().FindByName(index_type_name);
	if (!index_type) {
		throw MissingExtensionException("Cannot bind index %s, unknown index type '%s'. You need to load the "
		                                "extension that provides this index type before table %s can be modified.",
		                                unbound_index.GetTableName(), index_type_name, unbound_index.GetTableName());
	}

	auto &create_info = unbound_index.GetCreateInfo();
	auto &storage_info = unbound_index.GetStorageInfo();
	auto &parsed_expressions = unbound_index.GetParsedExpressions();

	// bind the parsed expressions to create unbound expressions
	vector<unique_ptr<Expression>> unbound_expressions;
	unbound_expressions.reserve(parsed_expressions.size());
	for (auto &expr : parsed_expressions) {
		auto copy = expr->Copy();
		unbound_expressions.push_back(Bind(copy));
	}

	CreateIndexInput input(context, unbound_index.table_io_manager, unbound_index.db, create_info.constraint_type,
	                       create_info.GetIndexName(), create_info.column_ids, unbound_expressions, storage_info,
	                       create_info.options);

	return index_type->create_instance(input);
}

void IndexBinder::InitCreateIndexInfo(LogicalGet &get, CreateIndexInfo &info, const Identifier &schema) {
	// the schema is taken from the table the index is created on - it can be a nested schema
	InitCreateIndexInfo(get, info);
}

void IndexBinder::InitCreateIndexInfo(LogicalGet &get, CreateIndexInfo &info) {
	auto &column_ids = get.GetColumnIds();
	for (auto &column_id : column_ids) {
		if (column_id.IsRowIdColumn()) {
			throw BinderException("cannot create an index on the rowid");
		}
		auto col_id = column_id.GetPrimaryIndex();
		info.column_ids.push_back(col_id);
		info.scan_types.push_back(get.returned_types[col_id]);
	}

	info.scan_types.emplace_back(LogicalType::ROW_TYPE);
	info.names = get.names;
	// the index lives in the same (possibly nested) schema as the table it is created on
	info.SetQualifiedName(get.GetTable()->schema.GetQualifiedName(info.GetQualifiedName().Name()));
	get.AddColumnId(COLUMN_IDENTIFIER_ROW_ID);
}

unique_ptr<LogicalOperator> IndexBinder::BindCreateIndex(ClientContext &context,
                                                         unique_ptr<CreateIndexInfo> create_index_info,
                                                         TableCatalogEntry &table_entry,
                                                         unique_ptr<LogicalOperator> plan,
                                                         unique_ptr<AlterTableInfo> alter_table_info) {
	// Add the dependencies.
	auto &dependencies = create_index_info->dependencies;
	auto &catalog = table_entry.ParentCatalog();
	SetCatalogLookupCallback([&dependencies, &catalog, &table_entry](CatalogEntry &entry) {
		if (&catalog != &entry.ParentCatalog()) {
			return;
		}
		if (&entry == &table_entry) {
			// indexes do not require CASCADE to be dropped, they are simply always dropped along with the table
			dependencies.AddDependency(entry, DependencyDependentFlags());
			return;
		}
		// other entries the index expressions use (e.g. macros) cannot be dropped, replaced or altered while the index
		// exists (note that blocking alone is the legacy value that is read as non-blocking for indexes)
		dependencies.AddDependency(entry, DependencyDependentFlags().SetBlocking().SetAlterBlocking());
	});

	// Bind the index expressions.
	// The expressions are bound again when the index is loaded, so they cannot depend on the session or the query
	// they are bound in (e.g. now(), getvariable()) - such functions are folded to constants while binding.
	auto &properties = binder.GetStatementProperties();
	const auto statement_requires_rebind = properties.always_require_rebind;
	vector<unique_ptr<Expression>> expressions;
	for (auto &expr : create_index_info->expressions) {
		properties.always_require_rebind = false;
		expressions.push_back(Bind(expr));
		// casts are bound without the binder, so a cast that reads settings such as TimeZone is found in the key
		if (properties.always_require_rebind || expressions.back()->RequiresRebind()) {
			throw BinderException("Index keys cannot contain expressions with side effects. They also cannot depend on "
			                      "settings such as TimeZone.");
		}
	}
	properties.always_require_rebind = statement_requires_rebind;

	auto &get = plan->Cast<LogicalGet>();
	InitCreateIndexInfo(get, *create_index_info);
	auto &bind_data = get.bind_data->Cast<TableScanBindData>();
	bind_data.is_create_index = true;

	auto result = make_uniq<LogicalCreateIndex>(std::move(create_index_info), std::move(expressions), table_entry,
	                                            std::move(alter_table_info));
	result->children.push_back(std::move(plan));
	return std::move(result);
}

BindResult IndexBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::WINDOW:
		return BindResult(BinderException::Unsupported(expr, "window functions are not allowed in index expressions"));
	case ExpressionClass::SUBQUERY:
		return BindResult(BinderException::Unsupported(expr, "cannot use subquery in index expressions"));
	default: {
		// restores the previous state on exit, binding is recursive
		struct IndexKeyScope {
			explicit IndexKeyScope(Binder &binder_p) : binder(binder_p), previous(binder_p.IsBindingIndexKey()) {
				binder.SetBindingIndexKey(true);
			}
			~IndexKeyScope() {
				binder.SetBindingIndexKey(previous);
			}
			Binder &binder;
			bool previous;
		} scope(binder);
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	}
}

string IndexBinder::UnsupportedAggregateMessage() {
	return "aggregate functions are not allowed in index expressions";
}

} // namespace duckdb
