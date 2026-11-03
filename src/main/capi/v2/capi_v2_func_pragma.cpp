#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"

namespace duckdb::capiv2 {

class CV2PragmaFunctionInfo final : public PragmaFunctionInfo {
public:
	duckdb_v2_pragma_function_query_callback_fn query_cb = nullptr;
	duckdb_v2_pragma_function_exec_callback_fn exec_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

class CV2PragmaQueryInfo {
public:
	//! The query the callback set, if any
	optional<string> out_query;
};

static auto Convert(duckdb_v2_pragma_function_query_info_handle info) -> CV2PragmaQueryInfo * {
	return reinterpret_cast<CV2PragmaQueryInfo *>(info);
}
static auto Convert(CV2PragmaQueryInfo *info) -> duckdb_v2_pragma_function_query_info_handle {
	return reinterpret_cast<duckdb_v2_pragma_function_query_info_handle>(info);
}

//! Hands the arguments of the PRAGMA statement to `callback`, and throws the error it reported
template <class FUNC>
static auto CV2PragmaInvoke(ClientContext &context, const FunctionParameters &parameters, FUNC &&callback) -> void {
	if (!parameters.function) {
		throw InternalException("A pragma function was called without its function");
	}
	auto &function = *parameters.function;
	auto &info = function.function_info->Cast<CV2PragmaFunctionInfo>();
	auto &signature = function.GetSignature();

	// the binder places the arguments as it does for a table function, so the call is laid out the same way
	BoundSimpleFunction bound_function;
	bound_function.SetName(function.name);
	bound_function.SetCallArguments(signature, parameters.values, parameters.named_parameters);
	CV2ConstantBindInfo bind_info(bound_function, signature, info.user_data ? info.user_data->GetData() : nullptr,
	                              parameters.values, parameters.named_parameters);
	bind_info.bind_data_error = "Pragma functions have no bind data";

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	callback(info, Convert(&bind_info), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2PragmaQuery(ClientContext &context, const FunctionParameters &parameters) -> string {
	CV2PragmaQueryInfo result_info;
	CV2PragmaInvoke(context, parameters, [&](const CV2PragmaFunctionInfo &info, auto bind_info, auto ctx, auto err) {
		info.query_cb(bind_info, Convert(&result_info), ctx, err);
	});
	if (!result_info.out_query) {
		throw InvalidInputException("The query callback of pragma function %s did not set a query.",
		                            parameters.function->name);
	}
	return std::move(*result_info.out_query);
}

static auto CV2PragmaExec(ClientContext &context, const FunctionParameters &parameters) -> void {
	CV2PragmaInvoke(context, parameters, [&](const CV2PragmaFunctionInfo &info, auto bind_info, auto ctx, auto err) {
		info.exec_cb(bind_info, ctx, err);
	});
}

class CV2PragmaFunction {
public:
	explicit CV2PragmaFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the pragma function to register on `target`.
	PragmaFunction Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "pragma function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (info.query_cb && info.exec_cb) {
			throw InvalidInputException("A pragma function cannot have both a query callback and an exec callback.");
		}
		if (!info.query_cb && !info.exec_cb) {
			throw InvalidInputException("Either a query callback or an exec callback must be set for the function.");
		}
		if (signature.GetReturnType().id() != LogicalTypeId::INVALID) {
			throw InvalidInputException("A pragma function signature cannot set a return type.");
		}

		signature.Verify();

		auto function = info.query_cb ? PragmaFunction::PragmaCall(name, CV2PragmaQuery, signature)
		                              : PragmaFunction::PragmaCall(name, CV2PragmaExec, signature);
		if (signature.GetParameterCount() == 0) {
			function.type = PragmaType::PRAGMA_STATEMENT;
		}
		function.function_info = make_shared_ptr<CV2PragmaFunctionInfo>(std::move(info));
		return function;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	FunctionSignature signature;
	CV2FunctionDocs docs;
	CV2PragmaFunctionInfo info;
	Identifier name;
};

static auto Convert(duckdb_v2_pragma_function_handle func) -> CV2PragmaFunction * {
	return reinterpret_cast<CV2PragmaFunction *>(func);
}
static auto Convert(CV2PragmaFunction *func) -> duckdb_v2_pragma_function_handle {
	return reinterpret_cast<duckdb_v2_pragma_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_pragma_function_create(duckdb_v2_factory_handle factory,
                                                 duckdb_v2_pragma_function_handle *function,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2PragmaFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_set_name(duckdb_v2_pragma_function_handle function,
                                                   const duckdb_v2_identifier_t *name,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_get_docs(duckdb_v2_pragma_function_handle function,
                                                   duckdb_v2_function_docs_handle *docs,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(docs);
	return WithErrorHandler(err, [&]() { *docs = Convert(&Convert(function)->docs); });
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_get_signature(duckdb_v2_pragma_function_handle function,
                                                        duckdb_v2_function_signature_handle *sig,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(sig);
	return WithErrorHandler(err, [&]() { *sig = Convert(&Convert(function)->signature); });
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_set_user_data(duckdb_v2_pragma_function_handle function,
                                                        duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_set_query_callback(duckdb_v2_pragma_function_handle function,
                                                             duckdb_v2_pragma_function_query_callback_fn callback,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.query_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_set_exec_callback(duckdb_v2_pragma_function_handle function,
                                                            duckdb_v2_pragma_function_exec_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.exec_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_query_set_query(duckdb_v2_pragma_function_query_info_handle info,
                                                          const duckdb_v2_str *query,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(query);
	return WithErrorHandler(err, [&]() { Convert(info)->out_query = duckdb::string(Convert(query)); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_pragma_function(duckdb_v2_connection_handle conn,
                                                              duckdb_v2_pragma_function_handle function,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &self = *Convert(function);
			duckdb::CreatePragmaFunctionInfo info(self.Build(*context.db));
			self.docs.AddTo(self.signature, info);
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			catalog.CreatePragmaFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_pragma_function(duckdb_v2_extension_handle extension,
                                                             duckdb_v2_pragma_function_handle function,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		duckdb::CreatePragmaFunctionInfo info(self.Build(loader.GetDatabaseInstance()));
		self.docs.AddTo(self.signature, info);
		loader.RegisterFunction(std::move(info));
	});
}

DUCKDB_V2_ERROR duckdb_v2_pragma_function_destroy(duckdb_v2_pragma_function_handle *function) {
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
