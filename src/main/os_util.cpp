#include "duckdb/main/os_util.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/windows_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"

#include <cstdlib>

namespace duckdb {

OSUtil::OSUtil(DatabaseInstance &db, unordered_set<string> configuration_env_p)
    : db(db), configuration_env(std::move(configuration_env_p)) {
}

OSUtil &OSUtil::Get(DatabaseInstance &db) {
	return db.GetOSUtil();
}

bool OSUtil::IsConfigurationEnv(const string &name) {
	// the home directory, the resource limits of the job the database runs in, its proxy and its time zone
	static const char *CONFIGURATION_ENV[] = {
	    "HOME", "USERPROFILE", "SLURM_CPUS_ON_NODE", "SLURM_MEM_PER_NODE", "SLURM_MEM_PER_CPU", "HTTP_PROXY", "TZ"};
	for (auto known : CONFIGURATION_ENV) {
		if (name == known) {
			return true;
		}
	}
	return false;
}

bool OSUtil::GetConfigurationEnv(const string &name, string &value) {
	if (!IsConfigurationEnv(name)) {
		throw InternalException("Environment variable \"%s\" is not a configuration variable of the engine - read it "
		                        "through the database with GetEnv",
		                        name);
	}
	return ReadEnv(name, value);
}

string OSUtil::GetConfigurationEnv(const string &name) {
	string value;
	if (!GetConfigurationEnv(name, value)) {
		return string();
	}
	return value;
}

bool OSUtil::GetEnv(const string &name, string &value) {
	if (!Settings::Get<EnableExternalAccessSetting>(db)) {
		throw PermissionException(
		    "Cannot read environment variable \"%s\" - environment access is disabled by configuration", name);
	}
	return ReadEnv(name, value);
}

string OSUtil::GetEnv(const string &name) {
	string value;
	if (!GetEnv(name, value)) {
		return string();
	}
	return value;
}

bool OSUtil::GetEnvUnrestricted(const string &name, string &value) {
	if (!IsConfigurationEnv(name) && configuration_env.find(name) == configuration_env.end()) {
		throw InternalException("Environment variable \"%s\" is not a configuration variable - read it with GetEnv",
		                        name);
	}
	return ReadEnv(name, value);
}

string OSUtil::GetEnvUnrestricted(const string &name) {
	string value;
	if (!GetEnvUnrestricted(name, value)) {
		return string();
	}
	return value;
}

#ifndef _WIN32

bool OSUtil::ReadEnv(const string &name, string &value) {
	const char *env = std::getenv(name.c_str());
	if (!env) {
		return false;
	}
	value = env;
	return true;
}

#else

bool OSUtil::ReadEnv(const string &name, string &value) {
	auto name_w = WindowsUtil::UTF8ToUnicode(name.c_str());
	auto value_w = _wgetenv(name_w.c_str());
	if (!value_w) {
		return false;
	}
	value = WindowsUtil::UnicodeToUTF8(value_w);
	return true;
}

#endif

} // namespace duckdb
