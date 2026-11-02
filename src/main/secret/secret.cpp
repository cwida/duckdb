#include "duckdb/main/secret/secret.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

int64_t BaseSecret::MatchScore(const string &path) const {
	int64_t longest_match = NumericLimits<int64_t>::Minimum();

	if (prefix_paths.empty()) {
		longest_match = 0;
	}

	for (const auto &prefix : prefix_paths) {
		// Handle empty scope which matches all at lowest possible score
		if (prefix.empty()) {
			longest_match = 0;
			continue;
		}
		if (StringUtil::StartsWith(path, prefix)) {
			longest_match = MaxValue<int64_t>(NumericCast<int64_t>(prefix.length()), longest_match);
		}
	}
	return longest_match;
}

void BaseSecret::SerializeBaseSecret(Serializer &serializer) const {
	serializer.WriteProperty(100, "type", type);
	serializer.WriteProperty(101, "provider", provider);
	serializer.WriteProperty(102, "name", name);
	serializer.WriteList(103, "scope", prefix_paths.size(),
	                     [&](Serializer::List &list, idx_t i) { list.WriteElement(prefix_paths[i]); });
}

string BaseSecret::ToString(SecretDisplayType display_type) const {
	return "";
}

void BaseSecret::Serialize(Serializer &serializer) const {
	throw InternalException("Attempted to serialize secret without serialize");
}

string KeyValueSecret::ToString(SecretDisplayType mode) const {
	string result;

	result += "name=" + name + ";";
	result += "type=" + type + ";";
	result += "provider=" + provider + ";";
	result += string("serializable=") + (serializable ? "true" : "false") + ";";
	result += "scope=";
	for (const auto &scope_it : prefix_paths) {
		result += scope_it + ",";
	}
	result = result.substr(0, result.size() - 1);
	result += ";";
	for (auto it = secret_map.begin(); it != secret_map.end(); it++) {
		result.append(it->first.GetIdentifierName());
		result.append("=");
		if (mode == SecretDisplayType::REDACTED && redact_keys.find(it->first) != redact_keys.end()) {
			result.append("redacted");
		} else {
			result.append(it->second.ToString());
		}
		if (it != --secret_map.end()) {
			result.append(";");
		}
	}

	return result;
}

// FIXME: use serialization scripts
void KeyValueSecret::Serialize(Serializer &serializer) const {
	BaseSecret::SerializeBaseSecret(serializer);

	vector<Value> map_values;
	for (auto it = secret_map.begin(); it != secret_map.end(); it++) {
		child_list_t<Value> map_struct;
		map_struct.emplace_back(make_pair("key", Value(it->first)));
		map_struct.emplace_back(make_pair("value", Value(it->second)));
		map_values.push_back(Value::STRUCT(map_struct));
	}

	// Warning: the secret map is serialized into a single MAP value with type ANY
	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::ANY);
	auto map = Value::MAP(ListType::GetChildType(map_type), map_values);
	serializer.WriteProperty(201, "secret_map", map);

	vector<Value> redact_key_values;
	for (auto it = redact_keys.begin(); it != redact_keys.end(); it++) {
		redact_key_values.push_back(*it);
	}
	auto list = Value::LIST(LogicalType::VARCHAR, redact_key_values);
	serializer.WriteProperty(202, "redact_keys", list);
}

