#include "duckdb/main/config.hpp"

#include "duckdb/common/cgroups.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/extension/external_extension_provider.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/path.hpp"

#ifndef DUCKDB_NO_THREADS
#include "duckdb/common/thread.hpp"
#endif

#include <cinttypes>
#include <cstdio>

namespace duckdb {

#ifdef DEBUG
bool DBConfigOptions::debug_print_bindings = false;
#endif
DebugVerificationMode DBConfigOptions::global_verification_mode = DebugVerificationMode::NONE;

#define DUCKDB_SETTING(_PARAM)                                                                                         \
	{                                                                                                                  \
		_PARAM::Name, _PARAM::Description, _PARAM::InputType, nullptr, nullptr, nullptr, nullptr, nullptr,             \
		    _PARAM::Scope, _PARAM::DefaultValue, nullptr, _PARAM::SettingIndex, _PARAM::IsDebug, _PARAM::IsDeprecated  \
	}
#define DUCKDB_SETTING_CALLBACK(_PARAM)                                                                                \
	{                                                                                                                  \
		_PARAM::Name, _PARAM::Description, _PARAM::InputType, nullptr, nullptr, nullptr, nullptr, nullptr,             \
		    _PARAM::Scope, _PARAM::DefaultValue, _PARAM::OnSet, _PARAM::SettingIndex, _PARAM::IsDebug,                 \
		    _PARAM::IsDeprecated                                                                                       \
	}
#define DUCKDB_GLOBAL(_PARAM)                                                                                          \
	{                                                                                                                  \
		_PARAM::Name, _PARAM::Description, _PARAM::InputType, _PARAM::SetGlobal, nullptr, _PARAM::ResetGlobal,         \
		    nullptr, _PARAM::GetSetting, SettingScopeTarget::INVALID, nullptr, nullptr, optional_idx(),                \
		    _PARAM::IsDebug, _PARAM::IsDeprecated                                                                      \
	}
#define DUCKDB_LOCAL(_PARAM)                                                                                           \
	{                                                                                                                  \
		_PARAM::Name, _PARAM::Description, _PARAM::InputType, nullptr, _PARAM::SetLocal, nullptr, _PARAM::ResetLocal,  \
		    _PARAM::GetSetting, SettingScopeTarget::INVALID, nullptr, nullptr, optional_idx(), _PARAM::IsDebug,        \
		    _PARAM::IsDeprecated                                                                                       \
	}
#define DUCKDB_GLOBAL_LOCAL(_PARAM)                                                                                    \
	{                                                                                                                  \
		_PARAM::Name, _PARAM::Description, _PARAM::InputType, _PARAM::SetGlobal, _PARAM::SetLocal,                     \
		    _PARAM::ResetGlobal, _PARAM::ResetLocal, _PARAM::GetSetting, SettingScopeTarget::INVALID, nullptr,         \
		    nullptr, optional_idx(), _PARAM::IsDebug, _PARAM::IsDeprecated                                             \
	}
#define FINAL_SETTING                                                                                                  \
	{                                                                                                                  \
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, SettingScopeTarget::INVALID, nullptr,  \
		    nullptr, optional_idx(), false, false                                                                      \
	}

#define DUCKDB_SETTING_ALIAS(_ALIAS, _PARAM)                                                                           \
	{ _ALIAS, _PARAM::Name }
#define FINAL_ALIAS                                                                                                    \
	{ nullptr, nullptr }

static const ConfigurationOption internal_options[] = {

    DUCKDB_GLOBAL(AccessModeSetting),
    DUCKDB_LOCAL(ActiveGrammarExtensionsSetting),
    DUCKDB_SETTING_CALLBACK(AllocatorBackgroundThreadsSetting),
    DUCKDB_GLOBAL(AllocatorBulkDeallocationFlushThresholdSetting),
    DUCKDB_SETTING_CALLBACK(AllocatorFlushThresholdSetting),
    DUCKDB_SETTING_CALLBACK(AllowCommunityExtensionsSetting),
    DUCKDB_SETTING_CALLBACK(AllowExtensionRepositoriesSetting),
    DUCKDB_SETTING(AllowExtensionsMetadataMismatchSetting),
    DUCKDB_SETTING_CALLBACK(AllowParserOverrideExtensionSetting),
    DUCKDB_GLOBAL(AllowPersistentSecretsSetting),
    DUCKDB_SETTING_CALLBACK(AllowUnredactedSecretsSetting),
    DUCKDB_SETTING_CALLBACK(AllowUnsignedExtensionsSetting),
    DUCKDB_GLOBAL(AllowedConfigsSetting),
    DUCKDB_GLOBAL(AllowedDirectoriesSetting),
    DUCKDB_GLOBAL(AllowedPathsSetting),
    DUCKDB_SETTING(ApproximateJoinOrderThresholdSetting),
    DUCKDB_SETTING(ArrowLargeBufferSizeSetting),
    DUCKDB_SETTING(ArrowLosslessConversionSetting),
    DUCKDB_SETTING(ArrowOutputListViewSetting),
    DUCKDB_SETTING_CALLBACK(ArrowOutputVersionSetting),
    DUCKDB_SETTING(AsofLoopJoinThresholdSetting),
    DUCKDB_GLOBAL(AsyncThreadsSetting),
    DUCKDB_SETTING(AutoCheckpointSkipWalThresholdSetting),
    DUCKDB_SETTING(AutoinstallExtensionRepositorySetting),
    DUCKDB_SETTING(AutoinstallKnownExtensionsSetting),
    DUCKDB_SETTING(AutoloadKnownExtensionsSetting),
    DUCKDB_SETTING(CacheLocalFilesSetting),
    DUCKDB_SETTING(CatalogErrorMaxSchemasSetting),
    DUCKDB_SETTING_CALLBACK(CheckpointOnDetachSetting),
    DUCKDB_GLOBAL(CheckpointThresholdSetting),
    DUCKDB_LOCAL(CurrentDialectSetting),
    DUCKDB_SETTING_CALLBACK(CurrentTransactionInvalidationPolicySetting),
    DUCKDB_SETTING(CustomExtensionRepositorySetting),
    DUCKDB_GLOBAL(CustomUserAgentSetting),
    DUCKDB_GLOBAL(DebugAbortOnWalFailureSetting),
    DUCKDB_SETTING(DebugAsofIejoinSetting),
    DUCKDB_SETTING_CALLBACK(DebugCheckpointAbortSetting),
    DUCKDB_SETTING(DebugCheckpointScanSleepMsSetting),
    DUCKDB_SETTING(DebugCheckpointSleepMsSetting),
    DUCKDB_GLOBAL(DebugDeltaOnlyVariantEncodingEnabledSetting),
    DUCKDB_SETTING(DebugDisableOptimizerSetting),
    DUCKDB_SETTING(EnableCachingOperatorsSetting),
    DUCKDB_SETTING(DebugEvictionQueueSleepMicroSecondsSetting),
    DUCKDB_SETTING(DebugFileDescriptorLimitSetting),
    DUCKDB_SETTING_CALLBACK(ForceBitpackingModeSetting),
    DUCKDB_SETTING(DebugForceCommitFailureSetting),
    DUCKDB_SETTING(DebugForceCommitRevertFailureSetting),
    DUCKDB_SETTING(DebugForceExternalSetting),
    DUCKDB_SETTING(DebugForceFetchRowSetting),
    DUCKDB_GLOBAL(ForceMbedtlsUnsafeSetting),
    DUCKDB_SETTING(DebugForceNoCrossProductSetting),
    DUCKDB_SETTING(ForceUpdateToDelAndInsertSetting),
    DUCKDB_GLOBAL(ForceVariantShredding),
    DUCKDB_SETTING(DebugForceWalFsyncFailureSetting),
    DUCKDB_SETTING(DebugLocalFileSystemDelayMsSetting),
    DUCKDB_GLOBAL(DebugOrderVerificationSetting),
    DUCKDB_SETTING_CALLBACK(DebugPhysicalTableScanExecutionStrategySetting),
    DUCKDB_SETTING(DebugSkipCheckpointOnCommitSetting),
    DUCKDB_GLOBAL(DebugVerificationModeSetting),
    DUCKDB_SETTING(DebugVerificationProjectionSetting),
    DUCKDB_SETTING(DebugVerifyAggregateStateExportSetting),
    DUCKDB_SETTING(DebugVerifyBlocksSetting),
    DUCKDB_SETTING(DebugVerifyColumnBindingsSetting),
    DUCKDB_SETTING_CALLBACK(DebugVerifyProgressSetting),
    DUCKDB_SETTING(DebugVerifyProgressIgnoreSetting),
    DUCKDB_SETTING(DebugVerifySerializerSetting),
    DUCKDB_SETTING_CALLBACK(DebugVerifyStatementSetting),
    DUCKDB_SETTING(DebugVerifyStatsSetting),
    DUCKDB_SETTING_CALLBACK(DebugVerifyVectorSetting),
    DUCKDB_SETTING(DebugWalFsyncSleepMsSetting),
    DUCKDB_SETTING_CALLBACK(DebugWindowModeSetting),
    DUCKDB_SETTING_CALLBACK(DefaultBlockSizeSetting),
    DUCKDB_SETTING_CALLBACK(DefaultCollationSetting),
    DUCKDB_SETTING_CALLBACK(DefaultNullOrderSetting),
    DUCKDB_SETTING_CALLBACK(DefaultOrderSetting),
    DUCKDB_GLOBAL(DefaultSecretStorageSetting),
    DUCKDB_SETTING_CALLBACK(DefaultTransactionInvalidationPolicySetting),
    DUCKDB_SETTING(DelimJoinAsCteSetting),
    DUCKDB_SETTING_CALLBACK(DialectCompatibilityModeSetting),
    DUCKDB_SETTING_CALLBACK(DisableDatabaseInvalidationSetting),
    DUCKDB_SETTING(DisableTimestamptzCastsSetting),
    DUCKDB_GLOBAL(DisabledCompressionMethodsSetting),
    DUCKDB_GLOBAL(DisabledFilesystemsSetting),
    DUCKDB_GLOBAL(DisabledLogTypes),
    DUCKDB_GLOBAL(DisabledOptimizersSetting),
    DUCKDB_SETTING_CALLBACK(DuckDBAPISetting),
    DUCKDB_SETTING(DynamicOrFilterThresholdSetting),
    DUCKDB_GLOBAL(EnableBlockAllocatorSetting),
    DUCKDB_SETTING_CALLBACK(EnableExternalAccessSetting),
    DUCKDB_SETTING_CALLBACK(EnableExternalFileCacheSetting),
    DUCKDB_SETTING(EnableFSSTVectorsSetting),
    DUCKDB_SETTING(EnableHTTPMetadataCacheSetting),
    DUCKDB_GLOBAL(EnableLogging),
    DUCKDB_SETTING(EnableMacroDependenciesSetting),
    DUCKDB_SETTING(EnableObjectCacheSetting),
    DUCKDB_SETTING(EnableOptimisticWriteSetting),
    DUCKDB_SETTING(EnableOptimizerSetting),
    DUCKDB_LOCAL(EnableProfilingSetting),
    DUCKDB_LOCAL(EnableProgressBarSetting),
    DUCKDB_LOCAL(EnableProgressBarPrintSetting),
    DUCKDB_SETTING(EnableViewDependenciesSetting),
    DUCKDB_GLOBAL(EnabledLogTypes),
    DUCKDB_SETTING(ErrorOnDivisionByZeroSetting),
    DUCKDB_SETTING(ErrorsAsJSONSetting),
    DUCKDB_SETTING(ExperimentalMetadataReuseSetting),
    DUCKDB_SETTING_CALLBACK(ExplainOutputSetting),
    DUCKDB_GLOBAL(ExtensionDirectoriesSetting),
    DUCKDB_SETTING(ExtensionDirectorySetting),
    DUCKDB_SETTING_CALLBACK(ExtensionRepositoryDirectorySetting),
    DUCKDB_SETTING_CALLBACK(ExternalFileCacheLocalMaxBlockSizeSetting),
    DUCKDB_SETTING_CALLBACK(ExternalFileCacheRemoteMaxBlockSizeSetting),
    DUCKDB_SETTING_CALLBACK(ExternalFileCacheRemoteMinBlockSizeSetting),
    DUCKDB_SETTING_CALLBACK(ExternalFileCacheRequestSizingSetting),
    DUCKDB_SETTING(ExternalFileCacheSpillSetting),
    DUCKDB_SETTING_CALLBACK(ExternalThreadsSetting),
    DUCKDB_SETTING(FileSearchPathSetting),
    DUCKDB_SETTING(ForceColumnMetadataReuseSetting),
    DUCKDB_SETTING_CALLBACK(ForceCompressionSetting),
    DUCKDB_SETTING_CALLBACK(FsyncModeSetting),
    DUCKDB_SETTING(GeometryMinimumShreddingSize),
    DUCKDB_SETTING_CALLBACK(HomeDirectorySetting),
    DUCKDB_GLOBAL(HTTPClientPoolCapacitySetting),
    DUCKDB_GLOBAL(HTTPProxySetting),
    DUCKDB_SETTING(HTTPProxyPasswordSetting),
    DUCKDB_SETTING(HTTPProxyUsernameSetting),
    DUCKDB_SETTING(IeeeFloatingPointOpsSetting),
    DUCKDB_SETTING(IgnoreUnknownCrsSetting),
    DUCKDB_SETTING(ImmediateTransactionModeSetting),
    DUCKDB_SETTING(IndexScanMaxCountSetting),
    DUCKDB_SETTING_CALLBACK(IndexScanPercentageSetting),
    DUCKDB_SETTING_CALLBACK(InitialColumnSegmentSizeSetting),
    DUCKDB_SETTING(IntegerDivisionSetting),
    DUCKDB_SETTING_CALLBACK(LambdaSyntaxSetting),
    DUCKDB_SETTING(LateMaterializationMaxRowsSetting),
    DUCKDB_SETTING(LegacyDisableNullTypeSetting),
    DUCKDB_SETTING(LegacyMetricsFormatSetting),
    DUCKDB_SETTING(LockConfigurationSetting),
    DUCKDB_SETTING_CALLBACK(LogQueryPathSetting),
    DUCKDB_GLOBAL(LoggingLevel),
    DUCKDB_GLOBAL(LoggingMode),
    DUCKDB_GLOBAL(LoggingStorage),
    DUCKDB_SETTING(MatchRecognizeMaxStatesSetting),
    DUCKDB_SETTING(MaxExecutionTimeSetting),
    DUCKDB_SETTING(MaxExpressionDepthSetting),
    DUCKDB_GLOBAL(MaxMemorySetting),
    DUCKDB_LOCAL(MaxStreamingBufferSizeSetting),
    DUCKDB_GLOBAL(MaxTempDirectorySizeSetting),
    DUCKDB_SETTING(MaxVacuumTasksSetting),
    DUCKDB_SETTING(MergeJoinThresholdSetting),
    DUCKDB_SETTING(NestedLoopJoinThresholdSetting),
    DUCKDB_SETTING(OldImplicitCastingSetting),
    DUCKDB_LOCAL(OperatorMemoryLimitSetting),
    DUCKDB_SETTING(OrderByNonIntegerLiteralSetting),
    DUCKDB_SETTING_CALLBACK(OrderedAggregateThresholdSetting),
    DUCKDB_SETTING(PartitionedWriteFlushThresholdSetting),
    DUCKDB_SETTING(PartitionedWriteMaxOpenFilesSetting),
    DUCKDB_SETTING(PasswordSetting),
    DUCKDB_SETTING_CALLBACK(PerfectHtThresholdSetting),
    DUCKDB_SETTING_CALLBACK(PinThreadsSetting),
    DUCKDB_SETTING(PivotFilterThresholdSetting),
    DUCKDB_SETTING(PivotLimitSetting),
    DUCKDB_SETTING(PreferRangeJoinsSetting),
    DUCKDB_SETTING_CALLBACK(PreserveIdentifierCaseSetting),
    DUCKDB_SETTING(PreserveInsertionOrderSetting),
    DUCKDB_SETTING(ProduceArrowStringViewSetting),
    DUCKDB_LOCAL(ProfilingCoverageSetting),
    DUCKDB_LOCAL(ProfilingModeSetting),
    DUCKDB_LOCAL(ProfilingOutputSetting),
    DUCKDB_LOCAL(ProfilingRendererSettingsSetting),
    DUCKDB_LOCAL(ProgressBarTimeSetting),
    DUCKDB_SETTING_CALLBACK(ReadAheadDepthSetting),
    DUCKDB_SETTING(RedactHttpLogsSetting),
    DUCKDB_SETTING_CALLBACK(RegexMatchOperatorSemanticsSetting),
    DUCKDB_SETTING(ScalarSubqueryErrorOnMultipleRowsSetting),
    DUCKDB_SETTING(SchedulerProcessPartialSetting),
    DUCKDB_LOCAL(SchemaSetting),
    DUCKDB_LOCAL(SearchPathSetting),
    DUCKDB_GLOBAL(SecretDirectorySetting),
    DUCKDB_SETTING_CALLBACK(ShowBehaviorSetting),
    DUCKDB_GLOBAL(StandardVectorSizeSetting),
    DUCKDB_SETTING_CALLBACK(StorageBlockPrefetchSetting),
    DUCKDB_GLOBAL(StorageCompatibilityVersionSetting),
    DUCKDB_SETTING_CALLBACK(TableFunctionIdentifierConversionSetting),
    DUCKDB_GLOBAL(TempDirectorySetting),
    DUCKDB_SETTING_CALLBACK(TempFileEncryptionSetting),
    DUCKDB_GLOBAL(ThreadsSetting),
    DUCKDB_LOCAL(TrackedMetricsSetting),
    DUCKDB_SETTING(UsernameSetting),
    DUCKDB_SETTING_CALLBACK(VacuumRebuildIndexesSetting),
    DUCKDB_SETTING_CALLBACK(ValidateExternalFileCacheSetting),
    DUCKDB_SETTING(VariantMinimumShreddingSizeSetting),
    DUCKDB_SETTING(WalAutocheckpointEntriesSetting),
    DUCKDB_SETTING_CALLBACK(WarningsAsErrorsSetting),
    DUCKDB_SETTING(WriteBufferRowGroupCountSetting),
    DUCKDB_GLOBAL(WriteBufferRowGroupMemoryLimitSetting),
    DUCKDB_SETTING(ZstdMinStringLengthSetting),
    FINAL_SETTING};

static const ConfigurationAlias setting_aliases[] = {
    DUCKDB_SETTING_ALIAS("__delta_only_variant_encoding_enabled", DebugDeltaOnlyVariantEncodingEnabledSetting),
    DUCKDB_SETTING_ALIAS("enable_caching_operators", EnableCachingOperatorsSetting),
    DUCKDB_SETTING_ALIAS("force_bitpacking_mode", ForceBitpackingModeSetting),
    DUCKDB_SETTING_ALIAS("force_mbedtls_unsafe", ForceMbedtlsUnsafeSetting),
    DUCKDB_SETTING_ALIAS("force_update_to_del_and_insert", ForceUpdateToDelAndInsertSetting),
    DUCKDB_SETTING_ALIAS("force_variant_shredding", ForceVariantShredding),
    DUCKDB_SETTING_ALIAS("memory_limit", MaxMemorySetting),
    DUCKDB_SETTING_ALIAS("null_order", DefaultNullOrderSetting),
    DUCKDB_SETTING_ALIAS("profile_output", ProfilingOutputSetting),
    DUCKDB_SETTING_ALIAS("streaming_buffer_size", MaxStreamingBufferSizeSetting),
    DUCKDB_SETTING_ALIAS("user", UsernameSetting),
    DUCKDB_SETTING_ALIAS("wal_autocheckpoint", CheckpointThresholdSetting),
    DUCKDB_SETTING_ALIAS("worker_threads", ThreadsSetting),
    FINAL_ALIAS};

vector<ConfigurationOption> DBConfig::GetOptions() {
	vector<ConfigurationOption> options;
	for (idx_t index = 0; internal_options[index].name; index++) {
		options.push_back(internal_options[index]);
	}
	return options;
}

vector<ConfigurationAlias> DBConfig::GetAliases() {
	vector<ConfigurationAlias> aliases;
	for (idx_t index = 0; index < GetAliasCount(); index++) {
		aliases.push_back(setting_aliases[index]);
	}
	return aliases;
}

SettingCallbackInfo::SettingCallbackInfo(ClientContext &context_p, SetScope scope)
    : config(DBConfig::GetConfig(context_p)), db(context_p.db.get()), context(context_p), scope(scope) {
}

SettingCallbackInfo::SettingCallbackInfo(DBConfig &config, optional_ptr<DatabaseInstance> db)
    : config(config), db(db), context(nullptr), scope(SetScope::GLOBAL) {
}

void SettingCallbackInfo::WarnDeprecated(const string &name) const {
	if (is_reset) {
		return;
	}
	auto message = StringUtil::Format("The '%s' setting is deprecated and will be removed in a future release.", name);
	if (context) {
		DUCKDB_LOG_WARNING(*context, message);
	} else if (db) {
		DUCKDB_LOG_WARNING(*db, message);
	}
	// a setting in the startup config has no database to log to yet
}

idx_t DBConfig::GetOptionCount() {
	return sizeof(internal_options) / sizeof(ConfigurationOption) - 1;
}

idx_t DBConfig::GetAliasCount() {
	return sizeof(setting_aliases) / sizeof(ConfigurationAlias) - 1;
}

vector<Identifier> DBConfig::GetOptionNames() {
	vector<Identifier> names;
	for (idx_t index = 0; internal_options[index].name; index++) {
		names.emplace_back(internal_options[index].name);
	}
	for (idx_t index = 0; setting_aliases[index].alias; index++) {
		names.emplace_back(setting_aliases[index].alias);
	}
	return names;
}

optional_ptr<const ConfigurationOption> DBConfig::GetOptionByIndex(idx_t target_index) {
	if (target_index >= GetOptionCount()) {
		return nullptr;
	}
	return internal_options + target_index;
}

optional_ptr<const ConfigurationAlias> DBConfig::GetAliasByIndex(idx_t target_index) {
	if (target_index >= GetAliasCount()) {
		return nullptr;
	}
	return setting_aliases + target_index;
}

static optional_ptr<const ConfigurationOption> FindOptionByName(const Identifier &name) {
	for (idx_t index = 0; internal_options[index].name; index++) {
		if (internal_options[index].name == name) {
			return internal_options + index;
		}
	}
	return nullptr;
}

optional_ptr<const ConfigurationOption> DBConfig::GetOptionByName(const Identifier &name) {
	auto option = FindOptionByName(name);
	if (option) {
		return option;
	}
	for (idx_t index = 0; setting_aliases[index].alias; index++) {
		if (setting_aliases[index].alias == name) {
			return FindOptionByName(setting_aliases[index].setting_name);
		}
	}
	return nullptr;
}

idx_t DBConfig::GetWriteBufferMemoryLimit() const {
	std::lock_guard<std::recursive_mutex> guard(config_lock);
	if (options.write_buffer_row_group_memory_limit.IsValid()) {
		return options.write_buffer_row_group_memory_limit.GetIndex();
	}
	return options.maximum_memory / 5 / (options.maximum_threads + 1);
}

vector<string> DBConfig::GetExtensionDirectories() const {
	std::lock_guard<std::recursive_mutex> guard(config_lock);
	return options.extension_directories;
}

void DBConfig::SetOption(const ConfigurationOption &option, const Value &value) {
	SetOption(nullptr, option, value);
}

void DBConfig::SetOptionByName(const Identifier &name, const Value &value) {
	if (is_user_config) {
		// for user config we just set the option in the `user_options`
		options.user_options[name] = value;
	}
	auto option = DBConfig::GetOptionByName(name);
	if (option) {
		SetOption(*option, value);
		return;
	}

	ExtensionOption extension_option;
	if (TryGetExtensionOption(name, extension_option)) {
		Value target_value = value.DefaultCastAs(extension_option.type);
		SetOption(extension_option.setting_index.GetIndex(), std::move(target_value));
	} else {
		options.unrecognized_options[name] = value;
	}
}

void DBConfig::SetOptionsByName(const identifier_map_t<Value> &values) {
	for (auto &kv : values) {
		auto &name = kv.first;
		auto &value = kv.second;
		SetOptionByName(name, value);
	}
}

void DBConfig::SetOption(optional_ptr<DatabaseInstance> db, const ConfigurationOption &option, const Value &value) {
	Value input = value.DefaultCastAs(ParseLogicalType(option.parameter_type));
	if (option.default_value) {
		// generic option
		SettingCallbackInfo info(*this, db);
		if (option.set_callback) {
			option.set_callback(info, input);
		}
		// after the callback, which may still refuse the value
		if (option.is_deprecated) {
			info.WarnDeprecated(option.name);
		}
		user_settings.SetUserSetting(option.setting_idx.GetIndex(), std::move(input));
		return;
	}
	if (!option.set_global) {
		throw InvalidInputException("Could not set option \"%s\" as a global option", option.name);
	}
	std::lock_guard<std::recursive_mutex> guard(config_lock);
	D_ASSERT(option.reset_global);
	option.set_global(db.get(), *this, input);
}

void DBConfig::ResetOption(optional_ptr<DatabaseInstance> db, const ConfigurationOption &option) {
	if (option.default_value) {
		// generic option
		user_settings.ClearSetting(option.setting_idx.GetIndex());
		return;
	}
	if (!option.reset_global) {
		throw InternalException("Could not reset option \"%s\" as a global option", option.name);
	}
	std::lock_guard<std::recursive_mutex> guard(config_lock);
	D_ASSERT(option.set_global);
	option.reset_global(db.get(), *this);
}

void DBConfig::SetOption(idx_t setting_index, Value value) {
	user_settings.SetUserSetting(setting_index, std::move(value));
}

void DBConfig::SetOption(const Identifier &name, Value value) {
	optional_ptr<const ConfigurationOption> option;
	auto setting_index = TryGetSettingIndex(name, option);
	if (!setting_index.IsValid()) {
		throw InternalException("Unrecognized option %s in DBConfig::SetOption", name);
	}
	SetOption(setting_index.GetIndex(), std::move(value));
}

void DBConfig::ResetOption(const ExtensionOption &extension_option) {
	auto &default_value = extension_option.default_value;
	auto setting_index = extension_option.setting_index.GetIndex();
	if (!default_value.IsNull()) {
		// Default is not NULL, override the setting
		user_settings.SetUserSetting(setting_index, default_value);
	} else {
		// Otherwise just remove it from the 'set_variables' map
		user_settings.ClearSetting(setting_index);
	}
}

void DBConfig::ResetGenericOption(idx_t setting_index) {
	user_settings.ClearSetting(setting_index);
}

LogicalType DBConfig::ParseLogicalType(const string &type) {
	if (StringUtil::EndsWith(type, "[]")) {
		// list - recurse
		auto child_type = ParseLogicalType(type.substr(0, type.size() - 2));
		return LogicalType::LIST(child_type);
	}

	if (StringUtil::EndsWith(type, "]")) {
		// array - recurse
		auto bracket_open_idx = type.rfind('[');
		if (bracket_open_idx == string::npos || bracket_open_idx == 0) {
			throw InternalException("Ill formatted type: '%s'", type);
		}
		idx_t array_size = 0;
		for (auto length_idx = bracket_open_idx + 1; length_idx < type.size() - 1; length_idx++) {
			if (!isdigit(static_cast<unsigned char>(type[length_idx]))) {
				throw InternalException("Ill formatted array type: '%s'", type);
			}
			array_size = array_size * 10 + static_cast<idx_t>(type[length_idx] - '0');
		}
		if (array_size == 0 || array_size > ArrayType::MAX_ARRAY_SIZE) {
			throw InternalException("Invalid array size: '%s'", type);
		}
		auto child_type = ParseLogicalType(type.substr(0, bracket_open_idx));
		return LogicalType::ARRAY(child_type, array_size);
	}

	if (StringUtil::StartsWith(type, "MAP(") && StringUtil::EndsWith(type, ")")) {
		// map - recurse
		string map_args = type.substr(4, type.size() - 5);
		vector<string> map_args_vect = StringUtil::SplitWithParentheses(map_args);
		if (map_args_vect.size() != 2) {
			throw InternalException("Ill formatted map type: '%s'", type);
		}
		StringUtil::Trim(map_args_vect[0]);
		StringUtil::Trim(map_args_vect[1]);
		auto key_type = ParseLogicalType(map_args_vect[0]);
		auto value_type = ParseLogicalType(map_args_vect[1]);
		return LogicalType::MAP(key_type, value_type);
	}

	if (StringUtil::StartsWith(type, "UNION(") && StringUtil::EndsWith(type, ")")) {
		// union - recurse
		string union_members_str = type.substr(6, type.size() - 7);
		vector<string> union_members_vect = StringUtil::SplitWithParentheses(union_members_str);
		child_list_t<LogicalType> union_members;
		for (idx_t member_idx = 0; member_idx < union_members_vect.size(); member_idx++) {
			StringUtil::Trim(union_members_vect[member_idx]);
			vector<string> union_member_parts = StringUtil::SplitWithParentheses(union_members_vect[member_idx], ' ');
			if (union_member_parts.size() != 2) {
				throw InternalException("Ill formatted union type: %s", type);
			}
			StringUtil::Trim(union_member_parts[0]);
			StringUtil::Trim(union_member_parts[1]);
			auto value_type = ParseLogicalType(union_member_parts[1]);
			union_members.emplace_back(make_pair(union_member_parts[0], value_type));
		}
		if (union_members.empty() || union_members.size() > UnionType::MAX_UNION_MEMBERS) {
			throw InternalException("Invalid number of union members: '%s'", type);
		}
		return LogicalType::UNION(union_members);
	}

	if (type == "STRUCT") {
		// empty struct
		return LogicalType::STRUCT({});
	}

	if (StringUtil::StartsWith(type, "STRUCT(") && StringUtil::EndsWith(type, ")")) {
		// struct - recurse
		string struct_members_str = type.substr(7, type.size() - 8);
		vector<string> struct_members_vect = StringUtil::SplitWithParentheses(struct_members_str);
		child_list_t<LogicalType> struct_members;
		for (idx_t member_idx = 0; member_idx < struct_members_vect.size(); member_idx++) {
			StringUtil::Trim(struct_members_vect[member_idx]);
			vector<string> struct_member_parts = StringUtil::SplitWithParentheses(struct_members_vect[member_idx], ' ');
			if (struct_member_parts.size() != 2) {
				throw InternalException("Ill formatted struct type: %s", type);
			}
			StringUtil::Trim(struct_member_parts[0]);
			StringUtil::Trim(struct_member_parts[1]);
			auto value_type = ParseLogicalType(struct_member_parts[1]);
			struct_members.emplace_back(make_pair(struct_member_parts[0], value_type));
		}
		return LogicalType::STRUCT(struct_members);
	}

	const auto type_id = StringUtil::CIEquals(type, "ANY") ? LogicalTypeId::ANY : TransformStringToLogicalTypeId(type);
	if (type_id == LogicalTypeId::UNBOUND) {
		throw InternalException("Error while generating extension function overloads - unrecognized logical type %s",
		                        type);
	}
	return type_id;
}

ExtensionOptionInfo::~ExtensionOptionInfo() {
}

SetScope ExtensionOption::ResolveScope(const Identifier &name, SetScope scope) const {
	if (scope == SetScope::AUTOMATIC) {
		scope = default_scope;
	}
	if (scope == SetScope::GLOBAL && !supports_global) {
		throw InvalidInputException("Setting %s cannot be set as a global variable - it can only be set per session",
		                            name);
	}
	if (scope == SetScope::SESSION && !supports_session) {
		throw InvalidInputException("Setting %s cannot be set as a session variable - it can only be set globally",
		                            name);
	}
	return scope;
}

bool DBConfig::HasExtensionOption(const Identifier &name) const {
	return user_settings.HasExtensionOption(name);
}

bool DBConfig::TryGetExtensionOption(const Identifier &name, ExtensionOption &result) const {
	return user_settings.TryGetExtensionOption(name, result);
}

void DBConfig::AddExtensionOption(const Identifier &name, string description, LogicalType parameter,
                                  const Value &default_value, set_option_callback_t function, SetScope default_scope,
                                  bool is_debug, bool is_deprecated) {
	AddExtensionOption(name, ExtensionOption(std::move(description), std::move(parameter), function, default_value,
	                                         default_scope, is_debug, is_deprecated));
}

void DBConfig::AddExtensionOption(const Identifier &name, ExtensionOption extension_option) {
	auto default_value = extension_option.default_value;
	auto setting_index = user_settings.AddExtensionOption(name, std::move(extension_option));
	// copy over unrecognized options, if they match the new extension option
	auto iter = options.unrecognized_options.find(name);
	if (iter != options.unrecognized_options.end()) {
		user_settings.SetUserSetting(setting_index, iter->second);
		options.unrecognized_options.erase(iter);
	}
	if (!default_value.IsNull() && !user_settings.IsSet(setting_index)) {
		// Default value is set, insert it into the 'set_variables' list
		user_settings.SetUserSetting(setting_index, default_value);
	}
}

identifier_map_t<ExtensionOption> DBConfig::GetExtensionSettings() const {
	return user_settings.GetExtensionSettings();
}

bool DBConfig::IsInMemoryDatabase(const char *database_path) {
	if (!database_path) {
		// Entirely empty
		return true;
	}
	if (strlen(database_path) == 0) {
		// '' empty string
		return true;
	}
	if (strcmp(database_path, ":memory:") == 0) {
		return true;
	}
	return false;
}

CastFunctionSet &DBConfig::GetCastFunctions() {
	return type_manager->GetCastFunctions();
}

const CastFunctionSet &DBConfig::GetCastFunctions() const {
	const auto &manager = *type_manager;
	return manager.GetCastFunctions();
}

TypeManager &DBConfig::GetTypeManager() {
	return *type_manager;
}

CollationBinding &DBConfig::GetCollationBinding() {
	return *collation_bindings;
}

IndexTypeSet &DBConfig::GetIndexTypes() {
	return *index_types;
}

void DBConfig::SetDefaultMaxMemory() {
	auto memory = GetSystemAvailableMemory(*file_system);
	if (memory == DBConfigOptions().maximum_memory) {
		// If GetSystemAvailableMemory returned the default, use it as is
		options.maximum_memory = memory;
	} else {
		// Otherwise, use 80% of the available memory
		options.maximum_memory = memory * 8 / 10;
	}
}

void DBConfig::SetDefaultTempDirectory() {
	if (!options.use_temporary_directory) {
		options.temporary_directory = string();
	} else if (DBConfig::IsInMemoryDatabase(options.database_path.c_str())) {
		options.temporary_directory = ".tmp";
	} else {
		options.temporary_directory = Path::AddSuffixToPath(options.database_path, ".tmp");
	}
}

void DBConfig::CheckLock(const Identifier &name) {
	if (!Settings::Get<LockConfigurationSetting>(*this)) {
		// not locked
		return;
	}
	identifier_set_t allowed_settings {"schema", "search_path"};
	if (allowed_settings.find(name) != allowed_settings.end()) {
		// we are always allowed to change these settings
		return;
	}
	if (!options.allowed_configs.empty()) {
		auto option = GetOptionByName(name);
		auto canonical_name = option ? Identifier(option->name) : name;
		if (options.allowed_configs.find(canonical_name) != options.allowed_configs.end()) {
			// settings that are allowed through allowed_configs
			return;
		}
	}
	// not allowed!
	throw InvalidInputException("Cannot change configuration option %s - the configuration has been locked", name);
}

idx_t DBConfig::GetSystemMaxThreads(FileSystem &fs) {
#ifdef DUCKDB_NO_THREADS
	return 1;
#else
	idx_t physical_cores = std::thread::hardware_concurrency();
#ifdef __linux__
	if (const char *slurm_cpus = getenv("SLURM_CPUS_ON_NODE")) {
		idx_t slurm_threads;
		if (TryCast::Operation<string_t, idx_t>(string_t(slurm_cpus), slurm_threads)) {
			return MaxValue<idx_t>(slurm_threads, 1);
		}
	}
	return MaxValue<idx_t>(CGroups::GetCPULimit(fs, physical_cores), 1);
#else
	return MaxValue<idx_t>(physical_cores, 1);
#endif
#endif
}

idx_t DBConfig::GetSystemMaxAsyncThreads(FileSystem &fs) {
#ifdef DUCKDB_NO_THREADS
	return 0;
#else
	return MinValue<idx_t>(4 * GetSystemMaxThreads(fs), 256);
#endif
}

idx_t DBConfig::GetSystemAvailableMemory(FileSystem &fs) {
	// System memory detection
	auto memory = FileSystem::GetAvailableMemory();
	auto available_memory = memory.IsValid() ? memory.GetIndex() : DBConfigOptions().maximum_memory;

#ifdef __linux__
	// Check SLURM environment variables first
	const char *slurm_mem_per_node = getenv("SLURM_MEM_PER_NODE");
	const char *slurm_mem_per_cpu = getenv("SLURM_MEM_PER_CPU");

	if (slurm_mem_per_node) {
		auto limit = ParseMemoryLimitSlurm(slurm_mem_per_node);
		if (limit.IsValid()) {
			return limit.GetIndex();
		}
	} else if (slurm_mem_per_cpu) {
		auto mem_per_cpu = ParseMemoryLimitSlurm(slurm_mem_per_cpu);
		if (mem_per_cpu.IsValid()) {
			idx_t num_threads = GetSystemMaxThreads(fs);
			return mem_per_cpu.GetIndex() * num_threads;
		}
	}

	// Check cgroup memory limit
	auto cgroup_memory_limit = CGroups::GetMemoryLimit(fs);
	if (cgroup_memory_limit.IsValid()) {
		auto cgroup_memory_limit_value = cgroup_memory_limit.GetIndex();
		return std::min(cgroup_memory_limit_value, available_memory);
	}
#endif

	return available_memory;
}

optional_idx DBConfig::ParseMemoryLimit(const string &arg) {
	if (arg[0] == '-' || arg == "null" || arg == "none") {
		// unlimited
		return optional_idx();
	}

	idx_t result;
	string error = StringUtil::TryParseFormattedBytes(arg, result);

	if (!error.empty()) {
		if (error == "Memory cannot be negative") {
			return optional_idx();
		}
		throw ParserException(error);
	}
	if (result == DConstants::INVALID_INDEX) {
		return optional_idx();
	}
	return result;
}

idx_t DBConfig::ParseMemoryLimitOrMaximum(const string &arg) {
	auto limit = ParseMemoryLimit(arg);
	return limit.IsValid() ? limit.GetIndex() : NumericLimits<idx_t>::Maximum();
}

optional_idx DBConfig::ParseMemoryLimitSlurm(const string &arg) {
	if (arg.empty()) {
		return optional_idx();
	}

	string number_str = arg;
	idx_t multiplier = 1000LL * 1000LL; // Default to MB if no unit specified

	// Check for SLURM-style suffixes
	if (arg.back() == 'K' || arg.back() == 'k') {
		number_str = arg.substr(0, arg.size() - 1);
		multiplier = 1000LL;
	} else if (arg.back() == 'M' || arg.back() == 'm') {
		number_str = arg.substr(0, arg.size() - 1);
		multiplier = 1000LL * 1000LL;
	} else if (arg.back() == 'G' || arg.back() == 'g') {
		number_str = arg.substr(0, arg.size() - 1);
		multiplier = 1000LL * 1000LL * 1000LL;
	} else if (arg.back() == 'T' || arg.back() == 't') {
		number_str = arg.substr(0, arg.size() - 1);
		multiplier = 1000LL * 1000LL * 1000LL * 1000LL;
	}

	// Parse the number
	double limit;
	if (!TryCast::Operation<string_t, double>(string_t(number_str), limit)) {
		return optional_idx();
	}

	if (Value::IsNan(limit)) {
		return optional_idx();
	}
	if (limit < 0) {
		return static_cast<idx_t>(NumericLimits<int64_t>::Maximum());
	}
	idx_t actual_limit;
	// double(idx_max) rounds up to 2^64, so equality already means the product is not representable
	if (limit >= static_cast<double>(NumericLimits<idx_t>::Maximum()) / static_cast<double>(multiplier)) {
		actual_limit = NumericLimits<idx_t>::Maximum();
	} else {
		actual_limit = LossyNumericCast<idx_t>(static_cast<double>(multiplier) * limit);
	}
	if (actual_limit == NumericLimits<idx_t>::Maximum()) {
		return static_cast<idx_t>(NumericLimits<int64_t>::Maximum());
	}
	return actual_limit;
}

// Right now we only really care about access mode when comparing DBConfigs
bool DBConfigOptions::operator==(const DBConfigOptions &other) const {
	return other.access_mode == access_mode && other.user_options == user_options &&
	       other.configuration_env == configuration_env;
}

bool DBConfig::operator==(const DBConfig &other) {
	return other.options == options;
}

bool DBConfig::operator!=(const DBConfig &other) {
	return !(other.options == options);
}

OrderType DBConfig::ResolveOrder(ClientContext &context, OrderType order_type) const {
	if (order_type != OrderType::ORDER_DEFAULT) {
		return order_type;
	}
	return Settings::Get<DefaultOrderSetting>(context);
}

SettingLookupResult DBConfig::TryGetCurrentUserSetting(idx_t setting_index, Value &result) const {
	return user_settings.TryGetSetting(setting_index, result);
}

SettingLookupResult DBConfig::TryGetDefaultValue(optional_ptr<const ConfigurationOption> option, Value &result) {
	if (!option || !option->default_value) {
		return SettingLookupResult();
	}
	auto input_type = ParseLogicalType(option->parameter_type);
	result = Value(option->default_value).DefaultCastAs(input_type);
	return SettingLookupResult(SettingScope::GLOBAL);
}

SettingLookupResult DBConfig::TryGetCurrentSetting(const Identifier &key, Value &result) const {
	optional_ptr<const ConfigurationOption> option;
	auto setting_index = TryGetSettingIndex(key, option);
	if (setting_index.IsValid()) {
		auto lookup_result = TryGetCurrentUserSetting(setting_index.GetIndex(), result);
		if (lookup_result) {
			return lookup_result;
		}
	}
	return TryGetDefaultValue(option, result);
}

optional_idx DBConfig::TryGetSettingIndex(const Identifier &name,
                                          optional_ptr<const ConfigurationOption> &option) const {
	ExtensionOption extension_option;
	if (TryGetExtensionOption(name, extension_option)) {
		// extension setting
		return extension_option.setting_index;
	}
	option = GetOptionByName(name);
	if (option) {
		// built-in setting
		return option->setting_idx;
	}
	// unknown setting
	return optional_idx();
}

OrderByNullType DBConfig::ResolveNullOrder(ClientContext &context, OrderType order_type,
                                           OrderByNullType null_type) const {
	if (null_type != OrderByNullType::ORDER_DEFAULT) {
		return null_type;
	}
	auto null_order = Settings::Get<DefaultNullOrderSetting>(context);
	switch (null_order) {
	case DefaultOrderByNullType::NULLS_FIRST:
		return OrderByNullType::NULLS_FIRST;
	case DefaultOrderByNullType::NULLS_LAST:
		return OrderByNullType::NULLS_LAST;
	case DefaultOrderByNullType::NULLS_FIRST_ON_ASC_LAST_ON_DESC:
		return order_type == OrderType::ASCENDING ? OrderByNullType::NULLS_FIRST : OrderByNullType::NULLS_LAST;
	case DefaultOrderByNullType::NULLS_LAST_ON_ASC_FIRST_ON_DESC:
		return order_type == OrderType::ASCENDING ? OrderByNullType::NULLS_LAST : OrderByNullType::NULLS_FIRST;
	default:
		throw InternalException("Unknown null order setting");
	}
}

string GetDefaultUserAgent() {
	return StringUtil::Format("duckdb/%s(%s)", DuckDB::LibraryVersion(), DuckDB::Platform());
}

const string DBConfig::UserAgent() const {
	auto user_agent = GetDefaultUserAgent();

	auto duckdb_api = Settings::Get<DuckDBAPISetting>(*this);
	if (!duckdb_api.empty()) {
		user_agent += " " + duckdb_api;
	}

	if (!options.custom_user_agent.empty()) {
		user_agent += " " + options.custom_user_agent;
	}
	return user_agent;
}

ExtensionCallbackManager &DBConfig::GetCallbackManager() {
	return *callback_manager;
}

const ExtensionCallbackManager &DBConfig::GetCallbackManager() const {
	return *callback_manager;
}

string DBConfig::SanitizeAllowedPath(const string &path_p, optional_ptr<FileOpener> opener) const {
	auto result = file_system->CanonicalizePath(FileSystem::ExpandPath(path_p, opener));
	// allowed_directories/allowed_path always uses forward slashes regardless of the OS
	auto path_sep = file_system->PathSeparator(path_p);
	if (path_sep != "/") {
		result = StringUtil::Replace(result, path_sep, "/");
	}
	return result;
}

void DBConfig::AddAllowedConfig(const Identifier &config_name) {
	if (config_name.empty()) {
		throw InvalidInputException("Cannot provide an empty string for allowed_configs");
	}
	duckdb::identifier_set_t always_disallowed_config {"allowed_configs", "lock_configuration"};
	if (always_disallowed_config.find(config_name) != always_disallowed_config.end()) {
		throw InvalidInputException("Cannot include %s in allowed_configs", config_name);
	}
	// Validate that the config name refers to a known setting (built-in or extension)
	// and resolve aliases to canonical names
	auto option = GetOptionByName(config_name);
	if (option) {
		// Store the canonical name so alias lookups work in CheckLock
		options.allowed_configs.insert(option->name);
		return;
	}
	ExtensionOption extension_option;
	if (TryGetExtensionOption(config_name, extension_option)) {
		options.allowed_configs.insert(config_name);
		return;
	}
	// Check if the setting belongs to a known extension (even if not yet loaded)
	auto extension_name = ExtensionHelper::FindExtensionInEntries(config_name, EXTENSION_SETTINGS);
	if (!extension_name.empty()) {
		// Accept the setting - the extension may be autoloaded later when the setting is used
		options.allowed_configs.insert(config_name);
		return;
	}
	throw InvalidInputException("Unknown configuration option %s in allowed_configs", config_name);
}

void DBConfig::AddAllowedDirectory(const string &path) {
	auto allowed_directory = SanitizeAllowedPath(path);
	if (allowed_directory.empty()) {
		throw InvalidInputException("Cannot provide an empty string for allowed_directory");
	}
	// ensure the directory ends with a path separator
	if (!StringUtil::EndsWith(allowed_directory, "/")) {
		allowed_directory += "/";
	}
	lock_guard<mutex> guard(allowed_paths_lock);
	options.allowed_directories.insert(allowed_directory);
}

bool DBConfig::IsOptimizerDisabled(OptimizerType type) const {
	if (!has_disabled_optimizers.load()) {
		return false;
	}
	lock_guard<mutex> guard(disabled_optimizers_lock);
	return options.disabled_optimizers.find(type) != options.disabled_optimizers.end();
}

set<OptimizerType> DBConfig::GetDisabledOptimizers() const {
	lock_guard<mutex> guard(disabled_optimizers_lock);
	return options.disabled_optimizers;
}

void DBConfig::SetDisabledOptimizers(set<OptimizerType> disabled_optimizers) {
	lock_guard<mutex> guard(disabled_optimizers_lock);
	options.disabled_optimizers = std::move(disabled_optimizers);
	has_disabled_optimizers.store(!options.disabled_optimizers.empty());
}

void DBConfig::SetAllowedDirectories(const vector<string> &paths) {
	decltype(options.allowed_directories) new_directories;
	for (auto &path : paths) {
		auto allowed_directory = SanitizeAllowedPath(path);
		if (allowed_directory.empty()) {
			throw InvalidInputException("Cannot provide an empty string for allowed_directory");
		}
		if (!StringUtil::EndsWith(allowed_directory, "/")) {
			allowed_directory += "/";
		}
		new_directories.insert(std::move(allowed_directory));
	}
	lock_guard<mutex> guard(allowed_paths_lock);
	options.allowed_directories = std::move(new_directories);
}

void DBConfig::SetAllowedPaths(const vector<string> &paths) {
	decltype(options.allowed_paths) new_paths;
	for (auto &path : paths) {
		new_paths.insert(SanitizeAllowedPath(path));
	}
	lock_guard<mutex> guard(allowed_paths_lock);
	options.allowed_paths = std::move(new_paths);
}

void DBConfig::ResetAllowedDirectories() {
	lock_guard<mutex> guard(allowed_paths_lock);
	options.allowed_directories = DBConfigOptions().allowed_directories;
}

void DBConfig::ResetAllowedPaths() {
	lock_guard<mutex> guard(allowed_paths_lock);
	options.allowed_paths = DBConfigOptions().allowed_paths;
}

void DBConfig::AddAllowedPath(const string &path) {
	auto allowed_path = SanitizeAllowedPath(path);
	lock_guard<mutex> guard(allowed_paths_lock);
	options.allowed_paths.insert(allowed_path);
}

void DBConfig::AddAllowedDatabasePath(const string &database_path) {
	AddAllowedPath(database_path);
	AddAllowedPath(database_path + ".wal");
	AddAllowedPath(database_path + ".wal.checkpoint");
	AddAllowedPath(database_path + ".wal.recovery");
}

vector<string> DBConfig::GetAllowedDirectories() const {
	lock_guard<mutex> guard(allowed_paths_lock);
	return vector<string>(options.allowed_directories.begin(), options.allowed_directories.end());
}

vector<string> DBConfig::GetAllowedPaths() const {
	lock_guard<mutex> guard(allowed_paths_lock);
	return vector<string>(options.allowed_paths.begin(), options.allowed_paths.end());
}

bool DBConfig::CanAccessFile(const string &input_path, FileType type, optional_ptr<FileOpener> opener) {
	if (Settings::Get<EnableExternalAccessSetting>(*this)) {
		// all external access is allowed
		return true;
	}
	string path = SanitizeAllowedPath(input_path, opener);

	lock_guard<mutex> guard(allowed_paths_lock);
	if (options.allowed_paths.count(path) > 0) {
		// path is explicitly allowed
		return true;
	}

	if (options.allowed_directories.empty()) {
		// no prefix directories specified
		return false;
	}
	if (type == FileType::FILE_TYPE_DIR) {
		// make sure directories end with a /
		if (!StringUtil::EndsWith(path, "/")) {
			path += "/";
		}
	}

	string prefix;
	for (const auto &allowed_directory : options.allowed_directories) {
		if (StringUtil::StartsWith(path, allowed_directory)) {
			prefix = allowed_directory;
			break;
		}
	}

	if (prefix.empty()) {
		// no common prefix found - path is not inside an allowed directory
		return false;
	}
	D_ASSERT(StringUtil::EndsWith(prefix, "/"));
	return true;
}

SerializationOptions::SerializationOptions(AttachedDatabase &db) {
	storage_compatibility = StorageCompatibility::FromDatabase(db);
}

void DBConfig::SetHTTPUtil(const shared_ptr<HTTPUtil> &new_http_util) {
	http_transport_manager->SetHTTPUtil(new_http_util);
}

void DBConfig::SetExternalExtensionProvider(const shared_ptr<ExternalExtensionProvider> &new_provider) {
	if (!new_provider) {
		throw InvalidInputException("External extension provider cannot be null");
	}
	external_extension_provider = new_provider;
}

ExternalExtensionProvider &DBConfig::GetExternalExtensionProvider() const {
	return *external_extension_provider;
}

HTTPUtil &DBConfig::GetHTTPUtil() const {
	return http_transport_manager->GetHTTPUtil();
}

HTTPTransportManager &DBConfig::GetHTTPTransportManager() {
	return *http_transport_manager;
}

const HTTPTransportManager &DBConfig::GetHTTPTransportManager() const {
	return *http_transport_manager;
}

} // namespace duckdb
