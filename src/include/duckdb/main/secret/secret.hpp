//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/secret/secret.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/named_parameter_map.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/main/setting_info.hpp"

namespace duckdb {
class BaseSecret;
struct SecretEntry;
struct FileOpenerInfo;
struct CreateSecretInfo;
class FileOpener;

//! The lifetime of a secret
enum class SecretPersistType : uint8_t { DEFAULT, TEMPORARY, PERSISTENT, TRANSACTION };

//! Input passed to a CreateSecretFunction
struct CreateSecretInput {
	//! type
	Identifier type;
	//! mode
	Identifier provider;
	//! should the secret be persisted?
	Identifier storage_type;
	//! (optional) alias provided by user
	Identifier name;
	//! (optional) scope provided by user
	vector<string> scope;
	//! (optional) named parameter map, each create secret function has defined it's own set of these
	case_insensitive_map_t<Value> options;
	//! how to handle conflicts
	OnCreateConflict on_conflict;
	//! lifetime of the secret
	SecretPersistType persist_type;
};

typedef unique_ptr<BaseSecret> (*secret_deserializer_t)(Deserializer &deserializer, BaseSecret base_secret);
typedef unique_ptr<BaseSecret> (*create_secret_function_t)(ClientContext &context, CreateSecretInput &input);

//! A CreateSecretFunction is a function adds a provider for a secret type.
class CreateSecretFunction {
public:
	string secret_type;
	Identifier provider;
	create_secret_function_t function;
	named_parameter_type_map_t named_parameters;
};

//! CreateSecretFunctionsSet contains multiple functions of a single type, identified by the provider. The provider
//! should be seen as the method of secret creation. (e.g. user-provided config, env variables, auto-detect)
class CreateSecretFunctionSet {
public:
	explicit CreateSecretFunctionSet(string &name) : name(name) {};
	identifier_map_t<CreateSecretFunction> &GetFunctions() {
		return functions;
	}

public:
	bool ProviderExists(const Identifier &provider_name);
	void AddFunction(CreateSecretFunction &function, OnCreateConflict on_conflict);
	CreateSecretFunction &GetFunction(const string &provider);

protected:
	//! Create Secret Function type name
	Identifier name;
	//! Maps of provider -> function
	identifier_map_t<CreateSecretFunction> functions;
};

//! Determines whether the secrets are allowed to be shown
enum class SecretDisplayType : uint8_t { REDACTED, UNREDACTED };

//! Secret types contain the base settings of a secret
struct SecretType {
	//! Unique name identifying the secret type
	Identifier name;
	//! The deserialization function for the type
	secret_deserializer_t deserializer;
	//! Provider to use when non is specified
	string default_provider;
	//! The extension that registered this secret type
	string extension;
};

enum class SecretSerializationType : uint8_t {
	//! The secret is serialized with a custom serialization function
	CUSTOM = 0,
	//! The secret has been serialized as a KeyValueSecret
	KEY_VALUE_SECRET = 1
};

//! Base class from which BaseSecret classes can be made.
class BaseSecret {
	friend class SecretManager;

public:
	BaseSecret(vector<string> prefix_paths_p, Identifier type_p, Identifier provider_p, Identifier name_p)
	    : prefix_paths(std::move(prefix_paths_p)), type(std::move(type_p)), provider(std::move(provider_p)),
	      name(std::move(name_p)), serializable(false) {
		D_ASSERT(!type.empty());
	}
	BaseSecret(const BaseSecret &other)
	    : prefix_paths(other.prefix_paths), type(other.type), provider(other.provider), name(other.name),
	      serializable(other.serializable) {
		D_ASSERT(!type.empty());
	}
	virtual ~BaseSecret() = default;

	//! The score of how well this secret's scope matches the path (by default: the length of the longest matching
	//! prefix)
	virtual int64_t MatchScore(const string &path) const;
	//! Prints the secret as a string
	virtual string ToString(SecretDisplayType mode = SecretDisplayType::REDACTED) const;
	//! Serialize this secret
	virtual void Serialize(Serializer &serializer) const;

