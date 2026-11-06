#pragma once

//----------------------------------------------------------------------------------------------------------------------
// 8888888b.                    888      8888888b.  888888b.               d8888 8888888b. 8888888
// 888  "Y88b                   888      888  "Y88b 888  "88b             d88888 888   Y88b  888
// 888    888                   888      888    888 888  .88P            d88P888 888    888  888
// 888    888 888  888  .d8888b 888  888 888    888 8888888K.           d88P 888 888   d88P  888
// 888    888 888  888 d88P"    888 .88P 888    888 888  "Y88b         d88P  888 8888888P"   888
// 888    888 888  888 888      888888K  888    888 888    888        d88P   888 888         888
// 888  .d88P Y88b 888 Y88b.    888 "88b 888  .d88P 888   d88P       d8888888888 888         888
// 8888888P"   "Y88888  "Y8888P 888  888 8888888P"  8888888P"       d88P     888 888       8888888
//----------------------------------------------------------------------------------------------------------------------

/// @file duckdb_cpp.hpp
/// The DuckDB C++ API.
///
/// Most classes here are handles: move-only, never copied and never reference-counted. A handle is either owning,
/// releasing its resource when destroyed, or borrowed, in which case its documentation says what it borrows from and
/// how long it stays valid. Whether a handle currently holds a resource is reported by its `explicit operator bool`.
/// The remaining types (`Exception`, `Signature`, the primitive value types, and the like) are ordinary copyable
/// values.
///
/// Errors are reported by throwing; no function returns an error code. Everything thrown derives from `Exception`.
/// Failures that are part of a function's contract are documented with `\@throws`; any other failure surfaces as a
/// plain `Exception`.
///
/// The usual path through the API: open an `Instance`, which hands out `Connection`s, and a `Connection` parses and
/// executes SQL into a streaming `QueryResult` that yields `DataChunk`s of `Vector`s.

#include <utility>
#include <string>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <limits>

// Arrow C Data and C Stream Interface structs. Forward declared only: the definitions come from `duckdb_v2.h` or from
// the consumer's own Arrow headers, under the standard ARROW_C_DATA_INTERFACE and ARROW_C_STREAM_INTERFACE guards.
struct ArrowSchema;
struct ArrowArray;
struct ArrowArrayStream;

namespace duckdb {
namespace cxx {

//----------------------------------------------------------------------------------------------------------------------
// Common Typedefs and forward declarations
//----------------------------------------------------------------------------------------------------------------------

/// Row indices, offsets, counts and sizes throughout the API.
typedef uint64_t idx_t;

class Exception;
class OptionDescription;
struct OptionValue;
class Config;
class CustomOption;
class Environment;
class Instance;
class Connection;
class SqlStatement;
class StatementIterator;
class Schema;
class Signature;
class LogicalType;
class Value;
class Vector;
class Arena;
class DataChunk;
class ColumnDataCollection;
class QueryResult;
class ArrowResult;
class PreparedStatement;
class ArrowImporter;
class ArrowExporter;

struct TypeParam;
struct NamedParam;
struct ArrowFormat;

enum class LogicalTypeId : uint32_t;
enum class CastMode : uint8_t;

template <class CTX>
class TypeBuilder;
class CustomType;
class CastFunction;
class PragmaFunction;
class LogSink;
class MacroFunction;
class ReplacementScan;
class QualifiedName;
class TableDescription;
class ColumnDescription;
class QueryMetrics;
class QueryMetricsNode;
class FileSystem;
class FileHandle;
class FileOpenOptions;
class MultiFileFunction;
class ScalarFunction;
class AggregateFunction;
class TableFunction;
class CopyFunction;
class Factory;
class Extension;
class Stats;

//----------------------------------------------------------------------------------------------------------------------
// Internal Implementation Details
//----------------------------------------------------------------------------------------------------------------------

/// @internal
/// Everything in `duckdb::cxx::detail` is an implementation detail: not part of the public API, and subject to change
/// without notice.
namespace detail {

/// @internal
/// Maps a C++ wrapper type to its underlying C-API handle type. The primary template is intentionally left undefined:
/// each wrapper specializes it in the .cpp, so C handle types never have to appear in this header.
template <class T>
struct HandleTraits;

/// @internal
/// Base class of every wrapper in this API: move-only custody of a single opaque handle.
template <class TYPE>
class Handle {
public:
	friend TYPE;

	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;

	Handle(Handle &&other) noexcept : impl(other.impl) {
		other.impl = nullptr;
	}

	Handle &operator=(Handle &&other) noexcept {
		std::swap(impl, other.impl);
		return *this;
	}

	virtual ~Handle() noexcept = default;

	/// @internal The underlying C-API handle, typed indirectly through HandleTraits<TYPE>.
	template <class TR = HandleTraits<TYPE>>
	auto handle() const -> typename TR::handle {
		return static_cast<typename TR::handle>(impl);
	}

	/// True while this wrapper holds a handle; false once it has been moved from.
	explicit operator bool() const noexcept {
		return impl != nullptr;
	}

private:
	Handle() : impl(nullptr) {
	}
	explicit Handle(void *impl) : impl(impl) {
	}

	/// @internal Detaches the handle and leaves this wrapper empty, for calls where the C API takes ownership.
	auto release() noexcept -> void * {
		auto *detached = impl;
		impl = nullptr;
		return detached;
	}
	void *impl;
};

/// @internal
/// Grants the .cpp access to the wrappers' private constructors, without making them public. `Handle::release` is not
/// reachable from here -- only the wrapper type itself befriends its `Handle` base, so calls where the C API takes
/// ownership release from inside a member of the consuming wrapper.
struct HandleFactory {
	template <class T, class... ARGS>
	static auto Make(ARGS &&... args) -> T {
		return T(std::forward<ARGS>(args)...);
	}
};

template <class T>
struct always_false : std::false_type {};

/// @internal
/// Deletes `ptr` as a `T *`: the destructor shape the C API's opaque data slots take.
template <class T>
void TypedDelete(void *ptr) {
	delete static_cast<T *>(ptr);
}

/// @internal
/// Compares two `T *` by `operator==`: the equality shape the C API's opaque data slots take.
template <class T>
bool TypedEquals(void *ptr_a, void *ptr_b) {
	return *static_cast<T *>(ptr_a) == *static_cast<T *>(ptr_b);
}

/// @internal
/// Whether `const T` supports `operator==`.
template <class T, class = void>
struct is_equality_comparable : std::false_type {};
template <class T>
struct is_equality_comparable<T, std::void_t<decltype(std::declval<const T &>() == std::declval<const T &>())>>
    : std::true_type {};

/// @internal
/// The equals callback for a `T`-typed opaque slot: `TypedEquals` when `T` is equality-comparable, null otherwise,
/// which makes the engine fall back to comparing the slots by identity.
/// @internal
/// Names `const Vector &` once per element of a type pack: lets a function declare exactly one vector parameter per
/// argument type. Routed through a class template so the pack expansion is a non-deduced context on every compiler:
/// MSVC misaligns a plain alias-template pack against the call arguments instead of fixing its length from the
/// explicitly given type list.
template <class T>
struct VectorPerArgImpl {
	using type = const Vector &;
};
template <class T>
using VectorPerArg = typename VectorPerArgImpl<T>::type;

template <class T>
constexpr auto SelectEquals() -> bool (*)(void *, void *) {
	if constexpr (is_equality_comparable<T>::value) {
		return TypedEquals<T>;
	} else {
		return nullptr;
	}
}

/// @internal
/// Move-only custody of a user-provided pointer plus the destructor that frees it.
class UserData {
public:
	UserData() : data(nullptr), destructor(nullptr) {
	}
	UserData(void *data, void (*destructor)(void *)) : data(data), destructor(destructor) {
	}

	UserData(const UserData &) = delete;
	UserData &operator=(const UserData &) = delete;

	UserData(UserData &&other) noexcept : data(other.data), destructor(other.destructor) {
		other.data = nullptr;
		other.destructor = nullptr;
	}

	UserData &operator=(UserData &&other) noexcept {
		if (this != &other) {
			if (data && destructor) {
				destructor(data);
			}
			data = other.data;
			destructor = other.destructor;
			other.data = nullptr;
			other.destructor = nullptr;
		}
		return *this;
	}

	~UserData() {
		if (data && destructor) {
			destructor(data);
		}
	}

	auto get() const -> void * {
		return data;
	}

private:
	void *data;
	void (*destructor)(void *);
};

} // namespace detail

//----------------------------------------------------------------------------------------------------------------------
// Exceptions
//----------------------------------------------------------------------------------------------------------------------
// Every failure in this API is reported by throwing. Catch `Exception` to handle any of them; the subclasses single
// out the cases worth reacting to on their own.
// TODO: add more exception types!

/// Base class of all errors thrown by DuckDB. For errors raised by the database engine, `what()` carries the fully
/// formatted message, prefix included, e.g. "Parser Error: ...".
class Exception : public std::runtime_error {
public:
	/// @param code The numeric error code.
	/// @param message The full message, as later returned by `what()`.
	/// @param raw_message The message body without the error-type prefix, when available.
	Exception(const int code, const std::string &message, std::string raw_message = {})
	    : std::runtime_error(message), code(code), raw_message(std::move(raw_message)) {
	}

	/// The numeric error code identifying the kind of error. Prefer catching a typed subclass where one exists.
	auto GetCode() const -> int {
		return code;
	}

	/// The message body alone, without the "Catalog Error:" / "Parser Error:" / ... prefix that `what()` carries.
	/// Empty when no raw message is available.
	auto GetRawMessage() const -> const std::string & {
		return raw_message;
	}

private:
	int code;
	std::string raw_message;
};

/// Invalid input: a malformed SQL string, an argument of the wrong kind, a misused handle, and the like.
class InvalidInputException : public Exception {
public:
	explicit InvalidInputException(const std::string &message, std::string raw_message = {});
};

/// A running query was canceled, e.g. by `Connection::Interrupt`.
class InterruptException : public Exception {
public:
	explicit InterruptException(const std::string &message, std::string raw_message = {});
};

//----------------------------------------------------------------------------------------------------------------------
// Instance Option
//----------------------------------------------------------------------------------------------------------------------
// Configuration settings. Reached through the `Config` of an `Instance`, a `Connection` or a `Context`: `SetOption`
// writes one, `GetOption` reads its current value and the scope it came from, and `DescribeOption` describes it
// -- its default value, description, the scopes it may be written at, and aliases. Settings that can only be chosen at
// startup are passed to the `Instance` constructor.

/// The scope a setting is written at, or its value was read from.
enum class SettingScope : uint8_t {
	/// Write only: the setting's default scope (not its default value), exactly like SQL `SET name = value`.
	DEFAULT = 0,
	/// The whole database, like SQL `SET GLOBAL`.
	GLOBAL = 1,
	/// One connection's session, like SQL `SET SESSION`.
	SESSION = 2,
};

/// The description of a configuration setting, as DuckDB declares it: the same from every source, and without its
/// current value (read that with `GetOption`). Read-only.
/// The string accessors return views borrowed from this option, valid until it is destroyed.
class OptionDescription final : public detail::Handle<OptionDescription> {
	friend detail::HandleFactory;

public:
	OptionDescription(OptionDescription &&) noexcept = default;
	OptionDescription &operator=(OptionDescription &&) noexcept = default;

	/// The setting's name.
	auto GetName() const -> std::string_view;

	/// The value the setting falls back to when it is not set. NULL when the setting declares no default.
	auto GetDefaultValue() const -> Value;

	/// A human-readable description of the setting.
	auto GetDescription() const -> std::string_view;

	/// Whether the setting may be written at a scope.
	/// @param scope GLOBAL or SESSION; DEFAULT asks whether it may be written at all.
	auto SupportsScope(SettingScope scope) const -> bool;

	/// The scope a write at `SettingScope::DEFAULT` goes to: GLOBAL or SESSION.
	auto GetDefaultScope() const -> SettingScope;

	/// How many alternative names resolve to this setting. 0 for an extension setting.
	auto GetAliasCount() const -> size_t;

	/// One of the setting's aliases.
	/// @param index Alias index in [0, GetAliasCount()).
	auto GetAliasByIndex(size_t index) const -> std::string_view;

	~OptionDescription() override;

private:
	explicit OptionDescription(void *impl);
};

/// A borrowed handle to the scope config settings are read from and written at: taken from an `Instance` (the GLOBAL
/// scope alone), a `Connection` or a `Context` (that client's cascade), and valid for as long as its source is.
class Config final : public detail::Handle<Config> {
	friend detail::HandleFactory;

public:
	~Config() override;
	Config(Config &&) noexcept = default;
	Config &operator=(Config &&) noexcept = default;

	/// How many settings this config's database exposes.
	auto GetOptionCount() const -> size_t;

	/// The description of one setting.
	/// @param index Setting index in [0, GetOptionCount()).
	auto DescribeOption(size_t index) const -> OptionDescription;

	/// The description of one setting.
	/// @param name The setting's name or one of its aliases; an alias resolves to the canonical setting.
	/// @throws InvalidInputException When no setting goes by that name.
	auto DescribeOption(std::string_view name) const -> OptionDescription;

	/// The current value of a setting in this config, and the scope it was read from.
	/// @param name The setting's name or one of its aliases.
	/// @throws InvalidInputException When no setting goes by that name.
	auto GetOption(std::string_view name) const -> OptionValue;

	/// `GetOption` for a name that may not be a setting: an empty optional instead of an exception.
	/// @param name The setting's name or one of its aliases.
	auto TryGetOption(std::string_view name) const -> std::optional<OptionValue>;

	/// Shorthand for `GetOption`.
	auto operator[](std::string_view name) const -> OptionValue;

