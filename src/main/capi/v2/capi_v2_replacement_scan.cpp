#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/column_data_ref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb::capiv2 {

// Which of the three mutually exclusive forms the callback used to claim the reference.
enum class CV2ReplacementScanClaim : uint8_t { NONE, FUNCTION, COLLECTION, SUBQUERY };

// The registration payload, owned by whichever scan list it was registered into: the connection's ClientConfig for a
// connection-scoped scan, the database's DBConfig otherwise.
class CV2ReplacementScanData final : public ReplacementScanData {
public:
	duckdb_v2_replacement_scan_callback_fn callback = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

class CV2ReplacementScanInfo {
public:
	void *in_user_data = nullptr;
	ReplacementScanInput *in_input = nullptr;
	// The callback only receives the info handle, but parsing a subquery needs the query's parser options.
	optional_ptr<ClientContext> in_context;

	CV2ReplacementScanClaim claim = CV2ReplacementScanClaim::NONE;
	Identifier out_alias;

	// FUNCTION
	QualifiedName out_function_name;
	vector<Value> out_arguments;
	vector<std::pair<Identifier, Value>> out_named_arguments;

	// COLLECTION
	optional_ptr<ColumnDataCollection> out_collection;
	vector<Identifier> out_column_names;

	// SUBQUERY
	unique_ptr<SelectStatement> out_subquery;

	// Refuses a second, different claim form. Repeating the same form is allowed and overwrites.
	void SetClaim(CV2ReplacementScanClaim new_claim) {
		if (claim != CV2ReplacementScanClaim::NONE && claim != new_claim) {
			throw InvalidInputException(
			    "The replacement scan already claimed this reference through a different form.");
		}
		claim = new_claim;
	}

	void RequireFunctionClaim(const char *function_name) {
		if (claim != CV2ReplacementScanClaim::FUNCTION) {
			throw InvalidInputException("%s requires duckdb_v2_replacement_scan_set_function_name to be called first.",
			                            function_name);
		}
	}