void KeyValueSecret::ReadSecretMap(Deserializer &deserializer, identifier_tree_t<Value> &result) {
	deserializer.ReadObject(201, "secret_map", [&](Deserializer &map) {
		auto stored_type = map.ReadProperty<LogicalType>(100, "type");
		// Before DuckDB 1.1.3, secrets declared VARCHAR values even when storing other types.
		if (stored_type.id() != LogicalTypeId::MAP || !stored_type.HasParameters() ||
		    MapType::KeyType(stored_type) != LogicalType::VARCHAR) {
			throw SerializationException("Secret map must be a MAP with VARCHAR keys");
		}
		if (map.ReadProperty<bool>(101, "is_null")) {
			throw SerializationException("Secret map cannot be NULL");
		}

		map.Set<const LogicalType &>(ListType::GetChildType(stored_type));
		map.ReadObject(102, "value", [&](Deserializer &obj) {
			obj.ReadList(100, "children", [&](Deserializer::List &list, idx_t i) {
				// Before DuckDB 1.2.0, entries carried their actual value types, even when the map declared VARCHAR.
				// Deserialize them separately so their payloads still undergo normal Value validation.
				auto entry = list.ReadElement<Value>();
				if (entry.IsNull() || entry.type().id() != LogicalTypeId::STRUCT) {
					throw SerializationException("Secret map entry must be a non-NULL struct");
				}
				const auto &children = StructValue::GetChildren(entry);
				if (children.size() != 2) {
					throw SerializationException("Secret map entry must have two fields");
				}
				if (children[0].IsNull() || children[0].type() != LogicalType::VARCHAR) {
					throw SerializationException("Secret map key must be a non-NULL VARCHAR");
				}
				result[Identifier(StringValue::Get(children[0]))] = children[1];
			});
		});
		map.Unset<LogicalType>();
	});
}

Value KeyValueSecret::TryGetValue(const Identifier &key, bool error_on_missing) const {
	auto lookup = secret_map.find(key);
	if (lookup == secret_map.end()) {
		if (error_on_missing) {
			throw InternalException("Failed to fetch key %s from secret %s of type %s", key, name, type);
		}
		return Value();
	}

	return lookup->second;
}

KeyValueSecretReader::KeyValueSecretReader(DatabaseInstance &db_p, const char **secret_types, idx_t secret_types_len,
                                           string path_p) {
	db = db_p;
	path = std::move(path_p);
	Initialize(secret_types, secret_types_len);
}

KeyValueSecretReader::KeyValueSecretReader(DatabaseInstance &db_p, const char *secret_type, string path)
    : KeyValueSecretReader(db_p, &secret_type, 1, std::move(path)) {
}

KeyValueSecretReader::KeyValueSecretReader(ClientContext &context_p, const char **secret_types, idx_t secret_types_len,
                                           string path_p) {
	context = context_p;
	path = std::move(path_p);
	Initialize(secret_types, secret_types_len);
}
KeyValueSecretReader::KeyValueSecretReader(ClientContext &context_p, const char *secret_type, string path)
    : KeyValueSecretReader(context_p, &secret_type, 1, std::move(path)) {
}

KeyValueSecretReader::KeyValueSecretReader(FileOpener &opener_p, optional_ptr<FileOpenerInfo> info,
                                           const char *secret_type)
    : KeyValueSecretReader(opener_p, info, &secret_type, 1) {
}

void KeyValueSecretReader::Initialize(const char **secret_types, idx_t secret_types_len) {
	if (!db) {
		// TODO does this even work?
		return;
	}

	auto &secret_manager = db->GetSecretManager();
	auto transaction = context ? CatalogTransaction::GetSystemCatalogTransaction(*context)
	                           : CatalogTransaction::GetSystemTransaction(*db);

	SecretMatch secret_match;
	for (idx_t i = 0; i < secret_types_len; i++) {
		auto &secret_type = secret_types[i];
		secret_match = secret_manager.LookupSecret(transaction, path, secret_type);
		if (secret_match.HasMatch()) {
			break;
		}
	}

	if (secret_match.HasMatch()) {
		secret = secret_match.GetSecret().Cast<KeyValueSecret>();
		secret_entry = std::move(secret_match.secret_entry);
	}
}

KeyValueSecretReader::KeyValueSecretReader(const KeyValueSecret &secret_p, FileOpener &opener_p) : secret(secret_p) {
	db = opener_p.TryGetDatabase();
	context = opener_p.TryGetClientContext();
}

KeyValueSecretReader::KeyValueSecretReader(FileOpener &opener_p, optional_ptr<FileOpenerInfo> info,
                                           const char **secret_types, idx_t secret_types_len) {
	db = opener_p.TryGetDatabase();
	context = opener_p.TryGetClientContext();

	if (info) {
		path = info->file_path;
	}

	Initialize(secret_types, secret_types_len);
}

