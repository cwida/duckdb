//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/column_definition.hpp"

namespace duckdb {
class CatalogEntry;
class Catalog;
class ClientContext;
class Expression;
class ExpressionExecutor;
class Transaction;

class AggregateFunction;
class AggregateFunctionSet;
class CopyFunction;
class PragmaFunction;
class ScalarFunctionSet;
class ScalarFunction;
class TableFunctionSet;
class TableFunction;

struct PragmaInfo;

struct FunctionData {
	virtual ~FunctionData() {
	}

	virtual unique_ptr<FunctionData> Copy() {
		throw InternalException("Unimplemented copy for FunctionData");
	};
	virtual bool Equals(FunctionData &other) {
		return true;
	}
	static bool Equals(FunctionData *left, FunctionData *right) {
		if (left == right) {
			return true;
		}
		if (!left || !right) {
			return false;
		}
		return left->Equals(*right);
	}
};

struct TableFunctionData : public FunctionData {
	// used to pass on projections to table functions that support them. NB, can contain COLUMN_IDENTIFIER_ROW_ID
	vector<idx_t> column_ids;
};

struct FunctionParameters {
	vector<Value> values;
	unordered_map<string, Value> named_parameters;
};

//! Function is the base class used for any type of function (scalar, aggregate or simple function)
class Function {
public:
	explicit Function(string name) : name(name) {
	}
	virtual ~Function() {
	}

	//! The name of the function
	string name;

public:
	//! Returns the formatted string name(arg1, arg2, ...)
	DUCKDB_API static string CallToString(const string &name, const vector<LogicalType> &arguments);
	//! Returns the formatted string name(arg1, arg2..) -> return_type
	DUCKDB_API static string CallToString(const string &name, const vector<LogicalType> &arguments,
	                                      const LogicalType &return_type);
	//! Returns the formatted string name(arg1, arg2.., np1=a, np2=b, ...)
	DUCKDB_API static string CallToString(const string &name, const vector<LogicalType> &arguments,
	                                      const unordered_map<string, LogicalType> &named_parameters);

	//! Bind a scalar function from the set of functions and input arguments. Returns the index of the chosen function,
	//! returns INVALID_INDEX and sets error if none could be found
	static idx_t BindFunction(const string &name, vector<ScalarFunction> &functions, vector<LogicalType> &arguments,
	                          string &error);
	static idx_t BindFunction(const string &name, vector<ScalarFunction> &functions,
	                          vector<unique_ptr<Expression>> &arguments, string &error);
	//! Bind an aggregate function from the set of functions and input arguments. Returns the index of the chosen
	//! function, returns INVALID_INDEX and sets error if none could be found
	static idx_t BindFunction(const string &name, vector<AggregateFunction> &functions, vector<LogicalType> &arguments,
	                          string &error);
	static idx_t BindFunction(const string &name, vector<AggregateFunction> &functions,
	                          vector<unique_ptr<Expression>> &arguments, string &error);
	//! Bind a table function from the set of functions and input arguments. Returns the index of the chosen
	//! function, returns INVALID_INDEX and sets error if none could be found
	static idx_t BindFunction(const string &name, vector<TableFunction> &functions, vector<LogicalType> &arguments,
	                          string &error);
	static idx_t BindFunction(const string &name, vector<TableFunction> &functions,
	                          vector<unique_ptr<Expression>> &arguments, string &error);
	//! Bind a pragma function from the set of functions and input arguments
	static idx_t BindFunction(const string &name, vector<PragmaFunction> &functions, PragmaInfo &info, string &error);
};

class SimpleFunction : public Function {
public:
	SimpleFunction(string name, vector<LogicalType> arguments,
	               LogicalType varargs = LogicalType(LogicalTypeId::INVALID))
	    : Function(name), arguments(move(arguments)), varargs(varargs) {
	}
	~SimpleFunction() override {
	}

