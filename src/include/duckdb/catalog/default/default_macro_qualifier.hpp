//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_macro_qualifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {
class MacroFunction;

//! Qualifies the function references inside a built-in macro body with the system catalog when
//! the default entry is created, so the body binds the same way under any caller search path.
//! Only meant for the core built-in definitions: they reference system functions only, use no
//! column named like a SQL value function, and no lookup-lowered operator other than slicing
class DefaultMacroQualifier {
public:
	//! Qualify function, operator, window, and bare SQL value function references in the body and
	//! default parameter values, and rewrite slice syntax into an array_slice call. References
	//! resolve to the macro's own schema when that schema defines a default macro of that name,
	//! otherwise to the default schema of the system catalog. Element extraction is left as is.
	static void Qualify(MacroFunction &macro, const Identifier &macro_schema);
};

} // namespace duckdb
