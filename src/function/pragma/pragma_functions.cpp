#include "duckdb/function/pragma/pragma_functions.hpp"

#include "duckdb/common/enums/output_type.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/logging/log_manager.hpp"

#include <cctype>

namespace duckdb {

static string PragmaEnableProfilingStatement(ClientContext &context, const FunctionParameters &parameters) {
	auto &config = ClientConfig::GetConfig(context);
	return StringUtil::Format("SET enable_profiling=%s", SQLString(config.profiler_print_format));
}

void RegisterEnableProfiling(BuiltinFunctions &set) {
	PragmaFunctionSet functions("");
	functions.AddFunction(PragmaFunction::PragmaStatement(Identifier(), PragmaEnableProfilingStatement));

	set.AddFunction("enable_profile", functions);
	set.AddFunction("enable_profiling", functions);
}

static void PragmaDisableProfiling(ClientContext &context, const FunctionParameters &parameters) {
	DBConfig::GetConfig(context).CheckLock(EnableProfilingSetting::Name);
	auto &config = ClientConfig::GetConfig(context);
	config.enable_profiler = false;
}

static string PragmaEnableProgressBar(ClientContext &context, const FunctionParameters &parameters) {
	return "SET enable_progress_bar=true";
}

static string PragmaDisableProgressBar(ClientContext &context, const FunctionParameters &parameters) {
	return "SET enable_progress_bar=false";
}

static string PragmaEnablePrintProgressBar(ClientContext &context, const FunctionParameters &parameters) {
	return "SET enable_progress_bar_print=true";
}

static string PragmaDisablePrintProgressBar(ClientContext &context, const FunctionParameters &parameters) {
	return "SET enable_progress_bar_print=false";
}

//! Warn that a pragma that no longer does anything is on its way out
static void WarnDeprecatedPragma(ClientContext &context, const char *name) {
	DUCKDB_LOG_WARNING(
	    context,
	    StringUtil::Format("The '%s' pragma no longer has any effect; it is deprecated and will be removed in a "
	                       "future release.",
	                       name));
}

static void PragmaEnableVerification(ClientContext &context, const FunctionParameters &parameters) {
	WarnDeprecatedPragma(context, "enable_verification");
}

static void PragmaDisableVerification(ClientContext &context, const FunctionParameters &parameters) {
	WarnDeprecatedPragma(context, "disable_verification");
}

static void PragmaEnableForceParallelism(ClientContext &context, const FunctionParameters &parameters) {
	DBConfig::GetConfig(context).CheckLock("verify_parallelism");
	ClientConfig::GetConfig(context).verify_parallelism = true;
}

static void PragmaForceCheckpoint(ClientContext &context, const FunctionParameters &parameters) {
	DBConfig::GetConfig(context).CheckLock("force_checkpoint");
	DBConfig::GetConfig(context).options.force_checkpoint = true;
}

static void PragmaDisableForceParallelism(ClientContext &context, const FunctionParameters &parameters) {
	DBConfig::GetConfig(context).CheckLock("verify_parallelism");
	ClientConfig::GetConfig(context).verify_parallelism = false;
}

static void PragmaEnableObjectCache(ClientContext &context, const FunctionParameters &parameters) {
	WarnDeprecatedPragma(context, "enable_object_cache");
}

static void PragmaDisableObjectCache(ClientContext &context, const FunctionParameters &parameters) {
	WarnDeprecatedPragma(context, "disable_object_cache");
}

static void PragmaEnableCheckpointOnShutdown(ClientContext &context, const FunctionParameters &parameters) {
	DBConfig::GetConfig(context).CheckLock("checkpoint_on_shutdown");
	DBConfig::GetConfig(context).options.checkpoint_on_shutdown = true;
}

static void PragmaDisableCheckpointOnShutdown(ClientContext &context, const FunctionParameters &parameters) {
	DBConfig::GetConfig(context).CheckLock("checkpoint_on_shutdown");
	DBConfig::GetConfig(context).options.checkpoint_on_shutdown = false;
}

static string PragmaEnableOptimizer(ClientContext &context, const FunctionParameters &parameters) {
	return "SET SESSION enable_optimizer=true";
}

static string PragmaDisableOptimizer(ClientContext &context, const FunctionParameters &parameters) {
	return "SET SESSION enable_optimizer=false";
}

void PragmaFunctions::RegisterFunction(BuiltinFunctions &set) {
	RegisterEnableProfiling(set);

	// NOTE: use of Pragma functions is discouraged. Instead, opt for adding a regular table function to be invoked with
	// CALL.
	//       see for example the "enable_logging" function

	set.AddFunction(PragmaFunction::PragmaStatement("disable_profile", PragmaDisableProfiling));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_profiling", PragmaDisableProfiling));

	set.AddFunction(PragmaFunction::PragmaStatement("enable_verification", PragmaEnableVerification));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_verification", PragmaDisableVerification));

	set.AddFunction(PragmaFunction::PragmaStatement("verify_parallelism", PragmaEnableForceParallelism));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_verify_parallelism", PragmaDisableForceParallelism));

	set.AddFunction(PragmaFunction::PragmaStatement("enable_object_cache", PragmaEnableObjectCache));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_object_cache", PragmaDisableObjectCache));

	set.AddFunction(PragmaFunction::PragmaStatement("enable_optimizer", PragmaEnableOptimizer));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_optimizer", PragmaDisableOptimizer));

	set.AddFunction(PragmaFunction::PragmaStatement("force_checkpoint", PragmaForceCheckpoint));

	set.AddFunction(PragmaFunction::PragmaStatement("enable_progress_bar", PragmaEnableProgressBar));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_progress_bar", PragmaDisableProgressBar));

	set.AddFunction(PragmaFunction::PragmaStatement("enable_print_progress_bar", PragmaEnablePrintProgressBar));
	set.AddFunction(PragmaFunction::PragmaStatement("disable_print_progress_bar", PragmaDisablePrintProgressBar));

	set.AddFunction(PragmaFunction::PragmaStatement("enable_checkpoint_on_shutdown", PragmaEnableCheckpointOnShutdown));
	set.AddFunction(
	    PragmaFunction::PragmaStatement("disable_checkpoint_on_shutdown", PragmaDisableCheckpointOnShutdown));
}

} // namespace duckdb
