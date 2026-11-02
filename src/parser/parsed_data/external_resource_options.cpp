#include "duckdb/parser/parsed_data/external_resource_options.hpp"

#include "duckdb/parser/parsed_data/parse_info.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

unique_ptr<ExternalResourceOptions> ExternalResourceOptions::Copy() const {
	auto result = make_uniq<ExternalResourceOptions>();
	for (auto &entry : parsed_params) {
		result->parsed_params[entry.first] = entry.second->Copy();
	}
	result->provider = provider;
	result->params = params;
	result->reference_name = reference_name;
	return result;
}

string ExternalResourceOptions::ToString() const {
	// Reference form: a bare identifier naming an already-registered resource.
	if (!reference_name.empty()) {
		return "EXTERNAL RESOURCE " + SQLIdentifier(reference_name);
	}
	// Create form: NEW TEMPORARY EXTERNAL RESOURCE '<type>' [(create opts)].
	string result = "NEW TEMPORARY EXTERNAL RESOURCE " + SQLString(provider);
	result += RenderOptionList(parsed_params, params);
	return result;
}

void ExternalResourceOptions::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<string>(101, "provider", provider);
	serializer.WritePropertyWithDefault<unordered_map<string, Value>>(102, "params", params);
	serializer.WritePropertyWithDefault<string>(103, "reference_name", reference_name);
}

unique_ptr<ExternalResourceOptions> ExternalResourceOptions::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<ExternalResourceOptions>();
	deserializer.ReadPropertyWithDefault<string>(101, "provider", result->provider);
	deserializer.ReadPropertyWithDefault<unordered_map<string, Value>>(102, "params", result->params);
	deserializer.ReadPropertyWithDefault<string>(103, "reference_name", result->reference_name);
	return result;
}

} // namespace duckdb
