#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"

namespace duckdb {
namespace variant {

//! Shreds VARIANT vectors using the type it was constructed with.
//! Values that do not match that type are kept in the unshredded part of the VARIANT.
class DUCKDB_API VariantShredder {
public:
	//! 'source_type' is the type that the VARIANT values are shredded into
	explicit VariantShredder(const LogicalType &source_type);
	~VariantShredder();

public:
	//! Shred 'unshredded' (a VARIANT vector) into 'result' (type VARIANT)
	bool Shred(Vector &unshredded, Vector &result, idx_t count);
	//! Whether the source type can be shredded
	bool SupportsShredding() const {
		return supports_shredding;
	}

private:
	//! The shredded type: STRUCT(unshredded, shredded)
	LogicalType shredded_type;
	//! Whether the source type can be shredded
	bool supports_shredding;
};

} // namespace variant
} // namespace duckdb
