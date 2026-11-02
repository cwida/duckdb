#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/main/capi_v2/extension_api_v2.hpp"
#include "duckdb/main/capi_v2/extension_load_v2.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

//! State kept while a V2 C API extension initializes. This object IS the token handed to the extension as its
//! duckdb_v2_extension_handle, and the one get_api takes back.
struct DuckDBExtensionLoadStateV2 {
	DuckDBExtensionLoadStateV2(DatabaseInstance &db, const ExtensionInitResult &init_result,
	                           const string &extension_name)
	    : db(db), init_result(init_result), loader(db, extension_name) {
	}

	static DuckDBExtensionLoadStateV2 &Get(duckdb_v2_extension_handle handle) {
		D_ASSERT(handle);
		return *reinterpret_cast<DuckDBExtensionLoadStateV2 *>(handle);
	}

	duckdb_v2_extension_handle ToCStruct() {
		return reinterpret_cast<duckdb_v2_extension_handle>(this);
	}

	void SetError(ErrorData error) {
		has_error = true;
		error_data = std::move(error);
	}

	DatabaseInstance &db;
	const ExtensionInitResult &init_result;

	//! Registers catalog entries (functions, types) on the extension's behalf, resolved from the handle via
	//! capiv2::GetExtensionLoader. Lives only as long as this state: the load.
	ExtensionLoader loader;

	//! The function pointer struct handed to the extension. The extension is expected to copy it during initialization
	duckdb_ext_api_v2 api_struct = {};
	//! Whether the extension fetched the API struct. A loadable extension that did not has an all-null vtable
	bool api_requested = false;

	bool has_error = false;
	ErrorData error_data;
};

namespace {

//! Opens a connection and a transaction for the duration of an extension load, for the paths that have no client
//! context to lend (startup, static linking, autoloading). Rolls back unless the load finished.
class CAPIV2LoadScope {
public:
	explicit CAPIV2LoadScope(DatabaseInstance &db) : con(Connection::CreateInternal(db)) {
		con.BeginTransaction();
	}

	~CAPIV2LoadScope() {
		if (finished) {
			return;
		}
		try {
			con.Rollback();
		} catch (...) { // NOLINT: a destructor must not throw
		}
	}

	ClientContext &GetContext() {
		return *con.context;
	}

	void Commit() {
		con.Commit();
		finished = true;
	}

	void Rollback() {
		con.Rollback();
		finished = true;
	}

private:
	Connection con;
	bool finished = false;
};

//! Called by the extension to get a pointer to the correctly versioned extension C API struct. Runs directly under the
//! extension's call frame, so it reports failure through the load state rather than throwing.
const void *ExtensionGetAPIV2(duckdb_v2_extension_handle handle, const char *version) {
	if (!handle) {
		return nullptr;
	}
	auto &load_state = DuckDBExtensionLoadStateV2::Get(handle);
	try {
		// C_STRUCT_UNSTABLE extensions are tied 1:1 to a DuckDB version, so they always receive the whole struct and
		// the version they report is a build tag rather than a semver.
		if (load_state.init_result.abi_type == ExtensionABIType::C_STRUCT) {
			string version_string = version ? version : "";
			idx_t major, minor, patch;
			const auto parsed = VersioningUtils::ParseSemver(version_string, major, minor, patch);
			if (!parsed || major != DUCKDB_EXTENSION_API_V2_VERSION_MAJOR ||
			    !VersioningUtils::IsSupportedCAPIVersion(major, minor, patch)) {
				load_state.SetError(ErrorData(
				    ExceptionType::UNKNOWN_TYPE,
				    "Unsupported C API v2 version detected during extension initialization: " + version_string));
				return nullptr;
			}
		}
		load_state.api_struct = CreateAPIv2();
		load_state.api_requested = true;
		return &load_state.api_struct;
	} catch (std::exception &ex) {
		load_state.SetError(ErrorData(ex));
		return nullptr;
	} catch (...) {
		load_state.SetError(
		    ErrorData(ExceptionType::UNKNOWN_TYPE, "Unknown error in get_api when trying to load extension!"));
		return nullptr;
	}
}

//! Runs the entrypoint against a context that already has a transaction, and reports what the extension made of it.
void CallEntrypoint(DuckDBExtensionLoadStateV2 &load_state, const string &extension_name,
                    ext_init_c_api_v2_fun_t init_fun, ClientContext &context, bool statically_linked) {
	// The slot is always live: the extension populates it, it never allocates or destroys one (there is no
	// error_info constructor in the API). Heap allocated so that an extension violating that and destroying it
	// deletes something that was actually new'd.
	auto error_info = make_uniq<capiv2::CV2ErrorInfo>();
	auto err_handle = capiv2::Convert(error_info.get());

	::duckdb_v2_extension_input input;
	input.get_api = statically_linked ? nullptr : ExtensionGetAPIV2;
	input.extension = load_state.ToCStruct();
	capiv2::CV2CallbackContext callback_context(context);
	input.context = capiv2::Convert(&callback_context);
	input.err = &err_handle;

	(*init_fun)(&input);

	if (!err_handle) {
		// The extension destroyed the slot; ownership went with it.
		error_info.release();
	}

	const string prefix = "An error was thrown during initialization of the extension '" + extension_name + "': ";

	// A get_api refusal takes precedence: the entrypoint returns without touching the error slot in that case, because
	// the reason was already recorded here.
	if (load_state.has_error) {
		load_state.error_data.Throw(prefix);
	}
	if (err_handle && capiv2::Convert(err_handle)->HasError()) {
		try {
			capiv2::Convert(err_handle)->ThrowAsException();
		} catch (std::exception &ex) {
			ErrorData(ex).Throw(prefix);
		}
	}
	if (!statically_linked && !load_state.api_requested) {
		// The entrypoint returns void, so an extension that skipped DUCKDB_EXTENSION_API_INIT cannot report that it did
		// nothing. Its vtable is all-null and the first API call would crash, so refuse the load instead.
		throw FatalException("Extension '%s' did not initialize the C API struct. This indicates an error in the "
		                     "extension: V2 C API extensions must call DUCKDB_EXTENSION_API_INIT before anything else "
		                     "in their entrypoint.",
		                     extension_name);
	}
}

} // namespace

namespace capiv2 {

auto GetExtensionLoader(duckdb_v2_extension_handle handle) -> ExtensionLoader & {
	return DuckDBExtensionLoadStateV2::Get(handle).loader;
}

} // namespace capiv2

void InvokeCAPIV2Entrypoint(DatabaseInstance &db, const ExtensionInitResult &init_result, const string &extension_name,
                            ext_init_c_api_v2_fun_t init_fun, optional_ptr<ClientContext> context,
                            bool statically_linked) {
	DuckDBExtensionLoadStateV2 load_state(db, init_result, extension_name);

	// A context is only usable here if a transaction is already running on it, which is the case when loading through
	// LOAD. Otherwise open one of our own, matching what a V1 extension does when it connects in its glue code.
	if (context && context->transaction.HasActiveTransaction()) {
		CallEntrypoint(load_state, extension_name, init_fun, *context, statically_linked);
		return;
	}

	CAPIV2LoadScope scope(db);
	CallEntrypoint(load_state, extension_name, init_fun, scope.GetContext(), statically_linked);
	scope.Commit();
}

} // namespace duckdb
