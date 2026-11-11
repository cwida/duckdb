#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/error_data.hpp"

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
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

namespace {

//! The storage version the keys of an index are written with: the version its stored data records (the ART saves the
//! version it was created under, an index written before v1.5.0 records none), or the version of the database for an
//! index that is being created. The version stays with the index when the database is upgraded to a newer storage
//! version, since the stored keys keep their types.
StorageVersion IndexKeyStorageVersion(AttachedDatabase &db, optional_ptr<const IndexStorageInfo> storage_info) {
	if (storage_info && storage_info->IsValid()) {
		auto entry = storage_info->options.find("storage_version");
		if (entry == storage_info->options.end()) {
			return StorageVersion::INVALID;
		}
		return static_cast<StorageVersion>(entry->second.GetValue<uint64_t>());
	}
	return db.GetStorageManager().GetStorageVersion();
}

//! Whether the keys of an index of the storage version are bound within 38 digits: the engines of the storage versions
//! before wide decimals bind every decimal key that way, and the keys they store have those types
bool KeysPredateWideDecimals(StorageVersion key_version) {
	return StorageManager::IsPriorToVersion(Decimal::WIDE_VERSION_ADDED, key_version);
}

//! The first expression of a wide decimal type within the key, if any: the engines before wide decimals bind every
//! subexpression within 38 digits, so a wide intermediate changes the computed key as much as a wide result
optional_ptr<const Expression> FindWideDecimalExpression(const Expression &expr) {
	if (StorageManager::ContainsWideDecimal(expr.GetReturnType())) {
		return &expr;
	}
	optional_ptr<const Expression> result;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!result) {
			result = FindWideDecimalExpression(child);
		}
	});
	return result;
}

optional_ptr<const Expression> FindWideDecimalExpression(const vector<unique_ptr<Expression>> &keys) {
	for (auto &key : keys) {
		auto wide = FindWideDecimalExpression(*key);
		if (wide) {
			return wide;
		}
	}
	return nullptr;
}

//! A stored index whose key binds a wide type here was written with keys computed in other types: binding it would
//! leave the stored keys unmatched (a UNIQUE index would accept duplicates), so it is not bound until created again
optional_ptr<const Expression> FindMismatchedStoredKey(const UnboundIndex &unbound_index,
                                                       const vector<unique_ptr<Expression>> &keys) {
	const auto key_version = IndexKeyStorageVersion(unbound_index.db, unbound_index.GetStorageInfo());
	if (!KeysPredateWideDecimals(key_version)) {
		return nullptr;
	}
	return FindWideDecimalExpression(keys);
}

void ThrowMismatchedStoredKey(const UnboundIndex &unbound_index, const Expression &wide) {
	const auto key_version = IndexKeyStorageVersion(unbound_index.db, unbound_index.GetStorageInfo());
	auto &name = unbound_index.GetIndexName();
	throw BinderException(
	    "Cannot bind index %s on table %s: its key expression %s binds to %s, but the index was stored at "
	    "storage version %s, whose engines bind decimal keys within %d digits, so the stored keys have "
	    "another type. DROP INDEX %s and CREATE it again, with every part of the key within %d digits or after "
	    "upgrading the database to storage version %s or later",
	    name, unbound_index.GetTableName(), wide.ToString(), wide.GetReturnType().ToString(),
	    GetStorageVersionName(key_version, false), Decimal::MAX_WIDTH_INT128, name, Decimal::MAX_WIDTH_INT128,
	    GetStorageVersionName(Decimal::WIDE_VERSION_ADDED, false));
}

//! The keys of a new index are stored at the storage version of its database: a key with a wide decimal expression is
//! refused where that version cannot hold it, like a wide column, so that the engines of that version compute the same
//! keys from the types they bind
void CheckNewKeyTypes(AttachedDatabase &db, const vector<unique_ptr<Expression>> &keys) {
	if (!db.HasStorageManager() || !KeysPredateWideDecimals(IndexKeyStorageVersion(db, nullptr))) {
		return;
	}
	auto wide = FindWideDecimalExpression(keys);
	if (wide) {
		throw InvalidInputException(
		    db.GetStorageManager().GetUnsupportedDecimalWidthReason(wide->GetReturnType(), "index key expressions"));
	}
}

} // namespace

IndexBinder::IndexBinder(Binder &binder, ClientContext &context, optional_ptr<TableCatalogEntry> table,
                         optional_ptr<CreateIndexInfo> info)
    : ExpressionBinder(binder, context), table(table), info(info) {
}

//! Returns a lambda function of the key, if any
static optional_ptr<const BoundFunctionExpression> FindLambdaFunction(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.Function().HasBindLambdaCallback()) {
			return &function;
		}
	}
	optional_ptr<const BoundFunctionExpression> result;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!result) {
			result = FindLambdaFunction(child);
		}
	});
	return result;
}

static bool IsValidIndexExpression(Binder &binder, const Expression &expr, ErrorData &error) {
	// casts are bound without the binder, so a cast that reads settings such as TimeZone is found in the key
	if (binder.GetStatementProperties().always_require_rebind || expr.RequiresRebind()) {
		error = BinderException("Index keys cannot contain expressions with side effects. They also cannot depend on "
		                        "settings such as TimeZone.");
		return false;
	}
	// Lambda functions require a ClientContext, but the index's expression executor has none, so reject.
	auto lambda_function = FindLambdaFunction(expr);
	if (lambda_function) {
		error = BinderException(*lambda_function, "Index keys cannot contain lambda functions such as %s.",
		                        lambda_function->Function().GetName());
		return false;
	}
	if (!expr.IsConsistent()) {
		error = BinderException("Index keys cannot contain expressions with side effects.");
		return false;
	}
	return true;
}

unique_ptr<BoundIndex> IndexBinder::BindIndex(const UnboundIndex &unbound_index, bool skip_mismatched_keys) {
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
	auto &properties = binder.GetStatementProperties();
	const auto statement_requires_rebind = properties.always_require_rebind;
	vector<unique_ptr<Expression>> unbound_expressions;
	unbound_expressions.reserve(parsed_expressions.size());
	for (auto &expr : parsed_expressions) {
		auto copy = expr->Copy();
		properties.always_require_rebind = false;
		unbound_expressions.push_back(Bind(copy));
		// Stored keys must satisfy the same requirements as newly created keys.
		ErrorData error;
		if (!IsValidIndexExpression(binder, *unbound_expressions.back(), error)) {
			throw DataCorruptionException("Invalid index %s: %s", create_info.GetIndexName(), error.RawMessage());
		}
	}
	properties.always_require_rebind = statement_requires_rebind;
	auto mismatched_key = FindMismatchedStoredKey(unbound_index, unbound_expressions);
	if (mismatched_key) {
		if (skip_mismatched_keys) {
			return nullptr;
		}
		ThrowMismatchedStoredKey(unbound_index, *mismatched_key);
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
		ErrorData error;
		if (!IsValidIndexExpression(binder, *expressions.back(), error)) {
			error.Throw();
		}
	}
	properties.always_require_rebind = statement_requires_rebind;
	CheckNewKeyTypes(table_entry.ParentCatalog().GetAttached(), expressions);

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
