#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include <algorithm>
#include "duckdb/function/function_binder.hpp"

namespace duckdb {
Identifier CollationFunctions::GetSchema() {
	return Identifier(SCHEMA);
}

void CollationFunctions::SetCombinable(ScalarFunction &function) {
	function.GetProperties().SetProperty(COMBINABLE_PROPERTY, Value::BOOLEAN(true));
}

CollationFunction CollationFunctions::Lookup(ClientContext &context, const string &collation) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	QualifiedName qualified_name(catalog.GetName(), GetSchema(), Identifier(collation));
	auto entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, qualified_name, OnEntryNotFound::RETURN_NULL);
	if (!entry &&
	    Catalog::AutoLoadExtensionByCatalogEntry(*context.db, CatalogType::COLLATION_ENTRY, Identifier(collation))) {
		entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, qualified_name, OnEntryNotFound::RETURN_NULL);
	}
	if (!entry) {
		auto candidates = StringUtil::TopNJaroWinkler(GetCollations(context), collation, 1);
		throw CatalogException::MissingEntry(CatalogType::COLLATION_ENTRY, Identifier(collation),
		                                     candidates.empty() ? string() : candidates[0]);
	}
	auto function = entry->functions.GetFunctionByArguments(context, {LogicalType::VARCHAR});
	if (!function) {
		throw CatalogException("Collation function \"%s\" must take a single VARCHAR argument", collation);
	}
	return CollationFunction {CollationFunctionName(GetSchema(), entry->name),
	                          function->GetProperties().GetBooleanProperty(COMBINABLE_PROPERTY)};
}

vector<string> CollationFunctions::GetCollations(ClientContext &context) {
	// only list what Lookup resolves: scalar functions in the collation schema that take a single VARCHAR
	vector<string> result;
	auto schema = Catalog::GetSystemCatalog(context).GetSchema(context, GetSchema(), OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return result;
	}
	schema->Scan(context, CatalogType::SCALAR_FUNCTION_ENTRY, [&](CatalogEntry &entry) {
		if (entry.type != CatalogType::SCALAR_FUNCTION_ENTRY) {
			return;
		}
		auto &function_entry = entry.Cast<ScalarFunctionCatalogEntry>();
		if (!FunctionBinder::SelectFunction(context, function_entry.functions, {LogicalType::VARCHAR}).IsSuccess()) {
			return;
		}
		result.push_back(entry.name.GetIdentifierName());
	});
	return result;
}

static vector<CollationFunctionName> GetVarcharCollationFunctions(ClientContext &context, const LogicalType &sql_type,
                                                   CollationType type) {
	if (sql_type.id() != LogicalTypeId::VARCHAR) {
		// only VARCHAR columns require collation
		return vector<CollationFunctionName>();
	}
	// replace default collation with system collation
	auto str_collation = StringType::GetCollation(sql_type);
	string collation;
	if (str_collation.empty()) {
		collation = Settings::Get<DefaultCollationSetting>(context);
	} else {
		collation = str_collation;
	}
	collation = StringUtil::Lower(collation);
	// bind the collation
	if (collation.empty() || collation == "binary" || collation == "c" || collation == "posix") {
		// no collation or binary collation: skip
		return vector<CollationFunctionName>();
	}
	vector<CollationFunctionName> functions;
	string non_combinable_collation;
	vector<CollationFunctionName> combinable_functions;
	unordered_set<string> collations;
	for (auto &collation_argument : StringUtil::Split(collation, ".")) {
		if (collations.count(collation_argument)) {
			// we already applied this collation
			continue;
		}
		collations.insert(collation_argument);
		auto function = CollationFunctions::Lookup(context, collation_argument);
		if (function.combinable) {
			combinable_functions.push_back(function.function);
			continue;
		}
		if (!functions.empty()) {
			throw BinderException("Cannot combine collation types %s and %s", Identifier(non_combinable_collation),
			                      Identifier(collation_argument));
		}
		non_combinable_collation = collation_argument;
		functions.push_back(function.function);
	}
	// combinable collations are applied first, in reverse order of specification
	std::reverse(combinable_functions.begin(), combinable_functions.end());
	if (type == CollationType::COMBINABLE_COLLATIONS) {
		// only apply the combinable collations
		return combinable_functions;
	}
	combinable_functions.insert(combinable_functions.end(), functions.begin(), functions.end());
	return combinable_functions;
}

