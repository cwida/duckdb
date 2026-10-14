//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/optimizer_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class Optimizer;
class ClientContext;
class DatabaseInstance;

//! The OptimizerExtensionInfo holds static information relevant to the optimizer extension
struct OptimizerExtensionInfo {
	virtual ~OptimizerExtensionInfo() {
	}
};

struct OptimizerExtensionInput {
	ClientContext &context;
	Optimizer &optimizer;
	optional_ptr<OptimizerExtensionInfo> info;
};

typedef void (*optimize_function_t)(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
typedef void (*pre_optimize_function_t)(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

class OptimizerExtension {
public:
	//! The optimize function of the optimizer extension.
	//! Takes a logical query plan as an input, which it can modify in place
	//! This runs, after the DuckDB optimizers have run
	optimize_function_t optimize_function = nullptr;
	//! The pre-optimize function of the optimizer extension.
	//! Takes a logical query plan as an input, which it can modify in place
	//! This runs, before the DuckDB optimizers have run
	pre_optimize_function_t pre_optimize_function = nullptr;

	//! Additional optimizer info passed to the optimize functions
	shared_ptr<OptimizerExtensionInfo> optimizer_info;

	static void Register(DBConfig &config, OptimizerExtension extension);
	//! Declare that a pre-optimize function keeps CTE filter dependencies (LogicalMaterializedCTE::filter_dependency)
	//! intact, so they are not cleared before it runs. The function must then clear them itself
	//! (CTEFilterPusher::ClearDependencies) whenever it rewrites the plan.
	//! Registered out-of-band on DatabaseInstance to preserve the OptimizerExtension ABI with prebuilt extensions.
	DUCKDB_API static void RegisterCTEFilterDependencyPreserving(DatabaseInstance &db,
	                                                             pre_optimize_function_t pre_optimize_function);
	DUCKDB_API static bool PreservesCTEFilterDependencies(ClientContext &context,
	                                                      pre_optimize_function_t pre_optimize_function);
	static ExtensionCallbackIteratorHelper<OptimizerExtension> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).OptimizerExtensions();
	}
};

} // namespace duckdb
