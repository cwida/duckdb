//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/macro_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {
class Binder;
class FunctionExpression;

class ScalarMacroFunction;

enum class MacroType : uint8_t { VOID_MACRO = 0, TABLE_MACRO = 1, SCALAR_MACRO = 2 };

struct MacroBindResult {
	explicit MacroBindResult(string error_p) : error(std::move(error_p)) {
	}
	explicit MacroBindResult(idx_t function_idx) : function_idx(function_idx) {
	}

	optional_idx function_idx;
	string error;
};

class MacroFunction {
public:
	explicit MacroFunction(MacroType type);

	//! The type
	MacroType type;
	//! The parameters (ColumnRefExpression)
	vector<unique_ptr<ParsedExpression>> parameters;
	//! The default values of the parameters
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> default_parameters;
	//! The types of the parameters
	vector<LogicalType> types;
	//! The declared return type, UNKNOWN when none was declared: the member's result is cast to it
	LogicalType return_type = LogicalType::UNKNOWN;

public:
	virtual ~MacroFunction() {
	}

	void CopyProperties(MacroFunction &other) const;

	virtual unique_ptr<MacroFunction> Copy() const = 0;

	vector<unique_ptr<ParsedExpression>> GetPositionalParametersForSerialization(Serializer &serializer) const;
	void FinalizeDeserialization();

	static MacroBindResult BindMacroFunction(
	    Binder &binder, const vector<unique_ptr<MacroFunction>> &macro_functions, const Identifier &name,
	    FunctionExpression &function_expr, vector<unique_ptr<ParsedExpression>> &positional_arguments,
	    InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> &named_arguments,
	    idx_t depth);
	static unique_ptr<DummyBinding>
	CreateDummyBinding(const MacroFunction &macro_def, const Identifier &name,
	                   vector<unique_ptr<ParsedExpression>> &positional_arguments,
	                   InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>>
	                       &named_arguments);

	virtual string ToSQL() const;

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<MacroFunction> Deserialize(Deserializer &deserializer);

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast macro to type - macro type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast macro to type - macro type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
