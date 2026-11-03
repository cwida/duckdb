//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {
class ExtensionLoader;

//! The Extension class is the base class used to define extensions
class Extension {
public:
	DUCKDB_API virtual ~Extension();

	DUCKDB_API virtual void Load(ExtensionLoader &db) = 0;
	DUCKDB_API virtual std::string Name() = 0;
	DUCKDB_API virtual std::string Version() const {
		return "";
	}
	DUCKDB_API static const char *DefaultVersion();
};

enum class ExtensionABIType : uint8_t {
	UNKNOWN = 0,
	//! Uses C++ ABI, version needs to match precisely
	CPP = 1,
	//! Uses C ABI, version needs to be equal or higher. The CAPI version major selects the API family: 1 uses the
	//! duckdb_ext_api_v1 struct and the <name>_init_c_api entrypoint, 2 the duckdb_ext_api_v2 struct and
	//! <name>_init_c_api_v2
	C_STRUCT = 2,
	//! Uses C ABI using the duckdb_ext_api_v2 struct including "unstable" functions, version needs to match precisely.
	//! Everything that was unstable in the V1 API was stabilized into v1.5.6, so "unstable" now always means V2
	C_STRUCT_UNSTABLE = 3
};

//! The parsed extension metadata footer
struct ParsedExtensionMetaData {
	//! Legacy footer: [metadata][256-byte signature]. Larger signatures: [metadata][signature][metadata][256 zeros]
	static constexpr const idx_t FOOTER_SIZE = 512;
	static constexpr const idx_t METADATA_SIZE = 256;
	//! Legacy RSA-2048 signature size, used when the footer carries no signature-size marker
	static constexpr const idx_t SIGNATURE_SIZE = 256;
	//! Supported signature sizes (RSA-2048/3072/4096)
	static constexpr const idx_t SUPPORTED_SIGNATURE_SIZES[] = {256, 384, 512};
	static constexpr const idx_t MAX_SIGNATURE_SIZE = 512;
	static constexpr const idx_t MAX_FOOTER_SIZE = FOOTER_SIZE + METADATA_SIZE + MAX_SIGNATURE_SIZE;
	static constexpr const char *EXPECTED_MAGIC_VALUE = {
	    "4\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"};

	string magic_value;

	ExtensionABIType abi_type;

	string platform;
	// (For ExtensionABIType::CPP or ExtensionABIType::C_STRUCT_UNSTABLE) the DuckDB version this extension is compiled
	// for
	string duckdb_version;
	// (only for ExtensionABIType::C_STRUCT) the CAPI version of the C_STRUCT (Currently interpreted as the minimum
	// DuckDB version)
	string duckdb_capi_version;
	string extension_version;
	string signature;
	string extension_abi_metadata;

	//! Signature size, from the signed marker in metadata field 6
	idx_t signature_size = SIGNATURE_SIZE;
	//! Offset of the signature in the file, which is also the length of the signed prefix
	idx_t signature_offset = 0;
	//! Set when the footer structure is malformed
	string metadata_error;

	bool AppearsValid() {
		return magic_value == EXPECTED_MAGIC_VALUE;
	}

	//! A malformed footer structure, which no setting can override
	bool HasFooterError() {
		return AppearsValid() && !metadata_error.empty();
	}

	// Returns an error string describing which parts of the metadata are mismatcheds
	string GetInvalidMetadataError();
};

struct VersioningUtils {
	//! Note: only supports format v{major}.{minor}.{patch}
	static bool ParseSemver(string &semver, idx_t &major_out, idx_t &minor_out, idx_t &patch_out);

	static bool IsReleaseVersion(const string &version_tag);

	//! Note: only supports format v{major}.{minor}.{patch}
	//! The major selects which C API family the version is checked against: 1 against duckdb_ext_api_v1, 2 against
	//! duckdb_ext_api_v2. Any other major is unsupported.
	static bool IsSupportedCAPIVersion(string &capi_version_string);
	static bool IsSupportedCAPIVersion(idx_t major, idx_t minor, idx_t patch);

	//! Whether a C_STRUCT extension built against this CAPI version uses the V2 API
	static bool IsCAPIV2Version(const string &capi_version_string);
};

} // namespace duckdb
