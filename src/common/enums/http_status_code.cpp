#include "duckdb/main/http/http_util.hpp"

namespace duckdb {

HTTPStatusCode HTTPUtil::ToStatusCode(int32_t status_code) {
	if (status_code < 100 || status_code > 599) {
		return HTTPStatusCode::INVALID;
	}
	return static_cast<HTTPStatusCode>(status_code);
}

string HTTPUtil::GetStatusMessage(HTTPStatusCode status) {
	switch (status) {
	case HTTPStatusCode::Continue_100:
		return "Continue";
	case HTTPStatusCode::SwitchingProtocol_101:
		return "Switching Protocol";
	case HTTPStatusCode::Processing_102:
		return "Processing";
	case HTTPStatusCode::EarlyHints_103:
		return "Early Hints";
	case HTTPStatusCode::OK_200:
		return "OK";
	case HTTPStatusCode::Created_201:
		return "Created";
	case HTTPStatusCode::Accepted_202:
		return "Accepted";
	case HTTPStatusCode::NonAuthoritativeInformation_203:
		return "Non-Authoritative Information";
	case HTTPStatusCode::NoContent_204:
		return "No Content";
	case HTTPStatusCode::ResetContent_205:
		return "Reset Content";
	case HTTPStatusCode::PartialContent_206:
		return "Partial Content";
	case HTTPStatusCode::MultiStatus_207:
		return "Multi-Status";
	case HTTPStatusCode::AlreadyReported_208:
		return "Already Reported";
	case HTTPStatusCode::IMUsed_226:
		return "IM Used";
	case HTTPStatusCode::MultipleChoices_300:
		return "Multiple Choices";
	case HTTPStatusCode::MovedPermanently_301:
		return "Moved Permanently";
	case HTTPStatusCode::Found_302:
		return "Found";
	case HTTPStatusCode::SeeOther_303:
		return "See Other";
	case HTTPStatusCode::NotModified_304:
		return "Not Modified";
	case HTTPStatusCode::UseProxy_305:
		return "Use Proxy";
	case HTTPStatusCode::unused_306:
		return "unused";
	case HTTPStatusCode::TemporaryRedirect_307:
		return "Temporary Redirect";
	case HTTPStatusCode::PermanentRedirect_308:
		return "Permanent Redirect";
	case HTTPStatusCode::BadRequest_400:
		return "Bad Request";
	case HTTPStatusCode::Unauthorized_401:
		return "Unauthorized";
	case HTTPStatusCode::PaymentRequired_402:
		return "Payment Required";
	case HTTPStatusCode::Forbidden_403:
		return "Forbidden";
	case HTTPStatusCode::NotFound_404:
		return "Not Found";
	case HTTPStatusCode::MethodNotAllowed_405:
		return "Method Not Allowed";
	case HTTPStatusCode::NotAcceptable_406:
		return "Not Acceptable";
	case HTTPStatusCode::ProxyAuthenticationRequired_407:
		return "Proxy Authentication Required";
	case HTTPStatusCode::RequestTimeout_408:
		return "Request Timeout";
	case HTTPStatusCode::Conflict_409:
		return "Conflict";
	case HTTPStatusCode::Gone_410:
		return "Gone";
	case HTTPStatusCode::LengthRequired_411:
		return "Length Required";
	case HTTPStatusCode::PreconditionFailed_412:
		return "Precondition Failed";
	case HTTPStatusCode::PayloadTooLarge_413:
		return "Payload Too Large";
	case HTTPStatusCode::UriTooLong_414:
		return "URI Too Long";
	case HTTPStatusCode::UnsupportedMediaType_415:
		return "Unsupported Media Type";
	case HTTPStatusCode::RangeNotSatisfiable_416:
		return "Range Not Satisfiable";
	case HTTPStatusCode::ExpectationFailed_417:
		return "Expectation Failed";
	case HTTPStatusCode::ImATeapot_418:
		return "I'm a teapot";
	case HTTPStatusCode::MisdirectedRequest_421:
		return "Misdirected Request";
	case HTTPStatusCode::UnprocessableContent_422:
		return "Unprocessable Content";
	case HTTPStatusCode::Locked_423:
		return "Locked";
	case HTTPStatusCode::FailedDependency_424:
		return "Failed Dependency";
	case HTTPStatusCode::TooEarly_425:
		return "Too Early";
	case HTTPStatusCode::UpgradeRequired_426:
		return "Upgrade Required";
	case HTTPStatusCode::PreconditionRequired_428:
		return "Precondition Required";
	case HTTPStatusCode::TooManyRequests_429:
		return "Too Many Requests";
	case HTTPStatusCode::RequestHeaderFieldsTooLarge_431:
		return "Request Header Fields Too Large";
	case HTTPStatusCode::UnavailableForLegalReasons_451:
		return "Unavailable For Legal Reasons";
	case HTTPStatusCode::NotImplemented_501:
		return "Not Implemented";
	case HTTPStatusCode::BadGateway_502:
		return "Bad Gateway";
	case HTTPStatusCode::ServiceUnavailable_503:
		return "Service Unavailable";
	case HTTPStatusCode::GatewayTimeout_504:
		return "Gateway Timeout";
	case HTTPStatusCode::HttpVersionNotSupported_505:
		return "HTTP Version Not Supported";
	case HTTPStatusCode::VariantAlsoNegotiates_506:
		return "Variant Also Negotiates";
	case HTTPStatusCode::InsufficientStorage_507:
		return "Insufficient Storage";
	case HTTPStatusCode::LoopDetected_508:
		return "Loop Detected";
	case HTTPStatusCode::NotExtended_510:
		return "Not Extended";
	case HTTPStatusCode::NetworkAuthenticationRequired_511:
		return "Network Authentication Required";

	case HTTPStatusCode::InternalServerError_500:
		return "Internal Server Error";
	default:
		return "Unknown";
	}
}

} // namespace duckdb