static vector<CollationFunctionName> GetTimeTZCollationFunctions(ClientContext &, const LogicalType &sql_type,
                                                                 CollationType) {
	if (sql_type.id() != LogicalTypeId::TIME_TZ) {
		return vector<CollationFunctionName>();
	}
	return {CollationFunctionName(Identifier("timetz_byte_comparable"))};
}

static vector<CollationFunctionName> GetBitStringCollationFunctions(ClientContext &, const LogicalType &sql_type,
                                                                    CollationType) {
	if (sql_type.id() != LogicalTypeId::BIT) {
		return vector<CollationFunctionName>();
	}
	return {CollationFunctionName(Identifier("bitstring_byte_comparable"))};
}

static vector<CollationFunctionName> GetIntervalCollationFunctions(ClientContext &, const LogicalType &sql_type,
                                                                   CollationType) {
	if (sql_type.id() != LogicalTypeId::INTERVAL) {
		return vector<CollationFunctionName>();
	}
	return {CollationFunctionName(Identifier("normalized_interval"))};
}

static vector<CollationFunctionName> GetVariantCollationFunctions(ClientContext &, const LogicalType &sql_type,
                                                                  CollationType) {
	if (sql_type.id() != LogicalTypeId::VARIANT) {
		return vector<CollationFunctionName>();
	}
	return {CollationFunctionName(Identifier("variant_comparator"))};
}

CollationBinding::CollationBinding() {
	RegisterCollation(CollationCallback(GetVarcharCollationFunctions));
	RegisterCollation(CollationCallback(GetTimeTZCollationFunctions));
	RegisterCollation(CollationCallback(GetBitStringCollationFunctions));
	RegisterCollation(CollationCallback(GetIntervalCollationFunctions));
	RegisterCollation(CollationCallback(GetVariantCollationFunctions));
}

void CollationBinding::RegisterCollation(CollationCallback callback) {
	collations.push_back(callback);
}

//! Binds the given collation function around "source"
static unique_ptr<Expression> ApplyCollationFunction(ClientContext &context, const CollationFunctionName &function_name,
                                                     unique_ptr<Expression> source) {
	auto source_alias = source->GetAlias();
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(source));
	FunctionBinder function_binder(context);
	unique_ptr<Expression> function;
	if (function_name.schema == Identifier::DefaultSchema()) {
		function = function_binder.BindScalarFunction(function_name.schema, function_name.name, std::move(children));
	} else {
		// functions outside of the default schema, such as the collations, live in the system catalog
		auto &catalog = Catalog::GetSystemCatalog(context);
		auto entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
		    context, QualifiedName(catalog.GetName(), function_name.schema, function_name.name),
		    OnEntryNotFound::THROW_EXCEPTION);
		ErrorData error;
		function = function_binder.BindScalarFunction(*entry, std::move(children), error);
		if (!function) {
			error.Throw();
		}
	}
	function->SetAlias(source_alias);
	return function;
}

