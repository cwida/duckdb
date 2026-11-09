#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension_helper.hpp"

using namespace duckdb;

namespace {

ParsedExtensionMetaData FooterFor(ExtensionABIType abi_type, const string &version) {
	ParsedExtensionMetaData metadata;
	metadata.magic_value = ParsedExtensionMetaData::EXPECTED_MAGIC_VALUE;
	metadata.abi_type = abi_type;
	metadata.platform = DuckDB::Platform();
	if (abi_type == ExtensionABIType::C_STRUCT) {
		metadata.duckdb_capi_version = version;
	} else {
		metadata.duckdb_version = version;
	}
	return metadata;
}

} // namespace

TEST_CASE("V2: an extension loads only when pinned to this DuckDB version", "[capi_v2]") {
	auto pinned = FooterFor(ExtensionABIType::C_STRUCT_UNSTABLE, ExtensionHelper::GetVersionDirectoryName());
	REQUIRE(pinned.GetInvalidMetadataError() == "");

	auto other_build = FooterFor(ExtensionABIType::C_STRUCT_UNSTABLE, "v0.0.0-not-this-build");
	REQUIRE(StringUtil::Contains(other_build.GetInvalidMetadataError(), "built specifically for DuckDB version"));

	for (auto version : {"v2.0.0", "v2.0.1", "v2.9.0"}) {
		auto versioned = FooterFor(ExtensionABIType::C_STRUCT, version);
		auto error = versioned.GetInvalidMetadataError();
		REQUIRE(StringUtil::Contains(error, "the V2 C API has no stable surface yet"));
		REQUIRE(!StringUtil::Contains(error, "and lower"));
	}

	auto v1 = FooterFor(ExtensionABIType::C_STRUCT, "v1.2.0");
	REQUIRE(v1.GetInvalidMetadataError() == "");

	auto v3 = FooterFor(ExtensionABIType::C_STRUCT, "v3.0.0");
	REQUIRE(StringUtil::Contains(v3.GetInvalidMetadataError(), "we can only load extensions built for DuckDB C API"));
}
