//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi_v2/capi_v2_function_internal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/common/named_parameter_map.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb::capiv2 {

//! An example of calling a function, with an optional title.
struct CV2FunctionExample {
	string title;
	string example;
};

//! The documentation of one overload, as `duckdb_functions()` reports it.
class CV2FunctionDocs {
public:
	//! Adds this overload's description to `info`, matched to the overload by its parameter types; the parameter names
	//! come from the signature itself. Adds nothing when no documentation was set.
	void AddTo(const FunctionSignature &signature, CreateFunctionInfo &info) const {
		vector<LogicalType> parameter_types;
		for (idx_t i = 0; i < signature.GetParameterCount(); i++) {
			auto &parameter = signature.GetParameter(i);
			if (parameter.IsVariadic()) {
				continue;
			}
			parameter_types.push_back(parameter.GetType());
		}
		AddTo(std::move(parameter_types), info);
	}
	//! As above, matched to the overload by the given parameter types
	void AddTo(vector<LogicalType> parameter_types, CreateFunctionInfo &info) const {
		if (description.empty() && examples.empty() && categories.empty()) {
			return;
		}
		FunctionDescription result;
		result.parameter_types = std::move(parameter_types);
		result.description = description;
		for (auto &entry : examples) {
			result.examples.push_back(entry.example);
		}
		result.categories = categories;
		info.descriptions.push_back(std::move(result));
	}

public:
	string description;
	vector<CV2FunctionExample> examples;
	vector<string> categories;
};

inline auto Convert(duckdb_v2_function_docs_handle docs) -> CV2FunctionDocs * {
	return reinterpret_cast<CV2FunctionDocs *>(docs);
}

inline auto Convert(CV2FunctionDocs *docs) -> duckdb_v2_function_docs_handle {
	return reinterpret_cast<duckdb_v2_function_docs_handle>(docs);
}

//! The part of a bind callback every function family shares: the arguments of the call site, the user data and the
//! bind data. The arguments are one list: the positional arguments, then the named arguments
class CV2FunctionBindInfo {
public:
	template <class FUNC>
	CV2FunctionBindInfo(const FUNC &function, void *user_data)
	    : CV2FunctionBindInfo(function, function.GetDefinition()->GetSignature(), user_data) {
	}
	CV2FunctionBindInfo(const BoundSimpleFunction &function, const FunctionSignature &signature, void *user_data)
	    : in_user_data(user_data), function(function), signature(signature) {
	}
	virtual ~CV2FunctionBindInfo() = default;

	virtual LogicalType GetArgType(idx_t index) const = 0;
	virtual Value GetArgValue(idx_t index) const = 0;

	idx_t GetArgCount() const {
		return function.GetPositionalArgumentCount() + function.GetNamedArguments().size();
	}
	//! The parameter name for a declared parameter, the name the caller passed for "**kwargs", empty for "*args"
	const Identifier &GetArgName(idx_t index) const;
	//! Looks up an argument by a name a caller can pass it by, so positional-only parameters are skipped
	optional_idx FindArg(const Identifier &name) const;

public:
	void *in_user_data;
	//! Why the bind data cannot be set through this handle, empty when it can
	string bind_data_error;
	//! The bind data the callback set, or nullptr
	shared_ptr<CV2UserData> out_bind_data;

	const BoundSimpleFunction &function;
	const FunctionSignature &signature;
};

//! The arguments of a scalar or aggregate call: the bound expressions
class CV2ExpressionBindInfo final : public CV2FunctionBindInfo {
public:
	template <class FUNC>
	CV2ExpressionBindInfo(const FUNC &function, void *user_data, const BindFunctionInput &input)
	    : CV2FunctionBindInfo(function, user_data), input(input) {
		D_ASSERT(GetArgCount() == input.GetArguments().size());
	}

	LogicalType GetArgType(idx_t index) const override;
	Value GetArgValue(idx_t index) const override;

private:
	const BindFunctionInput &input;
};

//! The arguments of a table or pragma function call: constants, the positional arguments and the named ones held apart
class CV2ConstantBindInfo final : public CV2FunctionBindInfo {
public:
	CV2ConstantBindInfo(const BoundSimpleFunction &function, const FunctionSignature &signature, void *user_data,
	                    const vector<Value> &positional, const named_argument_map_t &named);

	LogicalType GetArgType(idx_t index) const override;
	Value GetArgValue(idx_t index) const override;

private:
	const Value &GetArg(idx_t index) const;

	const vector<Value> &positional;
	const named_argument_map_t &named;
};

//! The statistics a stats callback reads or narrows. Argument statistics are read-only: they have no mutable view
class CV2Stats {
public:
	explicit CV2Stats(const BaseStatistics &stats) : stats(stats) {
	}
	static CV2Stats Writable(BaseStatistics &stats) {
		CV2Stats result(stats);
		result.mutable_stats = &stats;
		return result;
	}

	const BaseStatistics &GetStats() const {
		return stats;
	}
	//! Throws when the statistics are read-only
	BaseStatistics &GetMutableStats(const char *function_name);

private:
	const BaseStatistics &stats;
	optional_ptr<BaseStatistics> mutable_stats;
};

inline auto Convert(duckdb_v2_stats_handle stats) -> CV2Stats * {
	return reinterpret_cast<CV2Stats *>(stats);
}
inline auto Convert(CV2Stats *stats) -> duckdb_v2_stats_handle {
	return reinterpret_cast<duckdb_v2_stats_handle>(stats);
}

inline auto Convert(duckdb_v2_function_bind_info_handle info) -> CV2FunctionBindInfo * {
	return reinterpret_cast<CV2FunctionBindInfo *>(info);
}
inline auto Convert(CV2FunctionBindInfo *info) -> duckdb_v2_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_function_bind_info_handle>(info);
}

} // namespace duckdb::capiv2
