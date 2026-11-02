#include "duckdb/main/http/http_transport_config.hpp"

#include "duckdb/main/http/http_util.hpp"

namespace duckdb {

HTTPTransportConfig::HTTPTransportConfig(const HTTPParams &params)
    : override_verify_ssl(params.override_verify_ssl), verify_ssl(!params.override_verify_ssl || params.verify_ssl) {
	if (!params.http_proxy.empty()) {
		proxy_host = params.http_proxy;
		proxy_port = params.http_proxy_port;
		if (!params.http_proxy_username.empty()) {
			proxy_username = params.http_proxy_username;
			proxy_password = params.http_proxy_password;
		}
	}
}

bool HTTPTransportConfig::Matches(const HTTPParams &params) const {
	if (proxy_host != params.http_proxy || override_verify_ssl != params.override_verify_ssl ||
	    verify_ssl != (!params.override_verify_ssl || params.verify_ssl)) {
		return false;
	}
	if (proxy_host.empty()) {
		return true;
	}
	return proxy_port == params.http_proxy_port && proxy_username == params.http_proxy_username &&
	       (proxy_username.empty() || proxy_password == params.http_proxy_password);
}

bool HTTPTransportConfig::operator==(const HTTPTransportConfig &other) const {
	return proxy_host == other.proxy_host && proxy_port == other.proxy_port && proxy_username == other.proxy_username &&
	       proxy_password == other.proxy_password && override_verify_ssl == other.override_verify_ssl &&
	       verify_ssl == other.verify_ssl;
}

hash_t HTTPTransportConfig::Hash() const {
	auto result = std::hash<string> {}(proxy_host);
	auto combine = [&](hash_t value) {
		result ^= value + 0x9e3779b9U + (result << 6U) + (result >> 2U);
	};
	combine(std::hash<idx_t> {}(proxy_port));
	combine(std::hash<string> {}(proxy_username));
	combine(std::hash<string> {}(proxy_password));
	combine(std::hash<bool> {}(override_verify_ssl));
	combine(std::hash<bool> {}(verify_ssl));
	return result;
}

} // namespace duckdb