	auto BuildTableRef() -> unique_ptr<TableRef> {
		unique_ptr<TableRef> result;
		switch (claim) {
		case CV2ReplacementScanClaim::NONE:
			return nullptr;
		case CV2ReplacementScanClaim::FUNCTION: {
			vector<unique_ptr<ParsedExpression>> children;
			for (auto &argument : out_arguments) {
				children.push_back(ConstantExpression::FromValue(argument));
			}
			for (auto &named_argument : out_named_arguments) {
				// A named argument is an argument whose alias is the parameter name: the binder recovers the name
				// from the expression alias, and drops FunctionArgument::name outright.
				auto child = ConstantExpression::FromValue(named_argument.second);
				child->SetAlias(named_argument.first);
				children.push_back(std::move(child));
			}
			auto function = make_uniq<TableFunctionRef>();
			// The QualifiedName overload keeps any catalog/schema qualification; the Identifier one would drop it.
			function->function = make_uniq<FunctionExpression>(out_function_name, std::move(children));
			result = std::move(function);
			break;
		}
		case CV2ReplacementScanClaim::COLLECTION:
			// Borrowed: the caller owns the collection and keeps it alive.
			result = make_uniq<ColumnDataRef>(*out_collection, std::move(out_column_names));
			break;
		case CV2ReplacementScanClaim::SUBQUERY:
			result = make_uniq<SubqueryRef>(std::move(out_subquery));
			break;
		}
		result->alias = out_alias;
		return result;
	}
};

static auto Convert(duckdb_v2_replacement_scan_info_handle info) -> CV2ReplacementScanInfo * {
	return reinterpret_cast<CV2ReplacementScanInfo *>(info);
}
static auto Convert(CV2ReplacementScanInfo *info) -> duckdb_v2_replacement_scan_info_handle {
	return reinterpret_cast<duckdb_v2_replacement_scan_info_handle>(info);
}

static auto CV2ReplacementScanTrampoline(ClientContext &context, ReplacementScanInput &input,
                                         optional_ptr<ReplacementScanData> data) -> unique_ptr<TableRef> {
	const auto &scan_data = data->Cast<CV2ReplacementScanData>();

	CV2ReplacementScanInfo args;
	args.in_user_data = scan_data.user_data ? scan_data.user_data->GetData() : nullptr;
	args.in_input = &input;
	args.in_context = &context;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	scan_data.callback(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
	return args.BuildTableRef();
}

class CV2ReplacementScan {
public:
	explicit CV2ReplacementScan(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the scan to register on `target`.
	ReplacementScan Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "replacement scan");
		if (!info.callback) {
			throw InvalidInputException("Callback must be set for the replacement scan.");
		}
		if (registered) {
			throw InvalidInputException("The replacement scan is already registered.");
		}
		registered = true;

		auto data = make_uniq<CV2ReplacementScanData>();
		data->callback = info.callback;
		data->user_data = info.user_data;
		return ReplacementScan(CV2ReplacementScanTrampoline, std::move(data));
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	CV2ReplacementScanData info;
	bool registered = false;
};

static auto Convert(duckdb_v2_replacement_scan_handle scan) -> CV2ReplacementScan * {
	return reinterpret_cast<CV2ReplacementScan *>(scan);
}
static auto Convert(CV2ReplacementScan *scan) -> duckdb_v2_replacement_scan_handle {
	return reinterpret_cast<duckdb_v2_replacement_scan_handle>(scan);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_create(duckdb_v2_factory_handle factory,
                                                  duckdb_v2_replacement_scan_handle *scan,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(scan);
	*scan = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2ReplacementScan>(Convert(factory)->GetDatabase());
		*scan = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_set_callback(duckdb_v2_replacement_scan_handle scan,
                                                        duckdb_v2_replacement_scan_callback_fn callback,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(scan);
	return WithErrorHandler(err, [&]() { Convert(scan)->info.callback = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_set_user_data(duckdb_v2_replacement_scan_handle scan,
                                                         duckdb_v2_opaque *user_data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(scan);
	DUCKDB_CHECK_ARG(user_data);
	return WithErrorHandler(err, [&]() {
		Convert(scan)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_get_user_data(duckdb_v2_replacement_scan_info_handle info, void **data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_get_name(duckdb_v2_replacement_scan_info_handle info,
                                                    duckdb_v2_qname_handle *out_name,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(out_name);
	*out_name = nullptr;
	return WithErrorHandler(err, [&]() {
		// Owned, so the callback can keep it: the binder's own name dies with the call.
		*out_name = Convert(new duckdb::QualifiedName(Convert(info)->in_input->name));
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_set_function_name(duckdb_v2_replacement_scan_info_handle info,
                                                             duckdb_v2_qname_handle name,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		args.SetClaim(CV2ReplacementScanClaim::FUNCTION);
		args.out_function_name = *Convert(name);
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_add_argument(duckdb_v2_replacement_scan_info_handle info,
                                                        duckdb_v2_value_handle value,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		args.RequireFunctionClaim("duckdb_v2_replacement_scan_add_argument");
		args.out_arguments.push_back(*Convert(value));
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_add_named_argument(duckdb_v2_replacement_scan_info_handle info,
                                                              const duckdb_v2_identifier_t *name,
                                                              duckdb_v2_value_handle value,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		args.RequireFunctionClaim("duckdb_v2_replacement_scan_add_named_argument");
		args.out_named_arguments.emplace_back(duckdb::Identifier(ConvertIdentifierName(name)), *Convert(value));
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_set_collection(duckdb_v2_replacement_scan_info_handle info,
                                                          duckdb_v2_column_data_collection_handle collection,
                                                          const duckdb_v2_identifier_t *column_names,
                                                          idx_t column_count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(collection);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		auto &cdc = *Convert(collection);

		// The binder pads a short name list but never truncates a long one, and an empty name trips an assertion
		// deeper in, so both are refused here.
		if (column_count > 0 && !column_names) {
			throw duckdb::InvalidInputException("Column names cannot be null when column_count is non-zero.");
		}
		if (column_count > 0 && column_count != cdc.ColumnCount()) {
			throw duckdb::InvalidInputException(
			    "The number of column names (%llu) does not match the collection's column count (%llu).", column_count,
			    cdc.ColumnCount());
		}
		duckdb::vector<duckdb::Identifier> names;
		for (idx_t i = 0; i < column_count; i++) {
			auto name = duckdb::Identifier(ConvertIdentifierName(column_names[i]));
			if (name.empty()) {
				throw duckdb::InvalidInputException("Column names cannot be empty.");
			}
			names.push_back(std::move(name));
		}

		args.SetClaim(CV2ReplacementScanClaim::COLLECTION);
		args.out_collection = &cdc;
		args.out_column_names = std::move(names);
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_set_subquery(duckdb_v2_replacement_scan_info_handle info,
                                                        const duckdb_v2_str *sql, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(sql);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);

		// Parsed here rather than when the claim is materialized, so a bad query fails this call instead of
		// surfacing later as an opaque binding error.
		duckdb::Parser parser(*args.in_context);
		parser.ParseQuery(duckdb::string(Convert(sql)));
		if (parser.statements.size() != 1) {
			throw duckdb::InvalidInputException("The replacement subquery must be exactly one SELECT statement.");
		}
		if (parser.statements[0]->type != duckdb::StatementType::SELECT_STATEMENT) {
			throw duckdb::InvalidInputException("The replacement subquery must be a SELECT statement.");
		}

		args.SetClaim(CV2ReplacementScanClaim::SUBQUERY);
		args.out_subquery =
		    duckdb::unique_ptr_cast<duckdb::SQLStatement, duckdb::SelectStatement>(std::move(parser.statements[0]));
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_set_alias(duckdb_v2_replacement_scan_info_handle info,
                                                     const duckdb_v2_identifier_t *alias,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(alias);
	return WithErrorHandler(err,
	                        [&]() { Convert(info)->out_alias = duckdb::Identifier(ConvertIdentifierName(alias)); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_replacement_scan(duckdb_v2_connection_handle conn,
                                                               duckdb_v2_replacement_scan_handle scan,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(scan);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		auto built = Convert(scan)->Build(*context.db);
		// Connection-scoped: this touches only the client's own state, never the shared database config.
		context.config.replacement_scans.push_back(duckdb::make_shared_ptr<duckdb::ReplacementScan>(std::move(built)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_instance_register_replacement_scan(duckdb_v2_instance_handle instance,
                                                             duckdb_v2_replacement_scan_handle scan,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(scan);
	return WithErrorHandler(err, [&]() {
		auto &db = *Convert(instance)->GetDatabase().instance;
		duckdb::DBConfig::GetConfig(db).replacement_scans.push_back(Convert(scan)->Build(db));
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_replacement_scan(duckdb_v2_extension_handle extension,
                                                              duckdb_v2_replacement_scan_handle scan,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(scan);
	return WithErrorHandler(err, [&]() {
		auto &db = GetExtensionLoader(extension).GetDatabaseInstance();
		duckdb::DBConfig::GetConfig(db).replacement_scans.push_back(Convert(scan)->Build(db));
	});
}

DUCKDB_V2_ERROR duckdb_v2_replacement_scan_destroy(duckdb_v2_replacement_scan_handle *scan) {
	return WithErrorHandler(nullptr, [&]() {
		if (!scan) {
			return;
		}
		if (*scan) {
			delete Convert(*scan);
			*scan = nullptr;
		}
	});
}