//! Pushes collations into STRUCT/TUPLE fields using invoke with a lambda that rebuilds the value and preserves NULLs.
//! Evaluates the source once. Returns false if no fields require collation.
static bool PushStructCollation(ClientContext &context, unique_ptr<Expression> &source, const LogicalType &sql_type,
                                CollationType type, const CollationBinding &binding) {
	const Identifier lambda_parameter("x");
	auto lambda_parameter_expr = make_uniq<BoundReferenceExpression>(lambda_parameter, sql_type, idx_t(0));
	const bool is_tuple = sql_type.id() == LogicalTypeId::TUPLE;
	vector<unique_ptr<Expression>> fields;
	FunctionBinder function_binder(context);

	auto &child_types = StructType::GetChildTypes(sql_type);
	// check whether any field requires a collation first: binding the field extracts is linear in the number of
	// fields per field
	bool any_field_requires_collation = false;
	for (auto &child_type : child_types) {
		unique_ptr<Expression> field_ref = make_uniq<BoundReferenceExpression>(child_type.second, idx_t(0));
		if (binding.PushCollation(context, field_ref, child_type.second, type)) {
			any_field_requires_collation = true;
			break;
		}
	}
	if (!any_field_requires_collation) {
		return false;
	}

	bool requires_collation = false;
	for (idx_t i = 0; i < child_types.size(); i++) {
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(lambda_parameter_expr->Copy());
		arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(i + 1))));
		auto field = function_binder.BindScalarFunction(Identifier::DefaultSchema(),
		                                                Identifier(is_tuple ? "struct_extract" : "struct_extract_at"),
		                                                std::move(arguments));
		// Wrap the extracted field in its collation functions, recursing into nested fields.
		requires_collation |= binding.PushCollation(context, field, child_types[i].second, type);
		field->SetAlias(Identifier(child_types[i].first));
		fields.push_back(std::move(field));
	}

	if (!requires_collation) {
		return false;
	}

	// Rebuild with the transformed field types, as the fields type can change
	auto result = function_binder.BindScalarFunction(Identifier::DefaultSchema(),
	                                                 Identifier(is_tuple ? "row" : "struct_pack"), std::move(fields));
	// A NULL struct must remain distinct from a struct containing only NULL fields.
	auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	is_null->GetChildrenMutable().push_back(lambda_parameter_expr->Copy());
	auto null_result = make_uniq<BoundConstantExpression>(Value(result->GetReturnType()));
	auto body = make_uniq<BoundCaseExpression>(std::move(is_null), std::move(null_result), std::move(result));
	auto lambda =
	    make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, std::move(body), idx_t(1));
	lambda->SetParameterNames({lambda_parameter});

	// Invoke evaluates the source once, even when several fields need collations.
	auto source_alias = source->GetAlias();
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(lambda));
	arguments.push_back(std::move(source));
	source = function_binder.BindScalarFunction(Identifier::DefaultSchema(), "invoke", std::move(arguments), false,
	                                            nullptr, std::move(source_alias));
	return true;
}

//! Pushes a collation into a LIST/ARRAY type by wrapping the source in a list_transform that applies the collation to
//! every element via a lambda. Returns false if the elements do not require collation, or if list_transform is not
//! available (i.e. the core_functions extension is not loaded).
static bool PushNestedCollation(ClientContext &context, unique_ptr<Expression> &source, const LogicalType &child_type,
                                CollationType type, const CollationBinding &binding) {
	// build a lambda body that applies the collation to the lambda parameter (a reference to the child element)
	const Identifier lambda_parameter("x");
	unique_ptr<Expression> lambda_body = make_uniq<BoundReferenceExpression>(lambda_parameter, child_type, idx_t(0));
	if (!binding.PushCollation(context, lambda_body, child_type, type)) {
		// the child type does not require collation - nothing to push
		return false;
	}

	auto &catalog = Catalog::GetSystemCatalog(context);
	auto list_transform = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "list_transform"),
	    OnEntryNotFound::RETURN_NULL);
	if (!list_transform) {
		// list_transform is not available - cannot push the collation into the list
		return false;
	}

	auto bound_lambda =
	    make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, std::move(lambda_body), idx_t(1));
	bound_lambda->SetParameterNames({lambda_parameter});
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(source));
	children.push_back(std::move(bound_lambda));

	FunctionBinder function_binder(context);
	ErrorData error;
	auto function = function_binder.BindScalarFunction(*list_transform, std::move(children), error);
	if (!function) {
		error.Throw();
	}
	source = std::move(function);
	return true;
}

bool CollationBinding::PushCollation(ClientContext &context, unique_ptr<Expression> &source,
                                     const LogicalType &sql_type, CollationType type) const {
	// Leave collation of aliased structs, lists and arrays to the registered callbacks.
	if (!sql_type.HasAlias()) {
		if (StructType::IsStruct(sql_type)) {
			return PushStructCollation(context, source, sql_type, type, *this);
		}
		if (sql_type.id() == LogicalTypeId::LIST) {
			return PushNestedCollation(context, source, ListType::GetChildType(sql_type), type, *this);
		}
		if (sql_type.id() == LogicalTypeId::ARRAY) {
			return PushNestedCollation(context, source, ArrayType::GetChildType(sql_type), type, *this);
		}
	}
	for (auto &collation : collations) {
		auto functions = collation.get_collation_functions(context, sql_type, type);
		if (functions.empty()) {
			continue;
		}
		// successfully retrieved the collation functions - apply them to the source expression
		for (auto &function_name : functions) {
			source = ApplyCollationFunction(context, function_name, std::move(source));
		}
		return true;
	}
	return false;
}

} // namespace duckdb
