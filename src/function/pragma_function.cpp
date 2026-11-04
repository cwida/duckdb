#include "duckdb/function/pragma_function.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

PragmaFunctionInfo::~PragmaFunctionInfo() {
}

//! A pragma's varargs are a positional list, so it declares "*args" and never a "**kwargs" - an argument named after
//! no parameter stays an error rather than being absorbed
static FunctionSignature PragmaSignature(vector<LogicalType> arguments, LogicalType varargs) {
	FunctionSignature signature;
	for (auto &argument : arguments) {
		signature.AddParameter(std::move(argument));
	}
	if (varargs.id() != LogicalTypeId::INVALID) {
		signature.AddArgs("args", std::move(varargs));
	}
	return signature;
}

PragmaFunction::PragmaFunction(Identifier name, PragmaType pragma_type, pragma_query_t query,
                               pragma_function_t function, vector<LogicalType> arguments, LogicalType varargs)
    : SimpleFunction(std::move(name), PragmaSignature(std::move(arguments), std::move(varargs))), type(pragma_type),
      query(query), function(function) {
}

PragmaFunction PragmaFunction::PragmaCall(const Identifier &name, pragma_query_t query, vector<LogicalType> arguments,
                                          LogicalType varargs) {
	return PragmaFunction(name, PragmaType::PRAGMA_CALL, query, nullptr, std::move(arguments), std::move(varargs));
}

PragmaFunction PragmaFunction::PragmaCall(const Identifier &name, pragma_function_t function,
                                          vector<LogicalType> arguments, LogicalType varargs) {
	return PragmaFunction(name, PragmaType::PRAGMA_CALL, nullptr, function, std::move(arguments), std::move(varargs));
}

PragmaFunction::PragmaFunction(Identifier name, PragmaType pragma_type, pragma_query_t query,
                               pragma_function_t function, FunctionSignature signature)
    : SimpleFunction(std::move(name), std::move(signature)), type(pragma_type), query(query), function(function) {
}

PragmaFunction PragmaFunction::PragmaCall(const Identifier &name, pragma_query_t query, FunctionSignature signature) {
	return PragmaFunction(name, PragmaType::PRAGMA_CALL, query, nullptr, std::move(signature));
}

PragmaFunction PragmaFunction::PragmaCall(const Identifier &name, pragma_function_t function,
                                          FunctionSignature signature) {
	return PragmaFunction(name, PragmaType::PRAGMA_CALL, nullptr, function, std::move(signature));
}

PragmaFunction PragmaFunction::PragmaStatement(const Identifier &name, pragma_query_t query) {
	vector<LogicalType> types;
	return PragmaFunction(name, PragmaType::PRAGMA_STATEMENT, query, nullptr, std::move(types), LogicalType::INVALID);
}

PragmaFunction PragmaFunction::PragmaStatement(const Identifier &name, pragma_function_t function) {
	vector<LogicalType> types;
	return PragmaFunction(name, PragmaType::PRAGMA_STATEMENT, nullptr, function, std::move(types),
	                      LogicalType::INVALID);
}

string PragmaFunction::ToString() const {
	switch (type) {
	case PragmaType::PRAGMA_STATEMENT:
		return StringUtil::Format("PRAGMA %s", name);
	case PragmaType::PRAGMA_CALL: {
		return StringUtil::Format("PRAGMA %s", SimpleFunction::ToString());
	}
	default:
		return "UNKNOWN";
	}
}

} // namespace duckdb