	virtual unique_ptr<const BaseSecret> Clone() const {
		D_ASSERT(typeid(BaseSecret) == typeid(*this));
		return make_uniq<BaseSecret>(*this);
	}

	//! Getters
	const vector<string> &GetScope() const {
		return prefix_paths;
	}
	const Identifier &GetType() const {
		return type;
	}
	const Identifier &GetProvider() const {
		return provider;
	}
	const Identifier &GetName() const {
		return name;
	}
	bool IsSerializable() const {
		return serializable;
	}

protected:
	//! Helper function to serialize the base BaseSecret class variables
	virtual void SerializeBaseSecret(Serializer &serializer) const final;

	//! prefixes to which the secret applies
	vector<string> prefix_paths;

	//! Type of secret
	Identifier type;
	//! Provider of the secret
	Identifier provider;
	//! Name of the secret
	Identifier name;
	//! Whether the secret can be serialized/deserialized
	bool serializable;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! The KeyValueSecret is a class that implements a Secret as a set of key -> values. This class can be used
//! for most use-cases of secrets as secrets generally tend to fit in a key value map.
class KeyValueSecret : public BaseSecret {
public:
	KeyValueSecret(const vector<string> &prefix_paths, const Identifier &type, const Identifier &provider,
	               const Identifier &name)
	    : BaseSecret(prefix_paths, type, provider, name) {
		D_ASSERT(!type.empty());
		serializable = true;
	}
	explicit KeyValueSecret(const BaseSecret &secret)
	    : BaseSecret(secret.GetScope(), secret.GetType(), secret.GetProvider(), secret.GetName()) {
		serializable = true;
	};
	KeyValueSecret(const KeyValueSecret &secret)
	    : BaseSecret(secret.GetScope(), secret.GetType(), secret.GetProvider(), secret.GetName()) {
		secret_map = secret.secret_map;
		redact_keys = secret.redact_keys;
		serializable = true;
	};
	KeyValueSecret(KeyValueSecret &&secret) noexcept
	    : BaseSecret(std::move(secret.prefix_paths), std::move(secret.type), std::move(secret.provider),
	                 std::move(secret.name)) {
		secret_map = std::move(secret.secret_map);
		redact_keys = std::move(secret.redact_keys);
		serializable = true;
	};

	//! Print the secret as a key value map in the format 'key1=value;key2=value2'
	string ToString(SecretDisplayType mode = SecretDisplayType::REDACTED) const override;
	void Serialize(Serializer &serializer) const override;

	//! Tries to get the value at key <key>, depending on error_on_missing will throw or return Value()
	Value TryGetValue(const Identifier &key, bool error_on_missing = false) const;

	// FIXME: use serialization scripts
	template <class TYPE>
	static unique_ptr<BaseSecret>
	Deserialize(Deserializer &deserializer,
	            BaseSecret base_secret) { // NOLINT(performance-unnecessary-value-param): callback signature.
		auto result = make_uniq<TYPE>(base_secret);
		ReadSecretMap(deserializer, result->secret_map);

		Value redact_set_value;
		deserializer.ReadProperty(202, "redact_keys", redact_set_value);
		for (const auto &entry : ListValue::GetChildren(redact_set_value)) {
			result->redact_keys.insert(Identifier(entry.ToString()));
		}

		return duckdb::unique_ptr_cast<TYPE, BaseSecret>(std::move(result));
	}

	unique_ptr<const BaseSecret> Clone() const override {
		return make_uniq<KeyValueSecret>(*this);
	}

	// Get a value from the secret
	bool TryGetValue(const Identifier &key, Value &result) const {
		auto lookup = secret_map.find(key);
		if (lookup == secret_map.end()) {
			return false;
		}
		result = lookup->second;
		return true;
	}

	bool TrySetValue(const Identifier &key, const CreateSecretInput &input) {
		auto lookup = input.options.find(key.GetIdentifierName());
		if (lookup != input.options.end()) {
			secret_map[key] = lookup->second;
			return true;
		}
		return false;
	}

