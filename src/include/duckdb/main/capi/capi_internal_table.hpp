//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi/capi_internal_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.h"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// These need to be shared by both the table function API and the copy function API

struct CTableFunctionInfo : public TableFunctionInfo {
	~CTableFunctionInfo() override {
		if (extra_info && delete_callback) {
			delete_callback(extra_info);
		}
		extra_info = nullptr;
		delete_callback = nullptr;
	}

	duckdb_table_function_bind_t bind = nullptr;
	duckdb_table_function_init_t init = nullptr;
	duckdb_table_function_init_t local_init = nullptr;
	duckdb_table_function_t function = nullptr;
	void *extra_info = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

struct CTableBindData : public TableFunctionData {
	explicit CTableBindData(CTableFunctionInfo &info) : info(info) {
	}
	~CTableBindData() override {
		if (bind_data && delete_callback) {
			delete_callback(bind_data);
		}
		bind_data = nullptr;
		delete_callback = nullptr;
	}

	CTableFunctionInfo &info;
	void *bind_data = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
	unique_ptr<NodeStatistics> stats;

	bool SupportStatementCache() const override {
		return false;
	}
};

struct CTableInternalBindInfo {
	CTableInternalBindInfo(ClientContext &context, const vector<Value> &parameters,
	                       const named_argument_map_t &named_parameters, vector<LogicalType> &return_types,
	                       vector<Identifier> &names, CTableBindData &bind_data, CTableFunctionInfo &function_info)
	    : context(context), parameters(parameters), named_parameters(named_parameters), return_types(return_types),
	      names(names), bind_data(bind_data), function_info(function_info), success(true) {
	}

	ClientContext &context;

	vector<Value> parameters;
	named_argument_map_t named_parameters;

	vector<LogicalType> &return_types;
	vector<Identifier> &names;
	CTableBindData &bind_data;
	CTableFunctionInfo &function_info;
	bool success;
	string error;
};

} // namespace duckdb
