//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/planner_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class Binder;
class ClientContext;
class SQLStatement;

//! The PlannerExtensionInfo holds static information relevant to the planner extension
struct PlannerExtensionInfo {
	virtual ~PlannerExtensionInfo() {
	}
};

enum class GetSQLValueFunctionReturnType { CONTINUE_BINDING, FINISH_BINDING };

struct PlannerExtensionInput {
	ClientContext &context;
	Binder &binder;
	optional_ptr<PlannerExtensionInfo> info;
};

//! The post_bind function runs after binding succeeds, allowing modification of the bound statement
typedef void (*pre_bind_function_t)(PlannerExtensionInput &input, SQLStatement &statement);
typedef void (*post_bind_function_t)(PlannerExtensionInput &input, BoundStatement &statement);

typedef GetSQLValueFunctionReturnType (*get_sql_value_function_t)(PlannerExtensionInput &input,
                                                                  const string &column_name,
                                                                  unique_ptr<ParsedExpression> &result);

class PlannerExtension {
public:
	//! The pre-bind function of the planner extension.
	//! Takes the statement that is about to be bound. This runs right before the binder binds the statement,
	//! allowing e.g. a catalog to start loading the entries the statement references.
	pre_bind_function_t pre_bind_function = nullptr;

	//! The post-bind function of the planner extension.
	//! Takes a bound statement as input, which it can modify in place.
	//! This runs after the binder has successfully bound the statement,
	//! allowing modification of the plan and result types.
	post_bind_function_t post_bind_function = nullptr;

	//! Override that allows registering a different callback for SQL value functions
	get_sql_value_function_t get_sql_value_function = nullptr;

	//! Additional planner info passed to the functions
	shared_ptr<PlannerExtensionInfo> planner_info;

	static void Register(DBConfig &config, PlannerExtension extension);
	static ExtensionCallbackIteratorHelper<PlannerExtension> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).PlannerExtensions();
	}
};

} // namespace duckdb
