#include "duckdb/parser/parsed_data/extra_drop_info.hpp"

namespace duckdb {

ExtraDropTriggerInfo::ExtraDropTriggerInfo() : ExtraDropInfo(ExtraDropInfoType::TRIGGER_INFO) {
}

ExtraDropTriggerInfo::ExtraDropTriggerInfo(const ExtraDropTriggerInfo &info)
    : ExtraDropInfo(ExtraDropInfoType::TRIGGER_INFO), base_table(info.base_table ? info.base_table->Copy() : nullptr) {
}

unique_ptr<ExtraDropInfo> ExtraDropTriggerInfo::Copy() const {
	return make_uniq<ExtraDropTriggerInfo>(*this);
}

ExtraDropSecretInfo::ExtraDropSecretInfo()
    : ExtraDropInfo(ExtraDropInfoType::SECRET_INFO), persist_mode(SecretPersistType::DEFAULT) {
}

ExtraDropSecretInfo::ExtraDropSecretInfo(const ExtraDropSecretInfo &info)
    : ExtraDropInfo(ExtraDropInfoType::SECRET_INFO) {
	persist_mode = info.persist_mode;
	secret_storage = info.secret_storage;
}

unique_ptr<ExtraDropInfo> ExtraDropSecretInfo::Copy() const {
	return std::move(make_uniq<ExtraDropSecretInfo>(*this));
}

} // namespace duckdb