	//! The set of arguments of the function
	vector<LogicalType> arguments;
	//! The type of varargs to support, or LogicalTypeId::INVALID if the function does not accept variable length
	//! arguments
	LogicalType varargs;

public:
	virtual string ToString() {
		return Function::CallToString(name, arguments);
	}

	bool HasVarArgs() const {
		return varargs.id() != LogicalTypeId::INVALID;
	}
};

class SimpleNamedParameterFunction : public SimpleFunction {
public:
	SimpleNamedParameterFunction(string name, vector<LogicalType> arguments,
	                             LogicalType varargs = LogicalType(LogicalTypeId::INVALID))
	    : SimpleFunction(name, move(arguments), varargs) {
	}
	~SimpleNamedParameterFunction() override {
	}

	//! The named parameters of the function
	unordered_map<string, LogicalType> named_parameters;

public:
	string ToString() override {
		return Function::CallToString(name, arguments, named_parameters);
	}

	bool HasNamedParameters() {
		return named_parameters.size() != 0;
	}

	void EvaluateInputParameters(vector<LogicalType> &arguments, vector<Value> &parameters,
	                             unordered_map<string, Value> &named_parameters,
	                             vector<unique_ptr<ParsedExpression>> &children);
};

class BaseScalarFunction : public SimpleFunction {
public:
	BaseScalarFunction(string name, vector<LogicalType> arguments, LogicalType return_type, bool has_side_effects,
	                   LogicalType varargs = LogicalType(LogicalTypeId::INVALID))
	    : SimpleFunction(move(name), move(arguments), move(varargs)), return_type(return_type),
	      has_side_effects(has_side_effects) {
	}
	~BaseScalarFunction() override {
	}

	//! Return type of the function
	LogicalType return_type;
	//! Whether or not the function has side effects (e.g. sequence increments, random() functions, NOW()). Functions
	//! with side-effects cannot be constant-folded.
	bool has_side_effects;

public:
	hash_t Hash() const;

	//! Cast a set of expressions to the arguments of this function
	void CastToFunctionArguments(vector<unique_ptr<Expression>> &children);

	string ToString() override {
		return Function::CallToString(name, arguments, return_type);
	}
};

class BuiltinFunctions {
public:
	BuiltinFunctions(ClientContext &transaction, Catalog &catalog);

	//! Initialize a catalog with all built-in functions
	void Initialize();

public:
	void AddFunction(AggregateFunctionSet set);
	void AddFunction(AggregateFunction function);
	void AddFunction(ScalarFunctionSet set);
	void AddFunction(PragmaFunction function);
	void AddFunction(const string &name, vector<PragmaFunction> functions);
	void AddFunction(ScalarFunction function);
	void AddFunction(const vector<string> &names, ScalarFunction function);
	void AddFunction(TableFunctionSet set);
	void AddFunction(TableFunction function);
	void AddFunction(CopyFunction function);

	void AddCollation(string name, ScalarFunction function, bool combinable = false,
	                  bool not_required_for_equality = false);

private:
	ClientContext &context;
	Catalog &catalog;

private:
	template <class T>
	void Register() {
		T::RegisterFunction(*this);
	}

	// table-producing functions
	void RegisterSQLiteFunctions();
	void RegisterReadFunctions();
	void RegisterTableFunctions();
	void RegisterArrowFunctions();

	// aggregates
	void RegisterAlgebraicAggregates();
	void RegisterDistributiveAggregates();
	void RegisterNestedAggregates();
	void RegisterHolisticAggregates();
	void RegisterRegressiveAggregates();

	// scalar functions
	void RegisterDateFunctions();
	void RegisterGenericFunctions();
	void RegisterMathFunctions();
	void RegisterOperators();
	void RegisterStringFunctions();
	void RegisterNestedFunctions();
	void RegisterSequenceFunctions();
	void RegisterTrigonometricsFunctions();

	// pragmas
	void RegisterPragmaFunctions();
};

} // namespace duckdb
