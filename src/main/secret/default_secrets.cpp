#include "duckdb/main/secret/default_secrets.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/os_util.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

identifier_set_t CreateHTTPSecretFunctions::GetRedactedKeys() {
	return {"http_proxy_password", "bearer_token", "extra_http_headers"};
}

vector<SecretType> CreateHTTPSecretFunctions::GetDefaultSecretTypes() {
	vector<SecretType> result;

	// HTTP secret
	SecretType secret_type;
	secret_type.name = "http";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	result.push_back(std::move(secret_type));

	return result;
}

//! Get the default secret functions
vector<CreateSecretFunction> CreateHTTPSecretFunctions::GetDefaultSecretFunctions() {
	vector<CreateSecretFunction> result;

	// HTTP secret CONFIG provider
	CreateSecretFunction http_config_fun;
	http_config_fun.secret_type = "http";
	http_config_fun.provider = "config";
	http_config_fun.function = CreateHTTPSecretFromConfig;

	http_config_fun.named_parameters["verify_ssl"] = LogicalType::BOOLEAN;
	http_config_fun.named_parameters["http_proxy"] = LogicalType::VARCHAR;
	http_config_fun.named_parameters["http_proxy_password"] = LogicalType::VARCHAR;
	http_config_fun.named_parameters["http_proxy_username"] = LogicalType::VARCHAR;

	http_config_fun.named_parameters["extra_http_headers"] =
	    LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	http_config_fun.named_parameters["bearer_token"] = LogicalType::VARCHAR;
	result.push_back(std::move(http_config_fun));

	// HTTP secret ENV provider
	CreateSecretFunction http_env_fun;
	http_env_fun.secret_type = "http";
	http_env_fun.provider = "env";
	http_env_fun.function = CreateHTTPSecretFromEnv;

	http_env_fun.named_parameters["verify_ssl"] = LogicalType::BOOLEAN;
	http_env_fun.named_parameters["http_proxy"] = LogicalType::VARCHAR;
	http_env_fun.named_parameters["http_proxy_password"] = LogicalType::VARCHAR;
	http_env_fun.named_parameters["http_proxy_username"] = LogicalType::VARCHAR;

	http_env_fun.named_parameters["extra_http_headers"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	http_env_fun.named_parameters["bearer_token"] = LogicalType::VARCHAR;
	result.push_back(std::move(http_env_fun));

	return result;
}

//! Reads a proxy setting from the process environment into the secret, trying the lowercase name before the uppercase
static void TrySetValueFromEnv(OSUtil &os_util, KeyValueSecret &secret, const string &name) {
	string value;
	if (os_util.GetEnv(name, value) || os_util.GetEnv(StringUtil::Upper(name), value)) {
		secret.secret_map[Identifier(name)] = Value(value);
	}
}

unique_ptr<BaseSecret> CreateHTTPSecretFunctions::CreateHTTPSecretFromEnv(ClientContext &context,
                                                                          CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	auto &os_util = OSUtil::Get(*context.db);
	TrySetValueFromEnv(os_util, *secret, "http_proxy");
	TrySetValueFromEnv(os_util, *secret, "http_proxy_password");
	TrySetValueFromEnv(os_util, *secret, "http_proxy_username");

	// Allow overwrites
	secret->TrySetValue("verify_ssl", input);
	secret->TrySetValue("http_proxy", input);
	secret->TrySetValue("http_proxy_password", input);
	secret->TrySetValue("http_proxy_username", input);

	secret->TrySetValue("extra_http_headers", input);
	secret->TrySetValue("bearer_token", input);

	//! Set redact keys
	secret->redact_keys = GetRedactedKeys();

	return std::move(secret);
}

unique_ptr<BaseSecret> CreateHTTPSecretFunctions::CreateHTTPSecretFromConfig(ClientContext &context,
                                                                             CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	secret->TrySetValue("verify_ssl", input);
	secret->TrySetValue("http_proxy", input);
	secret->TrySetValue("http_proxy_password", input);
	secret->TrySetValue("http_proxy_username", input);

	secret->TrySetValue("extra_http_headers", input);
	secret->TrySetValue("bearer_token", input);

	//! Set redact keys
	secret->redact_keys = GetRedactedKeys();

	return std::move(secret);
}

} // namespace duckdb
