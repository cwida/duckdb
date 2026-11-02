//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/http/http_transport_config.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

struct HTTPParams;

//! Owns core's connection-affecting settings: active proxy endpoint/credentials and TLS verification override.
//! Inactive proxy fields and verify_ssl without an override do not affect compatibility.
//! Request headers, callbacks and provider-specific settings are not part of this key.
//! New connection-affecting HTTPParams fields must be included here.
class HTTPTransportConfig {
public:
	HTTPTransportConfig() = default;
	DUCKDB_API explicit HTTPTransportConfig(const HTTPParams &params);
	//! The same core compatibility rule for providers that also support direct client reuse.
	DUCKDB_API bool Matches(const HTTPParams &params) const;
	DUCKDB_API bool operator==(const HTTPTransportConfig &other) const;
	DUCKDB_API hash_t Hash() const;

private:
	string proxy_host;
	idx_t proxy_port = 0;
	string proxy_username;
	string proxy_password;
	bool override_verify_ssl = false;
	bool verify_ssl = true;
};

} // namespace duckdb