	//! the map of key -> values that make up the secret
	identifier_tree_t<Value> secret_map;
	//! keys that are sensitive and should be redacted
	identifier_set_t redact_keys;

private:
	static void ReadSecretMap(Deserializer &deserializer, identifier_tree_t<Value> &result);
};

// Helper class to fetch secret parameters in a cascading way. The idea being that in many cases there is a direct
// connection between a KeyValueSecret key and a setting and we want to:
// - check if the secret has a specific key, if so return the corresponding value
// - check if a setting exists, if so return its value
// - return a default value

class KeyValueSecretReader {
public:
	//! Manually pass in a secret reference
	KeyValueSecretReader(const KeyValueSecret &secret_p, FileOpener &opener_p);

	//! Initializes the KeyValueSecretReader by fetching the secret automatically
	KeyValueSecretReader(FileOpener &opener_p, optional_ptr<FileOpenerInfo> info, const char **secret_types,
	                     idx_t secret_types_len);
	KeyValueSecretReader(FileOpener &opener_p, optional_ptr<FileOpenerInfo> info, const char *secret_type);

	//! Initialize KeyValueSecretReader from a db instance
	KeyValueSecretReader(DatabaseInstance &db, const char **secret_types, idx_t secret_types_len, string path);
	KeyValueSecretReader(DatabaseInstance &db, const char *secret_type, string path);

	// Initialize KeyValueSecretReader from a client context
	KeyValueSecretReader(ClientContext &context, const char **secret_types, idx_t secret_types_len, string path);
	KeyValueSecretReader(ClientContext &context, const char *secret_type, string path);

	~KeyValueSecretReader();

	//! Lookup a KeyValueSecret value
	SettingLookupResult TryGetSecretKey(const Identifier &secret_key, Value &result);
	//! Lookup a KeyValueSecret value or a setting
	SettingLookupResult TryGetSecretKeyOrSetting(const Identifier &secret_key, const Identifier &setting_name,
	                                             Value &result);
	//! Lookup a KeyValueSecret value or a setting, throws InvalidInputException on not found
	Value GetSecretKey(const Identifier &secret_key);
	//! Lookup a KeyValueSecret value or a setting, throws InvalidInputException on not found
	Value GetSecretKeyOrSetting(const Identifier &secret_key, const Identifier &setting_name);

	//! Templating around TryGetSecretKey
	template <class TYPE>
	SettingLookupResult TryGetSecretKey(const Identifier &secret_key, TYPE &value_out) {
		Value result;
		auto lookup_result = TryGetSecretKey(secret_key, result);
		if (lookup_result) {
			value_out = result.GetValue<TYPE>();
		}
		return lookup_result;
	}

	//! Templating around TryGetSecretOrSetting
	template <class TYPE>
	SettingLookupResult TryGetSecretKeyOrSetting(const Identifier &secret_key, const Identifier &setting_name,
	                                             TYPE &value_out) {
		Value result;
		auto lookup_result = TryGetSecretKeyOrSetting(secret_key, setting_name, result);
		if (lookup_result) {
			if (!result.IsNull()) {
				value_out = result.GetValue<TYPE>();
			}
		}
		return lookup_result;
	}

	// Like a templated GetSecretOrSetting but instead of throwing on not found, return the default value
	template <class TYPE>
	TYPE GetSecretKeyOrSettingOrDefault(const Identifier &secret_key, const Identifier &setting_name,
	                                    TYPE default_value) {
		TYPE result;
		if (TryGetSecretKeyOrSetting(secret_key, setting_name, result)) {
			return result;
		}
		return default_value;
	}

protected:
	void Initialize(const char **secret_types, idx_t secret_types_len);

	[[noreturn]] void ThrowNotFoundError(const Identifier &secret_key);
	[[noreturn]] void ThrowNotFoundError(const Identifier &secret_key, const Identifier &setting_name);

	//! Fetching the secret
	optional_ptr<const KeyValueSecret> secret;
	//! Optionally an owning pointer to the secret entry
	shared_ptr<SecretEntry> secret_entry;

	//! Secrets/settings will be fetched either through a context (local + global settings) or a databaseinstance
	//! (global only)
	optional_ptr<DatabaseInstance> db;
	optional_ptr<ClientContext> context;

	string path;
};

} // namespace duckdb
