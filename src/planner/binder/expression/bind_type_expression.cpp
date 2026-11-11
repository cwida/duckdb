#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"

namespace duckdb {

static bool IsValidTypeLookup(optional_ptr<CatalogEntry> entry) {
	if (!entry) {
		return false;
	}
	return entry->Cast<TypeCatalogEntry>().user_type.id() != LogicalTypeId::INVALID;
}

BindResult ExpressionBinder::BindExpression(TypeExpression &type_expr, idx_t depth) {
	auto &type_name = type_expr.GetTypeName();

	QueryErrorContext error_context(type_expr);
	EntryLookupInfo type_lookup(CatalogType::TYPE_ENTRY, QualifiedName(type_name), error_context);

	optional_ptr<CatalogEntry> entry = nullptr;

	// Resolve the qualification the same way a table reference is resolved: a leading component is the catalog when
	// it names an attached database, and otherwise the outermost schema of a (possibly nested) schema path.
	auto bound_name = Binder::BindTableName(binder.EntryRetriever(), type_expr.GetQualifiedName());
	auto &type_catalog = bound_name.Catalog();
	bool is_qualified = bound_name.Path().size() > 1;

	if (type_catalog.empty() && !DatabaseManager::Get(context).HasAttachedDatabase()) {
		// Look in the system catalog if no catalog was specified
		entry = binder.entry_retriever.GetEntry(
		    EntryLookupInfo(type_lookup, bound_name.WithCatalog(Identifier::SystemCatalog())));
	} else {
		// Try to search from most specific to least specific
		// The search path should already have been set to the correct catalog/schema,
		// in case we are looking for a type in the same schema as a table we are creating

		entry = binder.entry_retriever.GetEntry(EntryLookupInfo(type_lookup, bound_name), OnEntryNotFound::RETURN_NULL);

		if (!IsValidTypeLookup(entry)) {
			if (is_qualified) {
				// re-run the lookup to report the qualification that was given
				entry = binder.entry_retriever.GetEntry(EntryLookupInfo(type_lookup, bound_name),
				                                        OnEntryNotFound::THROW_EXCEPTION);
			}
			entry = binder.entry_retriever.GetEntry(
			    EntryLookupInfo(type_lookup, QualifiedName(type_catalog, Identifier::InvalidSchema(),
			                                               type_lookup.GetEntryIdentifier())),
			    OnEntryNotFound::RETURN_NULL);
		}
		if (!IsValidTypeLookup(entry)) {
			entry = binder.entry_retriever.GetEntry(
			    EntryLookupInfo(type_lookup, QualifiedName(Identifier::InvalidCatalog(), Identifier::InvalidSchema(),
			                                               type_lookup.GetEntryIdentifier())),
			    OnEntryNotFound::RETURN_NULL);
		}
		if (!IsValidTypeLookup(entry)) {
			entry = binder.entry_retriever.GetEntry(
			    EntryLookupInfo(type_lookup, QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(),
			                                               type_lookup.GetEntryIdentifier())),
			    OnEntryNotFound::THROW_EXCEPTION);
		}
	}

	// By this point we have to have found a type in the catalog
	D_ASSERT(entry != nullptr);
	auto &type_entry = entry->Cast<TypeCatalogEntry>();

	// Now handle type parameters
	auto &unbound_parameters = type_expr.GetChildren();

	// Bind value parameters
	vector<TypeArgument> bound_parameters;

	for (auto &param : unbound_parameters) {
		// Otherwise, try to fold it to a constant value
		ConstantBinder binder(this->binder, context, StringUtil::Format("Type parameter for type %s", type_name));
		binder.ContinueStackDepth(*this);

		auto expr = param->Copy();
		auto bound_expr = binder.Bind(expr);

		if (!bound_expr->IsFoldable()) {
			throw BinderException(type_expr, "Type parameter expression for type %s is not a constant", type_name);
		}

		// The location comes from the parsed parameter, not the bound one.
		// Binding may have rewritten it into something with no corresponding source text.
		auto location = param->GetQueryLocation();

		// Shortcut for constant expressions
		if (bound_expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &const_expr = bound_expr->Cast<BoundConstantExpression>();
			bound_parameters.emplace_back(param->GetAlias().GetIdentifierName(), const_expr.GetValue(), location);
			continue;
		}

		// Otherwise we need to evaluate the expression
		auto bound_param = ExpressionExecutor::EvaluateScalar(context, *bound_expr);
		bound_parameters.emplace_back(param->GetAlias().GetIdentifierName(), bound_param, location);
	};

	// Select the matching constructor and call it
	auto result_type =
	    type_entry.constructors.Bind(context, type_entry.user_type, bound_parameters, type_expr.GetQueryLocation());

	// Return the resulting type!
	auto result_expr = make_uniq<BoundConstantExpression>(Value::TYPE(result_type));
	result_expr->SetQueryLocation(type_expr.GetQueryLocation());
	return BindResult(std::move(result_expr));
}

} // namespace duckdb