	/// Writes a setting in this config.
	/// @param name The setting to write, either its canonical name or one of its aliases.
	/// @param value The new value, cast to the setting's type as SQL `SET` does.
	/// @param scope GLOBAL to write it database-wide, SESSION for one session only, or DEFAULT for the setting's
	/// default scope. An `Instance`'s config supports only GLOBAL.
	/// @throws Exception When no setting goes by that name, the value does not cast, or the scope is not supported.
	auto SetOption(std::string_view name, const Value &value, SettingScope scope = SettingScope::DEFAULT) -> void;
	/// The text overload of the above, for a value in the textual form SQL `SET` accepts.
	auto SetOption(std::string_view name, std::string_view value, SettingScope scope = SettingScope::DEFAULT) -> void;

private:
	explicit Config(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Context
//----------------------------------------------------------------------------------------------------------------------
// The client context of an operation in flight, handed to code that DuckDB calls back into.
// It is more or less equivalent to a `Connection`, but viewed from the "inside" of the system.

/// The severity of a log message, compared against the database's configured threshold.
enum class LogLevel : uint32_t {
	LOG_TRACE = 10,
	LOG_DEBUG = 20,
	LOG_INFO = 30,
	LOG_WARNING = 40,
	LOG_ERROR = 50,
	LOG_FATAL = 60,
};

/// A borrowed handle to the scope values, types and data chunks are created in: taken from an `Instance`, a
/// `Connection` or a `Context`, and valid for as long as its source is. A factory resolves in the scope of its source:
/// a connection's or a context's factory reaches the catalog and the client's settings, while an instance's factory
/// knows only the built-in types and casts.
class Factory final : public detail::Handle<Factory> {
	friend detail::HandleFactory;

public:
	~Factory() override;
	Factory(Factory &&) noexcept = default;
	Factory &operator=(Factory &&) noexcept = default;

	/// Parses a SQL type expression into an owned type: primitives, parameterized kinds, and extension types alike.
	/// @param text A type as SQL spells it, e.g. "DECIMAL(18, 3)" or "STRUCT(a INTEGER, b VARCHAR)".
	/// @return The parsed type.
	/// @throws Exception When the text does not name a type.
	auto ParseType(std::string_view text) const -> LogicalType;

	/// Builds a type from a type name plus parameters. Like a structured version of `ParseType`
	/// @param name The type's unqualified name, e.g. "LIST" or "DECIMAL".
	/// @param params The type's parameters, in the order SQL takes them. A `TypeParam` with an empty name is considered
	/// a positional parameter.
	auto CreateType(std::string_view name, const std::vector<TypeParam> &params) const -> LogicalType;
	/// Parameterless overload of the above.
	auto CreateType(std::string_view name) const -> LogicalType;

	/// `CreateType` for a name that may be catalog- or schema-qualified. An unqualified name is resolved along the
	/// search path and then in the system catalog; a qualified one is resolved exactly as written.
	auto CreateType(const QualifiedName &name, const std::vector<TypeParam> &params) const -> LogicalType;
	/// Parameterless overload of the above.
	auto CreateType(const QualifiedName &name) const -> LogicalType;

	/// The id-keyed twin of `CreateType`: the id resolves to its canonical name and binds like it.
	/// @param id The type's id. Without parameters, only ids that name a complete type on their own are accepted;
	/// parameterized kinds such as LIST or DECIMAL require parameters.
	/// @param params The type's parameters, as in the name-keyed overload.
	auto CreateType(LogicalTypeId id, const std::vector<TypeParam> &params) const -> LogicalType;
	/// Parameterless overload of the above.
	auto CreateType(LogicalTypeId id) const -> LogicalType;

	/// Starts composing a type step by step.
	/// @return A `TypeBuilder` over this factory, for composing a nested type without assembling the parameter vector
	/// by hand.
	auto CreateType() -> TypeBuilder<Factory>;

	/// Creates a `Value` with this factory; see `Value::Create` for the accepted C++ types.
	/// @param value The C++ value to convert.
	template <class T>
	auto CreateValue(T &&value) -> Value;

	/// Creates an empty chunk with a column per type; see `DataChunk(Factory &, types)`.
	/// @param types One type per column. Types containing ANY are rejected.
	auto CreateDataChunk(const std::vector<LogicalType> &types) -> DataChunk;

	/// Creates an empty collection; see `ColumnDataCollection(Factory &, types)`.
	/// @param types One type per column, at least one. Types containing ANY are rejected.
	auto CreateColumnDataCollection(const std::vector<LogicalType> &types) -> ColumnDataCollection;

private:
	explicit Factory(void *impl);
};

/// A borrowed handle to a client context: the one a callback is handed, valid only for the duration of that callback.
class Context final : public detail::Handle<Context> {
	friend detail::HandleFactory;

public:
	~Context() override;
	Context(Context &&) noexcept = default;
	Context &operator=(Context &&) noexcept = default;

	/// The config of this context: its connection's cascade.
	auto GetConfig() -> Config & {
		return config;
	}
	/// Const overload of the above: a const config reads settings, but does not write them.
	auto GetConfig() const -> const Config & {
		return config;
	}

	/// The file system this context reads and writes through. Borrowed, and valid only while the context is.
	auto GetFileSystem() const -> FileSystem;

	/// The factory values, types and data chunks are created through in this context.
	auto GetFactory() -> Factory & {
		return factory;
	}

	/// Writes a message to DuckDB's log, readable through `SELECT * FROM duckdb_logs`.
	/// Whether the entry is recorded is up to the database's log configuration; a message it filters out is dropped
	/// without error.
	/// @param level The severity of the message.
	/// @param message The message body.
	/// @param log_type The log type to record under, matched case-sensitively. Empty selects the default type.
	auto Log(LogLevel level, std::string_view message, std::string_view log_type = {}) const -> void;

private:
	explicit Context(void *impl);

	Factory factory;
	Config config;
};

//----------------------------------------------------------------------------------------------------------------------
// Custom Option
//----------------------------------------------------------------------------------------------------------------------

/// A configuration setting of your own. Create one through a `Factory` of the database it will be registered in,
/// describe it (name, type, default value, scopes), then register it with `Connection::Register` or
/// `Extension::Register`. From then on it is a setting like any other: read and written through a `Config` and from
/// SQL with `SET` and `RESET`, and described by `Config::DescribeOption`.
///
/// Registration is not transactional: a registered option is visible to every connection at once, is not undone by a
/// rollback, and lives for as long as the database does.
class CustomOption final : public detail::Handle<CustomOption> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	class UpdateInput;

	/// Called whenever the option is set or reset, after the new value is cast to the option's type and before it is
	/// stored. May replace the value through `UpdateInput::SetValue`, or refuse it by throwing. Not called for the
	/// default value at registration, nor for a value given to the database at startup. Must not write settings
	/// itself.
	using UpdateCallback = void (*)(UpdateInput &input);

	CustomOption(CustomOption &&) noexcept = default;
	CustomOption &operator=(CustomOption &&) noexcept = default;

	~CustomOption() override;

	/// Creates an empty CustomOption through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> CustomOption;

	/// Sets the option's name, as `SET` and `Config` refer to it.
	auto SetName(const std::string &name) & -> CustomOption &;
	/// Sets the type every value written to the option is cast to. Optional when a default value is set, whose type
	/// the option then takes.
	auto SetType(const LogicalType &type) & -> CustomOption &;
	/// Sets the value the option has until it is set, and that `RESET` restores. Cast to the option's type at
	/// registration. Without one, the option reads as NULL until it is set.
	auto SetDefaultValue(const Value &value) & -> CustomOption &;
	/// Sets the description `OptionDescription::GetDescription` and `duckdb_settings()` report.
	auto SetDescription(std::string_view description) & -> CustomOption &;
	/// Sets the scope a write at `SettingScope::DEFAULT` goes to: GLOBAL or SESSION, and one the option supports.
	/// Defaults to SESSION, or to GLOBAL when the option does not support SESSION.
	auto SetDefaultScope(SettingScope scope) & -> CustomOption &;
	/// Sets whether the option can be written at a scope, GLOBAL or SESSION. Both are supported unless told otherwise.
	auto SetSupportsScope(SettingScope scope, bool supported) & -> CustomOption &;

	/// Constructs user data of type `T`, carried by the registered option and freed at database teardown; read it from
	/// the update callback via `UpdateInput::GetUserData<T>`. Consumed by registration: set it again before
	/// re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> CustomOption & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetUpdateCallback(UpdateCallback callback) & -> CustomOption &;

private:
	explicit CustomOption(void *impl);
	//! Moves what the callback needs into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	UpdateCallback update_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the update callback works with. Borrowed, valid only for the callback duration.
	class UpdateInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `CustomOption::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The value the option is updated to, cast to its type. On a reset, the default value, which may be NULL.
		auto GetValue() const -> Value;
		/// Replaces the value the option is updated to. Cast to the option's type.
		auto SetValue(const Value &value) -> void;
		/// The scope the value is written at: GLOBAL or SESSION, a DEFAULT write resolved to the default scope.
		auto GetScope() const -> SettingScope;
		/// Whether the update is a reset to the default value, like SQL `RESET`.
		auto IsReset() const -> bool;

		/// The context setting the option. When the option is written through the config of an `Instance`, or given
		/// at startup, this is an internal context whose session state is discarded after the update. Borrowed, valid
		/// only for the callback duration.
		auto GetContext() -> Context &;

	private:
		UpdateInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// SQL statements
//----------------------------------------------------------------------------------------------------------------------
// A SQL string is parsed once into statements, which can then be bound or executed repeatedly. Parsing is purely
// syntactic: it touches no catalog and opens no transaction, so unknown tables and type errors surface at bind or
// execution time instead.

/// The kind of a SQL statement, as the parser classifies it.
enum class StatementType : uint8_t {
	INVALID = 0,
	SELECT = 1,
	INSERT = 2,
	UPDATE = 3,
	CREATE = 4,
	DELETE = 5,
	PREPARE = 6,
	EXECUTE = 7,
	ALTER = 8,
	TRANSACTION = 9,
	COPY = 10,
	ANALYZE = 11,
	VARIABLE_SET = 12,
	CREATE_FUNC = 13,
	EXPLAIN = 14,
	DROP = 15,
	EXPORT = 16,
	PRAGMA = 17,
	VACUUM = 18,
	CALL = 19,
	SET = 20,
	LOAD = 21,
	RELATION = 22,
	EXTENSION = 23,
	LOGICAL_PLAN = 24,
	ATTACH = 25,
	DETACH = 26,
	MULTI = 27,
	COPY_DATABASE = 28,
	UPDATE_EXTENSIONS = 29,
	MERGE_INTO = 30,
	CONNECT = 31,
	DISCONNECT = 32,
	EXTERNAL_RESOURCE = 33,
};

/// An owned, parsed SQL statement, produced by `StatementIterator::Next` and executed by `Connection::Execute`.
/// Executing borrows the statement, so the same one can be executed any number of times.
class SqlStatement final : public detail::Handle<SqlStatement> {
	friend detail::HandleFactory;

public:
	SqlStatement(SqlStatement &&) noexcept = default;
	SqlStatement &operator=(SqlStatement &&) noexcept = default;

	~SqlStatement() override;

	/// The parser's classification of the statement, available without binding. It is the type before the rewrites
	/// execution applies, so a PRAGMA reports PRAGMA; `QueryResult::GetStatementType` reports what it became.
	auto GetStatementType() const -> StatementType;

	/// The statement's own text: its slice of the parsed string, from its first token up to the first token of the
	/// next statement or the end of the input, so a trailing `;` and the whitespace after it are included.
	/// @return A view borrowed from this statement, valid until it is destroyed.
	auto GetText() const -> std::string_view;

	/// The names of the statement's parameters, in binding order: the order of `Connection::Bind`'s `parameters`
	/// schema, so element i here names field i there. "1", "2", ... for positional parameters ($1 or ?), the
	/// identifier for named ones ($name). Parse-time, so it needs no catalog; only the types wait for `Bind`.
	/// @return Views borrowed from this statement, valid until it is destroyed.
	auto GetParameterNames() const -> std::vector<std::string_view>;

	/// Makes a collection visible to the statement as a table named `name`, as a common table expression. Each
	/// execution reads the collection's contents at that moment, so a prepared statement can run again after the
	/// collection was cleared and refilled. Only SELECT, INSERT, UPDATE, DELETE and MERGE INTO statements accept one.
	/// @param name The name the statement refers to the collection by; it shadows a catalog table of that name.
	/// @param collection Borrowed: it must outlive this statement, every statement copied or prepared from it, and
	/// every result executing it, and must not be cleared or reset while such a result is live.
	/// @param column_names Names for the collection's columns, one per column; empty names them col1..colN.
	/// @throws InvalidInputException For another statement type, a name the statement already defines, or a
	/// mismatched number of column names.
	auto AddCollection(std::string_view name, const ColumnDataCollection &collection,
	                   const std::vector<std::string> &column_names = {}) -> void;

private:
	explicit SqlStatement(void *impl);
};

/// An owned iterator over the statements in a SQL string, produced by `Connection::ParseSQL`.
/// Statements it has already yielded are independent of it and stay valid after the iterator is destroyed.
class StatementIterator final : public detail::Handle<StatementIterator> {
	friend detail::HandleFactory;

public:
	StatementIterator(StatementIterator &&) noexcept = default;
	StatementIterator &operator=(StatementIterator &&) noexcept = default;

	~StatementIterator() override;

	/// The next statement, or an empty handle once the string is exhausted; calling it again then keeps returning
	/// empty.
	/// @throws Exception When the next statement does not parse; the iterator is exhausted afterwards.
	auto Next() -> SqlStatement;

private:
	explicit StatementIterator(void *impl);
};

/// The lexical class of a token, as `Connection::Tokenize` reports it: what the tokenizer assigns before parsing, with
/// no catalog or grammar-role refinement.
enum class TokenType : uint8_t {
	/// Never the class of a token; the value of a zero-initialized `Token`.
	INVALID = 0,
	/// A keyword of the connection's grammar.
	KEYWORD = 1,
	/// A bare or double-quoted identifier, quotes included.
	IDENTIFIER = 2,
	/// A quoted or dollar-quoted string, delimiters included.
	STRING_LITERAL = 3,
	/// A numeric literal.
	NUMBER_LITERAL = 4,
	/// An operator or punctuation run other than the statement terminator.
	OPERATOR = 5,
	/// A line or block comment, delimiters included.
	COMMENT = 6,
	/// A statement-terminating semicolon.
	TERMINATOR = 7,
};

/// One token of a SQL string: its class and its byte range in the input, so `sql.substr(start, length)` is the lexeme.
struct Token {
	/// The token's lexical class.
	TokenType type = TokenType::INVALID;
	/// Byte offset of the token in the input.
	idx_t start = 0;
	/// Byte length of the token.
	idx_t length = 0;
};

/// The tokens of a SQL string, as `Connection::Tokenize` reports them.
struct TokenList {
	/// The tokens in input order; empty for empty or whitespace-only input.
	std::vector<Token> tokens;
	/// Whether the input ended before the closing delimiter of its last token: inside an open string, quoted
	/// identifier, block comment or dollar-quoted string, or in a line comment with no trailing newline. When true,
	/// the last token is the open one.
	bool ends_unterminated = false;
};

/// A statement bound and planned once, executable repeatedly. Produced by `Connection::Prepare`.
/// Where `Connection::Execute` re-binds on every call, this may run the plan it built at prepare time; ask
/// `ReusesPlan` which one you got. Execution returns the same `QueryResult`, with identical behaviour, or an
/// `ArrowResult` when an `ArrowFormat` is passed.
/// It keeps its connection's session alive, so it stays usable even after the `Connection` is gone.
class PreparedStatement final : public detail::Handle<PreparedStatement> {
	friend detail::HandleFactory;

public:
	PreparedStatement(PreparedStatement &&) noexcept = default;
	PreparedStatement &operator=(PreparedStatement &&) noexcept = default;

	~PreparedStatement() override;

	/// Executes with positional parameters ($1 = parameters[0]), returning a lazy streaming result.
	/// Non-consuming: execute the same statement again, with the same values or different ones.
	/// @param parameters Values for the statement's parameters, bound positionally.
	/// @param parameter_count How many values `parameters` points at.
	/// @return A streaming result. Execution is deferred until the result is read; binding errors throw here.
	/// @throws Exception While an earlier result on the connection is still live. A failed execution leaves the
	/// prepared statement usable.
	auto Execute(const Value *parameters, idx_t parameter_count) -> QueryResult;

	/// Executes a statement that takes no parameters.
	auto Execute() -> QueryResult;

	/// `std::vector` overload of the positional-parameter `Execute`.
	auto Execute(const std::vector<Value> &parameters) -> QueryResult;

	/// Executes with named parameters.
	/// @param parameters One binding per parameter; each binds to $name, or positionally when its name is empty.
	auto Execute(const std::vector<NamedParam> &parameters) -> QueryResult;

	/// Executes like the positional-parameter `Execute`, into a result that produces Arrow arrays.
	/// @param parameters Values for the statement's parameters, bound positionally. Not consumed.
	/// @param parameter_count How many values `parameters` points at.
	/// @param format The Arrow format options, chiefly the rows-per-array cap.
	/// @return A streaming Arrow result. Execution is deferred until the result is read; binding errors throw here.
	/// @throws Exception While an earlier result on the connection is still live, in either format. A failed
	/// execution leaves the prepared statement usable.
	auto Execute(const Value *parameters, idx_t parameter_count, ArrowFormat format) -> ArrowResult;

	/// The Arrow form of a statement that takes no parameters.
	auto Execute(ArrowFormat format) -> ArrowResult;

	/// `std::vector` overload of the positional-parameter Arrow `Execute`.
	auto Execute(const std::vector<Value> &parameters, ArrowFormat format) -> ArrowResult;

	/// The Arrow form of the named-parameter `Execute`.
	auto Execute(const std::vector<NamedParam> &parameters, ArrowFormat format) -> ArrowResult;

	/// Whether executions reuse the plan built at prepare time, rather than re-binding each time and being no
	/// faster than `Connection::Execute`. A static property of the built plan; see the C API's
	/// `duckdb_v2_prepared_statement_reuses_plan` for what qualifies.
	auto ReusesPlan() const -> bool;

private:
	explicit PreparedStatement(void *impl);

	auto ExecuteArrowNamed(const NamedParam *parameters, idx_t parameter_count, ArrowFormat format) -> ArrowResult;
};

//----------------------------------------------------------------------------------------------------------------------
// Connection
//----------------------------------------------------------------------------------------------------------------------
// A session on an `Instance`: the handle that parses, binds and executes SQL, creates types and values outside of a
// callback, and carries session-scoped settings. A connection is not thread-safe -- give each thread its own, via
// `Instance::Connect` -- with the deliberate exception of `Interrupt` and `GetQueryProgress`, which exist to be called
// while another thread runs a query.

/// A connection to a database.
/// It must not outlive the `Instance` it was opened on, and only one result may be live on it at a time.
class Connection final : public detail::Handle<Connection> {
	friend detail::HandleFactory;

public:
	Connection(Connection &&other) noexcept : factory(std::move(other.factory)), config(std::move(other.config)) {
		std::swap(impl, other.impl);
		std::swap(owned, other.owned);
	}

	Connection &operator=(Connection &&other) noexcept {
		std::swap(impl, other.impl);
		std::swap(factory, other.factory);
		std::swap(config, other.config);
		std::swap(owned, other.owned);
		return *this;
	}

	/// A snapshot of the running query's progress. Progress is only tracked while the `enable_progress_bar` setting is
	/// on; a percentage of -1 with both row counts at 0 means no information is available.
	struct QueryProgress {
		/// Completion in [0, 100], or -1 when unknown.
		double percentage;
		/// Rows processed so far.
		uint64_t rows_processed;
		/// Rows the query expects to process in total.
		uint64_t total_rows_to_process;
	};

	~Connection() override;

	/// The factory values, types and data chunks are created through on this connection.
	auto GetFactory() -> Factory & {
		return factory;
	}

	/// Registers on this connection's database, in the connection's transaction: inside an explicit transaction a
	/// rollback undoes catalog entries, though casts and replacement scans take effect immediately. Replacement scans
	/// stay local to this connection.
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, exec callback, or a usable return type is missing.
	auto Register(ScalarFunction &scalar_function) -> void;
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, a required callback, or a usable return type is missing.
	auto Register(AggregateFunction &aggregate_function) -> void;
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, bind callback or exec callback is missing, the signature
	/// declares a return type, or a partitioning callback is set without a partition data callback.
	auto Register(TableFunction &table_function) -> void;
	/// Registers the function. The function object remains valid.
	/// @throws InvalidInputException When the name or the single-file function is missing, or the single-file
	/// function does not exist or does not take the file to read as its only positional parameter.
	auto Register(MultiFileFunction &multi_file_function) -> void;
	/// Registers the type. The type object remains valid and may be adjusted
	/// and registered again.
	/// @throws InvalidInputException When the name or the base type is missing, or the base type is not concrete.
	auto Register(CustomType &custom_type) -> void;
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name is missing, neither side is configured, a configured
	/// `COPY ... TO` side lacks its batch or flush callback, or a configured `COPY ... FROM` side lacks its bind or
	/// exec callback.
	auto Register(CopyFunction &copy_function) -> void;
	/// Registers the cast, replacing whatever cast was registered for the same type pair. The function object remains
	/// valid and may be adjusted and registered again; user data set via `SetUserData` is consumed by the first
	/// registration.
	/// @throws InvalidInputException When the source type, target type, or exec callback is missing, or either type is
	/// not concrete.
	auto Register(CastFunction &cast_function) -> void;
	/// Registers the pragma. The function object remains valid; user data set via `SetUserData` is consumed by the
	/// first registration.
	/// @throws InvalidInputException When the name is missing, not exactly one of the query and exec callbacks is set,
	/// the signature declares a return type, or a pragma of the same name already exists.
	auto Register(PragmaFunction &pragma_function) -> void;
	/// Registers the option on the database, visible to every connection. Not transactional: it takes effect at once,
	/// also inside an explicit transaction, and a rollback does not undo it. The option object remains valid; user
	/// data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, or both the type and the default value, are missing, the default
	/// value does not cast to the type, the scopes are inconsistent, or an option of the same name already exists.
	auto Register(CustomOption &custom_option) -> void;
	/// Registers the scan. Scans are consulted in registration order within
	/// their scope, connection-scoped ones before database-wide ones, and the first to claim a name wins. A scan can
	/// be registered only once.
	/// @throws InvalidInputException When the callback is missing, or the scan is already registered.
	auto Register(ReplacementScan &replacement_scan) -> void;

	/// The config of this connection: its own cascade.
	auto GetConfig() -> Config & {
		return config;
	}
	/// Const overload of the above: a const config reads settings, but does not write them.
	auto GetConfig() const -> const Config & {
		return config;
	}

	/// The file system this connection reads and writes through, with its settings and secrets. Borrowed, and valid
	/// only while the connection is.
	auto GetFileSystem() const -> FileSystem;

	/// Writes a message to DuckDB's log, readable through `SELECT * FROM duckdb_logs`.
	/// Whether the entry is recorded is up to the database's log configuration; a message it filters out is dropped
	/// without error.
	/// @param level The severity of the message.
	/// @param message The message body.
	/// @param log_type The log type to record under, matched case-sensitively. Empty selects the default type.
	auto Log(LogLevel level, std::string_view message, std::string_view log_type = {}) const -> void;

	/// Parses a SQL string into an iterator over its statements, without binding or executing any of them.
	/// Parsing happens statement by statement as the iterator advances, so a syntax error surfaces from
	/// `StatementIterator::Next` rather than from this call.
	/// @param sql One or more semicolon-separated SQL statements.
	/// @param apply_invalidation_policy Whether a syntax error invalidates the active transaction, according to the
	/// transaction invalidation policy. Pass false when the statements are only inspected, e.g. with `Bind`, instead of
	/// run.
	auto ParseSQL(const char *sql, bool apply_invalidation_policy = true) -> StatementIterator;

	/// `std::string` overload of `ParseSQL`.
	auto ParseSQL(const std::string &sql, bool apply_invalidation_policy = true) -> StatementIterator {
		return ParseSQL(sql.c_str(), apply_invalidation_policy);
	}

	/// Splits a SQL string into its tokens without parsing it: no binding, no catalog access, no transaction. The
	/// connection supplies the grammar whose keyword set decides KEYWORD versus IDENTIFIER. Offsets are byte offsets
	/// into `sql` exactly as given, and whitespace is not a token. Malformed input does not throw: an unterminated
	/// string, block comment or dollar-quoted string yields a token that runs to the end of the input, and
	/// `TokenList::ends_unterminated` reports it.
	/// @param sql The SQL text; may contain interior null bytes.
	/// @return The tokens in input order, and whether the input ended inside an open token.
	auto Tokenize(std::string_view sql) const -> TokenList;

	/// Executes a statement, borrowing it rather than consuming it, so the same statement can be executed again.
	/// @param statement The statement to execute.
	/// @param parameters Values for the statement's parameters, bound positionally ($1 = parameters[0]).
	/// @param parameter_count How many values `parameters` points at.
	/// @return A streaming result. Execution is deferred until the result is read; binding errors throw here.
	/// @throws Exception While an earlier result on this connection is still live.
	auto Execute(const SqlStatement &statement, const Value *parameters, idx_t parameter_count) -> QueryResult;

	/// Executes a statement that takes no parameters.
	auto Execute(const SqlStatement &statement) -> QueryResult;

	/// `std::vector` overload of the positional-parameter `Execute`.
	auto Execute(const SqlStatement &statement, const std::vector<Value> &parameters) -> QueryResult;

	/// Executes a statement with named parameters.
	/// @param statement The statement to execute.
	/// @param parameters One binding per parameter; each binds to $name, or positionally when its name is empty. A
	/// statement cannot mix named and positional parameters.
	auto Execute(const SqlStatement &statement, const std::vector<NamedParam> &parameters) -> QueryResult;

	/// Parses and executes a single SQL statement in one call.
	/// @param sql Exactly one SQL statement. Use `ParseSQL` for multi-statement input.
	/// @throws InvalidInputException When `sql` does not hold exactly one statement.
	auto Execute(const std::string &sql) -> QueryResult;

	/// Executes a statement like `Execute`, into a result that produces Arrow arrays instead of `DataChunk`s.
	/// @param statement The statement to execute. Not consumed, so it can be executed again.
	/// @param parameters Values for the statement's parameters, bound positionally ($1 = parameters[0]). Not consumed.
	/// @param parameter_count How many values `parameters` points at.
	/// @param format The Arrow format options, chiefly the rows-per-array cap.
	/// @return A streaming Arrow result. Execution is deferred until the result is read; binding errors throw here.
	/// @throws Exception While an earlier result on this connection is still live, in either format.
	auto Execute(const SqlStatement &statement, const Value *parameters, idx_t parameter_count, ArrowFormat format)
	    -> ArrowResult;

	/// The Arrow form of a statement that takes no parameters.
	auto Execute(const SqlStatement &statement, ArrowFormat format) -> ArrowResult;

	/// `std::vector` overload of the positional-parameter Arrow `Execute`.
	auto Execute(const SqlStatement &statement, const std::vector<Value> &parameters, ArrowFormat format)
	    -> ArrowResult;

	/// The Arrow form of the named-parameter `Execute`.
	/// @param parameters One binding per parameter; each binds to $name, or positionally when its name is empty. A
	/// statement cannot mix named and positional parameters.
	auto Execute(const SqlStatement &statement, const std::vector<NamedParam> &parameters, ArrowFormat format)
	    -> ArrowResult;

	/// Parses and executes a single SQL statement in one call, into a result that produces Arrow arrays.
	/// @param sql Exactly one SQL statement. Use `ParseSQL` for multi-statement input.
	/// @throws InvalidInputException When `sql` does not hold exactly one statement.
	auto Execute(const std::string &sql, ArrowFormat format) -> ArrowResult;

	/// Binds a statement without executing it, borrowing it rather than consuming it.
	/// @param statement The statement to bind.
	/// @return Its signature: the columns it will produce and the parameters it expects.
	/// @throws Exception With code `DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED` for a statement that expands into several,
	/// whose columns are known only once it executes.
	auto Bind(const SqlStatement &statement) const -> Signature;

	/// Resolves a table name and snapshots its description: where it resolved, its columns, and per-column facts.
	/// @param name A possibly partial table name, resolved through the search path exactly as SQL resolves it.
	/// @throws Exception When the name resolves to nothing, to a view, or is ambiguous between a schema and an attached
	/// database.
	auto DescribeTable(const QualifiedName &name) const -> TableDescription;

	/// Enables or disables profiling for the queries this connection runs from now on, which gathers the metrics
	/// `GetQueryMetrics` reports. Profiling also prints every profile as the `enable_profiling` and `profiling_output`
	/// options configure, by default to the standard output; set `enable_profiling` to `no_output` to only gather
	/// metrics. Unstable API.
	auto EnableProfiling(bool enabled = true) -> void;

	/// Takes a snapshot of the metrics of the last query this connection finished. What it holds is decided by the
	/// profiling configuration, e.g. `SET enable_profiling = 'no_output'`. Unstable API.
	/// @throws Exception While a result on this connection is still live.
	auto GetQueryMetrics() const -> QueryMetrics;

	/// Prepares a statement into a handle that can be executed repeatedly, borrowing it rather than consuming it.
	/// @param statement The statement to prepare.
	/// @param require_cacheable When true, fail rather than return a statement whose plan is re-bound on every
	/// execution, so a caller who wants the handle only for the speedup finds out here.
	/// @throws Exception On a bind or catalog error, while a result on this connection is still live, or when
	/// `require_cacheable` is set and the plan would not be reused.
	auto Prepare(const SqlStatement &statement, bool require_cacheable = false) -> PreparedStatement;

	/// Asks the running query to stop. `QueryResult::Step` then reports CANCELLED, and `FetchChunk` / `Drain` throw
	/// `InterruptException`. An `ArrowResult` reacts the same way, with `FetchArray` in place of `FetchChunk`.
	/// Callable from any thread, and a no-op when no query is running.
	auto Interrupt() -> void;

	/// Reads how far the running query has come. Callable from any thread.
	auto GetQueryProgress() const -> QueryProgress;

	/// Wraps a connection handle owned by someone else.
	/// @param opaque The borrowed handle; must be a connection handle of the DuckDB C API.
	/// @return A non-owning `Connection`: it must not outlive the handle, and it will not disconnect on destruction.
	static auto FromOpaque(void *opaque) -> Connection {
		return Connection(opaque, false);
	}

private:
	explicit Connection(void *impl, bool owned);
	Factory factory;
	Config config;

	auto ExecuteArrowNamed(const SqlStatement &statement, const NamedParam *parameters, idx_t parameter_count,
	                       ArrowFormat format) -> ArrowResult;

	bool owned = false; // TODO: This should be fixed C++ side
};

//----------------------------------------------------------------------------------------------------------------------
// Instance
//----------------------------------------------------------------------------------------------------------------------
// An open database: the catalog, the storage behind it, and the settings shared by every session on it. Databases are
// opened by constructing an `Instance` and worked with through the `Connection`s they hand out. Every instance lives in
// an `Environment`, and a file can only be open in one instance of an environment at a time. Instances opened without
// an explicit environment share the process-wide default one.

/// The environment database instances are created in. Optional: an `Instance` constructed without one lives in the
/// process-wide `Environment::Default()`. Create one explicitly to control the lifetime of the environment, or to keep
/// a group of instances apart from the default one. It must outlive every `Instance` created in it; destroying it
/// while instances are still alive leaks it.
class Environment final : public detail::Handle<Environment> {
public:
	/// Creates a new environment.
	Environment();
	~Environment() override;
	Environment(Environment &&) noexcept = default;
	Environment &operator=(Environment &&) noexcept = default;

	/// The process-wide environment instances are created in when none is given. Created on first use and never
	/// destroyed, so it outlives every instance, including ones destroyed during static destruction.
	static auto Default() -> Environment &;

	/// How many database instances are currently alive in this environment.
	auto GetInstanceCount() const -> size_t;
};

/// The options an `Instance` can only be given when it is created: once it exists they can no longer change, or only
/// become stricter. Each setter returns the options, so they chain: `StartupOptions().EnableUnsignedExtensions(true)`.
/// An option left unset keeps DuckDB's default.
class StartupOptions {
	friend class Instance;

public:
	StartupOptions() = default;

	/// Whether extensions built by the community can be loaded. Defaults to true.
	auto EnableCommunityExtensions(bool value) -> StartupOptions & {
		allow_community_extensions = value;
		return *this;
	}

	/// Whether extensions with an invalid or missing signature can be loaded. Defaults to false.
	auto EnableUnsignedExtensions(bool value) -> StartupOptions & {
		allow_unsigned_extensions = value;
		return *this;
	}

	/// Whether a fatal error invalidates the instance, refusing every further query on it. Disabling this should be
	/// done with great care: DuckDB cannot guarantee correct behavior after a fatal error. Defaults to true.
	auto EnableDatabaseInvalidation(bool value) -> StartupOptions & {
		disable_database_invalidation = !value;
		return *this;
	}

	/// Whether secrets can be printed unredacted. Defaults to false.
	auto EnableUnredactedSecrets(bool value) -> StartupOptions & {
		allow_unredacted_secrets = value;
		return *this;
	}

	/// Metadata added to the user agent of the HTTP requests DuckDB makes, so callers and wrappers can tag their own
	/// traffic. Defaults to none.
	auto SetCustomUserAgent(std::string_view value) -> StartupOptions & {
		custom_user_agent = std::string(value);
		return *this;
	}

private:
	std::optional<bool> allow_community_extensions;
	std::optional<bool> allow_unsigned_extensions;
	std::optional<bool> allow_unredacted_secrets;
	std::optional<bool> disable_database_invalidation;
	std::optional<std::string> custom_user_agent;
};

class Instance final : public detail::Handle<Instance> {
	friend detail::HandleFactory;

public:
	/// Creates a database instance with nothing attached, in the default environment. Attach a database with
	/// `Attach` and make it the default with `SetDefault`.
	Instance();

	/// Creates a database instance with nothing attached, in the default environment, with the options that can only
	/// be chosen at startup.
	explicit Instance(const StartupOptions &options);

	/// Creates a database instance in the default environment, attaches `path`, and makes it the default database.
	/// @param path The database file, or ":memory:" / the empty string for an in-memory database.
	/// @param options The options that can only be chosen at startup.
	explicit Instance(const std::string &path, const StartupOptions &options = {});

	/// Creates a database instance with nothing attached, in `env`.
	/// @param env The environment to create the instance in. Must outlive the instance.
	/// @param options The options that can only be chosen at startup.
	explicit Instance(Environment &env, const StartupOptions &options = {});

	/// Creates a database instance in `env`, attaches `path`, and makes it the default database.
	/// @param env The environment to create the instance in. Must outlive the instance.
	/// @param path The database file, or ":memory:" / the empty string for an in-memory database.
	/// @param options The options that can only be chosen at startup.
	Instance(Environment &env, const std::string &path, const StartupOptions &options = {});

	~Instance() override;
	Instance(Instance &&) noexcept = default;
	Instance &operator=(Instance &&) noexcept = default;

	/// Attaches a database to this instance, like SQL `ATTACH 'path'`, under the name derived from the path: `memory`
	/// for an in-memory database, else the file name up to its first `.` (`/data/sales.2024.db` -> `sales`).
	/// @param path The database file, or ":memory:" / the empty string for an in-memory database.
	/// @param make_default Whether to make it the default database for sessions opened afterwards, as `SetDefault`
	/// would; false leaves the default alone.
	/// @throws Exception When the path is already attached in this process, or a database of that name exists.
	auto Attach(const std::string &path, bool make_default = false) -> void;

	/// Attaches a database under a name and with the per-database options SQL `ATTACH` takes, like
	/// `ATTACH 'path' AS name (KEY value, ...)`.
	/// @param path The database file, or ":memory:" / the empty string for an in-memory database.
	/// @param name The name to attach under; empty for the name derived from the path.
	/// @param options The `(KEY value)` options, e.g. `{{"READ_ONLY", "true"}, {"BLOCK_SIZE", "16384"}}`. Keys match
	/// case-insensitively and values are passed on as the text a quoted SQL literal carries; the engine casts the
	/// ones it knows and hands the rest to the storage extension owning the database.
	/// @param make_default Whether to make it the default database for sessions opened afterwards.
	auto Attach(const std::string &path, const std::string &name,
	            const std::unordered_map<std::string, std::string> &options, bool make_default = false) -> void;

	/// Detaches the database attached under `name`, like SQL `DETACH name`. Connections still using it keep it alive
	/// until they let go; if it was the default database, new sessions have no default until `SetDefault` names
	/// another.
	/// @throws InvalidInputException When no database is attached under `name`.
	auto Detach(const std::string &name) -> void;

	/// Makes the database attached under `name` the default database for sessions opened from now on: where their
	/// unqualified DDL and unqualified table lookups that miss the temporary catalog go, unless they `USE` another.
	/// Sessions already open keep the default they connected with.
	/// @throws InvalidInputException When no database is attached under `name`.
	auto SetDefault(const std::string &name) -> void;

	/// The config of this database: the GLOBAL scope. Settings that can only be chosen at startup are passed to the
	/// constructor instead.
	auto GetConfig() -> Config & {
		return config;
	}
	/// Const overload of the above: a const config reads settings, but does not write them.
	auto GetConfig() const -> const Config & {
		return config;
	}

	/// Opens a new session on this database, starting the instance if this is its first use.
	/// @return An owning `Connection`, which disconnects when destroyed. Open one per thread.
	auto Connect() -> Connection;

	/// The factory for the built-in types, their values and casts, and data chunks on this instance. Anything that
	/// needs a catalog throws; use a connection's or a context's factory for that.
	auto GetFactory() -> Factory & {
		return factory;
	}

	/// Registers a replacement scan on this database, visible to every connection. Not thread-safe against queries
	/// binding on other connections: register before issuing queries.
	/// Registers the scan. Scans are consulted in registration order within
	/// their scope, connection-scoped ones before database-wide ones, and the first to claim a name wins. A scan can
	/// be registered only once.
	/// @throws InvalidInputException When the callback is missing, or the scan is already registered.
	auto Register(ReplacementScan &replacement_scan) -> void;
	/// Registers the log sink, making it available to the logging configuration under its name. Not transactional;
	/// the sink lives as long as the database. User data set via `SetUserData` is consumed by registration.
	/// @throws InvalidInputException When the name or write callback is missing, or the name is taken, by a sink
	/// DuckDB provides itself or by one registered before.
	auto Register(LogSink &log_sink) -> void;

	/// The file system of this database, with its GLOBAL settings and secrets. Borrowed, and valid only while the
	/// instance is.
	auto GetFileSystem() const -> FileSystem;

	/// Writes a message to DuckDB's log, readable through `SELECT * FROM duckdb_logs`.
	/// Whether the entry is recorded is up to the database's log configuration; a message it filters out is dropped
	/// without error.
	/// @param level The severity of the message.
	/// @param message The message body.
	/// @param log_type The log type to record under, matched case-sensitively. Empty selects the default type.
	auto Log(LogLevel level, std::string_view message, std::string_view log_type = {}) const -> void;

private:
	explicit Instance(void *impl);
	//! The (name, value) pairs of the options that were set, in the textual form SQL `SET` accepts.
	static auto StartupOptionPairs(const StartupOptions &options) -> std::vector<std::pair<std::string, std::string>>;

	Factory factory;
	Config config;
};

//----------------------------------------------------------------------------------------------------------------------
// Extension
//----------------------------------------------------------------------------------------------------------------------
// Loading an extension is how catalog entries (functions, types, casts) and database-level hooks get installed under
// the extension's identity, so DuckDB can attribute them to the extension that provided them. Outside a load, the same
// objects are registered on a `Connection` or an `Instance` instead.

/// The extension being loaded, handed to its load entry point.
/// Borrowed for the duration of the load: never store or outlive one.
class Extension final : public detail::Handle<Extension> {
	friend detail::HandleFactory;

public:
	~Extension() override;

	/// Registers on the loading extension's database, attributed to the extension and visible to every connection.
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, exec callback, or a usable return type is missing.
	auto Register(ScalarFunction &scalar_function) -> void;
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, a required callback, or a usable return type is missing.
	auto Register(AggregateFunction &aggregate_function) -> void;
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, bind callback or exec callback is missing, the signature
	/// declares a return type, or a partitioning callback is set without a partition data callback.
	auto Register(TableFunction &table_function) -> void;
	/// Registers the function. The function object remains valid.
	/// @throws InvalidInputException When the name or the single-file function is missing, or the single-file
	/// function does not exist or does not take the file to read as its only positional parameter.
	auto Register(MultiFileFunction &multi_file_function) -> void;
	/// Registers the type. The type object remains valid and may be adjusted
	/// and registered again.
	/// @throws InvalidInputException When the name or the base type is missing, or the base type is not concrete.
	auto Register(CustomType &custom_type) -> void;
	/// Registers the function. The function object remains valid and may be
	/// adjusted and registered again; user data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name is missing, neither side is configured, a configured
	/// `COPY ... TO` side lacks its batch or flush callback, or a configured `COPY ... FROM` side lacks its bind or
	/// exec callback.
	auto Register(CopyFunction &copy_function) -> void;
	/// Registers the cast, replacing whatever cast was registered for the same type pair. The function object remains
	/// valid and may be adjusted and registered again; user data set via `SetUserData` is consumed by the first
	/// registration.
	/// @throws InvalidInputException When the source type, target type, or exec callback is missing, or either type is
	/// not concrete.
	auto Register(CastFunction &cast_function) -> void;
	/// Registers the pragma. The function object remains valid; user data set via `SetUserData` is consumed by the
	/// first registration.
	/// @throws InvalidInputException When the name is missing, not exactly one of the query and exec callbacks is set,
	/// the signature declares a return type, or a pragma of the same name already exists.
	auto Register(PragmaFunction &pragma_function) -> void;
	/// Registers the macro function. The function object remains valid.
	/// @throws InvalidInputException When the name or the body is missing, the body does not parse, the signature has
	/// a parameter that is not a standard one or declares a return type, or a function of the same name exists.
	auto Register(MacroFunction &macro_function) -> void;
	/// Registers the option on the database, visible to every connection. Not transactional: it takes effect at once,
	/// also inside an explicit transaction, and a rollback does not undo it. The option object remains valid; user
	/// data set via `SetUserData` is consumed by the first registration.
	/// @throws InvalidInputException When the name, or both the type and the default value, are missing, the default
	/// value does not cast to the type, the scopes are inconsistent, or an option of the same name already exists.
	auto Register(CustomOption &custom_option) -> void;
	/// Registers the scan. Scans are consulted in registration order within
	/// their scope, connection-scoped ones before database-wide ones, and the first to claim a name wins. A scan can
	/// be registered only once.
	/// @throws InvalidInputException When the callback is missing, or the scan is already registered.
	auto Register(ReplacementScan &replacement_scan) -> void;
	/// Registers the log sink, making it available to the logging configuration under its name. Not transactional;
	/// the sink lives as long as the database. User data set via `SetUserData` is consumed by registration.
	/// @throws InvalidInputException When the name or write callback is missing, or the name is taken, by a sink
	/// DuckDB provides itself or by one registered before.
	auto Register(LogSink &log_sink) -> void;

private:
	explicit Extension(void *impl);
};

namespace detail {
/// Backs `DUCKDB_CPP_EXTENSION_ENTRYPOINT` in duckdb_cpp_extension.hpp: wraps the raw loader handles and runs the
/// extension's body, reporting a thrown `Exception` through the error slot so it fails the load. The handles are
/// passed untyped to keep the C loader types out of this header.
auto RunExtensionEntry(void (*body)(Extension &, Context &), void *extension, void *context, void *err) -> void;
} // namespace detail

/// The version of the DuckDB library this program is linked against, e.g. "v1.5.0", with a suffix such as
/// "v1.5.0-dev123" on development builds.
auto LibraryVersion() -> std::string;

/// Renders a name as a SQL identifier, quoting and escaping only when required: the name itself when it is a legal
/// bare identifier, or double-quoted with interior quotes doubled when it is a keyword or contains characters that
/// require quoting. Use it for every name embedded in SQL text rather than quoting by hand.
/// @param name The name to render.
auto RenderQuotedIdentifier(std::string_view name) -> std::string;

//----------------------------------------------------------------------------------------------------------------------
// Logical Type
//----------------------------------------------------------------------------------------------------------------------
// The SQL type of a column, value or parameter. Types are built through a `Context` or `Connection`: by parsing SQL
// text, by name and parameters, or with a `TypeBuilder`. Once built, a `LogicalType` is an owned, self-contained value
// that can be inspected without any scope.

/// The "id" of a built-in SQL type.
/// Parameterized kinds such as LIST and DECIMAL are represented by their id plus parameters, the id alone does not name
/// a complete type.
enum class LogicalTypeId : uint32_t {
	INVALID = 0,
	SQLNULL = 1,
	UNKNOWN = 2,
	ANY = 3,
	TYPE = 6,
	BOOLEAN = 10,
	TINYINT = 11,
	SMALLINT = 12,
	INTEGER = 13,
	BIGINT = 14,
	DATE = 15,
	TIME = 16,
	TIMESTAMP_SEC = 17,
	TIMESTAMP_MS = 18,
	TIMESTAMP = 19,
	TIMESTAMP_NS = 20,
	DECIMAL = 21,
	FLOAT = 22,
	DOUBLE = 23,
	VARCHAR = 25,
	BLOB = 26,
	INTERVAL = 27,
	UTINYINT = 28,
	USMALLINT = 29,
	UINTEGER = 30,
	UBIGINT = 31,
	TIMESTAMP_TZ = 32,
	TIMESTAMP_TZ_NS = 33,
	TIME_TZ = 34,
	TIME_NS = 35,
	BIT = 36,
	BIGNUM = 39,
	UHUGEINT = 49,
	HUGEINT = 50,
	UUID = 54,
	GEOMETRY = 60,
	STRUCT = 100,
	LIST = 101,
	MAP = 102,
	ENUM = 104,
	UNION = 107,
	ARRAY = 108,
	VARIANT = 109,
	TUPLE = 110,
};

/// An owned SQL type: a kind plus its parameters, e.g. DECIMAL(18, 3) or STRUCT(a INTEGER, b VARCHAR), and in the case
/// of extension-defined types, its "alias"
class LogicalType final : public detail::Handle<LogicalType> {
	friend detail::HandleFactory;

public:
	LogicalType(LogicalType &&) noexcept = default;
	LogicalType &operator=(LogicalType &&) noexcept = default;

	~LogicalType() override;

	/// A copy of this type carrying `alias` as its name: the same representation under a distinct identity.
	/// The alias is by default not registered anywhere; parsing or creating a type with this name does not resolve to
	/// this type, unless it is explicitly registered in the catalog separately.
	/// @param factory The factory to create the copy with.
	/// @param alias The name the copy carries. Must not be empty.
	auto WithAlias(Factory &factory, std::string_view alias) const -> LogicalType;

	/// An owned copy of this type.
	auto Copy() const -> LogicalType;

	/// The kind of this type, e.g. `LogicalTypeId::DECIMAL` for any DECIMAL regardless of its parameters.
	auto GetTypeId() const -> LogicalTypeId;

	/// The type's name: its alias when it has one, otherwise the fixed name of its kind. Never empty.
	/// @return A view borrowed from this type, valid until it is destroyed.
	auto GetName() const -> std::string_view;

	/// Whether both types are the same type, parameters and alias included.
	bool operator==(const LogicalType &other) const;

	bool operator!=(const LogicalType &other) const {
		return !(*this == other);
	}

	/// Renders the type as SQL text. An aliased type is rendered as its alias.
	auto ToText() const -> std::string;

	/// How many parameters the type carries:
	/// - DECIMAL 2 (width, scale)
	/// - LIST 1 (element type)
	/// - ARRAY 2 (element type, size)
	/// - MAP 2 (key, value types)
	/// - STRUCT and TUPLE one per field
	/// - UNION one per member
	/// - ENUM one per dictionary entry;
	/// - VARCHAR 1 when a collation is set;
	/// - GEOMETRY 1 when a coordinate system is set;
	/// anything else 0.
	auto GetParamCount() const -> idx_t;

	/// Get one of the type's parameters. The parameter's name is empty when it is positional, otherwise it is the name
	/// of the field or member it describes.
	/// @param index Parameter index in [0, GetParamCount()).
	/// @return An owned name and an owned value. Child types arrive as TYPE values.
	auto GetParam(idx_t index) const -> TypeParam;

	// Per-kind shorthands over `GetParam` / `GetParamCount`. They do not verify the type's kind: on a type of another
	// kind they misread that type's parameters or throw, so check the kind first. Names and dictionary entries come
	// back as owned strings.

	/// The total number of digits a DECIMAL holds.
	auto GetDecimalWidth() const -> uint8_t;

	/// The number of digits a DECIMAL keeps after the point.
	auto GetDecimalScale() const -> uint8_t;

	/// How many entries an ENUM's dictionary holds.
	auto GetEnumSize() const -> idx_t;

	/// One ENUM dictionary entry.
	/// @param index Entry index in [0, GetEnumSize()).
	auto GetEnumValue(idx_t index) const -> std::string;

	/// The element type of a LIST.
	auto GetListChildType() const -> LogicalType;

	/// The element type of an ARRAY.
	auto GetArrayChildType() const -> LogicalType;

	/// How many elements every row of an ARRAY holds.
	auto GetArraySize() const -> idx_t;

	/// The key type of a MAP.
	auto GetMapKeyType() const -> LogicalType;

	/// The value type of a MAP.
	auto GetMapValueType() const -> LogicalType;

	/// How many fields a STRUCT has.
	auto GetStructChildCount() const -> idx_t;

	/// The name of one STRUCT field, empty for the fields of an unnamed STRUCT.
	/// @param index Field index in [0, GetStructChildCount()).
	auto GetStructChildName(idx_t index) const -> std::string;

	/// The type of one STRUCT field.
	/// @param index Field index in [0, GetStructChildCount()).
	auto GetStructChildType(idx_t index) const -> LogicalType;

	/// How many members a UNION has.
	auto GetUnionMemberCount() const -> idx_t;

	/// The tag of one UNION member.
	/// @param index Member index in [0, GetUnionMemberCount()).
	auto GetUnionMemberName(idx_t index) const -> std::string;

	/// The type of one UNION member.
	/// @param index Member index in [0, GetUnionMemberCount()).
	auto GetUnionMemberType(idx_t index) const -> LogicalType;

	/// The integer type a DECIMAL's digits are stored in, which is what its vector elements are; determined by the
	/// width.
	auto GetDecimalInternalTypeId() const -> LogicalTypeId;

	/// The unsigned integer type an ENUM's dictionary indices are stored in, which is what its vector elements are;
	/// determined by the dictionary size.
	auto GetEnumInternalTypeId() const -> LogicalTypeId;

private:
	explicit LogicalType(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Schema
//----------------------------------------------------------------------------------------------------------------------
// The shape of a row: which fields it has, in order, and of what type. Used both for what a statement produces and
// for what it expects.

/// An ordered list of (name, type) fields. Names may repeat, and a schema may have no fields at all.
class Schema final : public detail::Handle<Schema> {
	friend detail::HandleFactory;

public:
	Schema(Schema &&) noexcept = default;
	Schema &operator=(Schema &&) noexcept = default;

	~Schema() override;

	/// How many fields the schema has.
	auto GetFieldCount() const -> idx_t;

	/// The name of one field.
	/// @param index Field index in [0, GetFieldCount()).
	/// @return A view borrowed from this schema, valid until it is destroyed.
	auto GetFieldName(idx_t index) const -> std::string_view;

	/// An owned copy of one field's type.
	/// @param index Field index in [0, GetFieldCount()).
	auto GetFieldType(idx_t index) const -> LogicalType;

private:
	explicit Schema(void *impl);
};

/// What `Connection::Bind` learns about a statement without running it.
class Signature {
public:
	/// The columns the statement will produce.
	Schema output;
	/// The statement's parameters, ordered by binding key. A field's name is the key: "1", "2", ... for positional
	/// parameters, the name for named ones.
	Schema parameters;
};

//----------------------------------------------------------------------------------------------------------------------
// Primitive data types
//----------------------------------------------------------------------------------------------------------------------
// One C++ type per SQL type, laid out exactly as it appears as an element of a vector. These are what
// `VectorView::Data<T>` reads arrays of, and what `Value::Get<T>` and `Value::Create` are keyed on, so the type picked
// here also selects the SQL type read or written.
//
// A few of them store their value in an encoded form. Those carry `Decode` / `Encode` or named accessors, and the raw
// field is not the value you want.
//
// DATE and the TIMESTAMP types reserve their most extreme values as +/- infinity sentinels.

/// HUGEINT: a 128-bit signed integer, in two halves.
struct int128_t {
	/// The low 64 bits.
	uint64_t lower;
	/// The high 64 bits, carrying the sign.
	int64_t upper;
};

/// UHUGEINT: a 128-bit unsigned integer, in two halves.
struct uint128_t {
	/// The low 64 bits.
	uint64_t lower;
	/// The high 64 bits.
	uint64_t upper;
};

/// DATE: days since 1970-01-01.
struct date_t {
	int32_t days = 0;
};

/// TIME: microseconds since midnight.
struct dtime_t {
	int64_t micros = 0;
};

/// TIME_NS: nanoseconds since midnight.
struct dtime_ns_t {
	int64_t nanos = 0;
};

/// TIME_TZ: a time of day together with its UTC offset, packed into one integer -- time in the high 40 bits, the
/// offset reverse-ordered in the low 24. Read it through the accessors; a plain shift gives neither.
struct dtime_tz_t {
	static constexpr int OFFSET_BITS = 24;
	static constexpr uint64_t OFFSET_MASK = (uint64_t(1) << OFFSET_BITS) - 1;
	static constexpr int32_t MAX_OFFSET = 16 * 60 * 60 - 1; // +/-15:59:59

	dtime_tz_t() = default;

	/// Wraps the packed form, as it appears as a vector element.
	explicit dtime_tz_t(uint64_t bits) : bits(bits) {
	}

	/// Packs a time and an offset.
	/// @param micros Microseconds since midnight.
	/// @param offset_seconds The UTC offset in seconds, positive east, in [-MAX_OFFSET, MAX_OFFSET].
	dtime_tz_t(int64_t micros, int32_t offset_seconds)
	    : bits((static_cast<uint64_t>(micros) << OFFSET_BITS) |
	           (static_cast<uint64_t>(MAX_OFFSET - offset_seconds) & OFFSET_MASK)) {
	}

	/// Microseconds since midnight.
	auto GetMicros() const -> int64_t {
		return static_cast<int64_t>(bits >> OFFSET_BITS);
	}

	/// The UTC offset in seconds, positive east.
	auto GetOffset() const -> int32_t {
		return MAX_OFFSET - static_cast<int32_t>(bits & OFFSET_MASK);
	}

	/// The packed form, as it appears as a vector element.
	auto GetBits() const -> uint64_t {
		return bits;
	}

private:
	uint64_t bits = 0;
};

/// TIMESTAMP: microseconds since 1970-01-01 00:00:00.
struct timestamp_t {
	int64_t micros = 0;
};

/// TIMESTAMP_SEC: seconds since 1970-01-01 00:00:00.
struct timestamp_s_t {
	int64_t seconds = 0;
};

/// TIMESTAMP_MS: milliseconds since 1970-01-01 00:00:00.
struct timestamp_ms_t {
	int64_t millis = 0;
};

/// TIMESTAMP_NS: nanoseconds since 1970-01-01 00:00:00.
struct timestamp_ns_t {
	int64_t nanos = 0;
};

/// TIMESTAMP_TZ: microseconds since the epoch, in UTC. The display time zone is a session setting, not part of the
/// value.
struct timestamp_tz_t {
	int64_t micros = 0;
};

/// TIMESTAMP_TZ_NS: nanoseconds since the epoch, in UTC.
struct timestamp_tz_ns_t {
	int64_t nanos = 0;
};

/// INTERVAL: a span of time, kept in three separate units because months and days are not fixed lengths of time.
struct interval_t {
	int32_t months;
	int32_t days;
	int64_t micros;
};

/// The element of a LIST vector: where a row's elements are in the child vector, rather than the elements themselves.
struct list_entry_t {
	/// Index of the row's first element in the child vector.
	uint64_t offset = 0;
	/// How many elements the row has.
	uint64_t length = 0;
};

/// DECIMAL(WIDTH, SCALE): an exact number, kept as an integer of WIDTH digits with the point SCALE digits from the
/// right. The width picks the storage integer, so DECIMAL(9, 2) and DECIMAL(18, 2) are different C++ types and their
/// payloads are not interchangeable.
template <int8_t WIDTH, uint8_t SCALE>
struct decimal_t {
	static_assert(WIDTH >= 1 && WIDTH <= 38, "DECIMAL type width must be between 1 and 38");
	static_assert(SCALE <= WIDTH, "DECIMAL scale must be less than or equal to width");
	using storage_type = std::conditional_t<
	    (WIDTH <= 4), int16_t,
	    std::conditional_t<(WIDTH <= 9), int32_t, std::conditional_t<(WIDTH <= 18), int64_t, int128_t>>>;

	storage_type value;
};

/// BLOB: a string of bytes, in the form it takes as a vector element. Up to INLINE_LENGTH bytes live in the element
/// itself; longer ones live elsewhere and the element only points at them.
///
/// A blob never owns its bytes. Constructing one from a `std::string_view` borrows that memory rather than copying it,
/// so a long blob is only valid while whatever holds the bytes is: use `Arena::AddBlob` / `Vector::AssignString` to
/// put bytes somewhere that lives as long as the vector.
struct blob_t {
	/// The longest byte string that fits in a vector element without being stored elsewhere.
	static constexpr uint32_t INLINE_LENGTH = 12;
	/// How many leading bytes a non-inlined blob keeps alongside the pointer, for comparisons.
	static constexpr uint32_t PREFIX_LENGTH = 4;

	union {
		struct {
			uint32_t length;
			char prefix[PREFIX_LENGTH];
			char *ptr;
		} pointer;
		struct {
			uint32_t length;
			char inlined[INLINE_LENGTH];
		} inlined;
	} value;

	/// An empty blob.
	blob_t() {
		std::memset(&value, 0, sizeof(value));
	}

	/// Borrows the bytes of `str`, which must outlive the blob unless they fit inline.
	// NOLINTNEXTLINE
	blob_t(std::string_view str) : blob_t(str.data(), static_cast<uint32_t>(str.size())) {
	}

	/// Borrows `size` bytes at `data`, which must outlive the blob unless they fit inline.
	blob_t(const char *data, uint32_t size) {
		if (!data || size == 0) {
			std::memset(&value, 0, sizeof(value));
			value.inlined.length = size;
			return;
		}
		if (size <= INLINE_LENGTH) {
			std::memset(&value, 0, sizeof(value));
			value.inlined.length = size;
			std::memcpy(value.inlined.inlined, data, size);
		} else {
			std::memset(&value, 0, sizeof(value));
			value.pointer.length = size;
			value.pointer.ptr = const_cast<char *>(data); // NOLINT
			std::memcpy(value.pointer.prefix, data, PREFIX_LENGTH);
		}
	}

	explicit operator std::string_view() const {
		return std::string_view(data(), size());
	}

	explicit operator std::string() const {
		return std::string(data(), size());
	}

	/// Whether the bytes live in the value itself rather than behind the pointer.
	auto is_inlined() const -> bool {
		return value.inlined.length <= INLINE_LENGTH;
	}

	/// The number of bytes.
	auto size() const -> uint32_t {
		return value.inlined.length;
	}

	/// The bytes, wherever they live. Not null-terminated.
	auto data() const -> const char * {
		return is_inlined() ? value.inlined.inlined : value.pointer.ptr;
	}

	/// Mutable access to the bytes. Editing a non-inlined blob's bytes leaves its cached prefix stale, so prefer
	/// building a new value over patching one in place.
	auto data_mut() -> char * {
		return is_inlined() ? value.inlined.inlined : value.pointer.ptr;
	}

	/// The bytes as a view, valid as long as the blob and whatever holds its bytes are.
	auto view() const -> std::string_view {
		return std::string_view(data(), size());
	}
};

/// Validates a byte string, throwing InvalidInputException for malformed UTF-8.
void ValidateUTF8(std::string_view text);

/// VARCHAR: like `blob_t`, but naming a string of UTF-8 text rather than of arbitrary bytes.
/// Constructing a `varchar_t` does not validate UTF-8; use `Arena::AddString` or `Vector::AssignString` for checked
/// construction.
struct varchar_t : blob_t {
	using blob_t::blob_t;
};

/// BIT: a string of bits, stored as a padding-count byte followed by the data bytes. The count says how many leading
/// bits of the first data byte are not part of the bit string, and those padding bits are set to 1. The raw bytes are
/// not the bits: read them through the accessors, and note that the constructors inherited from `blob_t` take these
/// encoded bytes, not a bit string.
struct bit_t : blob_t {
	using blob_t::blob_t;

	/// How many leading bits of the first data byte are padding.
	auto GetPaddingBits() const -> uint8_t {
		return size() > 0 ? static_cast<uint8_t>(data()[0]) : 0;
	}

	/// The data bytes, past the padding-count byte.
	auto GetBits() const -> const char * {
		return size() > 0 ? data() + 1 : data();
	}

	/// How many data bytes there are.
	auto GetBitsSize() const -> uint32_t {
		return size() > 0 ? size() - 1 : 0;
	}

	/// How many bits the string actually holds, padding excluded.
	auto GetBitCount() const -> uint64_t {
		return GetBitsSize() == 0 ? 0 : static_cast<uint64_t>(GetBitsSize()) * 8 - GetPaddingBits();
	}
};

/// BIGNUM: an arbitrary-precision integer. Its bytes are encoded so that comparing them matches numeric order, which
/// among other things bit-inverts negative values, so they are not the magnitude. `Decode` and `Encode` translate; the
/// constructors inherited from `blob_t` take the encoded bytes.
struct bignum_t : blob_t {
	using blob_t::blob_t;

	/// A BIGNUM as a number: the integer is (-1)^is_negative * unsigned_big_endian(magnitude). Owns its bytes.
	struct Decoded {
		/// The magnitude, big-endian, without leading zeroes; zero is a single 0x00.
		std::vector<uint8_t> magnitude;
		/// Whether the integer is negative.
		bool is_negative;
	};

	/// The magnitude and sign this value stands for.
	auto Decode() const -> Decoded;

	/// The inverse: the bytes to build a BIGNUM value from.
	/// @param value The magnitude and sign, with the magnitude canonical: at least one byte, no leading zeroes, and
	/// zero written as a single 0x00 with `is_negative` false.
	static auto Encode(const Decoded &value) -> std::vector<uint8_t>;
};

/// UUID: a 128-bit identifier, stored as an integer with its high bit flipped so that comparing the integers matches
/// UUID order. The canonical bytes only come out through `Decode`. Distinct from `int128_t`, which names HUGEINT.
struct uuid_t {
	int128_t value {};

	uuid_t() = default;

	/// Wraps the stored form, as it appears as a vector element.
	explicit uuid_t(int128_t storage) : value(storage) {
	}

	/// A UUID as its canonical 16 big-endian bytes.
	struct Decoded {
		uint8_t bytes[16];
	};

	/// The canonical 16 big-endian bytes.
	auto Decode() const -> Decoded {
		const uint64_t upper = static_cast<uint64_t>(value.upper) ^ (static_cast<uint64_t>(1) << 63);
		Decoded out {};
		for (int i = 0; i < 8; i++) {
			out.bytes[i] = static_cast<uint8_t>((upper >> (56 - 8 * i)) & 0xFF);
			out.bytes[8 + i] = static_cast<uint8_t>((value.lower >> (56 - 8 * i)) & 0xFF);
		}
		return out;
	}

	/// The inverse: the stored form for a set of canonical bytes.
	/// @param bytes The UUID's 16 big-endian bytes.
	static auto Encode(const Decoded &bytes) -> uuid_t {
		uint64_t upper = 0;
		uint64_t lower = 0;
		for (int i = 0; i < 8; i++) {
			upper = (upper << 8) | bytes.bytes[i];
			lower = (lower << 8) | bytes.bytes[8 + i];
		}
		return uuid_t(int128_t {lower, static_cast<int64_t>(upper ^ (static_cast<uint64_t>(1) << 63))});
	}
};

//----------------------------------------------------------------------------------------------------------------------
// Value
//----------------------------------------------------------------------------------------------------------------------
// A single SQL value, type included: what a statement parameter binds to, what a type parameter carries, and the way
// to read or write one cell of a vector without caring how it is represented. Values are owned and self-contained;
// creating one is scoped to a `Context` or `Connection`.
//
// For bulk data, go through `Vector` instead: a value costs an allocation, so it is the wrong tool per row.

/// An owned SQL value.
class Value final : public detail::Handle<Value> {
	friend detail::HandleFactory;

public:
	~Value() override;

	Value(Value &&) noexcept = default;
	Value &operator=(Value &&) noexcept = default;

	/// Whether this is SQL NULL. A NULL value still has a type.
	auto IsNull() const -> bool;

	/// An owned copy of the value's type.
	auto GetLogicalType() const -> LogicalType;

	/// The value rendered the way DuckDB prints it, e.g. in query output.
	auto ToText() const -> std::string;

	/// Casts the value to another type, following the same rules as a SQL cast.
	/// @param factory The factory supplying the casts.
	/// @param target The type to cast to.
	/// @return The converted value. Throws when the cast is not allowed or the value does not fit.
	auto Cast(Factory &factory, const LogicalType &target) const -> Value;

	/// Reads the value as `T`, where `T` is one of the primitive types above, or `LogicalType` for a TYPE value.
	/// Numeric, temporal and boolean values of another type are converted, following cast rules; the remaining `T`s
	/// require a value of the matching type. Types with no specialization here do not compile.
	/// @return The value. For `varchar_t` / `blob_t` / `bit_t` / `bignum_t` the bytes are borrowed from this value and
	/// stay valid only as long as it does.
	/// @throws Exception When the value is NULL, or when it cannot be read as `T`.
	template <class T>
	auto Get() const -> T = delete;

	/// Reads the value as a DECIMAL, e.g. `value.Get<18, 3>()`, spelled with the width and scale instead of a type.
	/// @return The value.
	/// @throws InvalidInputException Unless the value is a DECIMAL of exactly this width and scale; payloads are not
	/// interchangeable between widths.
	template <int8_t WIDTH, uint8_t SCALE>
	auto Get() const -> decimal_t<WIDTH, SCALE> {
		int128_t value {};
		GetDecimal(value, WIDTH, SCALE);
		if constexpr (std::is_same_v<typename decimal_t<WIDTH, SCALE>::storage_type, int128_t>) {
			return decimal_t<WIDTH, SCALE> {value};
		} else {
			return decimal_t<WIDTH, SCALE> {static_cast<typename decimal_t<WIDTH, SCALE>::storage_type>(value.lower)};
		}
	}

	/// A NULL of the given type.
	/// @param factory The factory to create the value with.
	/// @param type The type the NULL carries.
	static auto CreateNull(Factory &factory, const LogicalType &type) -> Value;

	/// Creates a value from a C++ value. The overload picked decides the SQL type, so `dtime_t` yields TIME and
	/// `dtime_ns_t` yields TIME_NS; a `LogicalType` yields a TYPE value. Types with no overload here do not compile --
	/// cast or build them through the composite constructors instead. Byte strings are copied in, so the value does not
	/// borrow from the `varchar_t` / `blob_t` handed to it.
	/// @param factory The factory to create the value with.
	/// @param value The C++ value to convert.

	static auto Create(Factory &factory, bool value) -> Value;
	static auto Create(Factory &factory, uint8_t value) -> Value;
	static auto Create(Factory &factory, uint16_t value) -> Value;
	static auto Create(Factory &factory, uint32_t value) -> Value;
	static auto Create(Factory &factory, uint64_t value) -> Value;
	static auto Create(Factory &factory, uint128_t value) -> Value;
	static auto Create(Factory &factory, int8_t value) -> Value;
	static auto Create(Factory &factory, int16_t value) -> Value;
	static auto Create(Factory &factory, int32_t value) -> Value;
	static auto Create(Factory &factory, int64_t value) -> Value;
	static auto Create(Factory &factory, int128_t value) -> Value;
	static auto Create(Factory &factory, float value) -> Value;
	static auto Create(Factory &factory, double value) -> Value;
	static auto Create(Factory &factory, varchar_t value) -> Value;
	static auto Create(Factory &factory, blob_t value) -> Value;
	static auto Create(Factory &factory, const LogicalType &type) -> Value;
	static auto Create(Factory &factory, date_t value) -> Value;
	static auto Create(Factory &factory, dtime_t value) -> Value;
	static auto Create(Factory &factory, dtime_ns_t value) -> Value;
	static auto Create(Factory &factory, dtime_tz_t value) -> Value;
	static auto Create(Factory &factory, timestamp_t value) -> Value;
	static auto Create(Factory &factory, timestamp_s_t value) -> Value;
	static auto Create(Factory &factory, timestamp_ms_t value) -> Value;
	static auto Create(Factory &factory, timestamp_ns_t value) -> Value;
	static auto Create(Factory &factory, timestamp_tz_t value) -> Value;
	static auto Create(Factory &factory, timestamp_tz_ns_t value) -> Value;
	static auto Create(Factory &factory, interval_t value) -> Value;

	template <int8_t WIDTH, uint8_t SCALE>
	static auto Create(Factory &factory, decimal_t<WIDTH, SCALE> value) -> Value {
		return CreateDecimal(factory, WidenDecimal(value.value), WIDTH, SCALE);
	}

	static auto Create(Factory &factory, bit_t value) -> Value;
	static auto Create(Factory &factory, bignum_t value) -> Value;
	static auto Create(Factory &factory, uuid_t value) -> Value;
	template <class T>
	static auto Create(Factory &factory, T value) -> Value = delete;

	// Composite construction. Each constructor infers the composite's type from the children it is given, which is why
	// only the built-in composites are reachable this way: build an aliased or extension-registered composite by
	// casting one of these to it.
	//
	// Children are borrowed for the duration of the call and copied into the result, so the caller keeps its inputs
	// and nothing in the result points back at them.
	using ValueList = const std::vector<Value> &;
	using NamedValueList = const std::vector<std::pair<std::string, Value>> &;
	using KeyValueList = const std::vector<std::pair<Value, Value>> &;

	/// A LIST of the given elements.
	/// @param factory The factory to create the value with.
	/// @param values The elements. The element type is the common type of all of them and each is cast to it, so
	/// mixing INTEGER and VARCHAR yields VARCHAR elements. Must not be empty: with no element there is no type to
	/// infer, so use the child-type overload for an empty LIST.
	/// @throws Exception When the elements have no common type.
	static auto CreateList(Factory &factory, ValueList values) -> Value;

	/// An empty LIST.
	/// @param factory The factory to create the value with.
	/// @param child_type The element type, not the LIST type.
	static auto CreateList(Factory &factory, const LogicalType &child_type) -> Value;

	/// An ARRAY of the given elements, its size being how many there are.
	/// @param factory The factory to create the value with.
	/// @param values The elements, typed as in `CreateList`. Must not be empty: the smallest ARRAY holds one element.
	static auto CreateArray(Factory &factory, ValueList values) -> Value;

	/// A TUPLE, i.e. a struct whose fields have no names.
	/// @param factory The factory to create the value with.
	/// @param values The fields, in order. May be empty: the empty tuple is a type of its own.
	static auto CreateTuple(Factory &factory, ValueList values = {}) -> Value;

	/// A STRUCT of the given fields.
	/// @param factory The factory to create the value with.
	/// @param values The (name, value) fields, in order. Names should be unique and either all set or all empty; this
	/// is not validated. May be empty: the empty struct is a type of its own.
	static auto CreateStruct(Factory &factory, NamedValueList values = {}) -> Value;

	/// A MAP of the given entries.
	/// @param factory The factory to create the value with.
	/// @param values The (key, value) entries. The key and value types are the common types over all entries, and each
	/// entry is cast to them, as in `CreateList`. Keys must be unique and not NULL. Must not be empty: with no entry
	/// there are no types to infer, so use the key/value-type overload for an empty MAP.
	static auto CreateMap(Factory &factory, KeyValueList values) -> Value;

	/// An empty MAP.
	/// @param factory The factory to create the value with.
	/// @param key_type The key type, not the MAP type.
	/// @param value_type The value type, not the MAP type.
	static auto CreateMap(Factory &factory, const LogicalType &key_type, const LogicalType &value_type) -> Value;

	/// How many children a composite value has: elements for LIST and ARRAY, fields for STRUCT and TUPLE, two per
	/// entry for MAP, 2 for UNION, and 0 for anything else. A NULL value has no children.
	auto GetChildCount() const -> idx_t;

	/// An owned copy of one child. A MAP's children alternate key and value, so entry `i` is children 2*i and 2*i+1; a
	/// UNION's children are its tag, then its active member.
	/// @param index Child index in [0, GetChildCount()).
	auto GetChild(idx_t index) const -> Value;

	/// Shorthand for `GetChild`.
	auto operator[](idx_t index) const -> Value {
		return GetChild(index);
	}

private:
	/// @internal Reads the backing integer, throwing unless the value's own width and scale are the ones asked for.
	void GetDecimal(int128_t &out, uint8_t width, uint8_t scale) const;

	/// @internal The runtime forwarder behind the templated DECIMAL constructors, so those can be defined in the header
	/// without naming a C type.
	static auto CreateDecimal(Factory &factory, int128_t value, uint8_t width, uint8_t scale) -> Value;

	/// @internal Sign-extends a DECIMAL's backing integer to the widest storage tier, so one entry point can carry
	/// every tier.
	static auto WidenDecimal(int64_t value) -> int128_t {
		return int128_t {static_cast<uint64_t>(value), value < 0 ? -1 : 0};
	}
	static auto WidenDecimal(int128_t value) -> int128_t {
		return value;
	}

	explicit Value(void *impl);
};

template <>
auto Value::Get() const -> bool;
template <>
auto Value::Get() const -> uint8_t;
template <>
auto Value::Get() const -> uint16_t;
template <>
auto Value::Get() const -> uint32_t;
template <>
auto Value::Get() const -> uint64_t;
template <>
auto Value::Get() const -> uint128_t;
template <>
auto Value::Get() const -> int8_t;
template <>
auto Value::Get() const -> int16_t;
template <>
auto Value::Get() const -> int32_t;
template <>
auto Value::Get() const -> int64_t;
template <>
auto Value::Get() const -> int128_t;
template <>
auto Value::Get() const -> float;
template <>
auto Value::Get() const -> double;
template <>
auto Value::Get() const -> varchar_t;
template <>
auto Value::Get() const -> blob_t;
template <>
auto Value::Get() const -> date_t;
template <>
auto Value::Get() const -> dtime_t;
template <>
auto Value::Get() const -> dtime_ns_t;
template <>
auto Value::Get() const -> dtime_tz_t;
template <>
auto Value::Get() const -> timestamp_t;
template <>
auto Value::Get() const -> timestamp_s_t;
template <>
auto Value::Get() const -> timestamp_ms_t;
template <>
auto Value::Get() const -> timestamp_ns_t;
template <>
auto Value::Get() const -> timestamp_tz_t;
template <>
auto Value::Get() const -> timestamp_tz_ns_t;
template <>
auto Value::Get() const -> interval_t;
template <>
auto Value::Get() const -> bit_t;
template <>
auto Value::Get() const -> bignum_t;
template <>
auto Value::Get() const -> uuid_t;
template <>
auto Value::Get() const -> LogicalType;

template <class T>
auto Factory::CreateValue(T &&value) -> Value {
	return Value::Create(*this, std::forward<T>(value));
}

/// The current value of a setting, and the scope it was read from. Destructures:
/// `auto [value, scope] = config.GetOption(name)`.
struct OptionValue {
	/// The value, typed as the setting.
	Value value;
	/// GLOBAL or SESSION, as DuckDB attributes the value.
	SettingScope scope;
};

/// One parameter of a type: a value, plus a name when the parameter is a named one. A parameter with an empty name is
/// positional.
struct TypeParam {
	/// A named parameter.
	/// @param name The parameter's name.
	/// @param value The parameter's value; moved in.
	TypeParam(std::string_view name, Value value) : name(name), value(std::move(value)) {
	}

	/// A positional parameter.
	/// @param value The parameter's value; moved in.
	explicit TypeParam(Value value) : name(""), value(std::move(value)) {
	}

	/// The parameter's name, empty when it is positional.
	auto GetName() const -> const std::string & {
		return name;
	}

	/// The parameter's value.
	auto GetValue() const -> const Value & {
		return value;
	}

	/// The parameter's value, mutably.
	auto GetValue() -> Value & {
		return value;
	}

private:
	std::string name;
	Value value;
};

/// Builds a type a piece at a time, without assembling a parameter vector by hand. Start one from
/// `Factory::CreateType()`, chain the setters, and call `Build`.
/// Nested types are added by passing a callback that fills in a builder of its own.
template <class CTX>
class TypeBuilder {
public:
	explicit TypeBuilder(CTX &context) : ctx(context) {
	}

	/// Identifies the type to build by its id. Takes precedence over `SetName`.
	auto SetTypeId(LogicalTypeId type_id_p) -> TypeBuilder & {
		type_id = type_id_p;
		return *this;
	}

	/// Identifies the type to build by name, which is how extension-registered types are reached.
	auto SetName(std::string_view name_p) -> TypeBuilder & {
		name = name_p;
		return *this;
	}

	/// Appends a positional type parameter.
	auto AddParam(const LogicalType &type) -> TypeBuilder & {
		params.emplace_back("", ctx.CreateValue(type));
		return *this;
	}

	/// Appends a named type parameter, e.g. a STRUCT field.
	auto AddParam(std::string_view name_p, const LogicalType &type) -> TypeBuilder & {
		params.emplace_back(name_p, ctx.CreateValue(type));
		return *this;
	}

	/// Appends a named value parameter, e.g. a VARCHAR's collation.
	template <class T, class = std::enable_if_t<!std::is_invocable_v<T &, TypeBuilder &>>>
	auto AddParam(std::string_view name_p, const T &value) -> TypeBuilder & {
		params.emplace_back(name_p, ctx.CreateValue(value));
		return *this;
	}

	/// Appends a positional value parameter, e.g. a DECIMAL's width.
	template <class T, class = std::enable_if_t<!std::is_invocable_v<T &, TypeBuilder &>>>
	auto AddParam(const T &value) -> TypeBuilder & {
		params.emplace_back("", ctx.CreateValue(value));
		return *this;
	}

	/// Appends a named parameter holding a nested type.
	/// @param name_p The parameter's name.
	/// @param builder Called with a fresh builder; whatever it composes becomes the parameter.
	template <class F, class = std::enable_if_t<std::is_invocable_v<F &, TypeBuilder &>>>
	auto AddParam(std::string_view name_p, F &&builder) -> TypeBuilder & {
		TypeBuilder nested_builder(ctx);
		builder(nested_builder);
		params.emplace_back(name_p, ctx.CreateValue(nested_builder.Build()));
		return *this;
	}

	/// Appends a positional parameter holding a nested type.
	/// @param builder Called with a fresh builder; whatever it composes becomes the parameter.
	template <class F, class = std::enable_if_t<std::is_invocable_v<F &, TypeBuilder &>>>
	auto AddParam(F &&builder) -> TypeBuilder & {
		TypeBuilder nested_builder(ctx);
		builder(nested_builder);
		params.emplace_back("", ctx.CreateValue(nested_builder.Build()));
		return *this;
	}

	/// Builds the type, by id when one was set and by name otherwise. The builder keeps its name, id and parameters, so
	/// a later `Build` repeats the same type.
	auto Build() -> LogicalType {
		if constexpr (std::is_same_v<CTX, Connection>) {
			if (type_id != LogicalTypeId::INVALID) {
				return ctx.CreateType(type_id, params);
			} else {
				return ctx.CreateType(name, params);
			}
		} else if constexpr (std::is_same_v<CTX, Context>) {
			if (type_id != LogicalTypeId::INVALID) {
				return ctx.CreateType(type_id, params);
			} else {
				return ctx.CreateType(name, params);
			}
		} else {
			static_assert(detail::always_false<CTX>::value, "TypeBuilder can only be used with Connection or Context");
			return ctx.CreateType(name, params);
		}
	}

private:
	LogicalTypeId type_id = LogicalTypeId::INVALID;
	std::string name;
	std::vector<TypeParam> params;
	CTX &ctx;
};

/// One parameter binding for the named-parameter `Connection::Execute` overload.
struct NamedParam {
	/// The parameter to bind, matching $name case-insensitively. When empty, the binding is positional instead: the
	/// first `NamedParam` binds $1, the second $2, and so on. A statement cannot mix named and positional parameters.
	std::string name;
	/// The value to bind.
	Value value;
};

//----------------------------------------------------------------------------------------------------------------------
// Arena
//----------------------------------------------------------------------------------------------------------------------
// Vectors of variable-length types like strings, blobs, bits, bignums and geometries contain an auxiliary arena
// allocator called the "String Heap", which lives for as long as the vector itself lives. This is used to store the
// actual bytes e.g. a `blob_t` references in the non-inlined case. Writing a string or a blob to a vector is
// therefore usually done in two steps: allocate the bytes in the arena (heap), then place the resulting blob_t in the
// vector, referencing the bytes. `Vector::AssignString` does both at once and is what most fills want. Go through the
// heap directly when the bytes and their placement are decided separately, e.g. to write one copy of a value that
// several rows point at.

/// A borrowed handle to a vector's string heap, valid until that vector is destroyed or reshaped, e.g. by a `Flatten`.
class Arena final : public detail::Handle<Arena> {
	friend detail::HandleFactory;

public:
	Arena(Arena &&) noexcept = default;
	Arena &operator=(Arena &&) noexcept = default;

	~Arena() override;

	/// Reserves uninitialized bytes in the heap, to build a value in place rather than copy one in.
	/// @param byte_len How many bytes to reserve.
	/// @return The reserved memory, valid as long as the heap is. Write it, wrap it in a `varchar_t` / `blob_t`, and
	/// place that with `Vector::SetString`.
	auto Allocate(idx_t byte_len) -> uint8_t *;

	/// Copies a string into the heap.
	/// @param data The bytes to copy. Up to `varchar_t::INLINE_LENGTH` bytes are stored directly in the returned value.
	/// @return The string as a `varchar_t`, with validated UTF-8, valid as long as the heap is.
	/// Place it in the vector owning this heap with `Vector::SetString`.
	/// @throws Exception On malformed UTF-8 or when the data exceeds the 4 GiB an element can describe.
	auto AddString(std::string_view data) -> varchar_t {
		if (data.size() > std::numeric_limits<uint32_t>::max()) {
			ThrowStringTooLong(data.size());
		}
		ValidateUTF8(data);
		return AddStringUnsafe(data);
	}

	/// Copies text without UTF-8 validation. The caller must ensure that the text is valid.
	auto AddStringUnsafe(std::string_view data) -> varchar_t {
		auto bytes = AddBlob(data);
		return varchar_t(bytes.data(), bytes.size());
	}

	/// `AddString` for arbitrary bytes rather than text.
	auto AddBlob(std::string_view data) -> blob_t {
		if (data.size() > std::numeric_limits<uint32_t>::max()) {
			ThrowStringTooLong(data.size());
		}
		const auto size = static_cast<uint32_t>(data.size());
		if (size <= blob_t::INLINE_LENGTH) {
			return blob_t(data.data(), size);
		}
		auto *bytes = Allocate(size);
		std::memcpy(bytes, data.data(), size);
		return blob_t(reinterpret_cast<char *>(bytes), size);
	}

private:
	explicit Arena(void *impl);

	/// @internal Throws when a value exceeds the maximum byte string length.
	[[noreturn]] static void ThrowStringTooLong(idx_t size);
};

//----------------------------------------------------------------------------------------------------------------------
// Vector
//----------------------------------------------------------------------------------------------------------------------
// A column of values: one array of a primitive type, a validity mask saying which rows are NULL, and child vectors
// for the nested types. This is the preferred way to write/read data in and out of DuckDB. One call to
// `Vector::GetView` or `Vector::GetDataMutable` and the per-row work is plain array indexing -- as opposed to `Value`,
// which handles one cell at a time, and cost a lot in e.g. allocation overhead.
//
// The same column can be laid out in more than one way: a vector whose rows all share a value is stored once, and one
// that selects from another is stored as a selection over it. Reading through `VectorView` handles all of those.
// When writing you will generally produce a FLAT vector, but you can also call `Flatten` to force any vector into the
// basic contiguous data layout.

/// How a vector is laid out.
enum class VectorType : uint8_t {
	/// A layout with no direct read support here, such as a compressed or shredded one. `Flatten` first.
	OTHER = 0,
	/// One element per row.
	FLAT = 1,
	/// A single element standing for every row.
	CONSTANT = 2,
	/// A selection over another vector's elements.
	DICTIONARY = 3,
};

/// A read view over a vector: its data, validity, selection and row count, fetched in a single call so that everything
/// below is plain inline arithmetic.
///
/// The pointers are borrowed from the vector and stay valid until the chunk that owns it is destroyed -- or until the
/// vector is reshaped, e.g. by `Flatten` or `SetSize`, so re-take the view after any such call.
struct VectorView {
	/// The buffer holding the elements, to be read as an array of the primitive type matching the vector's type.
	const void *data;
	/// The validity mask, or `nullptr` when every row is valid.
	const uint64_t *validity;
	/// The selection: which element each row reads, or nullptr when row `i` reads element `i`.
	const uint32_t *sel;
	/// How many rows the view covers.
	idx_t count;

	/// The buffer, typed.
	template <class T>
	auto Data() const -> const T * {
		return static_cast<const T *>(data);
	}

	/// The element row `i` reads. A CONSTANT vector sends every row to element 0.
	auto SelAt(idx_t i) const -> idx_t {
		return sel ? static_cast<idx_t>(sel[i]) : i;
	}

	/// Whether an element is a value rather than NULL.
	/// @param row An element index, i.e. one already put through `SelAt`.
	auto RowIsValid(idx_t row) const -> bool {
		return !validity || (validity[row >> 6] & (static_cast<uint64_t>(1) << (row & 63))) != 0;
	}

	/// Whether a row holds a value rather than NULL. Validity follows the selection, so this is the one to use with a
	/// loop counter.
	/// @param i A row index in [0, count).
	auto IsValid(idx_t i) const -> bool {
		return RowIsValid(SelAt(i));
	}

	/// Whether every row in [0, count) holds a value rather than NULL.
	auto AllValid() const -> bool {
		if (!validity) {
			return true;
		}
		if (sel) {
			for (idx_t i = 0; i < count; i++) {
				if (!IsValid(i)) {
					return false;
				}
			}
			return true;
		}
		// Compare whole words, masking out the bits past `count` in the last one
		const auto whole_words = count / 64;
		for (idx_t i = 0; i < whole_words; i++) {
			if (validity[i] != ~static_cast<uint64_t>(0)) {
				return false;
			}
		}
		const auto tail_bits = count % 64;
		if (tail_bits != 0) {
			const auto tail_mask = (static_cast<uint64_t>(1) << tail_bits) - 1;
			if ((validity[whole_words] & tail_mask) != tail_mask) {
				return false;
			}
		}
		return true;
	}
};

/// A writer over a FLAT vector's validity mask, from `Vector::GetValidityMutable`. Word W bit N covers row W*64+N, and
/// a set bit means the row holds a value rather than NULL.
///
/// Writes here touch one vector's mask and nothing else. Since a NULL STRUCT or ARRAY row requires its descendant
/// elements to be NULL as well, use `Vector::SetNull` (which will set the validity of child vectors recursively) for
/// those rather than clearing the parent's bit. When filling nested masks by hand, use `SetAllInvalid` to clear the
/// child masks up front and then set the individual bits as values are written.
struct ValidityMask {
	/// The raw mask words, for writes this struct does not cover.
	uint64_t *words;

	/// Marks a row as holding a value.
	auto SetValid(idx_t row) -> void {
		words[row >> 6] |= uint64_t(1) << (row & 63);
	}

	/// Marks a row as NULL.
	auto SetInvalid(idx_t row) -> void {
		words[row >> 6] &= ~(uint64_t(1) << (row & 63));
	}

	/// Whether a row holds a value rather than NULL.
	auto RowIsValid(idx_t row) const -> bool {
		return (words[row >> 6] & (uint64_t(1) << (row & 63))) != 0;
	}

	/// Marks rows [0, count) NULL. Writes whole words, so bits past `count` in the last word are cleared too, which
	/// only matters if the vector grows afterwards.
	auto SetAllInvalid(idx_t count) -> void {
		for (idx_t i = 0; i < (count + 63) / 64; i++) {
			words[i] = 0;
		}
	}

	/// Marks rows [0, count) as holding values: the counterpart of `SetAllInvalid`, to reset a mask before refilling
	/// rather than setting each bit as it is written. Writes whole words, so bits past `count` in the last word are
	/// set too, which only matters if the vector grows afterwards.
	auto SetAllValid(idx_t count) -> void {
		for (idx_t i = 0; i < (count + 63) / 64; i++) {
			words[i] = ~uint64_t(0);
		}
	}
};

/// A borrowed handle to one column of a chunk, or to one child of another vector. Valid for as long as the chunk or
/// parent vector it belongs to is valid.
class Vector final : public detail::Handle<Vector> {
	friend detail::HandleFactory;

public:
	Vector(Vector &&) noexcept = default;
	Vector &operator=(Vector &&) noexcept = default;

	~Vector() override;

	/// The buffer for writing, typed. The vector must be FLAT or CONSTANT, and `T` must match its type.
	/// Raw writes to VARCHAR storage must preserve valid UTF-8.
	template <class T>
	auto GetDataMutable() -> T * {
		return static_cast<T *>(GetDataMutable());
	}

	/// The buffer for writing, untyped.
	/// @throws InvalidInputException Unless the vector is FLAT or CONSTANT.
	auto GetDataMutable() -> void *;

	/// How many child vectors this one has: 1 for LIST, MAP and ARRAY, one per field for STRUCT and TUPLE, one per
	/// member plus the leading tag for UNION, and 0 for anything else. A MAP's child is its entries, a STRUCT of the
	/// keys and the values.
	auto GetChildCount() const -> idx_t;

	/// One child vector, e.g. a LIST's elements or a STRUCT's field. A LIST's child is sized to its capacity, not to
	/// the parent's row count.
	/// @param index Child index in [0, GetChildCount()).
	/// @return A borrowed handle, valid until this vector is destroyed or reshaped.
	auto GetChild(idx_t index) const -> Vector;

	/// Rewrites the vector as a FLAT one, materializing one element per row. Pointers and views taken from it
	/// beforehand do not survive this.
	auto Flatten() const -> void;

	/// Repoints the vector at another vector's data, without copying: the two then alias the same buffers, in the
	/// source's layout, until one of them is reset or re-referenced. Works for any type, including nested ones, and
	/// is the way to hand an already-materialized vector to an output vector without a per-row copy. Pointers and
	/// views taken from this vector beforehand do not survive this.
	/// @param source The vector to reference. Its data must outlive every read of this vector.
	/// @throws InvalidInputException When the source's type does not match the vector's.
	auto Reference(const Vector &source) -> void;

	/// Casts every row into `target`, like SQL `CAST`, from this vector's type to the target's. `target` is replaced
	/// by the result, taking this vector's size; pointers and views taken from it beforehand do not survive this.
	/// A connection's or callback's factory casts with that client's cast functions and settings, an
	/// instance's factory with the built-in casts only.
	/// @throws Exception When a row does not convert; `target` is then unspecified.
	auto Cast(Factory &factory, Vector &target) const -> void;

	/// `Cast` like SQL `TRY_CAST`: a row that does not convert becomes NULL in `target` instead of failing the cast.
	/// @param error_message Optional. When not every row converted, receives the conversion error of one that did not.
	/// @return Whether every row converted.
	auto TryCast(Factory &factory, Vector &target, std::string *error_message = nullptr) const -> bool;

	/// How many rows the vector holds.
	auto GetSize() const -> idx_t;

	/// Sets how many rows the vector holds, reallocating if needed, which invalidates views and pointers taken
	/// earlier. A chunk derives its row count from its vectors, so this is also how a hand-filled chunk gets its
	/// cardinality: size every column alike.
	auto SetSize(idx_t size) -> void;

	/// Reads the vector in a single call, so that the per-row work afterwards is inline.
	/// @return A view borrowed from the vector. Taking the view of a DICTIONARY vector may flatten the vector it
	/// selects from, which invalidates views taken from that one earlier.
	/// @throws InvalidInputException On `VectorType::OTHER`; `Flatten` first.
	auto GetView() const -> VectorView;

	/// How the vector is laid out.
	auto GetVectorType() const -> VectorType;

	/// The logical type of the vector's rows.
	auto GetLogicalType() const -> LogicalType;

	/// The validity mask for writing, allocating it if the vector does not have one yet.
	/// @throws InvalidInputException Unless the vector is FLAT.
	auto GetValidityMutable() -> ValidityMask;

	/// Sets a row NULL, including the descendant elements of a STRUCT or ARRAY row. LIST (and MAP) children are left
	/// alone, since a NULL list has no elements to speak of. Use this rather than clearing mask bits for any nested
	/// type.
	/// @param row A row index within the vector's size.
	/// @throws InvalidInputException On a non-FLAT vector or an out-of-range row.
	auto SetNull(idx_t row) -> void;

	/// Sets the single validity bit of a CONSTANT vector. Setting it valid writes no value: element 0 keeps whatever
	/// was last written to it.
	/// @param valid Whether the vector holds a value rather than NULL.
	/// @throws InvalidInputException Unless the vector is CONSTANT.
	auto SetConstantValid(bool valid) -> void;

	/// Rewrites the vector as a CONSTANT one: a single value standing for every row.
	/// @param value The value every row takes.
	/// @param count How many rows the vector then holds.
	/// @throws InvalidInputException When the value's type does not match the vector's.
	auto MakeConstant(const Value &value, idx_t count) -> void;

	/// Rewrites the vector as the arithmetic sequence start, start + increment, ... The result reads as
	/// `VectorType::OTHER`, so `Flatten` before reading it through `GetView`.
	/// @param start The first row's value.
	/// @param increment The step between rows.
	/// @param count How many rows the vector then holds.
	auto MakeSequence(int64_t start, int64_t increment, idx_t count) -> void;

	/// Reads one cell as a `Value`, whatever the layout and whatever the type -- including the types without a view
	/// layout, such as VARIANT -- and without flattening. Costs an owned value per call, so read bulk data through
	/// `GetView` instead.
	/// @param row A row index within the vector's size.
	auto GetValue(idx_t row) const -> Value;

	/// Writes one cell from a `Value`, whatever the type. The value is copied in and cast to the vector's type. Costs
	/// a value write per call, so fill bulk data through `GetDataMutable` instead.
	/// @param row A row index within the vector's size.
	/// @param value The value to write.
	/// @throws InvalidInputException On a non-FLAT vector or an out-of-range row; flatten first.
	auto SetValue(idx_t row, const Value &value) -> void;

	/// Borrows this vector's string heap, to write bytes whose placement is decided separately -- deduplicating,
	/// scattering, reordering. For a straightforward fill prefer `AssignString`.
	/// @return The heap. The vector must be of a string-backed type such as VARCHAR, BLOB, BIT or BIGNUM.
	auto GetHeap() -> Arena;

	/// Copies bytes into the vector's heap and places the string value with `SetString`.
	/// VARCHAR values are constructed with `Arena::AddString`; binary values are copied without UTF-8 validation.
	/// Looks the heap up per call, so flattening in between is safe.
	/// @param index The element to write: any index within the size of a FLAT vector, only 0 for a CONSTANT one.
	/// @param data The bytes to copy. The vector must be of a string-backed type such as VARCHAR, BLOB, BIT or BIGNUM.
	/// @throws InvalidInputException On malformed VARCHAR text, before changing the slot.
	auto AssignString(idx_t index, std::string_view data) -> void;

	/// Like `AssignString`, but skips UTF-8 validation. The caller must ensure VARCHAR values contain valid UTF-8.
	auto AssignStringUnsafe(idx_t index, std::string_view data) -> void;

	/// Writes a string value into the vector without UTF-8 validation.
	/// @param index The element to write: any index within the size of a FLAT vector, only 0 for a CONSTANT one.
	/// @param value A string value from this vector's own heap. A non-inlined value from another vector dangles.
	/// VARCHAR values must contain valid UTF-8.
	auto SetString(idx_t index, varchar_t value) -> void;

private:
	explicit Vector(void *impl);

	/// @internal Throws `InvalidInputException` if [start, start + count) is not writable: a CONSTANT vector has a
	/// single element, so only index 0 may be written.
	auto CheckWriteRange(idx_t start, idx_t count) const -> void;
};

//----------------------------------------------------------------------------------------------------------------------
// DataChunk
//----------------------------------------------------------------------------------------------------------------------
// A batch of rows: one `Vector` per column, all of the same logical length.
// Chunks are the main "unit of execution" in DuckDB, and usually contain a couple of thousand rows at a time.

/// A batch of rows, column by column. A chunk owns its vectors, so it must outlive any `Vector`, `VectorView` or
/// pointer taken from it.
class DataChunk final : public detail::Handle<DataChunk> {
	friend detail::HandleFactory;

public:
	/// An empty chunk with a column per type, ready to be filled: write the columns' data and give every column its
	/// row count with `Vector::SetSize`. The chunk's memory is allocated through the factory's database, so it is
	/// accounted to that database.
	/// @param factory The factory whose database supplies the chunk's memory.
	/// @param types One type per column. Types containing ANY are rejected.
	DataChunk(Factory &factory, const std::vector<LogicalType> &types);

	DataChunk(DataChunk &&other) noexcept {
		std::swap(impl, other.impl);
		std::swap(owned, other.owned);
	}
	DataChunk &operator=(DataChunk &&other) noexcept {
		std::swap(impl, other.impl);
		std::swap(owned, other.owned);
		return *this;
	}

	~DataChunk() override;

	/// How many columns the chunk has.
	auto GetVectorCount() const -> idx_t;

	/// How many rows the chunk holds.
	auto GetRowCount() const -> idx_t;

	/// How many rows the chunk can hold, e.g. how many rows a table function may write into its output chunk in one
	/// call. Unstable API.
	auto GetCapacity() const -> idx_t;

	/// Empties the chunk so it can be filled again: its row count becomes 0, and its vectors writable FLAT vectors of
	/// their types, as on creation. Pointers and views taken from the chunk or its vectors beforehand do not survive
	/// this. Not for a chunk DuckDB lends to a callback for reading.
	auto Clear() -> void;

	/// Access one column, e.g. to read or write its data.
	/// @param index Column index in [0, GetVectorCount()).
	/// @return A borrowed handle, valid for as long as this chunk is.
	auto GetVector(idx_t index) const -> Vector;

	/// A deep copy of this chunk, its memory allocated through the factory's database. The copy is flattened and
	/// owns all its data, so it stays valid after this chunk -- or whatever backs it, such as a
	/// `ColumnDataCollection` scan -- is gone.
	/// @param factory The factory whose database supplies the copy's memory.
	auto Copy(Factory &factory) const -> DataChunk;

private:
	explicit DataChunk(void *impl, bool owned);
	bool owned = false; // TODO: This should be fixed C++ side
};

//----------------------------------------------------------------------------------------------------------------------
// Column Data Collection
//----------------------------------------------------------------------------------------------------------------------
// A buffer-managed set of rows: append chunks in, scan them back out in order. DuckDB owns the memory and spills the
// rows to disk when they outgrow it, so a collection can hold far more than memory allows. A collection must not be
// scanned while it is being appended to, and must not be appended to concurrently: for parallel appends, fill one
// collection per thread and `Combine` them at the end.

/// An owned collection of rows, all sharing one set of column types fixed at construction.
/// It must not outlive the `Connection` or `Context` it was created from.
class ColumnDataCollection final : public detail::Handle<ColumnDataCollection> {
	friend detail::HandleFactory;

public:
	/// Opaque state for appending, from `CreateAppendState`. Only meaningful with the collection that created it, and
	/// invalidated by `Reset`.
	class AppendState final : public detail::Handle<AppendState> {
		friend detail::HandleFactory;

	public:
		AppendState(AppendState &&) noexcept = default;
		AppendState &operator=(AppendState &&) noexcept = default;
		~AppendState() override;

	private:
		explicit AppendState(void *impl);
	};

	/// Opaque state shared by every thread of one scan, from `CreateSharedScanState`: it coordinates which rows each
	/// worker reads and tracks the scan's overall progress. Only meaningful with the collection that created it, and
	/// invalidated by `Reset`.
	class SharedScanState final : public detail::Handle<SharedScanState> {
		friend detail::HandleFactory;

	public:
		SharedScanState(SharedScanState &&) noexcept = default;
		SharedScanState &operator=(SharedScanState &&) noexcept = default;
		~SharedScanState() override;

	private:
		explicit SharedScanState(void *impl);
	};

	/// Opaque per-thread state of one scan, from `CreateWorkerScanState`. It also keeps the buffers backing the chunk
	/// its thread most recently scanned alive: scans are zero-copy, so a scanned chunk's data is only valid until this
	/// state's next `Scan` or its destruction.
	class WorkerScanState final : public detail::Handle<WorkerScanState> {
		friend detail::HandleFactory;

	public:
		WorkerScanState(WorkerScanState &&) noexcept = default;
		WorkerScanState &operator=(WorkerScanState &&) noexcept = default;
		~WorkerScanState() override;

	private:
		explicit WorkerScanState(void *impl);
	};

	/// An empty collection, its memory managed by the factory's database.
	/// @param factory The factory whose database supplies the collection's memory.
	/// @param types One type per column, at least one; every chunk appended must match them exactly. Types containing
	/// ANY are rejected.
	ColumnDataCollection(Factory &factory, const std::vector<LogicalType> &types);

	ColumnDataCollection(ColumnDataCollection &&) noexcept = default;
	ColumnDataCollection &operator=(ColumnDataCollection &&) noexcept = default;

	~ColumnDataCollection() override;

	/// How many rows the collection holds.
	auto GetRowCount() const -> idx_t;

	/// Drops all rows and releases their memory, keeping the column types; the collection is immediately appendable
	/// again. Outstanding append and scan states are invalidated: create new ones. Prefer `Clear` when the collection
	/// is about to be refilled.
	auto Reset() -> void;

	/// Drops all rows but keeps their memory, so the next appends write into buffers that are already allocated;
	/// prefer it over `Reset` when the collection is refilled repeatedly. The column types are unchanged and the
	/// collection is immediately appendable again. Outstanding append and scan states are invalidated: create new
	/// ones.
	auto Clear() -> void;

	/// Moves another collection's rows to the end of this one, consuming it. The source must have the same column
	/// types, and both collections must come from the same database -- the rows keep their original buffers rather
	/// than being copied.
	/// @param source The collection to consume. Left untouched when the merge is refused.
	/// @throws InvalidInputException When the column types differ, or when `source` is this collection.
	auto Combine(ColumnDataCollection &&source) -> void;

	/// Starts appending: the returned state carries the append's progress between `Append` calls.
	auto CreateAppendState() -> AppendState;

	/// Copies a chunk's rows to the end of the collection.
	/// @param state The append state to append through.
	/// @param chunk The rows to append. The chunk's column types must equal the collection's exactly, and the chunk is
	/// only borrowed: it can be reused, refilled and appended again.
	/// VARCHAR values must already be valid UTF-8; append does not validate text.
	/// @throws InvalidInputException When the chunk's columns do not match the collection's.
	auto Append(AppendState &state, const DataChunk &chunk) -> void;

	/// One-shot `Append`, creating and discarding an append state internally. Prefer keeping a state across calls when
	/// appending more than once.
	auto Append(const DataChunk &chunk) -> void;

	/// Starts a scan over the collection's rows. One shared state per scan; each participating thread additionally
	/// gets its own `CreateWorkerScanState`.
	auto CreateSharedScanState() const -> SharedScanState;

	/// Per-thread state for a scan started with `CreateSharedScanState`.
	auto CreateWorkerScanState() const -> WorkerScanState;

	/// Reads the next rows of the scan into a chunk. Threads sharing one `SharedScanState` each receive disjoint rows,
	/// so together they scan the collection exactly once.
	///
	/// The scan is zero-copy where possible: the chunk's vectors may reference the collection's buffers, kept alive by
	/// the worker state, so the chunk's data is only valid until that state's next `Scan` or its destruction.
	/// Make a copy with `DataChunk::Copy` to keep the scanned data longer.
	/// @param shared The scan's shared state.
	/// @param worker This thread's worker state.
	/// @param chunk The chunk to read into; its column types must equal the collection's exactly. Reset to empty once
	/// the scan is exhausted.
	/// @return Whether rows were produced; false once the scan is exhausted.
	/// @throws InvalidInputException When the chunk's columns do not match the collection's.
	auto Scan(SharedScanState &shared, WorkerScanState &worker, DataChunk &chunk) const -> bool;

private:
	explicit ColumnDataCollection(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Appender
//----------------------------------------------------------------------------------------------------------------------

/// Buffers rows on the client side and writes them to the database in bulk.
/// An appender pairs a `ColumnDataCollection` with a statement that reads it: `AppendChunk` fills the buffer without
/// touching the database, and `Flush` runs the statement over everything buffered so far and empties the buffer.
/// Many small inserts thereby become one bulk insert.
///
/// The appender is built on the public API alone: `SqlStatement::AddCollection` exposes the buffer to the statement
/// under a name. The same can be done by hand when a different shape is needed.
///
/// An appender is bound to the connection it was created on and can only be flushed there. It is not thread-safe:
/// append from one thread at a time, or give each thread its own appender. It must not outlive its `Connection`.
///
/// Destroying an appender does not flush: rows buffered but not flushed are dropped.
class Appender {
public:
	/// Creates an appender that inserts into `table`.
	/// The buffer takes the table's columns, in order, with their types, resolved once at construction: the appender
	/// keeps targeting that table even if the search path changes later.
	/// @param conn The connection to buffer against and flush on.
	/// @param table The table to append to, optionally qualified.
	/// @throws Exception When the table cannot be resolved.
	/// @note A table with generated columns needs the query constructor instead: this one lists every column, and
	/// the engine refuses an INSERT that names a generated one.
	Appender(Connection &conn, std::string_view table);

	/// Creates an appender that flushes by running `query`, which reads the buffer as a table named `buffer_name`.
	/// Use this when a plain INSERT of every column is not what is needed: a subset of columns, an UPDATE or MERGE
	/// driven by the buffer, or an INSERT with an ON CONFLICT clause.
	/// @param conn The connection to buffer against and flush on.
	/// @param query Exactly one SELECT, INSERT, UPDATE, DELETE or MERGE INTO statement, referring to the buffer by
	/// `buffer_name`.
	/// @param column_types One type per buffer column, at least one.
	/// @param buffer_name The name `query` reads the buffer under. It shadows a table of that name, and must not be
	/// the name of a common table expression `query` defines itself.
	/// @param column_names Names for the buffer's columns; empty names them col1..colN.
	/// @throws InvalidInputException When `column_types` is empty, `query` is not exactly one statement of a
	/// supported type, or it already defines `buffer_name`.
	Appender(Connection &conn, std::string_view query, std::vector<LogicalType> column_types,
	         std::string_view buffer_name, const std::vector<std::string> &column_names = {});

	Appender(const Appender &) = delete;
	Appender &operator=(const Appender &) = delete;
	Appender(Appender &&) noexcept = default;
	Appender &operator=(Appender &&) noexcept = default;

	/// Drops whatever is still buffered. Does not flush.
	~Appender();

	/// The buffer's column types, in order. Build a chunk over these to fill and append, e.g.
	/// `DataChunk chunk(appender.ColumnTypes())`. Valid for the appender's lifetime.
	auto ColumnTypes() const -> const std::vector<LogicalType> & {
		return types;
	}

	/// Buffers a whole chunk. Its column types must equal `ColumnTypes()` exactly; a mismatch is refused before
	/// anything is copied. VARCHAR values must already be valid UTF-8; append does not validate text.
	/// @throws InvalidInputException When the chunk's columns do not match, or a previous buffer operation failed.
	void AppendChunk(DataChunk &chunk);

	/// Runs the statement over everything buffered and empties the buffer, keeping its memory for the next batch.
	/// Does nothing when the buffer is empty.
	/// @throws Exception When the statement fails. The rows are kept when the connection was busy or the run was
	/// interrupted, so the flush can be retried; any other failure drops them.
	void Flush();

	/// Empties the buffer without running the statement, and recovers from a failed buffer operation.
	void Clear();

private:
	// The shared body of both constructors.
	void Initialize(Connection &conn, const std::string &query, std::vector<LogicalType> column_types,
	                const std::string &buffer_name, const std::vector<std::string> &column_names);
	void ResetBuffer();

	// The connection the statement runs on.
	Connection *connection = nullptr;
	std::vector<LogicalType> types;
	// The buffer, declared before the statement that borrows it so it is destroyed after it.
	std::unique_ptr<ColumnDataCollection> collection;
	// The statement, parsed once and re-executed per flush, and the append state, recreated after every reset.
	std::unique_ptr<SqlStatement> statement;
	std::unique_ptr<ColumnDataCollection::AppendState> append_state;
	// Set when a buffer operation failed mid-flight, leaving the buffered rows indeterminate: appends and flushes
	// refuse until Clear recovers.
	bool broken = false;
};

//----------------------------------------------------------------------------------------------------------------------
// Arrow
//----------------------------------------------------------------------------------------------------------------------
// Interop with the Arrow C Data Interface, in both directions. `ArrowExporter` fills caller-allocated Arrow structs,
// which the caller then releases; `ArrowImporter` hands an Arrow array's buffers to `DataChunk`s zero-copy. Both
// resolve their schema once, at construction, and both gather rows up to a batch size across inputs, so the last
// input is marked with `flush`. To execute SQL straight into Arrow arrays, pass an `ArrowFormat` to `Execute` and
// read the `ArrowResult`.

/// Converts Arrow arrays into `DataChunk`s against one resolved `ArrowSchema`. Construct it once, for example at a
/// table function's bind time, and reuse it for every array of that shape. Give it an array with `Append`, then call
/// `NextChunk` until it returns an empty handle. Pass `flush` on the last array, or call `Flush`.
/// The importer borrows the context it was created with and must not outlive it. One array is in flight at a time,
/// and an importer must not be used from two threads at once.
class ArrowImporter final : public detail::Handle<ArrowImporter> {
	friend detail::HandleFactory;

public:
	/// Resolves `schema` against `context`, extension types included.
	/// @param context A context with an active transaction: resolving reads the catalog.
	/// @param schema The schema to resolve. Read, not consumed; the caller keeps ownership.
	/// @param batch_size Maximum rows per chunk. A long array is split across several chunks, and rows left over
	/// that do not fill a batch are held back and joined with the next array unless flushed. 0 means no maximum:
	/// one chunk per array.
	ArrowImporter(Context &context, ArrowSchema &schema, idx_t batch_size = 0);

	ArrowImporter(ArrowImporter &&) noexcept = default;
	ArrowImporter &operator=(ArrowImporter &&) noexcept = default;

	~ArrowImporter() override;

	/// The resolved DuckDB schema: the name and logical type of every column, which is how a table function declares
	/// its result columns from an Arrow schema.
	auto GetSchema() const -> Schema;

	/// Gives the importer one array to convert. Take the chunks with `NextChunk`.
	/// @param array The array to convert.
	/// @param consume True to hand it over: its `release` is set to NULL, and the chunks reference its buffers
	/// zero-copy and keep them alive. False to keep it, in which case it must stay valid until the drain finishes and
	/// every chunk is a copy. A chunk that joins rows held back from the previous array is a copy either way.
	/// @param flush True to mark the end of the input, releasing rows that do not fill a batch as a final short chunk.
	/// @throws InvalidInputException When the previous array is not drained, or the array's shape does not match the
	/// resolved schema.
	auto Append(ArrowArray &array, bool consume = true, bool flush = false) -> void;

	/// Releases the held rows as a short chunk without supplying another array. Same as `Append` with no array and
	/// `flush` set.
	auto Flush() -> void;

	/// The next chunk of the appended array, or an empty handle once the array is drained. Rows that do not fill a
	/// batch are held back for the next array unless flushed, so an empty handle does not mean every row has come
	/// out. Runs under the context the importer was created with, which must still be alive.
	auto NextChunk() -> DataChunk;

private:
	explicit ArrowImporter(void *impl);
};

/// Converts `DataChunk`s into Arrow arrays for one fixed column list. The session's Arrow settings are captured at
/// construction, so the schema it reports and the arrays it produces always agree. Give it a chunk with `Append`,
/// then call `NextArray` until it returns false. Pass `flush` on the last chunk, or call `Flush`.
/// An exporter must not be used from two threads at once.
class ArrowExporter final : public detail::Handle<ArrowExporter> {
	friend detail::HandleFactory;

public:
	/// @param context A context with an active transaction, whose Arrow settings are captured.
	/// @param types The column types.
	/// @param names The column names, one per type.
	/// @param batch_size Maximum rows per array. A long chunk is split across several arrays, and rows left over
	/// that do not fill a batch are held back and joined with the next chunk unless flushed. 0 means no maximum:
	/// one array per chunk.
	ArrowExporter(Context &context, const std::vector<LogicalType> &types, const std::vector<std::string> &names,
	              idx_t batch_size = 0);

	ArrowExporter(ArrowExporter &&) noexcept = default;
	ArrowExporter &operator=(ArrowExporter &&) noexcept = default;

	~ArrowExporter() override;

	/// The Arrow schema of the arrays this exporter produces, written into the caller-allocated `out`, which the
	/// caller then owns and releases with `out.release(&out)`. `out` is left released on any throw.
	auto GetSchema(ArrowSchema &out) const -> void;

	/// Converts one chunk in full, making the arrays it completed available from `NextArray`. The chunk is borrowed
	/// and read before this returns, since the conversion copies. Rows that do not complete a batch are held back
	/// and finished by the next chunk.
	/// @param flush True to mark the end of the input, releasing the held rows as a final short array.
	/// @throws InvalidInputException When the chunk's types do not match the exporter's.
	auto Append(const DataChunk &chunk, bool flush = false) -> void;

	/// Releases the held rows as a short array without supplying another chunk. Same as `Append` with `flush` set.
	auto Flush() -> void;

	/// Takes the next completed array into the caller-allocated `out`, which the caller then owns and releases.
	/// @return False when none is ready, leaving `out` released; it is also left released on any throw. Rows held
	/// back towards an unfinished batch are not an array yet; they come out after a further `Append` or a `Flush`.
	auto NextArray(ArrowArray &out) -> bool;

private:
	explicit ArrowExporter(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Result
//----------------------------------------------------------------------------------------------------------------------
// A `QueryResult` is a lazily executed stream of chunks, produced by a query.
// Execution is deferred until the result is asked for its first chunk, and only one result may be live on a connection
// at a time, so read a result to the end (or destroy it) before executing again.
//
// There are two ways to consume a result, synchronously or asynchronously
// - `FetchChunk` blocks until the next chunk is ready, and is what most callers probably want.
// - `Step` performs a bounded amount of work and reports what came of it, which allows an async runtime to drive a
// query without necessarily occupying a thread indefinitely.
// An `ArrowResult` (below) is the same stream in Arrow form, read the same two ways.

/// The status of one `QueryResult::Step` or `ArrowResult::Step`.
enum class StepStatus : uint8_t {
	/// Nothing was produced by this step; call `Wait`, or come back later.
	WAITING = 0,
	/// A chunk was produced, or for an `ArrowResult`, an array.
	CHUNK = 1,
	/// The result is exhausted. Sticky.
	FINISHED = 2,
	/// The query was canceled. Sticky.
	CANCELLED = 3,
};

/// The shape of a result, as `QueryResult::GetResultType` and `ArrowResult::GetResultType` report it.
enum class ResultType : uint8_t {
	/// Produces rows: SELECT, EXPLAIN, RETURNING, and other row-producing statements.
	QUERY_RESULT = 0,
	/// Carries a count of affected rows: INSERT / UPDATE / DELETE and the like, without RETURNING.
	CHANGED_ROWS = 1,
	/// Produces no rows: most DDL and utility statements.
	NOTHING = 2,
};

/// A streaming query result.
class QueryResult final : public detail::Handle<QueryResult> {
	friend detail::HandleFactory;

public:
	/// The status of one `Step`; see `cxx::StepStatus`.
	using StepStatus = cxx::StepStatus;
	/// The shape of a result; see `cxx::ResultType`.
	using ResultType = cxx::ResultType;
	/// The kind of SQL statement a result came from; see `cxx::StatementType`.
	using StatementType = cxx::StatementType;

	/// The outcome of one `Step`.
	struct StepResult {
		/// What the step accomplished.
		StepStatus status;
		/// The chunk produced, empty unless `status` is CHUNK.
		DataChunk chunk;
	};

	QueryResult(QueryResult &&) noexcept = default;
	QueryResult &operator=(QueryResult &&) noexcept = default;

	~QueryResult() override;

	/// The result's columns, their names and types, as one owned `Schema`.
	/// @throws Exception With code `DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED` when the schema is not available yet, for
	/// a statement that expands into several; step the result first.
	auto GetSchema() const -> Schema;

	/// The shape of the result, so a caller can decide between consuming rows and draining without inspecting the SQL.
	/// @throws Exception With code `DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED` when the shape is not available yet, for a
	/// statement that expands into several; step the result first.
	auto GetResultType() const -> ResultType;

	/// The kind of SQL statement this result came from, as written: MULTI for one that expands into several.
	auto GetStatementType() const -> StatementType;

	/// Does a bounded amount of work and returns without blocking.
	/// @return What the step accomplished, and the chunk if it produced one.
	/// @throws Exception On an execution error; the error is sticky, and later `Step`s rethrow it.
	auto Step() -> StepResult;

	/// Blocks until `Step` may be able to make progress, returning immediately once the result is finished or
	/// canceled. May not block at all.
	/// @throws Exception On an execution error.
	auto Wait() -> void;

	/// The next chunk, blocking until it is ready.
	/// @return The chunk, or an empty one at the end of the stream; calling it again then keeps returning empty.
	/// @throws InterruptException When the query was canceled.
	auto FetchChunk() -> DataChunk;

	/// Runs the result to the end, applying its side effects and discarding any rows.
	/// @return The number of rows affected for a CHANGED_ROWS result, 0 otherwise.
	/// @throws InterruptException When the query was canceled.
	auto Drain() -> idx_t;

	/// Renders the result as the boxed table the CLI prints, consuming it. Whatever has not been read yet is
	/// materialized in memory first.
	/// @param max_rows How many rows to print before eliding the middle, 0 for the default.
	/// @param max_width How wide the table may be, 0 for the default.
	/// @param max_col_width How wide a single column may be, 0 for the default.
	/// @param null_value What to print for a NULL cell; empty prints "NULL".
	/// @param render_mode 0 to lay the result out in rows, 1 in columns; anything else throws.
	/// @param limit The LIMIT the query itself applied, 0 for none. When the result fills it exactly, the footer reads
	/// "? rows", since there may have been more.
	auto RenderBox(idx_t max_rows = 0, idx_t max_width = 0, idx_t max_col_width = 0, const std::string &null_value = "",
	               idx_t render_mode = 0, idx_t limit = 0) -> std::string;

private:
	explicit QueryResult(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Arrow Result
//----------------------------------------------------------------------------------------------------------------------
// An `ArrowResult` is the same lazily executed stream in Arrow form: `Execute` with an `ArrowFormat` produces one, and
// it is read through `FetchArray` and `Step` the way a `QueryResult` is read through `FetchChunk` and `Step`.

/// Selects the Arrow result format: passing one to `Execute` yields an `ArrowResult` instead of a `QueryResult`.
struct ArrowFormat {
	/// Explicit so a braced argument cannot silently turn a parameter value into a batch size.
	explicit ArrowFormat(idx_t batch_size = 0) : batch_size(batch_size) {
	}

	/// Maximum rows per array; arrays can be shorter. 0 means the engine's default, 131072 today.
	idx_t batch_size;
};

/// A streaming query result that produces Arrow arrays: the Arrow form of `QueryResult`, produced by passing an
/// `ArrowFormat` to `Connection::Execute` or `PreparedStatement::Execute`. It behaves like a `QueryResult` in all but
/// the format: execution is deferred until the result is read, it is the connection's one live result, and
/// `Connection::Interrupt` cancels it. Every array holds at most the batch size chosen at execution, and can hold
/// fewer. Arrays and schemas are written into caller-allocated structs, which the caller then owns and releases
/// through their `release` callback; they stay valid after the result, its connection, instance and environment are
/// destroyed. Use a result from one thread at a time.
class ArrowResult final : public detail::Handle<ArrowResult> {
	friend detail::HandleFactory;

public:
	/// The status of one `Step`; see `cxx::StepStatus`.
	using StepStatus = cxx::StepStatus;
	/// The shape of a result; see `cxx::ResultType`.
	using ResultType = cxx::ResultType;
	/// The kind of SQL statement a result came from; see `cxx::StatementType`.
	using StatementType = cxx::StatementType;

	ArrowResult(ArrowResult &&) noexcept = default;
	ArrowResult &operator=(ArrowResult &&) noexcept = default;

	~ArrowResult() override;

	/// The Arrow schema of the result's arrays, a struct with one child per column. Each call writes an independent
	/// copy. Available as soon as `QueryResult::GetSchema` would be: at once for most statements, after stepping for
	/// one that expands into several, such as PIVOT.
	/// @param out Caller-allocated; overwritten. The caller then owns it and releases it with `out.release(&out)`.
	/// @throws Exception With code `DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED` when the schema is not available yet;
	/// step the result first. `out` is left released on any throw.
	auto GetSchema(ArrowSchema &out) const -> void;

	/// The shape of the result, so a caller can decide between reading arrays and draining without inspecting the SQL.
	/// @throws Exception With code `DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED` when the shape is not available yet, for a
	/// statement that expands into several; step the result first.
	auto GetResultType() const -> ResultType;

	/// The kind of SQL statement this result came from, as written: MULTI for one that expands into several.
	auto GetStatementType() const -> StatementType;

	/// Does a bounded amount of work and returns without blocking.
	/// @param out Caller-allocated; overwritten. On CHUNK it holds the next array, which the caller then owns and
	/// releases; otherwise, and on any throw, it is left released.
	/// @return What the step accomplished.
	/// @throws Exception On an execution error; the error is sticky, and later `Step`s rethrow it.
	auto Step(ArrowArray &out) -> StepStatus;

	/// Blocks until `Step` may be able to make progress, returning immediately once the result is finished or
	/// canceled. May not block at all.
	/// @throws Exception On an execution error.
	auto Wait() -> void;

	/// Takes the next array, blocking until it is ready.
	/// @param out Caller-allocated; overwritten. Holds the array, which the caller then owns and releases, when this
	/// returns true; left released otherwise, and on any throw.
	/// @return False at the end of the stream; calling it again then keeps returning false.
	/// @throws InterruptException When the query was canceled.
	auto FetchArray(ArrowArray &out) -> bool;

	/// Runs the result to the end, applying its side effects and discarding any rows.
	/// @return The number of rows affected for a CHANGED_ROWS result, 0 otherwise.
	/// @throws InterruptException When the query was canceled.
	auto Drain() -> idx_t;

	/// Turns the result into an Arrow C stream, consuming it: the stream takes the result over, and this wrapper is
	/// empty afterwards. The stream continues where `Step` and `FetchArray` left off, so arrays already taken are not
	/// repeated. Converting runs no part of the query, but the stream's `get_schema` can: for a statement that expands
	/// into several, such as PIVOT, it runs the ones before the statement that returns rows. The stream keeps the
	/// result alive, with its connection busy and its instance open; releasing the stream destroys the result and
	/// frees the connection. A failing `get_next` or `get_schema` returns EIO, and `get_last_error` has the message.
	/// @param out Caller-allocated; overwritten. The caller then owns the stream and releases it with
	/// `out.release(&out)`.
	/// @throws Exception When the conversion fails; the result then stays with this wrapper, and `out` is left
	/// released.
	auto ToArrowStream(ArrowArrayStream &out) -> void;

private:
	explicit ArrowResult(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Function Signature
//----------------------------------------------------------------------------------------------------------------------

/// How a caller passes the argument for a parameter, following Python's parameter kinds. A signature orders its
/// parameters by kind, in the order the values are listed here.
enum class FunctionParameterKind : uint8_t {
	/// Passed by position only. A named argument with the parameter's name is received by `**kwargs`.
	POSITIONAL_ONLY = 0,
	/// Passed by position or by name.
	STANDARD = 1,
	/// `*args`: receives the positional arguments left over after the positional-only and standard parameters.
	POSITIONAL_VARIADIC = 2,
	/// Passed by name only.
	NAMED_ONLY = 3,
	/// `**kwargs`: receives the named arguments that match no other parameter.
	NAMED_VARIADIC = 4,
};

/// A function's declared parameters and return type.
/// Borrowed from the function it was read from via `GetSignature`.
/// Valid for as long as the owning function is.
/// Setters mutate the function's signature in place.
class FunctionSignature final : public detail::Handle<FunctionSignature> {
	friend detail::HandleFactory;

public:
	FunctionSignature(FunctionSignature &&) noexcept = default;
	FunctionSignature &operator=(FunctionSignature &&) noexcept = default;

	~FunctionSignature() override;

	/// Adds a parameter without a default value. The signature orders its parameters by kind, and parameters of the
	/// same kind in the order they were added. `LogicalTypeId::ANY` is accepted and leaves the argument un-cast.
	/// @param name The parameter's name.
	/// @param type The parameter's type.
	/// @param kind How a caller passes the argument.
	/// @throws InvalidInputException When the kind is not a `FunctionParameterKind`.
	auto AddParameter(const std::string &name, const LogicalType &type,
	                  FunctionParameterKind kind = FunctionParameterKind::STANDARD) -> FunctionSignature &;

	/// Adds a parameter with a default value: the caller may omit it, the function still receives the default.
	/// `*args` and `**kwargs` cannot have a default value; registration rejects one.
	/// @param name The parameter's name.
	/// @param type The parameter's type.
	/// @param default_value The value the parameter takes when the caller omits it.
	/// @param kind How a caller passes the argument.
	/// @throws InvalidInputException When the kind is not a `FunctionParameterKind`.
	auto AddParameter(const std::string &name, const LogicalType &type, const Value &default_value,
	                  FunctionParameterKind kind = FunctionParameterKind::STANDARD) -> FunctionSignature &;

	/// Adds the `*args` parameter, receiving any number of extra positional arguments. Pass `LogicalTypeId::ANY` to
	/// leave them un-cast.
	/// @param name The parameter's name.
	/// @param type The type every extra positional argument is cast to.
	auto AddArgs(const std::string &name, const LogicalType &type) -> FunctionSignature &;

	/// Adds the `**kwargs` parameter, receiving the named arguments that match no other parameter. Pass
	/// `LogicalTypeId::ANY` to leave them un-cast.
	/// @param name The parameter's name.
	/// @param type The type every such named argument is cast to.
	auto AddKwargs(const std::string &name, const LogicalType &type) -> FunctionSignature &;

	/// Sets the return type. Overwrites any prior return type.
	/// @param type The return type.
	auto SetReturnType(const LogicalType &type) -> FunctionSignature &;

private:
	explicit FunctionSignature(void *impl);
};

/// The documentation of a function overload, as `duckdb_functions()` reports it.
/// Borrowed from the function it was read from via `GetDocs`, and valid for as long as that function is.
/// Setters mutate the function's documentation in place, and return the documentation so they chain.
class FunctionDocs final : public detail::Handle<FunctionDocs> {
	friend detail::HandleFactory;

public:
	FunctionDocs(FunctionDocs &&) noexcept = default;
	FunctionDocs &operator=(FunctionDocs &&) noexcept = default;

	~FunctionDocs() override;

	/// Sets the description of the overload. Replaces any previous description.
	auto SetDescription(std::string_view description) -> FunctionDocs &;

	/// Adds an example of calling the overload, e.g. a SQL expression.
	/// @param title The title of the example; empty for none.
	/// @param example The example.
	auto AddExample(std::string_view title, std::string_view example) -> FunctionDocs &;

	/// Adds a category the overload belongs to, e.g. "string" or "aggregate".
	auto AddCategory(std::string_view category) -> FunctionDocs &;

private:
	explicit FunctionDocs(void *impl);
};

/// The sizes of the four parts of a function call's argument list, which follow each other in this order.
struct ArgumentCounts {
	/// One argument per positional-only and standard parameter, in declaration order.
	idx_t positional_fixed = 0;
	/// The arguments `*args` received, in call order.
	idx_t positional_variadic = 0;
	/// One argument per named-only parameter, in declaration order.
	idx_t named_fixed = 0;
	/// The arguments `**kwargs` received, in call order.
	idx_t named_variadic = 0;

	/// The index of the first named argument.
	auto NamedOffset() const -> idx_t {
		return positional_fixed + positional_variadic;
	}
	/// The number of arguments in all four parts.
	auto Total() const -> idx_t {
		return NamedOffset() + named_fixed + named_variadic;
	}
};

/// What every function's bind callback works with: the arguments of the call being bound, and the bind data.
/// Borrowed, valid only for the callback duration.
///
/// The arguments form one list in the four parts of `ArgumentCounts`. A standard parameter passed by name is at its
/// declared position, and a parameter the call omitted carries its default value.
class FunctionBindInput {
public:
	/// Constructs bind data of type `T`, owned by the bound function call and readable from every later callback via
	/// `GetBindData<T>`. The engine compares bind data when it compares expressions: by `operator==` when `T` has one,
	/// by identity otherwise.
	template <class T, class... ARGS>
	void SetBindData(ARGS &&... args) {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetBindDataInternal(ptr, detail::SelectEquals<T>(), detail::TypedDelete<T>);
	}

	/// How many arguments the call passes, in all four parts. Valid indices for the other argument accessors are
	/// [0, GetArgCount()).
	auto GetArgCount() const -> idx_t;

	/// The sizes of the four parts of the argument list.
	auto GetArgumentCounts() const -> ArgumentCounts;

	/// The resolved type of one argument. An ANY parameter reports the type the caller passed.
	/// @param index Argument index in [0, GetArgCount()).
	/// @throws InvalidInputException When the index is out of range.
	auto GetArgType(idx_t index) const -> LogicalType;

	/// The constant value of one argument, folded at bind time. Use it for arguments the function needs to know
	/// before execution, e.g. a format string or a target type. A table function's arguments are always constant.
	/// @param index Argument index in [0, GetArgCount()).
	/// @throws InvalidInputException When the index is out of range.
	/// @throws Exception When the argument is not a constant expression, e.g. a column reference.
	auto GetConstantArgument(idx_t index) const -> Value;

	/// `GetConstantArgument` without the failure: nullopt instead of an exception when the argument carries no
	/// constant value, i.e. it is not a constant expression, its value is not yet known (an unresolved
	/// prepared-statement parameter), or the index is out of range.
	/// @param index Argument index in [0, GetArgCount()).
	auto TryGetConstantArgument(idx_t index) const -> std::optional<Value>;

	/// The name of one argument: the parameter name for a declared parameter, the name the caller passed for an
	/// argument `**kwargs` received, and an empty name for an argument `*args` received.
	/// @param index Argument index in [0, GetArgCount()).
	/// @throws InvalidInputException When the index is out of range.
	auto GetArgName(idx_t index) const -> std::string;

	/// The index of the argument a caller passes by the given name, matched case-insensitively, or nullopt when the
	/// call passed no argument of that name. Positional-only parameters are skipped, as a caller cannot pass them by
	/// name.
	/// @param name The name to look up.
	auto FindArg(const std::string &name) const -> std::optional<idx_t>;

	/// The binding context. Borrowed, valid only for the callback duration.
	auto GetContext() -> Context &;

protected:
	FunctionBindInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
	}

	/// The user data slot of the function, which carries the function's info table
	void *GetFunctionInfo() const;

	void *args;
	Context context;

private:
	void SetBindDataInternal(void *data, bool (*equals)(void *a, void *b), void (*destructor)(void *));
};

//----------------------------------------------------------------------------------------------------------------------
// Function Properties
//----------------------------------------------------------------------------------------------------------------------
// Properties shared by every function category, set with the function classes' property setters (`SetStability`,
// `SetFallibility`, ...) to configure metadata that influences planning and execution. Category-specific properties
// (e.g. aggregate order/DISTINCT dependence) are declared as nested enums on the relevant function class.

/// How stable/deterministic a function's result is, used by the optimizer.
enum class FunctionStability : uint8_t {
	/// Always returns the same result for the same input.
	CONSISTENT = 0,
	/// The result may differ per row (e.g. random()).
	VOLATILE = 1,
	/// Stable within a single query/transaction but may change across queries (e.g. now()).
	CONSISTENT_WITHIN_QUERY = 2,
};

/// Whether a function handles NULL inputs itself.
enum class FunctionNullHandling : uint8_t {
	/// If any argument is NULL the result is NULL and the function is not invoked for that row.
	DEFAULT = 0,
	/// The function is invoked even when arguments are NULL and decides the result itself.
	SPECIAL = 1,
};

/// Whether a function can raise a runtime error.
enum class FunctionFallibility : uint8_t {
	/// The function never raises a runtime error. Declaring this promises the callbacks never throw; an exception
	/// thrown anyway becomes an internal error.
	INFALLIBLE = 0,
	/// The function may raise a runtime error for some inputs (default).
	FALLIBLE = 1,
};

/// How a function interacts with collations on its arguments.
enum class FunctionCollationHandling : uint8_t {
	/// Combines collations from its inputs and propagates them to its result (default).
	PROPAGATE = 0,
	/// Combinable collations are executed on the input arguments before the function runs.
	PUSH_COMBINABLE = 1,
	/// Collations are ignored by the function.
	IGNORE = 2,
};

/// The order guarantee a producer of rows makes about its output.
enum class OrderPreservation : uint8_t {
	/// The rows have no meaningful order and may be reordered freely.
	NO_ORDER = 0,
	/// The rows are in insertion order, kept unless the `preserve_insertion_order` setting is disabled (default).
	INSERTION_ORDER = 1,
	/// The rows are in an order that must be kept, as if sorted by an `ORDER BY`, even when the
	/// `preserve_insertion_order` setting is disabled.
	FIXED_ORDER = 2,
};

//----------------------------------------------------------------------------------------------------------------------
// Statistics
//----------------------------------------------------------------------------------------------------------------------

/// The statistics of an expression or a column: what values it can produce. A function's stats callback reads the
/// statistics of its arguments and narrows those of its result, and the engine uses them to simplify the query, e.g.
/// folding `x IS NULL` to false when `x` cannot be NULL.
///
/// Every statistic states what can occur: `true` means a kind of value may be present, `false` guarantees it is
/// absent. The engine relies on the guarantees, so claiming less than the data holds produces wrong query results.
/// Borrowed: valid only for the duration of the callback that handed it out. Unstable API.
class Stats final : public detail::Handle<Stats> {
	friend detail::HandleFactory;

public:
	Stats(Stats &&) noexcept = default;
	Stats &operator=(Stats &&) noexcept = default;
	~Stats() override = default;

	/// Whether a value can be NULL. False guarantees every value is non-NULL.
	auto CanHaveNull() const -> bool;
	/// Whether a value can be valid, i.e. non-NULL. False guarantees every value is NULL. When both this and
	/// `CanHaveNull` are false, there are no values at all.
	auto CanHaveValid() const -> bool;

	/// Sets whether a value can be NULL. Setting false is a guarantee the engine relies on; setting true is always
	/// safe.
	/// @throws InvalidInputException When the statistics are read-only, such as those of an argument.
	auto SetCanHaveNull(bool value) -> void;
	/// Sets whether a value can be valid, i.e. non-NULL. Setting false is a guarantee the engine relies on; setting
	/// true is always safe.
	/// @throws InvalidInputException When the statistics are read-only, such as those of an argument.
	auto SetCanHaveValid(bool value) -> void;

private:
	explicit Stats(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined scalar function, built up with the setters and made live with `Connection::Register` or
/// `Extension::Register`. Create one through a `Factory` of the database it will be registered in, describe it (name,
/// signature, callbacks), then register it with `Connection::Register` or `Extension::Register`. The function object
/// may be destroyed after registration; the registered function lives on in the catalog.
///
/// The callbacks receive their state through the input objects: `SetUserData` plants data readable from every
/// callback, the resolve types callback may resolve the return type, the bind callback may plant bind data for init
/// and exec, and the init callback may plant init data for exec. A callback reports failure by throwing; the exception
/// surfaces as the query's error.
class ScalarFunction final : public detail::Handle<ScalarFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	class ResolveTypesInput;
	class BindInput;
	class InitInput;
	class ExecInput;
	class StatsInput;

	/// Called once per query while the function call is bound, before the arguments are cast to the parameter types.
	/// Optional; required when the return type is ANY.
	using ResolveTypesCallback = void (*)(ResolveTypesInput &input);
	/// Called once per query while the function call is bound, after the arguments are cast to the parameter types.
	/// Optional.
	using BindCallback = void (*)(BindInput &input);
	/// Called once per execution thread before the first `ExecCallback` on it. Optional.
	using InitCallback = void (*)(InitInput &input);
	/// Called for every batch of rows; must fill the result vector. Required.
	using ExecCallback = void (*)(ExecInput &input);
	/// Called while the query is optimized, possibly more than once, to narrow the statistics of the result from
	/// those of the arguments. Optional; without it the engine knows nothing about the result. Unstable API.
	using StatsCallback = void (*)(StatsInput &input);

	ScalarFunction(ScalarFunction &&) noexcept = default;
	ScalarFunction &operator=(ScalarFunction &&) noexcept = default;

	~ScalarFunction() override;

	/// Creates an empty ScalarFunction through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> ScalarFunction;

	/// Sets the function's name, as SQL will call it.
	auto SetName(const std::string &name) & -> ScalarFunction &;
	/// The documentation of this overload, borrowed for in-place mutation.
	auto GetDocs() -> FunctionDocs;

	/// Calls `configure` with the documentation of this overload, borrowed for in-place mutation.
	template <class F>
	auto WithDocs(F &&configure) & -> ScalarFunction & {
		auto docs = GetDocs();
		configure(docs);
		return *this;
	}

	/// The function's signature, borrowed for in-place mutation. Registration requires a return type that is either
	/// a fully defined concrete type, or ANY combined with a resolve types callback that resolves it.
	auto GetSignature() -> FunctionSignature;

	/// Calls `configure` with the function's signature, borrowed for in-place mutation. Registration requires a return
	/// type that is either a fully defined concrete type, or ANY combined with a resolve types callback that resolves
	/// it.
	template <class F>
	auto WithSignature(F &&configure) & -> ScalarFunction & {
		auto sig = GetSignature();
		configure(sig);
		return *this;
	}

	/// Constructs user data of type `T`, carried by the registered function and freed at engine teardown; read it from
	/// any callback via the inputs' `GetUserData<T>`. Consumed by registration: set it again before
	/// re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> ScalarFunction & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetResolveTypesCallback(ResolveTypesCallback callback) & -> ScalarFunction &;
	auto SetBindCallback(BindCallback callback) & -> ScalarFunction &;
	auto SetInitCallback(InitCallback callback) & -> ScalarFunction &;
	auto SetExecCallback(ExecCallback callback) & -> ScalarFunction &;
	auto SetStatsCallback(StatsCallback callback) & -> ScalarFunction &;

	/// How stable the function's result is across rows and queries. Defaults to `CONSISTENT`.
	auto SetStability(FunctionStability value) & -> ScalarFunction &;
	/// Whether the function handles NULL inputs itself. Defaults to `DEFAULT` (NULL in, NULL out).
	auto SetNullHandling(FunctionNullHandling value) & -> ScalarFunction &;
	/// Whether the function can raise a runtime error. Defaults to `FALLIBLE`; declaring `INFALLIBLE` promises the
	/// callbacks never throw.
	auto SetFallibility(FunctionFallibility value) & -> ScalarFunction &;
	/// How the function interacts with collations on its arguments. Defaults to `PROPAGATE`.
	auto SetCollationHandling(FunctionCollationHandling value) & -> ScalarFunction &;

private:
	explicit ScalarFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	ResolveTypesCallback resolve_types_callback = nullptr;
	BindCallback bind_callback = nullptr;
	InitCallback init_callback = nullptr;
	ExecCallback exec_callback = nullptr;
	StatsCallback stats_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the resolve types callback works with. The argument types are the types the caller passed, before they
	/// are cast to the parameter types. Borrowed, valid only for the callback duration.
	class ResolveTypesInput final : public FunctionBindInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `ScalarFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Resolves the declared return type; required, and only permitted, when the signature declared it as ANY.
		/// @param type The concrete return type of this bound call.
		auto SetReturnType(const LogicalType &type) -> void;

	private:
		ResolveTypesInput(void *args, void *result, void *context) : FunctionBindInput(args, context), result(result) {
		}

		/// The bind data can only be set from the bind callback
		using FunctionBindInput::SetBindData;

		void *result;

		void *GetUserDataInternal() const;
	};

