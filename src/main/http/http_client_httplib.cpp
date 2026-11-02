#include "duckdb/main/http/http_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb_static_extension.h"

#include "httplib.hpp"

#if defined(DUCKDB_DISABLE_BUILTIN_HTTPLIB) || defined(DISABLE_DUCKDB_REMOTE_INSTALL) ||                               \
    defined(DUCKDB_DISABLE_EXTENSION_LOAD)
#define DUCKDB_HTTPLIB_DEFINE_WARNING                                                                                  \
	"DUCKDB_DISABLE_BUILTIN_HTTPLIB, DISABLE_DUCKDB_REMOTE_INSTALL and DUCKDB_DISABLE_EXTENSION_LOAD no longer "       \
	"disable the built-in httplib client: leave duckdb_httplib out of the link instead (ENABLE_BUILTIN_HTTPLIB=OFF, "  \
	"or package_build.py with builtin_httplib=False)"
#if defined(_MSC_VER)
#pragma message("warning: " DUCKDB_HTTPLIB_DEFINE_WARNING)
#else
#pragma GCC warning DUCKDB_HTTPLIB_DEFINE_WARNING
#endif
#endif

namespace duckdb {

class HTTPLibClient : public HTTPClient {
public:
	HTTPLibClient(HTTPParams &http_params, const string &proto_host_port) : HTTPClient(proto_host_port) {
		client = make_uniq<duckdb_httplib::Client>(proto_host_port);
		Initialize(http_params);
	}
	void Initialize(HTTPParams &http_params) override {
		auto sec = static_cast<time_t>(http_params.timeout);
		auto usec = static_cast<time_t>(http_params.timeout_usec);
		client->set_follow_location(http_params.follow_location);
		client->set_keep_alive(http_params.keep_alive);
		client->set_write_timeout(sec, usec);
		client->set_read_timeout(sec, usec);
		client->set_connection_timeout(sec, usec);
		client->set_decompress(false);

		if (!http_params.http_proxy.empty()) {
			client->set_proxy(http_params.http_proxy, static_cast<int>(http_params.http_proxy_port));

			if (!http_params.http_proxy_username.empty()) {
				client->set_proxy_basic_auth(http_params.http_proxy_username, http_params.http_proxy_password);
			}
		}
	}
	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override {
		auto headers = TransformHeaders(info.headers, info.params);
		if (!info.response_handler && !info.content_handler) {
			return TransformResult(client->Get(info.path, headers));
		} else {
			return TransformResult(client->Get(
			    info.path, headers,
			    [&](const duckdb_httplib::Response &response) {
				    auto http_response = TransformResponse(response);
				    return info.response_handler(*http_response);
			    },
			    [&](const char *data, size_t data_length) {
				    return info.content_handler(const_data_ptr_cast(data), data_length);
			    }));
		}
	}
	unique_ptr<HTTPResponse> Put(PutRequestInfo &info) override {
		throw NotImplementedException("PUT request not implemented");
	}

	unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) override {
		throw NotImplementedException("HEAD request not implemented");
	}

	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) override {
		throw NotImplementedException("DELETE request not implemented");
	}

	unique_ptr<HTTPResponse> Post(PostRequestInfo &info) override {
		throw NotImplementedException("POST request not implemented");
	}

	unique_ptr<HTTPResponse> Options(OptionsRequestInfo &info) override {
		throw NotImplementedException("OPTIONS request not implemented");
	}

	unique_ptr<duckdb_httplib::Client> client;

private:
	duckdb_httplib::Headers TransformHeaders(const HTTPHeaders &header_map, const HTTPParams &params) {
		duckdb_httplib::Headers headers;
		for (auto &entry : header_map) {
			headers.insert(entry);
		}
		return headers;
	}

	unique_ptr<HTTPResponse> TransformResponse(const duckdb_httplib::Response &response) {
		auto status_code = HTTPUtil::ToStatusCode(response.status);
		auto result = make_uniq<HTTPResponse>(status_code);
		result->body = response.body;
		result->reason = response.reason;
		for (auto &entry : response.headers) {
			result->headers.Append(entry.first, entry.second);
		}
		return result;
	}

	unique_ptr<HTTPResponse> TransformResult(const duckdb_httplib::Result &res) {
		if (res.error() == duckdb_httplib::Error::Success) {
			auto &response = res.value();
			return TransformResponse(response);
		} else {
			auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			result->request_error = to_string(res.error());
			return result;
		}
	}
};

class HTTPLibHTTPUtil : public HTTPUtil {
public:
	string GetName() const override {
		return "Built-In";
	}

	unique_ptr<HTTPClient> InitializeClient(HTTPParams &http_params, const string &proto_host_port) override {
		return make_uniq<HTTPLibClient>(http_params, proto_host_port);
	}
};

static void RegisterHTTPLibClient(DatabaseInstance &db) {
	db.config.SetHTTPUtil(make_shared_ptr<HTTPLibHTTPUtil>());
}

} // namespace duckdb

//! Registers the built-in httplib client for every database opened afterwards, through
//! duckdb_register_static_extension. It is not an extension, so it asks for a database callback.
extern "C" int32_t duckdb_extension_httplib_describe(duckdb_extension_descriptor *descriptor) {
	if (descriptor->version < 2) {
		descriptor->set_error(descriptor, "httplib needs descriptor layout 2");
		return 1;
	}
	descriptor->version = 2;
	descriptor->name = "httplib";
	descriptor->database_callback = reinterpret_cast<void (*)(void)>(duckdb::RegisterHTTPLibClient);
	return 0;
}
