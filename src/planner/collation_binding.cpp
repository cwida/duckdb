#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/catalog/catalog_entry/collate_catalog_entry.hpp"
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
#include "duckdb/function/function_binder.hpp"

namespace duckdb {
constexpr const char *CollateCatalogEntry::Name;

static vector<string> GetVarcharCollationFunctions(ClientContext &context, const LogicalType &sql_type,
                                                   CollationType type) {
	vector<string> result;
	if (sql_type.id() != LogicalTypeId::VARCHAR) {
		// only VARCHAR columns require collation
		return result;
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
		return result;
	}
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto splits = StringUtil::Split(StringUtil::Lower(collation), ".");
	vector<reference<CollateCatalogEntry>> entries;
	unordered_set<string> collations;
	for (auto &collation_argument : splits) {
		if (collations.count(collation_argument)) {
			// we already applied this collation
			continue;
		}
		auto &collation_entry = catalog.GetEntry<CollateCatalogEntry>(
		    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), Identifier(collation_argument)));
		if (collation_entry.combinable) {
			entries.insert(entries.begin(), collation_entry);
		} else {
			if (!entries.empty() && !entries.back().get().combinable) {
				throw BinderException("Cannot combine collation types %s and %s", entries.back().get().name,
				                      collation_entry.name);
			}
			entries.push_back(collation_entry);
		}
		collations.insert(collation_argument);
	}
	for (auto &entry : entries) {
		auto &collation_entry = entry.get();
		if (!collation_entry.combinable && type == CollationType::COMBINABLE_COLLATIONS) {
			// not a combinable collation - only apply the (preceding) combinable collations
			break;
		}
		result.push_back(collation_entry.function.GetName().GetIdentifierName());
	}
	return result;
}

static vector<string> GetTimeTZCollationFunctions(ClientContext &, const LogicalType &sql_type, CollationType) {
	if (sql_type.id() != LogicalTypeId::TIME_TZ) {
		return vector<string>();
	}
	return {"timetz_byte_comparable"};
}

static vector<string> GetBitStringCollationFunctions(ClientContext &, const LogicalType &sql_type, CollationType) {
	if (sql_type.id() != LogicalTypeId::BIT) {
		return vector<string>();
	}
	return {"bitstring_byte_comparable"};
}

static vector<string> GetIntervalCollationFunctions(ClientContext &, const LogicalType &sql_type, CollationType) {
	if (sql_type.id() != LogicalTypeId::INTERVAL) {
		return vector<string>();
	}
	return {"normalized_interval"};
}

static vector<string> GetVariantCollationFunctions(ClientContext &, const LogicalType &sql_type, CollationType) {
	if (sql_type.id() != LogicalTypeId::VARIANT) {
		return vector<string>();
	}
	return {"variant_comparator"};
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

//! Binds the scalar function with the given name (looked up from the system catalog) around "source".
static unique_ptr<Expression> ApplyCollationFunction(ClientContext &context, const string &function_name,
                                                     unique_ptr<Expression> source) {
	auto source_alias = source->GetAlias();
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(source));
	FunctionBinder function_binder(context);
	auto function =
	    function_binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier(function_name), std::move(children));
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
	source = function_binder.BindScalarFunction(Identifier::DefaultSchema(), "invoke", std::move(arguments));
	source->SetAlias(source_alias);
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