KeyValueSecretReader::~KeyValueSecretReader() {
}

SettingLookupResult KeyValueSecretReader::TryGetSecretKey(const Identifier &secret_key, Value &result) {
	if (secret && secret->TryGetValue(secret_key, result)) {
		return SettingLookupResult(SettingScope::SECRET);
	}
	return SettingLookupResult();
}

SettingLookupResult KeyValueSecretReader::TryGetSecretKeyOrSetting(const Identifier &secret_key,
                                                                   const Identifier &setting_name, Value &result) {
	if (secret && secret->TryGetValue(secret_key, result)) {
		return SettingLookupResult(SettingScope::SECRET);
	}
	if (context) {
		auto res = context->TryGetCurrentSetting(setting_name, result);
		if (res) {
			return res;
		}
	}
	if (db) {
		db->TryGetCurrentSetting(setting_name, result);
	}
	return SettingLookupResult();
}

Value KeyValueSecretReader::GetSecretKey(const Identifier &secret_key) {
	Value result;
	if (TryGetSecretKey(secret_key, result)) {
		return result;
	}
	ThrowNotFoundError(secret_key);
}

Value KeyValueSecretReader::GetSecretKeyOrSetting(const Identifier &secret_key, const Identifier &setting_name) {
	Value result;
	if (TryGetSecretKeyOrSetting(secret_key, setting_name, result)) {
		return result;
	}
	ThrowNotFoundError(secret_key, setting_name);
}

void KeyValueSecretReader::ThrowNotFoundError(const Identifier &secret_key) {
	string base_message = "Failed to fetch required secret key %s from secret";

	if (!secret) {
		string secret_scope = path;
		string secret_scope_hint_message = secret_scope.empty() ? "." : " for '" + secret_scope + "'.";
		throw InvalidConfigurationException(base_message + ", because no secret was found%s", secret_key,
		                                    secret_scope_hint_message);
	}

	throw InvalidConfigurationException(base_message + " %s.", secret_key, secret->GetName());
}

void KeyValueSecretReader::ThrowNotFoundError(const Identifier &secret_key, const Identifier &setting_name) {
	string base_message = "Failed to fetch a parameter from either the secret key %s or the setting %s";

	if (!secret) {
		string secret_scope = path;
		string secret_scope_hint_message = secret_scope.empty() ? "." : " for '" + secret_scope + "'.";
		throw InvalidConfigurationException(base_message + ": no secret was found%s", secret_key, setting_name,
		                                    secret_scope_hint_message);
	}

	throw InvalidConfigurationException(base_message +
	                                        ": secret %s did not contain the key, also the setting was not found.",
	                                    secret_key, setting_name, secret->GetName());
}

bool CreateSecretFunctionSet::ProviderExists(const Identifier &provider_name) {
	return functions.find(Identifier(provider_name)) != functions.end();
}

void CreateSecretFunctionSet::AddFunction(CreateSecretFunction &function, OnCreateConflict on_conflict) {
	if (ProviderExists(function.provider)) {
		if (on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			throw InternalException(
			    "Attempted to override a Create Secret Function with OnCreateConflict::ERROR_ON_CONFLICT for: %s",
			    function.provider);
		} else if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
			functions[function.provider] = function;
		} else if (on_conflict == OnCreateConflict::ALTER_ON_CONFLICT) {
			throw NotImplementedException("ALTER_ON_CONFLICT not implemented for CreateSecretFunctionSet");
		}
	} else {
		functions[function.provider] = function;
	}
}

CreateSecretFunction &CreateSecretFunctionSet::GetFunction(const string &provider) {
	const auto &lookup = functions.find(Identifier(provider));

	if (lookup == functions.end()) {
		throw InternalException("Could not find Create Secret Function with provider %s");
	}

	return lookup->second;
}

} // namespace duckdb
