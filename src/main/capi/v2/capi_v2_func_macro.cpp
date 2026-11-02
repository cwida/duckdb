#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb::capiv2 {

class CV2MacroFunction {
public:
	CV2MacroFunction(DatabaseInstance &db, MacroType type) : db(db), type(type) {
	}

	//! Validates the configuration and builds the macro to register on `target`.
	unique_ptr<CreateMacroInfo> Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "macro function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (body.empty()) {
			throw InvalidInputException("A body must be set for the macro function.");
		}
		if (signature.GetReturnType().id() != LogicalTypeId::INVALID) {
			throw InvalidInputException(
			    "A macro function signature cannot set a return type: the result follows from the body.");
		}
		if (signature.GetTypedKwargs()) {
			throw InvalidInputException("A macro function cannot have variadic parameters.");
		}
		signature.Verify();

		auto macro = ParseBody();
		for (idx_t i = 0; i < signature.GetParameterCount(); i++) {
			auto &parameter = signature.GetParameter(i);
			switch (parameter.GetKind()) {
			case FunctionParameterKind::STANDARD:
				break;
			case FunctionParameterKind::VAR_POSITIONAL:
			case FunctionParameterKind::VAR_KEYWORD:
				throw InvalidInputException("A macro function cannot have variadic parameters, but \"%s\" is one.",
				                            parameter.GetName().GetIdentifierName());
			default:
				throw InvalidInputException(
				    "The parameters of a macro function can always be passed by position or by name, so \"%s\" must "
				    "be a standard parameter.",
				    parameter.GetName().GetIdentifierName());
			}
			auto &parameter_name = parameter.GetName();
			// ANY leaves the parameter untyped, as a parameter declared without a type
			auto parameter_type =
			    parameter.GetType().id() == LogicalTypeId::ANY ? LogicalType::UNKNOWN : parameter.GetType();
			if (parameter.HasDefaultValue()) {
				auto default_value = *parameter.GetDefaultValue();
				if (parameter_type.id() != LogicalTypeId::UNKNOWN) {
					default_value = default_value.DefaultCastAs(parameter_type);
				}
				auto default_expr = ConstantExpression::FromValue(default_value);
				default_expr->SetAlias(parameter_name);
				macro->default_parameters[parameter_name] = std::move(default_expr);
			}
			macro->parameters.push_back(make_uniq<ColumnRefExpression>(parameter_name));
			macro->types.push_back(std::move(parameter_type));
		}

		auto catalog_type = type == MacroType::TABLE_MACRO ? CatalogType::TABLE_MACRO_ENTRY : CatalogType::MACRO_ENTRY;
		auto parameter_types = macro->types;
		auto info = make_uniq<CreateMacroInfo>(catalog_type);
		info->macros.push_back(std::move(macro));
		info->SetQualifiedName(QualifiedName(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), name));
		info->internal = true;
		docs.AddTo(std::move(parameter_types), *info);
		return info;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	MacroType type;
	Identifier name;
	string body;
	FunctionSignature signature;
	CV2FunctionDocs docs;

private:
	unique_ptr<MacroFunction> ParseBody() const {
		auto parser = Parser::GetBuiltinParser();
		if (type == MacroType::SCALAR_MACRO) {
			auto expressions = parser.ParseExpressionList(body);
			if (expressions.size() != 1) {
				throw InvalidInputException("The body of a scalar macro function must be a single expression.");
			}
			return make_uniq<ScalarMacroFunction>(std::move(expressions[0]));
		}
		parser.ParseQuery(body);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			throw InvalidInputException("The body of a table macro function must be a single SELECT query.");
		}
		auto &select = parser.statements[0]->Cast<SelectStatement>();
		return make_uniq<TableMacroFunction>(std::move(select.node));
	}
};

static auto Convert(duckdb_v2_macro_function_handle function) -> CV2MacroFunction * {
	return reinterpret_cast<CV2MacroFunction *>(function);
}
static auto Convert(CV2MacroFunction *function) -> duckdb_v2_macro_function_handle {
	return reinterpret_cast<duckdb_v2_macro_function_handle>(function);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_macro_function_create(duckdb_v2_factory_handle factory, DUCKDB_V2_MACRO_FUNCTION_TYPE type,
                                                duckdb_v2_macro_function_handle *function,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		duckdb::MacroType macro_type;
		switch (type) {
		case DUCKDB_V2_MACRO_FUNCTION_TYPE_SCALAR:
			macro_type = duckdb::MacroType::SCALAR_MACRO;
			break;
		case DUCKDB_V2_MACRO_FUNCTION_TYPE_TABLE:
			macro_type = duckdb::MacroType::TABLE_MACRO;
			break;
		default:
			throw duckdb::InvalidInputException("Invalid macro function type %d", static_cast<int>(type));
		}
		auto result = duckdb::make_uniq<CV2MacroFunction>(Convert(factory)->GetDatabase(), macro_type);
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_macro_function_set_name(duckdb_v2_macro_function_handle function,
                                                  const duckdb_v2_identifier_t *name,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_macro_function_set_body(duckdb_v2_macro_function_handle function, const duckdb_v2_str *body,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(body);
	return WithErrorHandler(err, [&]() { Convert(function)->body = duckdb::string(Convert(body)); });
}

DUCKDB_V2_ERROR duckdb_v2_macro_function_get_signature(duckdb_v2_macro_function_handle function,
                                                       duckdb_v2_function_signature_handle *sig,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(sig);
	return WithErrorHandler(err, [&]() { *sig = Convert(&Convert(function)->signature); });
}

DUCKDB_V2_ERROR duckdb_v2_macro_function_get_docs(duckdb_v2_macro_function_handle function,
                                                  duckdb_v2_function_docs_handle *docs,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(docs);
	return WithErrorHandler(err, [&]() { *docs = Convert(&Convert(function)->docs); });
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_macro_function(duckdb_v2_extension_handle extension,
                                                            duckdb_v2_macro_function_handle function,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &loader = GetExtensionLoader(extension);
		auto info = Convert(function)->Build(loader.GetDatabaseInstance());
		loader.RegisterFunction(*info);
	});
}

DUCKDB_V2_ERROR duckdb_v2_macro_function_destroy(duckdb_v2_macro_function_handle *function) {
	return WithErrorHandler(nullptr, [&]() {
		if (!function) {
			return;
		}
		if (*function) {
			delete Convert(*function);
			*function = nullptr;
		}
	});
}
