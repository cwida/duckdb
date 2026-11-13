#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/binding_alias.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

void QualifiedName::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<vector<Identifier>>(100, "path", path);
}

QualifiedName QualifiedName::Deserialize(Deserializer &deserializer) {
	QualifiedName result;
	result.path = deserializer.ReadPropertyWithDefault<vector<Identifier>>(100, "path");
	return result;
}

string QualifiedName::QualificationToString(QualifiedNameToStringMode mode) const {
	string result;
	// render every qualification component (the path can hold a nested schema chain)
	for (idx_t i = 0; i + 1 < path.size(); i++) {
		auto &component = path[i];
		if (component.empty()) {
			continue;
		}
		if (mode == QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA && result.empty() && i + 2 == path.size() &&
		    component == DEFAULT_SCHEMA) {
			// the only qualification is the default schema - hide it
			continue;
		}
		result += SQLIdentifier(component) + ".";
	}
	return result;
}

string QualifiedName::ToString(QualifiedNameToStringMode mode) const {
	if (path.empty()) {
		return string();
	}
	return QualificationToString(mode) + SQLIdentifier(Name());
}

//! This parses a superset of the strings that the actual SQL parser accepts: it allows whitespace, most special
//! characters like ()'- and keywords without requiring double quotes. It only requires double quotes around .
//! characters and doubled double quotes (which collapse into a single double quote). It's only possible to fully
//! double quote a component or not quote it at all.
static bool TryParseComponents(const string &input, vector<Identifier> &result, string &error) {
	idx_t idx = 0;
	while (idx < input.size()) {
		string entry;
		if (input[idx] == '"') {
			if (!StringUtil::TryParseQuotedString(input, idx, entry)) {
				error = StringUtil::Format("Unterminated quote in qualified name! (input: %s)", input);
				return false;
			}
			if (entry.empty()) {
				error = StringUtil::Format("Zero-length delimited identifier in qualified name! (input: %s)", input);
				return false;
			}
			if (idx < input.size() && input[idx] != '.') {
				error = StringUtil::Format(
				    "Unexpected character after a quoted identifier in a qualified name! (input: %s)", input);
				return false;
			}
		} else {
			for (; idx < input.size() && input[idx] != '.'; idx++) {
				if (input[idx] == '"') {
					error = StringUtil::Format(
					    "Unexpected quote in the middle of a qualified name component! (input: %s)", input);
					return false;
				}
				entry += input[idx];
			}
		}
		result.emplace_back(std::move(entry));
		if (idx < input.size()) {
			idx++;
		}
	}
	return true;
}

vector<Identifier> QualifiedName::ParseComponents(const string &input) {
	vector<Identifier> result;
	string error;
	if (!TryParseComponents(input, result, error)) {
		throw ParserException(error);
	}
	return result;
}

hash_t QualifiedName::Hash() const {
	// hash the full path - the qualification can be deeper than [catalog, schema]
	hash_t result = duckdb::Hash<idx_t>(path.size());
	for (auto &component : path) {
		result = CombineHash(result, component.Hash());
	}
	return result;
}

bool QualifiedName::operator==(const QualifiedName &rhs) const {
	// compare the full path - the qualification can be deeper than [catalog, schema]
	if (path.size() != rhs.path.size()) {
		return false;
	}
	for (idx_t i = 0; i < path.size(); i++) {
		if (path[i] != rhs.path[i]) {
			return false;
		}
	}
	return true;
}

bool QualifiedName::operator!=(const QualifiedName &rhs) const {
	return !(*this == rhs);
}

QualifiedName QualifiedName::Parse(const string &input) {
	return FromPath(ParseComponents(input));
}

bool QualifiedName::TryParse(const string &input, QualifiedName &result, string &error) {
	vector<Identifier> components;
	if (!TryParseComponents(input, components, error)) {
		return false;
	}
	if (components.empty()) {
		error = StringUtil::Format("Zero-length identifier in qualified name! (input: %s)", input);
		return false;
	}
	for (auto &component : components) {
		if (component.empty()) {
			error = StringUtil::Format("Zero-length identifier in qualified name! (input: %s)", input);
			return false;
		}
	}
	result = FromPath(std::move(components));
	return true;
}

QualifiedColumnName::QualifiedColumnName() {
}
QualifiedColumnName::QualifiedColumnName(Identifier column_p) : column(std::move(column_p)) {
}
QualifiedColumnName::QualifiedColumnName(Identifier table_p, Identifier column_p)
    : table(std::move(table_p)), column(std::move(column_p)) {
}
QualifiedColumnName::QualifiedColumnName(const BindingAlias &alias, Identifier column_p)
    : catalog(alias.GetCatalog()), schema(alias.GetSchema()), table(alias.GetAlias()), column(std::move(column_p)) {
}

QualifiedColumnName QualifiedColumnName::Parse(string &input) {
	auto components = QualifiedName::ParseComponents(input);
	if (components.size() == 1) {
		return QualifiedColumnName(components[0]);
	} else if (components.size() == 2) {
		return QualifiedColumnName(components[0], components[1]);
	} else if (components.size() == 3) {
		QualifiedColumnName qname;
		qname.schema = components[0];
		qname.table = components[1];
		qname.column = components[2];
		return qname;
	} else if (components.size() == 4) {
		QualifiedColumnName qname;
		qname.catalog = components[0];
		qname.schema = components[1];
		qname.table = components[2];
		qname.column = components[3];
		return qname;
	} else {
		throw ParserException(
		    "Expected at most 4 entries (catalog.schema.table.column), but found %zu entries (input: %s)",
		    components.size(), input);
	}
}

string QualifiedColumnName::ToString() const {
	string result;
	if (!catalog.empty()) {
		result += SQLIdentifier(catalog) + ".";
	}
	if (!schema.empty()) {
		result += SQLIdentifier(schema) + ".";
	}
	if (!table.empty()) {
		result += SQLIdentifier(table) + ".";
	}
	result += SQLIdentifier(column);
	return result;
}

string QualifiedColumnName::ToDisplayString() const {
	string result;
	if (!catalog.empty()) {
		result += SQLQuotedIdentifier::ToString(catalog) + ".";
	}
	if (!schema.empty()) {
		result += SQLQuotedIdentifier::ToString(schema) + ".";
	}
	if (!table.empty()) {
		result += SQLQuotedIdentifier::ToString(table) + ".";
	}
	result += SQLQuotedIdentifier::ToString(column);
	return result;
}

bool QualifiedColumnName::IsQualified() const {
	return !catalog.empty() || !schema.empty() || !table.empty();
}

bool QualifiedColumnName::operator==(const QualifiedColumnName &rhs) const {
	return catalog == rhs.catalog && schema == rhs.schema && table == rhs.table && column == rhs.column;
}

} // namespace duckdb
