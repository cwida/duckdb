#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/parser/qualified_name.hpp"

namespace duckdb::capiv2 {

// The handle invariant: between one and three parts, none of them empty. Partial qualification is fewer parts, never
// an empty placeholder, so this also rejects the placeholder spelling the engine's own constructors accept.
static void CheckQNameParts(const QualifiedName &name, const char *function_name) {
	auto &path = name.Path();
	if (path.empty() || path.size() > 3) {
		throw InvalidInputException("A qualified name must have between one and three parts in %s.", function_name);
	}
	for (auto &part : path) {
		if (part.empty()) {
			throw InvalidInputException("A qualified name part cannot be empty in %s.", function_name);
		}
	}
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_qname_parse(const duckdb_v2_str *text, duckdb_v2_qname_handle *out_name,
                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(text);
	DUCKDB_CHECK_ARG(out_name);
	*out_name = nullptr;
	return WithErrorHandler(err, [&]() {
		duckdb::QualifiedName parsed;
		duckdb::string error;
		if (!duckdb::QualifiedName::TryParse(duckdb::string(ConvertIdentifierName(text)), parsed, error)) {
			throw duckdb::InvalidInputException("%s in duckdb_v2_qname_parse.", error);
		}
		CheckQNameParts(parsed, "duckdb_v2_qname_parse");
		*out_name = Convert(new duckdb::QualifiedName(std::move(parsed)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_qname_create(const duckdb_v2_identifier_t *parts, idx_t part_count,
                                       duckdb_v2_qname_handle *out_name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_name);
	*out_name = nullptr;
	return WithErrorHandler(err, [&]() {
		if (part_count == 0 || part_count > 3) {
			throw duckdb::InvalidInputException(
			    "A qualified name must have between one and three parts in duckdb_v2_qname_create.");
		}
		if (!parts) {
			throw duckdb::InvalidInputException("Parts cannot be null when part_count is non-zero.");
		}

		// The last part is the object name; everything before it is the qualification.
		duckdb::vector<duckdb::Identifier> qualification;
		for (idx_t i = 0; i + 1 < part_count; i++) {
			qualification.push_back(duckdb::Identifier(ConvertIdentifierName(parts[i])));
		}
		auto name = duckdb::QualifiedName(std::move(qualification),
		                                  duckdb::Identifier(ConvertIdentifierName(parts[part_count - 1])));
		CheckQNameParts(name, "duckdb_v2_qname_create");
		*out_name = Convert(new duckdb::QualifiedName(std::move(name)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_qname_get_part_count(duckdb_v2_qname_handle name, idx_t *out_count,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = Convert(name)->Path().size(); });
}

DUCKDB_V2_ERROR duckdb_v2_qname_get_part(duckdb_v2_qname_handle name, idx_t index, duckdb_v2_identifier_t *out_part,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_part);
	*out_part = duckdb_v2_identifier_t {nullptr, 0};
	return WithErrorHandler(err, [&]() {
		auto &path = Convert(name)->Path();
		if (index >= path.size()) {
			throw duckdb::Exception(duckdb::ExceptionType::OUT_OF_RANGE,
			                        duckdb::StringUtil::Format("Part index %llu is out of range for a qualified name "
			                                                   "with %llu parts in duckdb_v2_qname_get_part.",
			                                                   static_cast<uint64_t>(index),
			                                                   static_cast<uint64_t>(path.size())));
		}
		*out_part = Convert(path[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_qname_render(duckdb_v2_qname_handle name, char *out_text, idx_t out_capacity,
                                       idx_t *out_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_length);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		FillCallerText(out_text, out_capacity, out_length, Convert(name)->ToString(), "duckdb_v2_qname_render");
	});
}

DUCKDB_V2_ERROR duckdb_v2_qname_equals(duckdb_v2_qname_handle left, duckdb_v2_qname_handle right, bool *result,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(left);
	DUCKDB_CHECK_ARG(right);
	DUCKDB_CHECK_ARG(result);
	return WithErrorHandler(err, [&]() { *result = *Convert(left) == *Convert(right); });
}

DUCKDB_V2_ERROR duckdb_v2_qname_hash(duckdb_v2_qname_handle name, uint64_t *out_hash,
                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_hash);
	return WithErrorHandler(err, [&]() { *out_hash = Convert(name)->Hash(); });
}

DUCKDB_V2_ERROR duckdb_v2_qname_destroy(duckdb_v2_qname_handle *name) {
	return WithErrorHandler(nullptr, [&]() {
		if (!name) {
			return;
		}
		if (*name) {
			delete Convert(*name);
			*name = nullptr;
		}
	});
}