	/// What the bind callback works with. The argument types are the parameter types the arguments were cast to.
	/// Borrowed, valid only for the callback duration.
	class BindInput final : public FunctionBindInput {
		friend detail::HandleFactory;

	public:
		/// Constructs bind data of type `T`, owned by the bound function call and readable from the init and exec
		/// callbacks via `GetBindData<T>`. The engine compares bind data when it compares expressions: by
		/// `operator==` when `T` has one, by identity otherwise.
		template <class T, class... ARGS>
		void SetBindData(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetBindDataInternal(ptr, detail::SelectEquals<T>(), detail::TypedDelete<T>);
		}

		/// The user data set via `ScalarFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

	private:
		BindInput(void *args, void *result, void *context) : FunctionBindInput(args, context), result(result) {
		}

		void *result;

		void SetBindDataInternal(void *data, bool (*equals)(void *a, void *b), void (*destructor)(void *));
		void *GetUserDataInternal() const;
	};

	/// What the init callback works with. Borrowed, valid only for the callback duration.
	class InitInput {
		friend detail::HandleFactory;

	public:
		/// Constructs init data of type `T`, owned by this execution thread's function state and readable from the
		/// exec callback via `GetInitData<T>`.
		template <class T, class... ARGS>
		void SetInitData(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetInitDataInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `ScalarFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The context the function is initialized in. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		InitInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetInitDataInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the exec callback works with. Borrowed, valid only for the callback duration.
	class ExecInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The init data set via `InitInput::SetInitData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetInitData() const -> T & {
			return *static_cast<T *>(GetInitDataInternal());
		}

		/// The user data set via `ScalarFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many rows this execution must produce: the exec callback writes exactly this many rows to the result
		/// vector. May be less than a full vector; with all-constant arguments the function runs for a single row and
		/// the engine expands the result.
		auto GetRowCount() const -> idx_t;

		/// How many argument vectors this execution carries, in all four parts. Valid indices for `GetArg` are
		/// [0, GetArgCount()).
		auto GetArgCount() const -> idx_t;

		/// The sizes of the four parts of the argument list, as the bind callback saw them.
		auto GetArgumentCounts() const -> ArgumentCounts;

		/// One argument vector, at the index the bind callback used for the argument.
		/// @param index Argument index in [0, GetArgCount()).
		auto GetArg(idx_t index) const -> Vector;

		/// The result vector to fill.
		auto GetResult() const -> Vector;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		ExecInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetInitDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the stats callback works with. Borrowed, valid only for the callback duration.
	///
	/// The result statistics start out unknown, with every kind of value possible. A callback that leaves them
	/// unchanged tells the engine nothing.
	class StatsInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `ScalarFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many arguments the call has, in all four parts. Valid indices for `GetArgStats` are
		/// [0, GetArgCount()).
		auto GetArgCount() const -> idx_t;

		/// The sizes of the four parts of the argument list, as the bind callback saw them.
		auto GetArgumentCounts() const -> ArgumentCounts;

		/// The statistics of one argument, at the index the bind callback used for it. Read-only.
		/// @param index Argument index in [0, GetArgCount()).
		auto GetArgStats(idx_t index) const -> Stats;

		/// The statistics of the result, for the callback to narrow.
		auto GetResultStats() const -> Stats;

		/// The query's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		StatsInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Aggregate Function
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined aggregate function, built up with the setters and made live with `Connection::Register` or
/// `Extension::Register`. Create one through a `Factory` of the database it will be registered in, describe it (name,
/// signature, callbacks), then register it with `Connection::Register` or `Extension::Register`. The function object
/// may be destroyed after registration; the registered function lives on in the catalog.
///
/// The aggregate keeps one state per group. The size callback reports how large a single state is; the init callback
/// constructs a batch of freshly allocated states; the update callback folds a batch of input rows into their rows'
/// states; the combine callback merges partial states (e.g. across threads); the finalize callback turns a batch of
/// states into result rows; and the optional destroy callback releases resources a state owns.
///
/// The callbacks receive their state through the input objects: `SetUserData` plants data readable from every
/// callback, and the bind callback may plant bind data readable from every later callback. A callback reports failure
/// by throwing; the exception surfaces as the query's error -- except in the destroy callback, whose errors are
/// dropped, as it runs on a path that must not fail.
class AggregateFunction final : public detail::Handle<AggregateFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	/// Whether the aggregate's result depends on the order in which rows are aggregated.
	enum class OrderDependence : uint8_t {
		/// The result depends on input order (default).
		DEPENDENT = 0,
		/// The result does not depend on input order.
		INDEPENDENT = 1,
	};

	/// Whether the aggregate's result is affected by a DISTINCT modifier.
	enum class DistinctDependence : uint8_t {
		/// The result is affected by DISTINCT (default).
		DEPENDENT = 0,
		/// The result is not affected by DISTINCT.
		INDEPENDENT = 1,
	};

	class BindInput;
	class SizeInput;
	class InitInput;
	class UpdateInput;
	class CombineInput;
	class FinalizeInput;
	class DestroyInput;
	class StatsInput;

	/// Called once per query while the function call is bound. Optional; required when the return type is ANY.
	using BindCallback = void (*)(BindInput &input);
	/// Called to size a single aggregate state; must call `SetStateSize`. Required.
	using SizeCallback = void (*)(SizeInput &input);
	/// Called to initialize a batch of freshly allocated states in place. Required.
	using InitCallback = void (*)(InitInput &input);
	/// Called to fold a batch of input rows into their rows' states. Required.
	using UpdateCallback = void (*)(UpdateInput &input);
	/// Called to merge a batch of source states into their target states. Required.
	using CombineCallback = void (*)(CombineInput &input);
	/// Called to turn a batch of states into result rows. Required.
	using FinalizeCallback = void (*)(FinalizeInput &input);
	/// Called to release resources a batch of states owns. Optional; must not fail.
	using DestroyCallback = void (*)(DestroyInput &input);
	/// Called while the query is optimized, possibly more than once, to narrow the statistics of the result from
	/// those of the arguments. Optional; without it the engine knows nothing about the result. Unstable API.
	using StatsCallback = void (*)(StatsInput &input);

	AggregateFunction(AggregateFunction &&) noexcept = default;
	AggregateFunction &operator=(AggregateFunction &&) noexcept = default;

	~AggregateFunction() override;

	/// Creates an empty AggregateFunction through `factory`. Register it through a `Connection` or an `Extension` of
	/// the factory's database.
	static auto Create(Factory &factory) -> AggregateFunction;

	/// Sets the function's name, as SQL will call it.
	auto SetName(const std::string &name) & -> AggregateFunction &;
	/// The documentation of this overload, borrowed for in-place mutation.
	auto GetDocs() -> FunctionDocs;

	/// Calls `configure` with the documentation of this overload, borrowed for in-place mutation.
	template <class F>
	auto WithDocs(F &&configure) & -> AggregateFunction & {
		auto docs = GetDocs();
		configure(docs);
		return *this;
	}

	/// The function's signature, borrowed for in-place mutation. Registration requires a return type that is either
	/// a fully defined concrete type, or ANY combined with a bind callback that resolves it.
	auto GetSignature() -> FunctionSignature;

	/// Calls `configure` with the function's signature, borrowed for in-place mutation. Registration requires a return
	/// type that is either a fully defined concrete type, or ANY combined with a bind callback that resolves it.
	template <class F>
	auto WithSignature(F &&configure) & -> AggregateFunction & {
		auto sig = GetSignature();
		configure(sig);
		return *this;
	}

	/// Constructs user data of type `T`, carried by the registered function and freed at engine teardown; read it from
	/// any callback via the inputs' `GetUserData<T>`. Consumed by registration: set it again before
	/// re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> AggregateFunction & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetBindCallback(BindCallback callback) & -> AggregateFunction &;
	auto SetSizeCallback(SizeCallback callback) & -> AggregateFunction &;
	auto SetInitCallback(InitCallback callback) & -> AggregateFunction &;
	auto SetUpdateCallback(UpdateCallback callback) & -> AggregateFunction &;
	auto SetCombineCallback(CombineCallback callback) & -> AggregateFunction &;
	auto SetFinalizeCallback(FinalizeCallback callback) & -> AggregateFunction &;
	auto SetDestroyCallback(DestroyCallback callback) & -> AggregateFunction &;
	auto SetStatsCallback(StatsCallback callback) & -> AggregateFunction &;

	/// How stable the function's result is across rows and queries. Defaults to `CONSISTENT`.
	auto SetStability(FunctionStability value) & -> AggregateFunction &;
	/// Whether the function handles NULL inputs itself. Defaults to `DEFAULT` (NULL in, NULL out).
	auto SetNullHandling(FunctionNullHandling value) & -> AggregateFunction &;
	/// Whether the function can raise a runtime error. Defaults to `FALLIBLE`; declaring `INFALLIBLE` promises the
	/// callbacks never throw.
	auto SetFallibility(FunctionFallibility value) & -> AggregateFunction &;
	/// How the function interacts with collations on its arguments. Defaults to `PROPAGATE`.
	auto SetCollationHandling(FunctionCollationHandling value) & -> AggregateFunction &;
	/// Whether the result depends on the order in which rows are aggregated. Defaults to `DEPENDENT`.
	auto SetOrderDependence(OrderDependence value) & -> AggregateFunction &;
	/// Whether the result is affected by a DISTINCT modifier. Defaults to `DEPENDENT`.
	auto SetDistinctDependence(DistinctDependence value) & -> AggregateFunction &;

private:
	explicit AggregateFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	BindCallback bind_callback = nullptr;
	SizeCallback size_callback = nullptr;
	InitCallback init_callback = nullptr;
	UpdateCallback update_callback = nullptr;
	CombineCallback combine_callback = nullptr;
	FinalizeCallback finalize_callback = nullptr;
	DestroyCallback destroy_callback = nullptr;
	StatsCallback stats_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the bind callback works with. Borrowed, valid only for the callback duration.
	class BindInput final : public FunctionBindInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Resolves the declared return type; required, and only permitted, when the signature declared it as ANY.
		/// @param type The concrete return type of this bound call.
		auto SetReturnType(const LogicalType &type) -> void;

	private:
		BindInput(void *args, void *result, void *context) : FunctionBindInput(args, context), result(result) {
		}

		void *result;

		void *GetUserDataInternal() const;
	};

	/// What the size callback works with. Borrowed, valid only for the callback duration.
	class SizeInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Reports the size of a single aggregate state, in bytes; the callback must call this.
		/// @param size The state size, in bytes.
		auto SetStateSize(idx_t size) -> void;

	private:
		explicit SizeInput(void *args) : args(args) {
		}

		void *args;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the init callback works with. Borrowed, valid only for the callback duration.
	class InitInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many states this invocation must initialize: the length of `GetStates`.
		auto GetStateCount() const -> idx_t;

		/// The states to initialize, one pointer per state. Each points to uninitialized memory of the size the size
		/// callback reported; the callback must initialize all of them in place.
		auto GetStates() const -> void **;

	private:
		explicit InitInput(void *args) : args(args) {
		}

		void *args;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the update callback works with. Borrowed, valid only for the callback duration.
	class UpdateInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many input rows this invocation carries: the length of the argument vectors and of `GetStates`.
		auto GetRowCount() const -> idx_t;

		/// How many argument vectors this invocation carries, in all four parts. Valid indices for `GetArg` are
		/// [0, GetArgCount()).
		auto GetArgCount() const -> idx_t;

		/// The sizes of the four parts of the argument list, as the bind callback saw them.
		auto GetArgumentCounts() const -> ArgumentCounts;

		/// One argument vector, at the index the bind callback used for the argument.
		/// @param index Argument index in [0, GetArgCount()).
		auto GetArg(idx_t index) const -> Vector;

		/// The states to update, one pointer per input row: row i of every argument vector must be aggregated into
		/// state i. Different rows may point to the same state.
		auto GetStates() const -> void **;

	private:
		explicit UpdateInput(void *args) : args(args) {
		}

		void *args;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the combine callback works with. Borrowed, valid only for the callback duration.
	class CombineInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many source/target state pairs this invocation must combine: the length of `GetSources` and
		/// `GetTargets`.
		auto GetStateCount() const -> idx_t;

		/// The source states, one pointer per pair. Source i must be combined into target i; the source must not be
		/// modified.
		auto GetSources() const -> void **;

		/// The target states, one pointer per pair. Source i must be combined into target i.
		auto GetTargets() const -> void **;

	private:
		explicit CombineInput(void *args) : args(args) {
		}

		void *args;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the finalize callback works with. Borrowed, valid only for the callback duration.
	class FinalizeInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many states this invocation must finalize: the length of `GetStates` and the number of rows to write.
		auto GetStateCount() const -> idx_t;

		/// The states to finalize, one pointer per state. State i must be finalized into result row
		/// `GetResultOffset() + i`.
		auto GetStates() const -> void **;

		/// The result vector to fill, starting at `GetResultOffset`.
		auto GetResult() const -> Vector;

		/// The offset in the result vector at which to start writing: state i must be finalized into result row
		/// `GetResultOffset() + i`.
		auto GetResultOffset() const -> idx_t;

	private:
		explicit FinalizeInput(void *args) : args(args) {
		}

		void *args;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the destroy callback works with. Borrowed, valid only for the callback duration.
	class DestroyInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many states this invocation must destroy: the length of `GetStates`.
		auto GetStateCount() const -> idx_t;

		/// The states to destroy, one pointer per state. The callback must release any resources the states own.
		auto GetStates() const -> void **;

	private:
		explicit DestroyInput(void *args) : args(args) {
		}

		void *args;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the stats callback works with. Borrowed, valid only for the callback duration.
	///
	/// The result statistics start out unknown, with every kind of value possible. They describe every result the
	/// aggregate produces, including that of a group with no input rows, such as an ungrouped aggregate over an empty
	/// table. A callback that leaves them unchanged tells the engine nothing.
	class StatsInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `AggregateFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many arguments the call has, in all four parts. Valid indices for `GetArgStats` are
		/// [0, GetArgCount()).
		auto GetArgCount() const -> idx_t;

		/// The sizes of the four parts of the argument list, as the bind callback saw them.
		auto GetArgumentCounts() const -> ArgumentCounts;

		/// The statistics of one argument, at the index the bind callback used for it. Read-only.
		/// @param index Argument index in [0, GetArgCount()).
		auto GetArgStats(idx_t index) const -> Stats;

		/// The statistics of the result, for the callback to narrow.
		auto GetResultStats() const -> Stats;

		/// The query's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		StatsInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Bound Expression
//----------------------------------------------------------------------------------------------------------------------

/// The type of a bound expression node. Mirrors the C API's `DUCKDB_V2_EXPRESSION_TYPE`: the node types a filter
/// predicate can contain, every other node type reporting `INVALID`.
///
/// Comparisons, `BETWEEN` and casts are regular scalar function calls once bound: they have the function's children
/// and name, and only their type tells them apart from any other function. A cast has one child, the value being
/// cast, and its target type is the node's return type.
enum class ExpressionType : uint8_t {
	/// A node type this API does not model. Its children can still be walked.
	INVALID = 0,
	/// A cast. One child; `Expression::GetCastMode` tells a `TRY_CAST` apart.
	OPERATOR_CAST = 12,
	/// Logical `NOT`. One child.
	OPERATOR_NOT = 13,
	/// `IS NULL`. One child.
	OPERATOR_IS_NULL = 14,
	/// `IS NOT NULL`. One child.
	OPERATOR_IS_NOT_NULL = 15,
	/// `=`. Two children.
	COMPARE_EQUAL = 25,
	/// `<>`. Two children.
	COMPARE_NOTEQUAL = 26,
	/// `<`. Two children.
	COMPARE_LESSTHAN = 27,
	/// `>`. Two children.
	COMPARE_GREATERTHAN = 28,
	/// `<=`. Two children.
	COMPARE_LESSTHANOREQUALTO = 29,
	/// `>=`. Two children.
	COMPARE_GREATERTHANOREQUALTO = 30,
	/// `IN`. The first child is the value tested, the remaining children are the candidates.
	COMPARE_IN = 35,
	/// `NOT IN`. The first child is the value tested, the remaining children are the candidates.
	COMPARE_NOT_IN = 36,
	/// `IS DISTINCT FROM`. Two children.
	COMPARE_DISTINCT_FROM = 37,
	/// `BETWEEN`. Three children: the value tested, the lower bound and the upper bound.
	COMPARE_BETWEEN = 38,
	/// `IS NOT DISTINCT FROM`. Two children.
	COMPARE_NOT_DISTINCT_FROM = 40,
	/// Logical `AND`. Two or more children.
	CONJUNCTION_AND = 50,
	/// Logical `OR`. Two or more children.
	CONJUNCTION_OR = 51,
	/// A constant. No children; read it with `Expression::GetConstantValue`.
	VALUE_CONSTANT = 75,
	/// A prepared statement parameter whose value is not known yet. No children.
	VALUE_PARAMETER = 76,
	/// A call to a scalar function other than the ones listed above. The children are its arguments; read the name
	/// with `Expression::GetFunctionName`.
	BOUND_FUNCTION = 141,
	/// A `CASE` expression. The children are each `WHEN` condition followed by its `THEN` result, then the `ELSE`
	/// result.
	CASE_EXPR = 150,
	/// `COALESCE`. One or more children.
	OPERATOR_COALESCE = 152,
	/// A reference to a column. No children; read it with `Expression::GetColumnIndex`.
	BOUND_COLUMN_REF = 228,
};

/// A node of a bound expression tree: an expression the engine has resolved every column and function of, handed
/// to callbacks that may want to look inside a predicate (see `TableFunction::FilterPushdownInput`). Borrowed and
/// read-only: valid only for the duration of the callback that handed it out, and the children obtained via
/// `GetChild` share their parent's lifetime.
class Expression final : public detail::Handle<Expression> {
	friend detail::HandleFactory;

public:
	Expression(Expression &&) noexcept = default;
	Expression &operator=(Expression &&) noexcept = default;
	~Expression() override = default;

	/// The node's type, which decides what its children mean and which of the accessors below apply.
	auto GetType() const -> ExpressionType;
	/// The logical type the node evaluates to. For a cast, the target type.
	auto GetReturnType() const -> LogicalType;
	/// How many child nodes the node has. Works for every type, including `ExpressionType::INVALID`.
	auto GetChildCount() const -> idx_t;
	/// A child node, ordered as `ExpressionType` describes.
	/// @throws InvalidInputException When the index is out of bounds.
	auto GetChild(idx_t index) const -> Expression;
	/// The value of a `VALUE_CONSTANT` node.
	/// @throws InvalidInputException When the node is not a constant.
	auto GetConstantValue() const -> Value;
	/// The column a `BOUND_COLUMN_REF` node points at. The index counts the columns of the operator the predicate is
	/// evaluated against; resolve it through whatever handed out the expression, e.g.
	/// `TableFunction::FilterPushdownInput::GetColumnIndex`.
	/// @throws InvalidInputException When the node is not a column reference.
	auto GetColumnIndex() const -> idx_t;
	/// The name of the scalar function a function node calls: `BOUND_FUNCTION`, the comparisons, `COMPARE_BETWEEN`
	/// and `OPERATOR_CAST`. A comparison's name is its operator, e.g. `<`; `BETWEEN` and casts carry internal names,
	/// so dispatch on the type for those.
	/// @throws InvalidInputException When the node is not a function call.
	auto GetFunctionName() const -> std::string;
	/// The qualified name of the scalar function a function node calls: the name of `GetFunctionName`, qualified with
	/// the catalog and schema the function was resolved in where known.
	/// @throws InvalidInputException When the node is not a function call.
	auto GetFunctionQualifiedName() const -> QualifiedName;
	/// Whether an `OPERATOR_CAST` node is a regular `CAST` or a `TRY_CAST`.
	/// @throws InvalidInputException When the node is not a cast.
	auto GetCastMode() const -> CastMode;

private:
	explicit Expression(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Table Function
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined table function, built up with the setters and made live with `Connection::Register` or
/// `Extension::Register`. Create one through a `Factory` of the database it will be registered in, describe it (name,
/// signature, callbacks), then register it with `Connection::Register` or `Extension::Register`. The function object
/// may be destroyed after registration; the registered function lives on in the catalog.
///
/// A table function produces a table rather than a value: the bind callback declares the columns it returns, and the
/// exec callback is then invoked repeatedly to fill batches of rows until it produces an empty one. Between them, the
/// optional init callbacks set up the state the scan runs on: one global state shared by every thread, and one local
/// state per thread.
///
/// The callbacks receive their state through the input objects: `SetUserData` plants data readable from every
/// callback, and the bind callback may plant bind data readable from every later callback. A callback reports failure
/// by throwing; the exception surfaces as the query's error.
class TableFunction final : public detail::Handle<TableFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	class BindInput;
	class InitGlobalInput;
	class InitLocalInput;
	class ExecInput;
	class ProgressInput;
	class FilterPushdownInput;
	class PartitionDataInput;
	class PartitioningInput;
	class ClaimBatchInput;
	class GetBindInfoInput;
	class StatsInput;

	/// Whether, and how, the scan is partitioned by a set of columns. Reported from the partitioning callback.
	enum class PartitionInfo : uint8_t {
		/// The scan is not known to be partitioned by the requested columns.
		NOT_PARTITIONED = 0,
		/// Every batch the scan produces carries exactly one distinct value for the requested columns.
		SINGLE_VALUE_PARTITIONS = 1,
		/// The batches the scan produces overlap only at their boundaries.
		OVERLAPPING_PARTITIONS = 2,
		/// The batches the scan produces are disjoint ranges.
		DISJOINT_PARTITIONS = 3,
	};

	/// Called once per query while the function call is bound; declares the columns the function returns. Required.
	using BindCallback = void (*)(BindInput &input);
	/// Called once per scan, before the first `ExecCallback`. Optional.
	using InitGlobalCallback = void (*)(InitGlobalInput &input);
	/// Called once per scanning thread, before the first `ExecCallback` on it. Optional.
	using InitLocalCallback = void (*)(InitLocalInput &input);
	/// Called repeatedly to produce the next batch of rows; an empty batch ends the scan. Required.
	using ExecCallback = void (*)(ExecInput &input);
	/// Called on demand during execution to report how far the scan has advanced. Optional.
	using ProgressCallback = void (*)(ProgressInput &input);
	/// Called while the query is optimized, possibly more than once, with the predicates the query applies to the
	/// function's rows; accepts the ones the function will apply itself. Optional.
	using FilterPushdownCallback = void (*)(FilterPushdownInput &input);
	/// Called after every batch the exec callback produces, on the thread that produced it, to report the batch's
	/// ordering position and, when requested, the value it carries for each partitioning column. Optional.
	using PartitionDataCallback = void (*)(PartitionDataInput &input);
	/// Called while the query is planned, possibly more than once, with a candidate `GROUP BY` column set; reports
	/// whether every batch carries a single value for it. Optional; requires a partition data callback.
	using PartitioningCallback = void (*)(PartitioningInput &input);
	/// Called on a scanning thread to claim its next batch of work - before the exec callback first runs on the thread,
	/// and again whenever the exec callback leaves the output chunk empty. The exec callback then produces the rows of
	/// the claimed batch only. The callback may run while the other scanning threads wait for it, so it should only
	/// claim the work (e.g. reserve a block number) and leave reading it to the exec callback. Optional; unstable API.
	using ClaimBatchCallback = void (*)(ClaimBatchInput &input);
	/// Called with the bind data of a bound call when the engine needs to know more about the call than its columns:
	/// identifiers of its columns (e.g. field ids), and options - key-value facts about what the call reads. A
	/// `MultiFileFunction` wrapping the function asks for every file it binds, maps the columns of the files by the
	/// identifiers, and exposes the options as the metadata of the reader of the file. May be called more than once
	/// for the same bind data: only report what the bind already determined. Optional; unstable API.
	using GetBindInfoCallback = void (*)(GetBindInfoInput &input);
	/// Called while the query is optimized, once per declared column the query reads, to narrow that column's
	/// statistics. Optional; without it the engine knows nothing about the columns. Unstable API.
	using StatsCallback = void (*)(StatsInput &input);

	TableFunction(TableFunction &&) noexcept = default;
	TableFunction &operator=(TableFunction &&) noexcept = default;

	~TableFunction() override;

	/// Creates an empty TableFunction through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> TableFunction;

	/// Sets the function's name, as SQL will call it.
	auto SetName(const std::string &name) & -> TableFunction &;
	/// The documentation of this overload, borrowed for in-place mutation.
	auto GetDocs() -> FunctionDocs;

	/// Calls `configure` with the documentation of this overload, borrowed for in-place mutation.
	template <class F>
	auto WithDocs(F &&configure) & -> TableFunction & {
		auto docs = GetDocs();
		configure(docs);
		return *this;
	}

	/// The function's signature, borrowed for in-place mutation. A parameter without a default value becomes a
	/// required positional argument, one with a default becomes a named argument the caller may omit. Registration
	/// rejects a return type: a table function declares the columns it returns from its bind callback.
	auto GetSignature() -> FunctionSignature;

	/// Calls `configure` with the function's signature, borrowed for in-place mutation. See `GetSignature`.
	template <class F>
	auto WithSignature(F &&configure) & -> TableFunction & {
		auto sig = GetSignature();
		configure(sig);
		return *this;
	}

	/// Constructs user data of type `T`, carried by the registered function and freed at engine teardown; read it from
	/// any callback via the inputs' `GetUserData<T>`. Consumed by registration: set it again before
	/// re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> TableFunction & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetBindCallback(BindCallback callback) & -> TableFunction &;
	auto SetInitGlobalCallback(InitGlobalCallback callback) & -> TableFunction &;
	auto SetInitLocalCallback(InitLocalCallback callback) & -> TableFunction &;
	auto SetExecCallback(ExecCallback callback) & -> TableFunction &;
	auto SetProgressCallback(ProgressCallback callback) & -> TableFunction &;
	auto SetFilterPushdownCallback(FilterPushdownCallback callback) & -> TableFunction &;
	auto SetPartitionDataCallback(PartitionDataCallback callback) & -> TableFunction &;
	auto SetPartitioningCallback(PartitioningCallback callback) & -> TableFunction &;
	/// Makes the function scan one batch at a time: see `ClaimBatchCallback`. Telling the batches apart lets a caller
	/// that scans several of them in parallel put their rows back in order - e.g. a `MultiFileFunction` wrapping this
	/// function keeps the rows of a file in order also when several threads scan it. Unstable API.
	auto SetClaimBatchCallback(ClaimBatchCallback callback) & -> TableFunction &;
	/// Lets the function describe what a bound call reads: see `GetBindInfoCallback`. Unstable API.
	auto SetGetBindInfoCallback(GetBindInfoCallback callback) & -> TableFunction &;
	/// Lets the function narrow the statistics of the columns it produces: see `StatsCallback`. Unstable API.
	auto SetStatsCallback(StatsCallback callback) & -> TableFunction &;

	/// Declares whether the function supports projection pushdown. Defaults to false. With it, the engine asks for
	/// only the columns a query uses: the exec callback's output chunk holds one vector per requested column, and
	/// the init and exec inputs' `GetColumnIndex` says which declared column each vector stands for. Without it the
	/// output chunk always holds every declared column, and the engine drops the unused ones itself.
	auto SetProjectionPushdown(bool enable) & -> TableFunction &;

private:
	explicit TableFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	BindCallback bind_callback = nullptr;
	InitGlobalCallback init_global_callback = nullptr;
	InitLocalCallback init_local_callback = nullptr;
	ExecCallback exec_callback = nullptr;
	ProgressCallback progress_callback = nullptr;
	FilterPushdownCallback filter_pushdown_callback = nullptr;
	PartitionDataCallback partition_data_callback = nullptr;
	PartitioningCallback partitioning_callback = nullptr;
	ClaimBatchCallback claim_batch_callback = nullptr;
	GetBindInfoCallback get_bind_info_callback = nullptr;
	StatsCallback stats_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the bind callback works with. Borrowed, valid only for the callback duration.
	class BindInput final : public FunctionBindInput {
		friend detail::HandleFactory;

	public:
		/// Declares one of the columns the function returns. Call it once per column, in order: the exec callback's
		/// output chunk carries one vector per declared column, in the same order. At least one column is required.
		/// @param name The column's name.
		/// @param type The column's type. Must be a fully defined concrete type; ANY is rejected.
		auto AddResultColumn(const std::string &name, const LogicalType &type) -> void;

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Hints how many rows the scan will produce, for the optimizer. Producing a different number of rows is not
		/// an error.
		/// @param cardinality The estimated row count.
		/// @param is_exact Whether the estimate is exact, which also makes it an upper bound.
		auto SetCardinality(idx_t cardinality, bool is_exact) -> void;

		/// Sets the order guarantee of the rows this call produces. Defaults to `OrderPreservation::INSERTION_ORDER`.
		/// With `OrderPreservation::NO_ORDER`, a scan that allows more than one thread runs in parallel into query
		/// results, INSERT and COPY without a partition data callback, and the rows arrive in no particular order.
		/// @param order The order guarantee of the produced rows.
		/// @throws InvalidInputException When order is not a declared enum value.
		auto SetOrderPreservation(OrderPreservation order) -> void;

	private:
		BindInput(void *args, void *result, void *context) : FunctionBindInput(args, context), result(result) {
		}

		void *result;

		void *GetUserDataInternal() const;
	};

	/// What the global init callback works with. Borrowed, valid only for the callback duration.
	class InitGlobalInput {
		friend detail::HandleFactory;

	public:
		/// Constructs global state of type `T`, shared by every thread scanning the function and readable from the
		/// local init, exec and progress callbacks. Since every thread sees the same object, the function must
		/// synchronize its own access to it.
		template <class T, class... ARGS>
		void SetGlobalState(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetGlobalStateInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Caps how many threads may scan the function in parallel. Defaults to 1, a single-threaded scan. The engine
		/// creates at most this many local states, and may use fewer.
		/// @param max_threads The maximum thread count. Must be at least 1.
		auto SetMaxThreads(idx_t max_threads) -> void;

		/// How many columns the scan produces: with projection pushdown the columns the query uses, which is the number
		/// of vectors in the exec callback's output chunk; without it, the columns declared in bind.
		auto GetColumnCount() const -> idx_t;
		/// Which declared column (in `BindInput::AddResultColumn` order) the scan's column at `index` stands for. The
		/// identity without projection pushdown.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetColumnIndex(idx_t index) const -> idx_t;

		/// The scan's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		InitGlobalInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetGlobalStateInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the local init callback works with. Borrowed, valid only for the callback duration.
	class InitLocalInput {
		friend detail::HandleFactory;

	public:
		/// Constructs local state of type `T`, owned by this scanning thread and readable from the exec callback via
		/// `ExecInput::GetLocalState<T>`. No other thread observes it, so it needs no synchronization.
		template <class T, class... ARGS>
		void SetLocalState(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetLocalStateInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `InitGlobalInput::SetGlobalState`, typically to claim this thread's share of the
		/// work from it. Shared with every other scanning thread; access must be synchronized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many columns the scan produces; see `InitGlobalInput::GetColumnCount`.
		auto GetColumnCount() const -> idx_t;
		/// Which declared column the scan's column at `index` stands for; see `InitGlobalInput::GetColumnIndex`.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetColumnIndex(idx_t index) const -> idx_t;

		/// The scan's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		InitLocalInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetLocalStateInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the exec callback works with. Borrowed, valid only for the callback duration.
	class ExecInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `InitGlobalInput::SetGlobalState`. Shared with every other scanning thread;
		/// access must be synchronized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The local state set via `InitLocalInput::SetLocalState`, private to this thread.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetLocalState() const -> T & {
			return *static_cast<T *>(GetLocalStateInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The chunk to write the next batch of rows into, with one vector per column declared in bind. It starts out
		/// empty on every invocation: write the rows, then give the batch its row count with `Vector::SetSize` on the
		/// chunk's first vector, which the engine propagates to the others. Leaving it empty ends the scan.
		/// @return A borrowed chunk, valid only for the callback duration.
		auto GetOutputChunk() const -> DataChunk;

		/// How many vectors the output chunk holds; see `InitGlobalInput::GetColumnCount`.
		auto GetColumnCount() const -> idx_t;
		/// Which declared column the output chunk's vector at `index` must be filled with; see
		/// `InitGlobalInput::GetColumnIndex`.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetColumnIndex(idx_t index) const -> idx_t;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		ExecInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetLocalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the progress callback works with. Borrowed, valid only for the callback duration.
	class ProgressInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `InitGlobalInput::SetGlobalState`. This callback runs while the scan is running,
		/// so the state must be read in a thread-safe way.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Reports how far the scan has advanced, as a fraction between 0.0 and 1.0; values outside that range are
		/// clamped. A callback that returns without calling this reports no progress.
		/// @param progress The fraction of the scan that is complete.
		auto SetProgress(double progress) -> void;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		ProgressInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the filter pushdown callback works with. Borrowed, valid only for the callback duration.
	///
	/// The callback runs while the query is optimized, after bind and before any init callback, with the predicates
	/// the query applies to the function's rows. Accepting one with `Accept` is a promise: the engine stops
	/// applying it, so the scan must produce only rows that satisfy it. Predicates left unaccepted are applied by
	/// the engine as usual, so a callback that recognizes nothing can simply return. The optimizer may run the
	/// callback more than once for the same query, each time with the predicates not yet accepted, and never with
	/// none.
	class FilterPushdownInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`, mutable: the same object the init and exec callbacks later
		/// receive, so an accepted predicate can be recorded in it for the scan to apply.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> T & {
			return *static_cast<T *>(GetBindDataInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many predicates are offered. They are combined with `AND`.
		auto GetFilterCount() const -> idx_t;
		/// A predicate: a bound expression evaluating to `BOOLEAN`. Resolve the column references it contains via
		/// `GetColumnIndex`.
		/// @return A borrowed expression, valid only for the callback duration.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetFilter(idx_t index) const -> Expression;
		/// Accepts the predicate at `index`: the function will apply it itself.
		/// @throws InvalidInputException When the index is out of bounds.
		auto Accept(idx_t index) -> void;
		/// How many columns the predicates can refer to: the columns the query reads from the function, which is what
		/// `Expression::GetColumnIndex` indexes.
		auto GetColumnCount() const -> idx_t;
		/// Resolves the index a column reference node reports to the declared column (in `BindInput::AddResultColumn`
		/// order) it refers to.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetColumnIndex(idx_t index) const -> idx_t;

		/// The query's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		FilterPushdownInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the partition data callback works with. Borrowed, valid only for the callback duration.
	///
	/// The callback runs after every batch the exec callback produces, on the thread that produced it. A batch index
	/// must be reported on every call with `SetBatchIndex`, whether or not `RequiresBatchIndex` is true. When
	/// `RequiresPartitionColumns` is true, `SetPartitionValue` must be called once for every index below
	/// `GetPartitionColumnCount`. The engine reads the partition values only when the batch index changes: within one
	/// thread the index must not decrease, and the values may only change together with it.
	class PartitionDataInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `InitGlobalInput::SetGlobalState`. Shared with every other scanning thread;
		/// access must be synchronized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The local state set via `InitLocalInput::SetLocalState`, private to this thread.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetLocalState() const -> T & {
			return *static_cast<T *>(GetLocalStateInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Whether a downstream operator uses the batch index to restore the scan's order.
		auto RequiresBatchIndex() const -> bool;
		/// Whether a downstream operator needs the batch's value for each partitioning column.
		auto RequiresPartitionColumns() const -> bool;
		/// How many partitioning columns are requested; zero unless `RequiresPartitionColumns` is true.
		auto GetPartitionColumnCount() const -> idx_t;
		/// Which declared column (in `BindInput::AddResultColumn` order) the partitioning column at `index` stands for.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetPartitionColumnIndex(idx_t index) const -> idx_t;

		/// Reports the batch's ordering position. Required on every call. Must not decrease across calls on the same
		/// thread, and must change whenever the reported partition values change.
		/// @throws InvalidInputException When the value is out of range.
		auto SetBatchIndex(idx_t batch_index) -> void;
		/// Reports the single value every row of the batch carries for the partitioning column at `index`. The value
		/// is copied and must be of the declared column's type.
		/// @throws InvalidInputException When the index is out of bounds or the type does not match.
		auto SetPartitionValue(idx_t index, const Value &value) -> void;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		PartitionDataInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetLocalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the partitioning callback works with. Borrowed, valid only for the callback duration.
	///
	/// The callback runs on the planning thread, only while the optimizer considers a partitioned aggregate over the
	/// scan, and may run more than once for one query. It must answer deterministically for a given column set. Only
	/// `PartitionInfo::SINGLE_VALUE_PARTITIONS` unlocks the optimization; a callback that returns without calling
	/// `SetPartitionInfo` reports `PartitionInfo::NOT_PARTITIONED`.
	class PartitioningInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many columns the candidate `GROUP BY` set holds.
		auto GetPartitionColumnCount() const -> idx_t;
		/// Which declared column (in `BindInput::AddResultColumn` order) the candidate column at `index` stands for.
		/// @throws InvalidInputException When the index is out of bounds.
		auto GetPartitionColumnIndex(idx_t index) const -> idx_t;

		/// Reports whether, and how, the scan is partitioned by the candidate column set.
		/// @throws InvalidInputException When the value is not one of the enum's declared values.
		auto SetPartitionInfo(PartitionInfo partition_info) -> void;

		/// The query's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		PartitioningInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the claim batch callback works with. Borrowed, valid only for the callback duration. Unstable API.
	///
	/// The callback claims the next batch of work for the thread's local state - typically from the global state,
	/// which is shared with the other scanning threads - and reports whether it claimed one with `SetClaimed`. A
	/// callback that returns without claiming a batch ends the scan for the thread.
	class ClaimBatchInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `InitGlobalInput::SetGlobalState`. Shared with every other scanning thread;
		/// access must be synchronized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The local state set via `InitLocalInput::SetLocalState`, of the thread claiming a batch.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetLocalState() const -> T & {
			return *static_cast<T *>(GetLocalStateInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Reports whether a batch was claimed.
		auto SetClaimed(bool claimed) -> void;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		ClaimBatchInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetLocalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the get bind info callback works with. Borrowed, valid only for the callback duration. Unstable API.
	class GetBindInfoInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData` for the bound call being described.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Attaches an identifier to a result column of the bound call: an INTEGER field id, or a VARCHAR name, by
		/// which a `MultiFileFunction` maps the columns of every file rather than by name.
		/// @param column_index The column, in `BindInput::AddResultColumn` order.
		/// @throws InvalidInputException When the column was not declared, or the identifier is not an INTEGER or
		/// VARCHAR.
		auto SetColumnIdentifier(idx_t column_index, const Value &identifier) -> void;

		/// Attaches an identifier to a field nested inside a result column, addressed by a path of child indexes: a
		/// STRUCT field by its index, the elements of a LIST or ARRAY by 0, the keys of a MAP by 0 and its values by
		/// 1, and a UNION member by its index. See `SetColumnIdentifier`.
		/// @throws InvalidInputException When the column was not declared, or the path does not address a nested
		/// field of it.
		auto SetColumnIdentifier(idx_t column_index, const std::vector<idx_t> &child_path, const Value &identifier)
		    -> void;

		/// Sets an option describing the bound call: a key-value fact about what the call reads. A `MultiFileFunction`
		/// exposes the options as the metadata of the reader of the file. Setting a key again replaces its value.
		auto SetOption(const std::string &key, const Value &value) -> void;

		/// The context of the query the bound call is part of. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		GetBindInfoInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the stats callback works with. Borrowed, valid only for the callback duration.
	///
	/// The column statistics start out unknown, with every kind of value possible, and describe every value the scan
	/// produces for the column. A callback that leaves them unchanged tells the engine nothing.
	class StatsInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `BindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `TableFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Which declared column (in `BindInput::AddResultColumn` order) the statistics describe.
		auto GetColumnIndex() const -> idx_t;

		/// The statistics of the column, for the callback to narrow.
		auto GetResultStats() const -> Stats;

		/// The query's context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		StatsInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Multi-File Function
//----------------------------------------------------------------------------------------------------------------------

/// A table function that reads many files, built on top of a registered `TableFunction` that reads a single one.
/// Unstable API.
///
/// The single-file function takes the file to read as its only positional parameter, of type ANY, and is bound and
/// scanned once for every file that is read. It receives the file as a VARCHAR path, or as a file struct that also
/// carries the options to open the file with: what a file system reported about it while globbing, or the options a
/// caller passed along with it. Open the file with `GetFilePath` and `FileOpenOptions::SetValues` to make use of
/// them. A single-file function that takes a VARCHAR parameter instead only receives the path. The multi-file function
/// adds everything that involves several files: globbing, lists of files, hive partitioning, the `filename` column,
/// `union_by_name` and the like. Every named parameter of the single-file function is also a named parameter of the
/// multi-file function, forwarded as given.
///
/// The single-file function can describe the file it reads in more detail than its columns with a get bind info
/// callback (`TableFunction::SetGetBindInfoCallback`): the field ids of its columns, which the files of the scan are
/// mapped onto one another by, and options, which become the metadata of the reader of the file. With a claim batch
/// callback, the rows of a file keep their order also when several threads scan it.
class MultiFileFunction final : public detail::Handle<MultiFileFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	MultiFileFunction(MultiFileFunction &&) noexcept = default;
	MultiFileFunction &operator=(MultiFileFunction &&) noexcept = default;

	~MultiFileFunction() override;

	/// Creates an empty MultiFileFunction through `factory`. Register it through a `Connection` or an `Extension` of
	/// the factory's database.
	static auto Create(Factory &factory) -> MultiFileFunction;

	/// Sets the function's name, as SQL will call it.
	auto SetName(const std::string &name) & -> MultiFileFunction &;

	/// Sets the name of the registered table function that reads a single file.
	auto SetSingleFileFunction(const std::string &name) & -> MultiFileFunction &;

	/// Sets how the files are referred to in messages, e.g. "Avro". Defaults to the name of the function.
	auto SetReaderType(const std::string &reader_type) & -> MultiFileFunction &;

	/// Sets the extension of the files the function reads, without a leading dot, e.g. "avro". A path naming a
	/// directory then reads the files with that extension inside it.
	auto SetFileExtension(const std::string &extension) & -> MultiFileFunction &;

private:
	explicit MultiFileFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;
};

//----------------------------------------------------------------------------------------------------------------------
// Custom Type
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined type, built up with the setters and made live with `Connection::Register` or `Extension::Register`.
/// Create one through a `Factory` of the database it will be registered in, give it a name and a base type, then
/// register it with `Connection::Register` or `Extension::Register`. The type object may be destroyed after
/// registration; the registered type lives on in the catalog.
///
/// A custom type borrows its base type's internal representation, so the execution engine needs no special handling
/// for it, while staying logically distinct from the base type so it can carry its own casts. Values of it are
/// produced and read with the base type's vector accessors; to hand one back out under the custom name, alias the
/// base type with `LogicalType::WithAlias`.
class CustomType final : public detail::Handle<CustomType> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	CustomType(CustomType &&) noexcept = default;
	CustomType &operator=(CustomType &&) noexcept = default;

	~CustomType() override;

	/// Creates an empty CustomType through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> CustomType;

	/// Sets the type's name, as SQL will refer to it, and as every logical type instance of it carries as its alias.
	auto SetName(const std::string &name) & -> CustomType &;

	/// Sets the type whose representation this type borrows. Must be a fully defined concrete type.
	auto SetBaseType(const LogicalType &type) & -> CustomType &;

private:
	explicit CustomType(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;
};

//----------------------------------------------------------------------------------------------------------------------
// Copy Function
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined file format for `COPY`, built up with the setters and made live with `Connection::Register` or
/// `Extension::Register`. Create one through a `Factory` of the database it will be registered in, describe it (name,
/// callbacks), then register it with `Connection::Register` or `Extension::Register`. The function object may be
/// destroyed after registration; the registered function lives on in the catalog and is reached from SQL with `COPY ...
/// TO 'path' (FORMAT name)` and `COPY table FROM 'path' (FORMAT name)`.
///
/// The two directions are configured separately and a function may implement either or both. The `COPY ... TO` side
/// gathers the rows being written into batches and drives its callbacks in this order: bind (once, during planning),
/// batch size (once, during planning, unless the statement sets `BATCH_SIZE`), init (once per output file), batch
/// (once per batch, possibly on several threads at once), flush (once per prepared batch, never concurrently for the
/// same file) and finalize (once per output file). The `COPY ... FROM` side reads like a table function whose columns
/// are fixed by the target table: bind (once, during planning), init global (once per statement), init local (once
/// per thread), exec (repeatedly, until it produces an empty batch), plus an optional progress hook.
///
/// The callbacks receive their state through the input objects: `SetUserData` plants data readable from every
/// callback of either side, and each side's bind callback may plant bind data readable from that side's later
/// callbacks. A callback reports failure by throwing; the exception surfaces as the query's error.
class CopyFunction final : public detail::Handle<CopyFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	class CopyToBindInput;
	class CopyToBatchSizeInput;
	class CopyToInitInput;
	class CopyToBatchInput;
	class CopyToFlushInput;
	class CopyToFinalizeInput;
	class CopyToStatisticsInput;
	class CopyFromBindInput;
	class CopyFromInitGlobalInput;
	class CopyFromInitLocalInput;
	class CopyFromExecInput;
	class CopyFromProgressInput;

	/// Called once per `COPY ... TO` statement while it is bound. Optional.
	using CopyToBindCallback = void (*)(CopyToBindInput &input);
	/// Called after bind, for a `COPY ... TO` statement that sets no `BATCH_SIZE` itself, to report how many rows a
	/// batch should carry; must call `SetTarget`. Optional: without it, a batch is cut for every chunk of rows sunk,
	/// i.e. a vector at a time.
	using CopyToBatchSizeCallback = void (*)(CopyToBatchSizeInput &input);
	/// Called once per output file before its first batch is prepared. Optional.
	using CopyToInitCallback = void (*)(CopyToInitInput &input);
	/// Called to prepare a batch of rows for writing; may run on several threads at once. Required for the side.
	using CopyToBatchCallback = void (*)(CopyToBatchInput &input);
	/// Called to write a prepared batch to the output; never concurrently for the same file. Required for the side.
	using CopyToFlushCallback = void (*)(CopyToFlushInput &input);
	/// Called once per output file after its last batch has been flushed. Optional.
	using CopyToFinalizeCallback = void (*)(CopyToFinalizeInput &input);
	/// Called once per output file after its finalize callback, to report the statistics of the written file when the
	/// statement asks for them (`RETURN_STATS`). Optional: without it, `RETURN_STATS` is rejected. Unstable API.
	using CopyToStatisticsCallback = void (*)(CopyToStatisticsInput &input);

	/// Called once per `COPY ... FROM` statement while it is bound. Required for the side.
	using CopyFromBindCallback = void (*)(CopyFromBindInput &input);
	/// Called once per statement at the start of execution. Optional.
	using CopyFromInitGlobalCallback = void (*)(CopyFromInitGlobalInput &input);
	/// Called once per thread reading the file. Optional.
	using CopyFromInitLocalCallback = void (*)(CopyFromInitLocalInput &input);
	/// Called to fill the next batch of rows; leaving the chunk empty ends the read. Required for the side.
	using CopyFromExecCallback = void (*)(CopyFromExecInput &input);
	/// Called on demand during execution to report progress. Optional.
	using CopyFromProgressCallback = void (*)(CopyFromProgressInput &input);

	CopyFunction(CopyFunction &&) noexcept = default;
	CopyFunction &operator=(CopyFunction &&) noexcept = default;

	~CopyFunction() override;

	/// Creates an empty CopyFunction through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> CopyFunction;

	/// Sets the function's name: the format SQL selects it with, as in `COPY ... TO 'path' (FORMAT name)`.
	auto SetName(const std::string &name) & -> CopyFunction &;

	/// Sets the extension of the files the function writes, without the leading dot, e.g. `parquet`. DuckDB uses it
	/// when it names files itself, as for `PARTITION_BY`; without it such names end in a bare dot.
	/// @throws InvalidInputException When the extension starts with a dot.
	auto SetFileExtension(const std::string &extension) & -> CopyFunction &;

	/// Constructs user data of type `T`, carried by the registered function and freed at engine teardown; read it from
	/// any callback via the inputs' `GetUserData<T>`. Consumed by registration: set it again before
	/// re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> CopyFunction & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetCopyToBindCallback(CopyToBindCallback callback) & -> CopyFunction &;
	auto SetCopyToBatchSizeCallback(CopyToBatchSizeCallback callback) & -> CopyFunction &;
	auto SetCopyToInitCallback(CopyToInitCallback callback) & -> CopyFunction &;
	auto SetCopyToBatchCallback(CopyToBatchCallback callback) & -> CopyFunction &;
	auto SetCopyToFlushCallback(CopyToFlushCallback callback) & -> CopyFunction &;
	auto SetCopyToFinalizeCallback(CopyToFinalizeCallback callback) & -> CopyFunction &;
	auto SetCopyToStatisticsCallback(CopyToStatisticsCallback callback) & -> CopyFunction &;

	auto SetCopyFromBindCallback(CopyFromBindCallback callback) & -> CopyFunction &;
	auto SetCopyFromInitGlobalCallback(CopyFromInitGlobalCallback callback) & -> CopyFunction &;
	auto SetCopyFromInitLocalCallback(CopyFromInitLocalCallback callback) & -> CopyFunction &;
	auto SetCopyFromExecCallback(CopyFromExecCallback callback) & -> CopyFunction &;
	auto SetCopyFromProgressCallback(CopyFromProgressCallback callback) & -> CopyFunction &;

private:
	explicit CopyFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	CopyToBindCallback copy_to_bind_callback = nullptr;
	CopyToBatchSizeCallback copy_to_batch_size_callback = nullptr;
	CopyToInitCallback copy_to_init_callback = nullptr;
	CopyToBatchCallback copy_to_batch_callback = nullptr;
	CopyToFlushCallback copy_to_flush_callback = nullptr;
	CopyToFinalizeCallback copy_to_finalize_callback = nullptr;
	CopyToStatisticsCallback copy_to_statistics_callback = nullptr;
	CopyFromBindCallback copy_from_bind_callback = nullptr;
	CopyFromInitGlobalCallback copy_from_init_global_callback = nullptr;
	CopyFromInitLocalCallback copy_from_init_local_callback = nullptr;
	CopyFromExecCallback copy_from_exec_callback = nullptr;
	CopyFromProgressCallback copy_from_progress_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the `COPY ... TO` bind callback works with. Borrowed, valid only for the callback duration.
	class CopyToBindInput {
		friend detail::HandleFactory;

	public:
		/// Constructs bind data of type `T`, owned by the bound statement and readable from every later `COPY ... TO`
		/// callback via `GetBindData<T>`. The engine compares bind data when it compares statements: by `operator==`
		/// when `T` has one, by identity otherwise.
		template <class T, class... ARGS>
		void SetBindData(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetBindDataInternal(ptr, detail::SelectEquals<T>(), detail::TypedDelete<T>);
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The path the statement writes to, as written. The path of each file actually written is only known to the
		/// init callback.
		auto GetFilePath() const -> std::string;

		/// How many columns are being written: the columns of every batch's rows. Valid indices for `GetColumnName`
		/// and `GetColumnType` are [0, GetColumnCount()).
		auto GetColumnCount() const -> idx_t;

		/// One column's name.
		/// @param index Column index in [0, GetColumnCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetColumnName(idx_t index) const -> std::string;

		/// One column's type.
		/// @param index Column index in [0, GetColumnCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetColumnType(idx_t index) const -> LogicalType;

		/// How many options the statement passed to the function, ordered by name. The engine's own options (e.g.
		/// `USE_TMP_FILE`) are not included. Valid indices for the option accessors are [0, GetOptionCount()).
		auto GetOptionCount() const -> idx_t;

		/// One option's name, a SQL identifier matched case-insensitively.
		/// @param index Option index in [0, GetOptionCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetOptionName(idx_t index) const -> std::string;

		/// One option's value: the value itself for `DELIM ','`, the BOOLEAN true for a bare option such as `HEADER`,
		/// and a tuple (an unnamed STRUCT with one field per element, in order) for a parenthesized list.
		/// @param index Option index in [0, GetOptionCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetOptionValue(idx_t index) const -> Value;

		/// The binding context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToBindInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetBindDataInternal(void *data, bool (*equals)(void *a, void *b), void (*destructor)(void *));
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... TO` batch size callback works with. Borrowed, valid only for the callback duration.
	class CopyToBatchSizeInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `CopyToBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Reports how many rows a batch should carry, as the target the engine cuts batches at. Not calling this
		/// leaves the engine's default in effect. A batch may still be smaller (the last one of a file, or when
		/// `BATCH_SIZE_BYTES` cuts it first).
		/// @param rows The number of rows a batch should carry; must be greater than 0.
		auto SetTarget(idx_t rows) -> void;

		/// The binding context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToBatchSizeInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... TO` init callback works with. Borrowed, valid only for the callback duration.
	class CopyToInitInput {
		friend detail::HandleFactory;

	public:
		/// Constructs init data of type `T`, owned by the file being written and readable from the batch, flush and
		/// finalize callbacks of that file via `GetInitData<T>`. Batches may be prepared on several threads at once, so
		/// the batch callback must synchronize its own access to it.
		template <class T, class... ARGS>
		void SetInitData(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetInitDataInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `CopyToBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The path of the file being written, after the engine's own rewrites (e.g. a temporary name while the file
		/// is being written, or a per-partition path).
		auto GetFilePath() const -> std::string;

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToInitInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetInitDataInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... TO` batch callback works with. Borrowed, valid only for the callback duration.
	class CopyToBatchInput {
		friend detail::HandleFactory;

	public:
		/// Constructs batch data of type `T`: the prepared form of the batch, handed to the flush callback via
		/// `CopyToFlushInput::GetBatchData<T>` and freed once the batch has been flushed.
		template <class T, class... ARGS>
		void SetBatchData(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetBatchDataInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `CopyToBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The init data set via `CopyToInitInput::SetInitData` for the file this batch belongs to.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetInitData() const -> T & {
			return *static_cast<T *>(GetInitDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Takes ownership of the rows of the batch: one column per column reported by
		/// `CopyToBindInput::GetColumnCount`, in the same order. The batch can only be taken once; a batch that is
		/// never taken is released when the callback returns. It may be kept beyond the callback, e.g. moved into the
		/// batch data via `SetBatchData`.
		/// @throws InvalidInputException When the batch was already taken.
		auto TakeBatch() -> ColumnDataCollection;

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToBatchInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetBatchDataInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetInitDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... TO` flush callback works with. Borrowed, valid only for the callback duration.
	class CopyToFlushInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `CopyToBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The init data set via `CopyToInitInput::SetInitData` for the file this batch belongs to.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetInitData() const -> T & {
			return *static_cast<T *>(GetInitDataInternal());
		}

		/// The batch data set via `CopyToBatchInput::SetBatchData` for the batch being flushed.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBatchData() const -> T & {
			return *static_cast<T *>(GetBatchDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToFlushInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetInitDataInternal() const;
		void *GetBatchDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... TO` finalize callback works with. Borrowed, valid only for the callback duration.
	class CopyToFinalizeInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `CopyToBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The init data set via `CopyToInitInput::SetInitData` for the file being finalized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetInitData() const -> T & {
			return *static_cast<T *>(GetInitDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToFinalizeInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetInitDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... TO` statistics callback works with. Borrowed, valid only for the callback duration. Unstable
	/// API.
	class CopyToStatisticsInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `CopyToBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The init data set via `CopyToInitInput::SetInitData` for the file being reported.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetInitData() const -> T & {
			return *static_cast<T *>(GetInitDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Reports how many rows were written to the file. Reported as 0 when not set.
		auto SetRowCount(idx_t row_count) -> void;

		/// Reports the size of the file in bytes. Reported as 0 when not set.
		auto SetFileSize(idx_t file_size_bytes) -> void;

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyToStatisticsInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetInitDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... FROM` bind callback works with. Borrowed, valid only for the callback duration.
	class CopyFromBindInput {
		friend detail::HandleFactory;

	public:
		/// Constructs bind data of type `T`, owned by the bound statement and readable from every later
		/// `COPY ... FROM` callback via `GetBindData<T>`. The engine compares bind data when it compares statements:
		/// by `operator==` when `T` has one, by identity otherwise.
		template <class T, class... ARGS>
		void SetBindData(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetBindDataInternal(ptr, detail::SelectEquals<T>(), detail::TypedDelete<T>);
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The path the statement reads from, as written. The engine expands no globs and checks nothing; both are
		/// left to the function.
		auto GetFilePath() const -> std::string;

		/// How many columns the target table expects: the columns every batch the exec callback produces must carry,
		/// in this order. Valid indices for `GetColumnName` and `GetColumnType` are [0, GetColumnCount()).
		auto GetColumnCount() const -> idx_t;

		/// One column's name.
		/// @param index Column index in [0, GetColumnCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetColumnName(idx_t index) const -> std::string;

		/// One column's type.
		/// @param index Column index in [0, GetColumnCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetColumnType(idx_t index) const -> LogicalType;

		/// How many options the statement passed to the function, ordered by name; every option other than `FORMAT`.
		/// Valid indices for the option accessors are [0, GetOptionCount()).
		auto GetOptionCount() const -> idx_t;

		/// One option's name, a SQL identifier matched case-insensitively.
		/// @param index Option index in [0, GetOptionCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetOptionName(idx_t index) const -> std::string;

		/// One option's value: the value itself for `DELIM ','`, the BOOLEAN true for a bare option such as `HEADER`,
		/// and a tuple (an unnamed STRUCT with one field per element, in order) for a parenthesized list.
		/// @param index Option index in [0, GetOptionCount()).
		/// @throws InvalidInputException When the index is out of range.
		auto GetOptionValue(idx_t index) const -> Value;

		/// Hints how many rows the read will produce, for the optimizer. Producing a different number of rows is not
		/// an error.
		/// @param cardinality The estimated row count.
		/// @param is_exact Whether the estimate is exact, which also makes it an upper bound.
		auto SetCardinality(idx_t cardinality, bool is_exact) -> void;

		/// The binding context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyFromBindInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetBindDataInternal(void *data, bool (*equals)(void *a, void *b), void (*destructor)(void *));
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... FROM` global init callback works with. Borrowed, valid only for the callback duration.
	class CopyFromInitGlobalInput {
		friend detail::HandleFactory;

	public:
		/// Constructs global state of type `T`, shared by every thread reading the file and readable from the local
		/// init, exec and progress callbacks. Since every thread sees the same object, the function must synchronize
		/// its own access to it.
		template <class T, class... ARGS>
		void SetGlobalState(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetGlobalStateInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `CopyFromBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Caps how many threads may read the file in parallel. Defaults to 1, a single-threaded read. The engine
		/// creates at most this many local states, and may use fewer.
		/// @param max_threads The maximum thread count. Must be at least 1.
		auto SetMaxThreads(idx_t max_threads) -> void;

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyFromInitGlobalInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetGlobalStateInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... FROM` local init callback works with. Borrowed, valid only for the callback duration.
	class CopyFromInitLocalInput {
		friend detail::HandleFactory;

	public:
		/// Constructs local state of type `T`, owned by this reading thread and readable from the exec callback via
		/// `CopyFromExecInput::GetLocalState<T>`. No other thread observes it, so it needs no synchronization.
		template <class T, class... ARGS>
		void SetLocalState(ARGS &&... args) {
			auto ptr = new T(std::forward<ARGS>(args)...);
			SetLocalStateInternal(ptr, detail::TypedDelete<T>);
		}

		/// The bind data set via `CopyFromBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `CopyFromInitGlobalInput::SetGlobalState`, typically to claim this thread's share
		/// of the work from it. Shared with every other reading thread; access must be synchronized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The query context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyFromInitLocalInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void SetLocalStateInternal(void *data, void (*destructor)(void *));
		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... FROM` exec callback works with. Borrowed, valid only for the callback duration.
	class CopyFromExecInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `CopyFromBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `CopyFromInitGlobalInput::SetGlobalState`. Shared with every other reading thread;
		/// access must be synchronized.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The local state set via `CopyFromInitLocalInput::SetLocalState`, private to this thread.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetLocalState() const -> T & {
			return *static_cast<T *>(GetLocalStateInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The chunk to write the next batch of rows into, with one vector per column reported by
		/// `CopyFromBindInput::GetColumnCount`. It starts out empty on every invocation: write the rows, then give the
		/// batch its row count with `Vector::SetSize` on the chunk's first vector, which the engine propagates to the
		/// others. Leaving it empty ends the read.
		/// @return A borrowed chunk, valid only for the callback duration.
		auto GetOutputChunk() const -> DataChunk;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyFromExecInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetLocalStateInternal() const;
		void *GetUserDataInternal() const;
	};

	/// What the `COPY ... FROM` progress callback works with. Borrowed, valid only for the callback duration.
	class CopyFromProgressInput {
		friend detail::HandleFactory;

	public:
		/// The bind data set via `CopyFromBindInput::SetBindData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetBindData() const -> const T & {
			return *static_cast<const T *>(GetBindDataInternal());
		}

		/// The global state set via `CopyFromInitGlobalInput::SetGlobalState`. This callback runs while the read is
		/// running, so the state must be read in a thread-safe way.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetGlobalState() const -> T & {
			return *static_cast<T *>(GetGlobalStateInternal());
		}

		/// The user data set via `CopyFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// Reports how far the read has advanced, as a fraction between 0.0 and 1.0; values outside that range are
		/// clamped. A callback that returns without calling this reports no progress.
		/// @param progress The fraction of the read that is complete.
		auto SetProgress(double progress) -> void;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		CopyFromProgressInput(void *args, void *context)
		    : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetBindDataInternal() const;
		void *GetGlobalStateInternal() const;
		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Cast Function
//----------------------------------------------------------------------------------------------------------------------

/// The mode a cast runs in, which decides what a conversion failure means.
enum class CastMode : uint8_t {
	/// A regular cast. Throwing from the exec callback aborts the query.
	NORMAL = 0,
	/// A "try" cast (SQL TRY_CAST). The rows the callback could not convert should be left NULL in the output; an
	/// exception thrown anyway is swallowed and those rows stay NULL.
	TRY = 1,
};

/// A user-defined cast between two types, built up with the setters and made live with `Connection::Register` or
/// `Extension::Register`. Create one through a `Factory` of the database it will be registered in, describe it (source
/// type, target type, exec callback), then register it with `Connection::Register` or `Extension::Register`. The
/// function object may be destroyed after registration; the registered cast lives on.
///
/// A cast is keyed by its (source, target) type pair rather than by a name, and is reached from SQL through CAST and
/// TRY_CAST -- and, when it declares a non-negative implicit cast cost, through the binder converting argument types
/// on its own. The exec callback converts a whole batch at a time; whether a per-row failure aborts the query or
/// becomes a NULL follows from `ExecInput::GetMode`.
class CastFunction final : public detail::Handle<CastFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	class ExecInput;

	/// Called for every batch of values; must fill the output vector. Required.
	using ExecCallback = void (*)(ExecInput &input);

	CastFunction(CastFunction &&) noexcept = default;
	CastFunction &operator=(CastFunction &&) noexcept = default;

	~CastFunction() override;

	/// Creates an empty CastFunction through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> CastFunction;

	/// Sets the type the cast converts from. Must be a fully defined concrete type.
	auto SetSourceType(const LogicalType &type) & -> CastFunction &;

	/// Sets the type the cast converts to. Must be a fully defined concrete type.
	auto SetTargetType(const LogicalType &type) & -> CastFunction &;

	/// Sets what it costs to apply this cast implicitly. The binder uses the cost to choose between candidate
	/// implicit casts: a lower non-negative cost makes this cast more likely to be picked. Built-in widening casts
	/// sit in the [0, 20] range. A negative cost -- the default -- keeps the cast out of implicit conversion
	/// entirely, so it is reached only through an explicit CAST or TRY_CAST.
	/// @param cost The cost, or a negative value to disable implicit casting.
	auto SetImplicitCastCost(int64_t cost) & -> CastFunction &;

	/// Constructs user data of type `T`, carried by the registered cast and freed at engine teardown; read it from the
	/// exec callback via `ExecInput::GetUserData<T>`. Consumed by registration: set it again before
	/// re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> CastFunction & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetExecCallback(ExecCallback callback) & -> CastFunction &;

private:
	explicit CastFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	ExecCallback exec_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the exec callback works with. Borrowed, valid only for the callback duration.
	class ExecInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `CastFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// How many rows this execution must convert: the callback writes exactly this many entries to the output
		/// vector. May be less than a full vector; a constant input is converted as a single row and the engine
		/// expands the result.
		auto GetRowCount() const -> idx_t;

		/// The input vector holding the values to convert.
		auto GetInput() const -> Vector;

		/// The output vector to fill.
		auto GetOutput() const -> Vector;

		/// The mode this cast runs in. In `CastMode::TRY` a failed row should be left NULL rather than reported by
		/// throwing.
		auto GetMode() const -> CastMode;

		/// The execution context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		ExecInput(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Pragma Function
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined pragma, called with the `PRAGMA` statement. Create one through a `Factory` of the database it will be
/// registered in, describe it (name, signature, callback), then register it with `Connection::Register` or
/// `Extension::Register`. The function object may be destroyed after registration; the registered pragma lives on in
/// the catalog.
///
/// A pragma takes constant arguments and is one of two kinds, decided by the callback it is given: a query pragma
/// returns SQL that runs in place of the `PRAGMA` statement, and an exec pragma runs for its side effects and produces
/// no rows. Set exactly one of the two callbacks. A pragma without parameters is called as `PRAGMA name`, one with
/// parameters as `PRAGMA name(...)`. A callback reports failure by throwing; the exception surfaces as the statement's
/// error.
class PragmaFunction final : public detail::Handle<PragmaFunction> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;

public:
	class QueryInput;
	class ExecInput;

	/// Called before the `PRAGMA` statement is bound; returns the SQL to run in its place, which may hold several
	/// statements.
	using QueryCallback = std::string (*)(QueryInput &input);
	/// Called when the `PRAGMA` statement executes.
	using ExecCallback = void (*)(ExecInput &input);

	PragmaFunction(PragmaFunction &&) noexcept = default;
	PragmaFunction &operator=(PragmaFunction &&) noexcept = default;

	~PragmaFunction() override;

	/// Creates an empty PragmaFunction through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> PragmaFunction;

	/// Sets the pragma's name, as the `PRAGMA` statement calls it.
	auto SetName(const std::string &name) & -> PragmaFunction &;
	/// The documentation of the pragma, borrowed for in-place mutation.
	auto GetDocs() -> FunctionDocs;

	/// Calls `configure` with the documentation of the pragma, borrowed for in-place mutation.
	template <class F>
	auto WithDocs(F &&configure) & -> PragmaFunction & {
		auto docs = GetDocs();
		configure(docs);
		return *this;
	}

	/// The pragma's signature, borrowed for in-place mutation. A pragma produces no value, so registration rejects a
	/// signature with a return type.
	auto GetSignature() -> FunctionSignature;

	/// Calls `configure` with the pragma's signature, borrowed for in-place mutation.
	template <class F>
	auto WithSignature(F &&configure) & -> PragmaFunction & {
		auto sig = GetSignature();
		configure(sig);
		return *this;
	}

	/// Constructs user data of type `T`, carried by the registered pragma and freed at engine teardown; read it from
	/// the callback via the input's `GetUserData<T>`. Consumed by registration: set it again before re-registering.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> PragmaFunction & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	/// Makes this a query pragma. Registration fails when an exec callback is set as well.
	auto SetQueryCallback(QueryCallback callback) & -> PragmaFunction &;
	/// Makes this an exec pragma. Registration fails when a query callback is set as well.
	auto SetExecCallback(ExecCallback callback) & -> PragmaFunction &;

private:
	explicit PragmaFunction(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	QueryCallback query_callback = nullptr;
	ExecCallback exec_callback = nullptr;
	detail::UserData user_data;

public:
	/// What the query callback works with: the arguments of the `PRAGMA` statement. Borrowed, valid only for the
	/// callback duration.
	class QueryInput final : public FunctionBindInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `PragmaFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

	private:
		QueryInput(void *args, void *context) : FunctionBindInput(args, context) {
		}

		/// Pragmas have no bind data
		using FunctionBindInput::SetBindData;

		void *GetUserDataInternal() const;
	};

	/// What the exec callback works with: the arguments of the `PRAGMA` statement. Borrowed, valid only for the
	/// callback duration.
	class ExecInput final : public FunctionBindInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `PragmaFunction::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

	private:
		ExecInput(void *args, void *context) : FunctionBindInput(args, context) {
		}

		/// Pragmas have no bind data
		using FunctionBindInput::SetBindData;

		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Macro Function
//----------------------------------------------------------------------------------------------------------------------

/// A function defined in SQL: a call is replaced with the body, an expression for a scalar macro and a query for a
/// table macro, which refers to the parameters by name. Create one through a `Factory` of the database it will be
/// registered in, describe it (name, signature, body), then register it with `Extension::Register`; outside an
/// extension, define a macro with SQL `CREATE MACRO` instead. The function object may be destroyed after
/// registration; the registered macro lives on in the catalog.
///
/// A macro's parameters can always be passed by position or by name, so the signature may only hold standard
/// parameters, optionally with a default value. A parameter of type ANY is untyped. The result follows from the body,
/// so the signature cannot set a return type.
class MacroFunction final : public detail::Handle<MacroFunction> {
	friend detail::HandleFactory;
	friend class Extension;

public:
	MacroFunction(MacroFunction &&) noexcept = default;
	MacroFunction &operator=(MacroFunction &&) noexcept = default;

	~MacroFunction() override;

	/// Creates an empty scalar macro through `factory`: its body is an expression, and it is called like a scalar
	/// function. Register it through an `Extension` of the factory's database.
	static auto CreateScalar(Factory &factory) -> MacroFunction;
	/// Creates an empty table macro through `factory`: its body is a SELECT query, and it is called like a table
	/// function. Register it through an `Extension` of the factory's database.
	static auto CreateTable(Factory &factory) -> MacroFunction;

	/// Sets the macro's name, as SQL calls it.
	auto SetName(const std::string &name) & -> MacroFunction &;
	/// Sets the body, as SQL text: an expression for a scalar macro, a SELECT query for a table macro. Parsed at
	/// registration; names in it are resolved when the macro is called.
	auto SetBody(std::string_view body) & -> MacroFunction &;
	/// The documentation of the macro, borrowed for in-place mutation.
	auto GetDocs() -> FunctionDocs;

	/// Calls `configure` with the documentation of the macro, borrowed for in-place mutation.
	template <class F>
	auto WithDocs(F &&configure) & -> MacroFunction & {
		auto docs = GetDocs();
		configure(docs);
		return *this;
	}

	/// The macro's signature, borrowed for in-place mutation.
	auto GetSignature() -> FunctionSignature;

	/// Calls `configure` with the macro's signature, borrowed for in-place mutation.
	template <class F>
	auto WithSignature(F &&configure) & -> MacroFunction & {
		auto sig = GetSignature();
		configure(sig);
		return *this;
	}

private:
	explicit MacroFunction(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Log Sink
//----------------------------------------------------------------------------------------------------------------------

/// The scope a log entry was written in, which decides which of its ids are present.
enum class LogScope : uint8_t {
	/// Written for the database as a whole, carrying no connection, transaction or query id.
	DATABASE = 10,
	/// Written by a connection, carrying its connection id and, inside one, the transaction and query ids.
	CONNECTION = 20,
	/// Written by a thread executing a query, carrying the thread id next to the connection's ids.
	THREAD = 30,
};

/// Code of your own that receives DuckDB's log entries. Create one through a `Factory` of the database it will be
/// registered in, give it a name and a write callback, then register it with `Instance::Register` or
/// `Extension::Register`, which makes it available to the logging configuration under its name; once the
/// configuration directs entries to it (currently with `SET logging_storage = '<name>'`), the write callback receives
/// them. A log sink only receives entries: they cannot be read back through `duckdb_logs`.
///
/// Which entries a sink receives, and when and on which thread its callbacks run, are up to DuckDB: the callbacks must
/// return quickly, must be safe to run concurrently and on any thread, and must not log or change the logging
/// configuration. An exception a callback throws drops the entry; it never fails the operation that logged.
class LogSink final : public detail::Handle<LogSink> {
	friend detail::HandleFactory;
	friend class Instance;
	friend class Extension;

public:
	class Entry;
	class FlushInput;

	/// Called for every entry to store. Required.
	using WriteCallback = void (*)(Entry &entry);
	/// Called when DuckDB flushes its log, so entries the sink holds on to can be written out. When is up to DuckDB.
	/// Optional.
	using FlushCallback = void (*)(FlushInput &input);

	LogSink(LogSink &&) noexcept = default;
	LogSink &operator=(LogSink &&) noexcept = default;

	~LogSink() override;

	/// Creates an empty LogSink through `factory`. Register it through an `Instance` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> LogSink;

	/// Sets the name the logging configuration refers to the sink by, matched case-insensitively.
	auto SetName(const std::string &name) & -> LogSink &;

	/// Constructs user data of type `T`, carried by the registered sink and freed at database teardown; read it
	/// from the callbacks via `GetUserData<T>`. Consumed by registration.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> LogSink & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

	auto SetWriteCallback(WriteCallback callback) & -> LogSink &;
	auto SetFlushCallback(FlushCallback callback) & -> LogSink &;

private:
	explicit LogSink(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	WriteCallback write_callback = nullptr;
	FlushCallback flush_callback = nullptr;
	detail::UserData user_data;

public:
	/// One log entry, borrowed for the duration of the write callback.
	class Entry {
		friend detail::HandleFactory;

	public:
		/// The user data set via `LogSink::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// When the entry was written.
		auto GetTimestamp() const -> timestamp_tz_t;
		/// The severity of the entry.
		auto GetLevel() const -> LogLevel;
		/// The log type the entry was written under, e.g. `QueryLog`. Valid only for the callback duration.
		auto GetLogType() const -> std::string_view;
		/// The message. Valid only for the callback duration.
		auto GetLogMessage() const -> std::string_view;
		/// The scope the entry was written in, which decides which of the ids below are present.
		auto GetScope() const -> LogScope;
		/// The ids `duckdb_logs` reports for the entry, each absent when the entry's scope has none. The thread id
		/// identifies the thread that logged, which need not be the thread the write callback runs on.
		auto GetConnectionId() const -> std::optional<idx_t>;
		auto GetTransactionId() const -> std::optional<idx_t>;
		auto GetQueryId() const -> std::optional<idx_t>;
		auto GetThreadId() const -> std::optional<idx_t>;

	private:
		Entry(void *info, void *user_data, int64_t timestamp, LogLevel level, LogScope scope, std::string_view type,
		      std::string_view message)
		    : info(info), user_data(user_data), timestamp(timestamp), level(level), scope(scope), type(type),
		      message(message) {
		}

		void *info;
		void *user_data;
		int64_t timestamp;
		LogLevel level;
		LogScope scope;
		std::string_view type;
		std::string_view message;

		void *GetUserDataInternal() const;
	};

	/// What the flush callback works with. Borrowed, valid only for the callback duration.
	class FlushInput {
		friend detail::HandleFactory;

	public:
		/// The user data set via `LogSink::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

	private:
		explicit FlushInput(void *user_data) : user_data(user_data) {
		}

		void *user_data;

		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// File System
//----------------------------------------------------------------------------------------------------------------------

/// How `FileSystem::OpenFile` opens a file. Not a bitmask -- apply these one at a time with
/// `FileOpenOptions::SetFlag`, or pass several as a braced list to `FileSystem::OpenFile`.
enum class FileFlags : uint8_t {
	/// Not a flag. The zero value, so an uninitialized variable does not name a behaviour; applying it is an error.
	INVALID = 0,
	/// Open the file with "read" capabilities.
	READ = 1,
	/// Open the file with "write" capabilities.
	WRITE = 2,
	/// Create the file if it does not exist, and open it as it is if it does.
	/// The FILE_ prefix matches the engine's flag names and keeps CREATE clear of the `<windows.h>` macro.
	FILE_CREATE = 3,
	/// Create the file if it does not exist, and truncate it to empty if it does. To fail instead of truncating,
	/// combine `FILE_CREATE` with `EXCLUSIVE_CREATE`.
	FILE_CREATE_NEW = 4,
	/// Open the file in "append" mode.
	APPEND = 5,
	/// Fail if the file already exists. A modifier on `FILE_CREATE`, and meaningless without it.
	EXCLUSIVE_CREATE = 6,
	/// The file will be read and written at explicit offsets from several threads at once. Pass it whenever `ReadAt`
	/// or `WriteAt` are used concurrently, so that a file system which would otherwise assume sequential access does
	/// not.
	PARALLEL_ACCESS = 7,
	/// Read the file through DuckDB's external file cache, which keeps the bytes that were read in memory for later
	/// reads - also by later queries. Whether a local file is cached is decided by the `cache_local_files` setting.
	/// Only for reading. Unstable API.
	EXTERNAL_FILE_CACHE = 8,
};

/// An open file, obtained from `FileSystem::OpenFile`. Closes on destruction.
/// Only usable while the `FileSystem` it came from is still valid.
class FileHandle final : public detail::Handle<FileHandle> {
	friend detail::HandleFactory;

public:
	FileHandle(FileHandle &&) noexcept = default;
	FileHandle &operator=(FileHandle &&) noexcept = default;

	~FileHandle() override;

	/// Flushes buffered writes to persistent storage, which is what makes them durable across a crash. Closing or
	/// destroying the handle flushes as well.
	void Sync();

	/// Closes the file, releasing its operating-system resources. The handle stays valid but can no longer be used
	/// to read, write or seek.
	void Close();

	/// Moves the read/write position to an absolute byte offset from the start of the file. Seeking past the end is
	/// allowed; reading from there yields nothing.
	void Seek(idx_t position);

	/// The current read/write position, as a byte offset from the start of the file.
	auto Tell() const -> idx_t;

	/// The total size of the file in bytes.
	auto Size() const -> idx_t;

	/// Reads up to `size` bytes from the current position, advancing it by however many were read.
	/// @return How many bytes were read. Fewer than asked for is normal at the end of the file, and zero means
	/// there is nothing left; neither is an error.
	auto Read(void *buffer, idx_t size) -> idx_t;

	/// Writes up to `size` bytes at the current position, advancing it by however many were written.
	/// @return How many bytes were written.
	auto Write(const void *buffer, idx_t size) -> idx_t;

	/// Reads exactly `size` bytes from `location`, leaving the file's position alone. Unlike `Read`, a short read is
	/// an error rather than a result, so there is no count to return. Safe to call from several threads at once when
	/// the file was opened with `FileFlags::PARALLEL_ACCESS`.
	/// @throws Exception When the file ends before `size` bytes have been read.
	void ReadAt(void *buffer, idx_t size, idx_t location);

	/// Writes exactly `size` bytes at `location`, leaving the file's position alone and extending the file when the
	/// offset is past its end. Safe to call from several threads at once when the file was opened with
	/// `FileFlags::PARALLEL_ACCESS` and the threads write disjoint ranges.
	void WriteAt(const void *buffer, idx_t size, idx_t location);

private:
	explicit FileHandle(void *impl);
};

/// How a file is opened: the flags, plus any values the file system handling the path cares about.
/// Created from the `FileSystem` it will be used with, and reusable across any number of opens.
class FileOpenOptions final : public detail::Handle<FileOpenOptions> {
	friend detail::HandleFactory;

public:
	FileOpenOptions(FileOpenOptions &&) noexcept = default;
	FileOpenOptions &operator=(FileOpenOptions &&) noexcept = default;

	~FileOpenOptions() override;

	/// Creates an empty set of options for `fs`. Flags must be set before they can open anything.
	static auto Create(const FileSystem &fs) -> FileOpenOptions;

	/// Applies one flag. Additive, and applying the same flag twice is harmless; at least one flag is required
	/// before the options can open anything. There is no way to take a flag back -- build a fresh set instead.
	/// @throws InvalidInputException When the value is `FileFlags::INVALID` or not a flag at all.
	auto SetFlag(FileFlags flag) & -> FileOpenOptions &;

	/// Attaches a named value, a hint for whichever file system ends up handling the path. What a name means is that
	/// file system's business, and one it does not recognize is ignored. Setting the same name again replaces it.
	auto SetValue(std::string_view name, const Value &value) & -> FileOpenOptions &;

	/// Attaches the options a file is specified with, as table functions that read files take it: a STRUCT holding
	/// the path of the file in its `filename` field, and an option to open the file with in every other field - e.g.
	/// `{'filename': 'f.parquet', 'file_size': 42}`. A field set to NULL is an option that was not specified. A
	/// VARCHAR path carries no options, and attaches nothing. See `GetFilePath`.
	/// @param file The file, a VARCHAR path or a file struct.
	/// @throws InvalidInputException When the file is neither, or a file struct has no VARCHAR `filename` field.
	auto SetValues(const Value &file) & -> FileOpenOptions &;

private:
	explicit FileOpenOptions(void *impl);
};

/// The path of a file, as table functions that read files take it: a VARCHAR path, or a STRUCT holding the path in
/// its `filename` field and the options to open the file with in its other fields. See `FileOpenOptions::SetValues`.
/// @param file The file, a VARCHAR path or a file struct.
/// @throws InvalidInputException When the file is neither, or a file struct has no VARCHAR `filename` field.
auto GetFilePath(const Value &file) -> std::string;

/// The file system DuckDB itself reads and writes through, so files open the way the engine would open them --
/// including through virtual and remote file systems registered by other extensions.
/// Borrowed from a `Context` or `Connection`, and valid only for as long as that is.
class FileSystem final : public detail::Handle<FileSystem> {
	friend detail::HandleFactory;

public:
	FileSystem(FileSystem &&) noexcept = default;
	FileSystem &operator=(FileSystem &&) noexcept = default;

	~FileSystem() override;

	/// Opens a file with nothing but flags, which is what most opens need.
	/// @param path The path to open, routed the way the engine would route it.
	/// @param flags How to open it, e.g. `{FileFlags::WRITE, FileFlags::FILE_CREATE}`; at least one is required.
	/// @throws Exception When the file cannot be opened.
	auto OpenFile(const std::string &path, std::initializer_list<FileFlags> flags) const -> FileHandle;

	/// Opens a file with a prepared set of options, for when the file system needs values as well as flags.
	/// @throws Exception When the file cannot be opened, or the options carry no flags.
	auto OpenFile(const std::string &path, const FileOpenOptions &options) const -> FileHandle;

	/// Creates an empty set of options for this file system, the same as `FileOpenOptions::Create(*this)`.
	auto CreateOpenOptions() const -> FileOpenOptions;

	/// Whether a file exists at `path`, routed the way `OpenFile` routes it. A directory is not a file.
	auto Exists(const std::string &path) const -> bool;
	/// Whether `path` names a remote file, e.g. under `s3://` or `https://`. Decided from the path alone, so it
	/// answers even when the extension providing the remote file system is not loaded.
	auto IsRemote(const std::string &path) const -> bool;
	/// `path` expanded as `OpenFile` expands it: a leading `~` becomes the configured home directory.
	auto Expand(const std::string &path) const -> std::string;

	/// The paths of the files matching a glob pattern, e.g. `data/*.csv`, in the order the file system lists them.
	/// A pattern without glob characters matches the file it names, if there is one.
	auto Glob(const std::string &pattern) const -> std::vector<std::string>;
	/// Moves, or renames, a file. Whether an existing file at `target` is replaced, and whether the move is atomic, is
	/// up to the file system; moving between two file systems is not supported.
	/// @throws Exception When the file cannot be moved.
	auto Move(const std::string &source, const std::string &target) const -> void;
	/// Removes the file at `path`, if there is one.
	/// @return Whether a file was removed.
	/// @throws Exception When the file exists but cannot be removed.
	auto Remove(const std::string &path) const -> bool;

private:
	explicit FileSystem(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Qualified Name
//----------------------------------------------------------------------------------------------------------------------

/// The ordered path of identifiers that names a database object.
/// The path is the whole representation: partial qualification is expressed by having fewer parts (`{"t"}`,
/// `{"s", "t"}`, `{"c", "s", "t"}`), never by empty placeholders, and the last part is always the object itself.
/// Whether a two-part name means catalog.object or schema.object is decided by resolution, not by the name.
///
/// Owned, so a name obtained from somewhere transient -- a replacement scan callback, say -- can be kept for as long
/// as you like.
class QualifiedName final : public detail::Handle<QualifiedName> {
	friend detail::HandleFactory;

public:
	QualifiedName(QualifiedName &&) noexcept = default;
	QualifiedName &operator=(QualifiedName &&) noexcept = default;

	~QualifiedName() override;

	/// Parses SQL text into a qualified name: dots separate parts, and a double-quoted part may contain dots and
	/// doubled interior quotes.
	/// @throws Exception When the text does not parse, or yields no parts or more than three.
	static auto Parse(std::string_view text) -> QualifiedName;

	/// Builds a name from its parts, outermost first, so the last one is the object name.
	/// @param parts Between one and three non-empty parts.
	/// @throws InvalidInputException When there are no parts, more than three, or any is empty.
	static auto Create(const std::vector<std::string> &parts) -> QualifiedName;

	/// How many parts the name has; always at least one.
	auto GetPartCount() const -> idx_t;

	/// One part, outermost first. The view is valid until this name is destroyed.
	/// @param index Part index in [0, GetPartCount()).
	/// @throws Exception When the index is out of range.
	auto GetPart(idx_t index) const -> std::string_view;

	/// The object name: the last part. The view is valid until this name is destroyed.
	auto GetName() const -> std::string_view;

	/// The name as SQL text, quoting each part only where the identifier requires it, so it parses back to an equal
	/// name.
	auto Render() const -> std::string;

	/// Whether two names have the same parts, compared case-insensitively the way the engine compares identifiers.
	auto Equals(const QualifiedName &other) const -> bool;

	/// A hash consistent with `Equals`. Not stable across processes or versions: for in-process lookup only.
	auto Hash() const -> uint64_t;

private:
	explicit QualifiedName(void *impl);
};

inline bool operator==(const QualifiedName &lhs, const QualifiedName &rhs) {
	return lhs.Equals(rhs);
}
inline bool operator!=(const QualifiedName &lhs, const QualifiedName &rhs) {
	return !lhs.Equals(rhs);
}

//----------------------------------------------------------------------------------------------------------------------
// Table Description
//----------------------------------------------------------------------------------------------------------------------

/// An owned snapshot of one base table, taken by `Connection::DescribeTable`: where the name resolved, the table's
/// columns in declared order (generated columns included), and per-column catalog facts. Later DDL does not update it.
class TableDescription final : public detail::Handle<TableDescription> {
	friend detail::HandleFactory;

public:
	TableDescription(TableDescription &&) noexcept = default;
	TableDescription &operator=(TableDescription &&) noexcept = default;

	~TableDescription() override;

	/// The fully resolved name: the catalog, schema and table the lookup landed on, with the casing the table was
	/// created with.
	auto GetQualifiedName() const -> QualifiedName;

	/// How many columns the table has, generated columns included.
	auto GetColumnCount() const -> idx_t;

	/// An owned description of one column, independent of this table description.
	/// @param index Column index in [0, GetColumnCount()).
	/// @throws Exception When the index is out of range.
	auto GetColumn(idx_t index) const -> ColumnDescription;

	/// Whether the catalog the table lives in was attached read-only.
	auto IsReadOnly() const -> bool;

private:
	explicit TableDescription(void *impl);
};

/// An owned snapshot of one column of a described table, from `TableDescription::GetColumn`: its name, type and
/// catalog facts. Independent of the table description it came from.
class ColumnDescription final : public detail::Handle<ColumnDescription> {
	friend detail::HandleFactory;

public:
	ColumnDescription(ColumnDescription &&) noexcept = default;
	ColumnDescription &operator=(ColumnDescription &&) noexcept = default;

	~ColumnDescription() override;

	/// The column name, with the casing it was declared with.
	/// @return A view borrowed from this description, valid until it is destroyed.
	auto GetName() const -> std::string_view;

	/// An owned copy of the column type.
	auto GetType() const -> LogicalType;

	/// Whether the column declares a default expression. Generated columns report false.
	auto HasDefault() const -> bool;

	/// Whether the column is generated: computed by the engine and not writable.
	auto HasGenerated() const -> bool;

private:
	explicit ColumnDescription(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Query Metrics
//----------------------------------------------------------------------------------------------------------------------

/// An owned snapshot of the metrics of a query, taken by `Connection::GetQueryMetrics`: a tree whose root describes
/// the query as a whole and whose other nodes describe its operators. Later queries do not update it. Which metrics
/// are present, their names and the shape of the tree may change between versions. Unstable API.
class QueryMetrics final : public detail::Handle<QueryMetrics> {
	friend detail::HandleFactory;

public:
	QueryMetrics(QueryMetrics &&) noexcept = default;
	QueryMetrics &operator=(QueryMetrics &&) noexcept = default;

	~QueryMetrics() override;

	/// The node describing the query as a whole.
	/// @return A borrowed node, valid for as long as this snapshot is.
	auto GetRoot() const -> QueryMetricsNode;

private:
	explicit QueryMetrics(void *impl);
};

/// One node of a `QueryMetrics` snapshot, carrying a set of named, numeric metrics. Borrowed, valid for as long as the
/// snapshot is. Unstable API.
class QueryMetricsNode final : public detail::Handle<QueryMetricsNode> {
	friend detail::HandleFactory;

public:
	QueryMetricsNode(QueryMetricsNode &&) noexcept = default;
	QueryMetricsNode &operator=(QueryMetricsNode &&) noexcept = default;

	~QueryMetricsNode() override;

	/// `QUERY` for the root node, the name of the operator, e.g. `TABLE_SCAN`, for the others.
	/// @return A view borrowed from the snapshot.
	auto GetName() const -> std::string_view;

	/// How many children the node has.
	auto GetChildCount() const -> idx_t;

	/// One child of the node.
	/// @param index Child index in [0, GetChildCount()).
	/// @throws Exception When the index is out of range.
	auto GetChild(idx_t index) const -> QueryMetricsNode;

	/// How many metrics the node carries.
	auto GetMetricCount() const -> idx_t;

	/// The name of one metric, e.g. `query.total_time`. The order of the metrics is unspecified.
	/// @param index Metric index in [0, GetMetricCount()).
	/// @return A view borrowed from the snapshot.
	/// @throws Exception When the index is out of range.
	auto GetMetricName(idx_t index) const -> std::string_view;

	/// The value of one metric, a number such as a UBIGINT count or a DOUBLE duration.
	/// @param index Metric index in [0, GetMetricCount()).
	/// @throws Exception When the index is out of range.
	auto GetMetricValue(idx_t index) const -> Value;

	/// Looks a metric up by its exact name.
	/// @return Its value, or nothing when the node does not carry it.
	auto GetMetric(std::string_view name) const -> std::optional<Value>;

private:
	explicit QueryMetricsNode(void *impl);
};

//----------------------------------------------------------------------------------------------------------------------
// Replacement Scan
//----------------------------------------------------------------------------------------------------------------------

/// A user-defined replacement scan, built up with the setters and made live with `Connection::Register` or
/// `Extension::Register`. Create one through a `Factory` of the database it will be registered on, set its callback and
/// user data, then register it with `Connection::Register` or `Extension::Register`. The scan object may be destroyed
/// after registration; the registered scan lives on until its scope ends.
///
/// The binder consults replacement scans when a table name cannot be resolved in the catalog; this is what makes
/// `SELECT * FROM 'file.parquet'` work. The callback inspects the unresolved name and either claims it, by naming a
/// table function to call, a `ColumnDataCollection` to read or a query to run instead, or declines it by claiming
/// nothing, which lets the next registered scan try. When no scan claims the name, the usual "table does not exist"
/// error is raised. A callback reports failure by throwing; the exception surfaces as the query's error.
///
/// Scope follows the registration. A scan registered through a `Connection` is visible only to that connection,
/// is released when it closes, and is consulted before every instance-wide scan, including the built-in file scans. A
/// scan registered through an `Instance` or an `Extension` is visible to every connection to that instance
/// and lives until it closes; registering one is not thread-safe against queries binding on other connections, so do it
/// during extension load or before issuing queries. A registered scan cannot be unregistered.
class ReplacementScan final : public detail::Handle<ReplacementScan> {
	friend detail::HandleFactory;
	friend class Connection;
	friend class Extension;
	friend class Instance;

public:
	class Input;

	/// Called once per table reference the catalog could not resolve; claims it or declines it. Required.
	using Callback = void (*)(Input &input);

	ReplacementScan(ReplacementScan &&) noexcept = default;
	ReplacementScan &operator=(ReplacementScan &&) noexcept = default;

	~ReplacementScan() override;

	/// Creates an empty ReplacementScan through `factory`. Register it through a `Connection` or an `Extension` of the
	/// factory's database.
	static auto Create(Factory &factory) -> ReplacementScan;

	auto SetCallback(Callback callback) & -> ReplacementScan &;

	/// Constructs user data of type `T`, carried by the registered scan and freed when its scope ends; read it from
	/// the callback via `Input::GetUserData<T>`. Consumed by registration.
	template <class T, class... ARGS>
	auto SetUserData(ARGS &&... args) & -> ReplacementScan & {
		auto ptr = new T(std::forward<ARGS>(args)...);
		SetUserDataInternal(ptr, detail::TypedDelete<T>);
		return *this;
	}

private:
	explicit ReplacementScan(void *impl);
	//! Moves what the callbacks need into the C handle, right before it is registered.
	auto PrepareRegistration() -> void;

	auto SetUserDataInternal(void *data, void (*destructor)(void *)) -> void;

	Callback callback = nullptr;
	detail::UserData user_data;

public:
	/// What the callback works with. Borrowed, valid only for the callback duration, as are the name views it hands
	/// out.
	class Input {
		friend detail::HandleFactory;

	public:
		/// The user data set via `ReplacementScan::SetUserData`.
		/// @throws InvalidInputException When none was set.
		template <class T>
		auto GetUserData() const -> T & {
			return *static_cast<T *>(GetUserDataInternal());
		}

		/// The name the catalog could not resolve, as written in the query. An unqualified reference has a single
		/// part; for a file-backed one that part is the path. Owned, so it can outlive the callback.
		auto GetName() const -> QualifiedName;

		/// Claims the reference by naming a table function to call instead; its arguments are then supplied with
		/// `AddArgument` and `AddNamedArgument`. Parts are matched case-insensitively, and a qualified name targets a
		/// function in a particular schema or catalog. The name is not resolved here: an unknown function fails
		/// later, when the replacement is bound. The three claim forms, `SetFunctionName`, `SetCollection` and
		/// `SetSubquery`, are mutually exclusive.
		/// @throws InvalidInputException When a different claim form was already used.
		auto SetFunctionName(const QualifiedName &name) -> void;

		/// `SetFunctionName` for the common case of an unqualified function.
		auto SetFunctionName(std::string_view name) -> void;

		/// Appends a positional argument to the claimed table function. Positional arguments are passed in the order
		/// they are added, before any named ones.
		/// @throws InvalidInputException Unless `SetFunctionName` was called first.
		auto AddArgument(const Value &value) -> void;

		/// Appends a named argument to the claimed table function, the equivalent of `name := value`.
		/// @throws InvalidInputException Unless `SetFunctionName` was called first.
		auto AddNamedArgument(std::string_view name, const Value &value) -> void;

		/// Claims the reference by naming a collection to read instead. The collection is borrowed: it must stay
		/// alive, and must not be cleared, reset or destroyed, for as long as any result reading it is live. A
		/// `PreparedStatement` over a claimed name extends that further: it captures the borrow in its plan and,
		/// since a collection claim reads no database, reuses that plan without consulting the callback again
		/// (`PreparedStatement::ReusesPlan` reports true). Destroy such statements before releasing the collection.
		/// The three claim forms, `SetFunctionName`, `SetCollection` and `SetSubquery`, are mutually exclusive.
		/// @param collection The collection to read.
		/// @param column_names Names for its columns, in order; empty names them col1..colN.
		/// @throws InvalidInputException When the names do not match the collection's columns, or a different claim
		/// form was already used.
		auto SetCollection(const ColumnDataCollection &collection, const std::vector<std::string> &column_names = {})
		    -> void;

		/// Claims the reference by naming a query to run instead. The text is parsed here and must contain exactly
		/// one SELECT statement. The three claim forms, `SetFunctionName`, `SetCollection` and `SetSubquery`, are
		/// mutually exclusive.
		/// @throws Exception When the text does not parse, or is not a single SELECT.
		/// @throws InvalidInputException When a different claim form was already used.
		auto SetSubquery(std::string_view sql) -> void;

		/// Sets the alias the claimed replacement is bound under. Optional: an alias written in the query takes
		/// precedence, and without either the reference's own table name is used.
		auto SetAlias(std::string_view alias) -> void;

		/// The binding context. Borrowed, valid only for the callback duration.
		auto GetContext() -> Context &;

	private:
		Input(void *args, void *context) : args(args), context(detail::HandleFactory::Make<Context>(context)) {
		}

		void *args;
		Context context;

		void *GetUserDataInternal() const;
	};
};

//----------------------------------------------------------------------------------------------------------------------
// Scalar Executor
//----------------------------------------------------------------------------------------------------------------------

namespace detail {

/// @internal
/// Names `Vector` once per element of a type pack, to declare one owned child vector per tuple element.
template <class T>
using VectorPerField = Vector;

/// @internal
/// Reads one argument row as a `T`. The primary template covers the primitive types; the specializations add the
/// composed forms: `std::reference_wrapper<const T>` reads in place, `std::optional<T>` absorbs NULLs, and
/// `std::tuple<Ts...>` reads a STRUCT/TUPLE vector's fields through its child vectors, recursively.
/// `IsValid` reports whether a row can produce a value: false when a non-optional part of it is NULL.
template <class T>
struct VectorReader {
	VectorView view;

	explicit VectorReader(const Vector &vec) : view(vec.GetView()) {
	}
	bool AllValid() const {
		return view.AllValid();
	}
	bool IsValid(idx_t row) const {
		return view.IsValid(row);
	}
	T Get(idx_t row) const {
		return view.Data<T>()[view.SelAt(row)];
	}
};

template <class T>
struct VectorReader<std::reference_wrapper<const T>> {
	VectorView view;

	explicit VectorReader(const Vector &vec) : view(vec.GetView()) {
	}
	bool AllValid() const {
		return view.AllValid();
	}
	bool IsValid(idx_t row) const {
		return view.IsValid(row);
	}
	std::reference_wrapper<const T> Get(idx_t row) const {
		return std::cref(view.Data<T>()[view.SelAt(row)]);
	}
};

template <class T>
struct VectorReader<std::optional<T>> {
	VectorReader<T> inner;

	explicit VectorReader(const Vector &vec) : inner(vec) {
	}
	bool AllValid() const {
		return inner.AllValid();
	}
	// A NULL is a value here (nullopt), so the row always participates.
	bool IsValid(idx_t) const {
		return true;
	}
	std::optional<T> Get(idx_t row) const {
		if (!inner.IsValid(row)) {
			return std::nullopt;
		}
		return inner.Get(row);
	}
};

template <class... Ts>
struct VectorReader<std::tuple<Ts...>> {
	VectorView view;
	std::tuple<VectorReader<Ts>...> children;

	explicit VectorReader(const Vector &vec)
	    : view(vec.GetView()), children(MakeChildren(vec, std::index_sequence_for<Ts...> {})) {
	}
	bool AllValid() const {
		return AllValidImpl(std::index_sequence_for<Ts...> {});
	}
	bool IsValid(idx_t row) const {
		// The row's struct entry must be non-NULL, and so must every non-optional field of it. The children index
		// by entry: their rows align with this vector's elements, so the selection applies once, here.
		return view.IsValid(row) && IsValidImpl(view.SelAt(row), std::index_sequence_for<Ts...> {});
	}
	std::tuple<Ts...> Get(idx_t row) const {
		return GetImpl(view.SelAt(row), std::index_sequence_for<Ts...> {});
	}

private:
	template <size_t... Is>
	static std::tuple<VectorReader<Ts>...> MakeChildren(const Vector &vec, std::index_sequence<Is...>) {
		return std::tuple<VectorReader<Ts>...> {VectorReader<Ts>(vec.GetChild(Is))...};
	}
	template <size_t... Is>
	bool AllValidImpl(std::index_sequence<Is...>) const {
		return view.AllValid() && (... && std::get<Is>(children).AllValid());
	}
	template <size_t... Is>
	bool IsValidImpl(idx_t entry, std::index_sequence<Is...>) const {
		return (... && std::get<Is>(children).IsValid(entry));
	}
	template <size_t... Is>
	std::tuple<Ts...> GetImpl(idx_t entry, std::index_sequence<Is...>) const {
		return std::tuple<Ts...> {std::get<Is>(children).Get(entry)...};
	}
};

/// @internal
/// Writes one result row as a `T` into a FLAT vector, mirroring `VectorReader`'s type forms: primitives write the
/// element, `std::optional<T>` turns nullopt into a NULL row, and `std::tuple<Ts...>` writes a STRUCT/TUPLE vector's
/// fields through its child vectors, recursively. NULLs go through the validity mask directly (fetched on first use, so
/// the all-valid path allocates no mask): the result's size is set by the engine only after the callback, so the
/// size-checked `Vector::SetNull` is not usable here. `ResetValidity` readies the masks before a loop that may write
/// NULLs; nulling a tuple row nulls its fields too, keeping the engine's nested-NULL invariant.
template <class T>
struct VectorWriter {
	Vector &vec;
	T *data;
	ValidityMask mask {nullptr};

	explicit VectorWriter(Vector &vec) : vec(vec), data(vec.GetDataMutable<T>()) {
	}
	void ResetValidity(idx_t count) {
		Mask().SetAllValid(count);
	}
	void Set(idx_t row, const T &value) {
		data[row] = value;
	}
	void SetNull(idx_t row) {
		Mask().SetInvalid(row);
	}

private:
	ValidityMask &Mask() {
		if (!mask.words) {
			mask = vec.GetValidityMutable();
		}
		return mask;
	}
};

template <class T>
struct VectorWriter<std::optional<T>> {
	VectorWriter<T> inner;

	explicit VectorWriter(Vector &vec) : inner(vec) {
	}
	void ResetValidity(idx_t count) {
		inner.ResetValidity(count);
	}
	void Set(idx_t row, const std::optional<T> &value) {
		if (value) {
			inner.Set(row, *value);
		} else {
			inner.SetNull(row);
		}
	}
	void SetNull(idx_t row) {
		inner.SetNull(row);
	}
};

template <class... Ts>
struct VectorWriter<std::tuple<Ts...>> {
	Vector &vec;
	std::tuple<VectorPerField<Ts>...> child_vectors;
	std::tuple<VectorWriter<Ts>...> children;
	ValidityMask mask {nullptr};

	explicit VectorWriter(Vector &vec)
	    : vec(vec), child_vectors(MakeChildVectors(vec, std::index_sequence_for<Ts...> {})),
	      children(MakeChildWriters(child_vectors, std::index_sequence_for<Ts...> {})) {
	}
	void ResetValidity(idx_t count) {
		Mask().SetAllValid(count);
		ResetValidityImpl(count, std::index_sequence_for<Ts...> {});
	}
	void Set(idx_t row, const std::tuple<Ts...> &value) {
		SetImpl(row, value, std::index_sequence_for<Ts...> {});
	}
	void SetNull(idx_t row) {
		// A NULL struct entry requires NULL fields, so the row nulls recursively.
		Mask().SetInvalid(row);
		SetNullImpl(row, std::index_sequence_for<Ts...> {});
	}

private:
	template <size_t... Is>
	static std::tuple<VectorPerField<Ts>...> MakeChildVectors(Vector &vec, std::index_sequence<Is...>) {
		return std::tuple<VectorPerField<Ts>...> {vec.GetChild(Is)...};
	}
	template <size_t... Is>
	static std::tuple<VectorWriter<Ts>...> MakeChildWriters(std::tuple<VectorPerField<Ts>...> &vectors,
	                                                        std::index_sequence<Is...>) {
		return std::tuple<VectorWriter<Ts>...> {VectorWriter<Ts>(std::get<Is>(vectors))...};
	}
	template <size_t... Is>
	void ResetValidityImpl(idx_t count, std::index_sequence<Is...>) {
		(std::get<Is>(children).ResetValidity(count), ...);
	}
	template <size_t... Is>
	void SetImpl(idx_t row, const std::tuple<Ts...> &value, std::index_sequence<Is...>) {
		(std::get<Is>(children).Set(row, std::get<Is>(value)), ...);
	}
	template <size_t... Is>
	void SetNullImpl(idx_t row, std::index_sequence<Is...>) {
		(std::get<Is>(children).SetNull(row), ...);
	}
	ValidityMask &Mask() {
		if (!mask.words) {
			mask = vec.GetValidityMutable();
		}
		return mask;
	}
};

} // namespace detail

/// Row-at-a-time execution helper for exec callbacks: give it the value types and a callable and it handles the
/// vector plumbing -- argument layout (selection vectors, constants), NULL propagation, and writing the result. For a
/// function this covers, the whole exec callback is one call:
///
///     void AddExec(ScalarFunction::ExecInput &input) {
///         ScalarExecutor::Execute<int64_t, int64_t, int64_t>(input, [](int64_t a, int64_t b) { return a + b; });
///     }
///
/// The type list is the result type followed by one type per argument; a nullary function passes just the result
/// type. Each type is composed from these forms:
///
/// - A primitive type (`VectorView::Data`'s element types) passes or returns the value itself.
/// - `std::reference_wrapper<const T>` (arguments only) passes a reference into the vector instead of a copy.
/// - `std::optional<T>` makes NULL part of the value: an argument arrives as nullopt when its row is NULL instead of
///   nulling the row, and a result of nullopt makes the row NULL. Meaningful for arguments only when the function
///   uses special NULL handling; under the default the engine assumes NULL in, NULL out.
/// - `std::tuple<Ts...>` maps a STRUCT/TUPLE vector to its fields, read from and written to the child vectors; the
///   forms nest, so a field can itself be optional, a reference, or a tuple.
///
/// Rows follow SQL's default NULL handling: a row where any non-optional part of any argument is NULL yields NULL,
/// and the callable is not invoked for it. Functions over strings or other unrepresented types drive the vectors
/// directly instead.
class ScalarExecutor {
public:
	/// Fills the exec callback's result vector by invoking `fun` once per participating row.
	/// @tparam RESULT The result type; `fun` must return it (or something convertible).
	/// @tparam ARGS One type per argument, in signature order; `fun` is invoked with one value of each.
	/// @param input The exec callback's input.
	/// @param fun The callable computing one row: `RESULT(ARGS...)`.
	/// @throws InvalidInputException When the call carries a different number of arguments than `ARGS` lists.
	template <class RESULT, class... ARGS, class FUN>
	static void Execute(ScalarFunction::ExecInput &input, FUN fun) {
		if (input.GetArgCount() != sizeof...(ARGS)) {
			throw InvalidInputException("ScalarExecutor::Execute: the call carries " +
			                            std::to_string(input.GetArgCount()) + " arguments but the type list names " +
			                            std::to_string(sizeof...(ARGS)));
		}
		auto result = input.GetResult();
		ExecuteImpl<RESULT, ARGS...>(input, result, input.GetRowCount(), fun, std::index_sequence_for<ARGS...> {});
	}

	/// The vector-level form, for vectors that come from somewhere other than an exec callback (e.g. a `DataChunk`):
	/// one vector parameter per type in `ARGS`, so passing the wrong number of vectors is a compile error.
	/// @tparam RESULT The result type; `fun` must return it (or something convertible).
	/// @tparam ARGS One type per argument vector, in order; `fun` is invoked with one value of each.
	/// @param args One vector per argument, each covering at least `count` rows.
	/// @param result The FLAT vector to fill; must have room for `count` rows.
	/// @param count How many rows to produce.
	/// @param fun The callable computing one row: `RESULT(ARGS...)`.
	template <class RESULT, class... ARGS, class FUN>
	static void Execute(detail::VectorPerArg<ARGS>... args, Vector &result, idx_t count, FUN fun) {
		std::tuple<detail::VectorReader<ARGS>...> readers {detail::VectorReader<ARGS>(args)...};
		detail::VectorWriter<RESULT> writer(result);
		Run(readers, writer, count, fun, std::index_sequence_for<ARGS...> {});
	}

private:
	template <class RESULT, class... ARGS, class FUN, size_t... Is>
	static void ExecuteImpl(ScalarFunction::ExecInput &input, Vector &result, idx_t count, FUN &fun,
	                        std::index_sequence<Is...> seq) {
		std::tuple<detail::VectorReader<ARGS>...> readers {detail::VectorReader<ARGS>(input.GetArg(Is))...};
		detail::VectorWriter<RESULT> writer(result);
		Run(readers, writer, count, fun, seq);
	}

	template <class RESULT, class... ARGS, class FUN, size_t... Is>
	static void Run(const std::tuple<detail::VectorReader<ARGS>...> &readers, detail::VectorWriter<RESULT> &writer,
	                idx_t count, FUN &fun, std::index_sequence<Is...>) {
		if ((... && std::get<Is>(readers).AllValid())) {
			for (idx_t i = 0; i < count; i++) {
				writer.Set(i, fun(std::get<Is>(readers).Get(i)...));
			}
			return;
		}

		// Some argument row is NULL: propagate row by row through the result's mask.
		writer.ResetValidity(count);
		for (idx_t i = 0; i < count; i++) {
			if ((... && std::get<Is>(readers).IsValid(i))) {
				writer.Set(i, fun(std::get<Is>(readers).Get(i)...));
			} else {
				writer.SetNull(i);
			}
		}
	}
};

inline auto Factory::CreateType() -> TypeBuilder<Factory> {
	return TypeBuilder(*this);
}

inline auto Connection::Execute(const SqlStatement &statement, const std::vector<Value> &parameters) -> QueryResult {
	return Execute(statement, parameters.data(), parameters.size());
}

inline auto PreparedStatement::Execute(const std::vector<Value> &parameters) -> QueryResult {
	return Execute(parameters.data(), parameters.size());
}

inline auto Connection::Execute(const SqlStatement &statement, const std::vector<Value> &parameters, ArrowFormat format)
    -> ArrowResult {
	return Execute(statement, parameters.data(), parameters.size(), format);
}

inline auto Connection::Execute(const SqlStatement &statement, const std::vector<NamedParam> &parameters,
                                ArrowFormat format) -> ArrowResult {
	return ExecuteArrowNamed(statement, parameters.data(), parameters.size(), format);
}

inline auto Connection::Execute(const std::string &sql, ArrowFormat format) -> ArrowResult {
	auto statements = ParseSQL(sql);
	auto statement = statements.Next();
	if (!statement || statements.Next()) {
		throw InvalidInputException("Execute expects exactly one statement; use ParseSQL for multi-statement input");
	}
	return Execute(statement, format);
}

inline auto PreparedStatement::Execute(const std::vector<Value> &parameters, ArrowFormat format) -> ArrowResult {
	return Execute(parameters.data(), parameters.size(), format);
}

inline auto PreparedStatement::Execute(const std::vector<NamedParam> &parameters, ArrowFormat format) -> ArrowResult {
	return ExecuteArrowNamed(parameters.data(), parameters.size(), format);
}

} // namespace cxx
} // namespace duckdb
