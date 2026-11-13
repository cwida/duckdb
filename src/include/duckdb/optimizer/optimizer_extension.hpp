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
typedef void (*pushdown_filter_function_t)(OptimizerExtensionInput &input, const LogicalOperator &op,
                                           unique_ptr<Expression> &filter);

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
	//! The pushdown filter function of the optimizer extension.
	//! Called by filter pushdown for each filter it pushes into an operator, at every operator the filter reaches
	//! It may replace the filter with an equivalent one over the same columns, e.g. ExpressionBarrier::Wrap(filter)
	pushdown_filter_function_t pushdown_filter_function = nullptr;

	//! Additional optimizer info passed to the optimize functions
	shared_ptr<OptimizerExtensionInfo> optimizer_info;

	static void Register(DBConfig &config, OptimizerExtension extension);
	static ExtensionCallbackIteratorHelper<OptimizerExtension> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).OptimizerExtensions();
	}
};

} // namespace duckdb
