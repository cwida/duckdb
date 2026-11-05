#include "duckdb/parser/parsed_data/attach_info.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

unique_ptr<AttachInfo> AttachInfo::Copy() const {
	auto result = make_uniq<AttachInfo>();
	result->name = name;
	result->path = path;
	if (parsed_path) {
		result->parsed_path = parsed_path->Copy();
	}
	result->options = options;
	for (auto &entry : parsed_options) {
		result->parsed_options[entry.first] = entry.second->Copy();
	}
	result->on_conflict = on_conflict;
	if (external_resource) {
		result->external_resource = external_resource->Copy();
	}
	return result;
}

string AttachInfo::ToString() const {
	string result = "";
	// `ATTACH TO [NEW TEMPORARY] EXTERNAL RESOURCE <resource> [(create opts)] AS name [(attach opts)]`
	if (external_resource) {
		// No IF NOT EXISTS / OR REPLACE: AttachToExternalResource has no slot for either, so rendering
		// one would produce SQL that cannot be parsed back.
		result += "ATTACH TO " + external_resource->ToString();
		if (!name.empty()) {
			result += " AS " + SQLIdentifier(name);
		}
		result += RenderOptionList(parsed_options, options);
		result += ";";
		return result;
	}
	result += "ATTACH";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		result += " IF NOT EXISTS";
	} else if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		result += " OR REPLACE";
	}
	result += " DATABASE ";
	if (parsed_path) {
		result += parsed_path->ToString();
	} else {
		result += SQLString(path);
	}
	if (!name.empty()) {
		result += " AS " + SQLIdentifier(name);
	}
	result += RenderOptionList(parsed_options, options);
	result += ";";
	return result;
}

} // namespace duckdb
