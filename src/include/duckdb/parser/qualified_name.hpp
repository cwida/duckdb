//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/qualified_name.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
struct BindingAlias;
class Serializer;
class Deserializer;

//! Controls how QualifiedName::ToString renders the schema qualification
enum class QualifiedNameToStringMode : uint8_t {
	//! Always render every (non-empty) component
	DEFAULT,
	//! Omit the schema when it is a system schema (the default/"main" schema)
	HIDE_DEFAULT_SCHEMA
};

struct QualifiedName {
	QualifiedName() = default;
	//! Construct an unqualified name (no catalog/schema). Implicit so that an Identifier can be passed wherever an
	//! unqualified QualifiedName lookup is expected (also preserves backwards-compatibility for extensions).
	QualifiedName(Identifier name_p) { // NOLINT: allow implicit conversion
		path.push_back(std::move(name_p));
	}
	QualifiedName(Identifier catalog_p, Identifier schema_p, Identifier name_p) {
		// store the catalog/schema/name as a single path - deeper (nested schema) paths are built with the
		// vector<Identifier> constructor below
		if (!catalog_p.empty()) {
			path.push_back(std::move(catalog_p));
			path.push_back(std::move(schema_p));
		} else if (!schema_p.empty()) {
			path.push_back(std::move(schema_p));
		}
		path.push_back(std::move(name_p));
	}
	//! Construct from an explicit schema path (the catalog/schema components actually present) and a name. Use this to
	//! avoid passing INVALID_CATALOG/INVALID_SCHEMA placeholders for components that are not set.
	QualifiedName(vector<Identifier> schema_path_p, Identifier name_p) : path(std::move(schema_path_p)) {
		path.push_back(std::move(name_p));
	}
	//! Construct from a complete path, including its final name.
	static QualifiedName FromPath(vector<Identifier> path) {
		QualifiedName result;
		result.path = std::move(path);
		return result;
	}
	//! Qualify a schema path with an explicit catalog, without inferring a catalog from the path's length.
	static QualifiedName FromCatalogSchema(Identifier catalog, vector<Identifier> schema_path, Identifier name) {
		if (schema_path.empty()) {
			return QualifiedName(std::move(catalog), Identifier(), std::move(name));
		}
		if (!catalog.empty()) {
			schema_path.insert(schema_path.begin(), std::move(catalog));
		}
		return QualifiedName(std::move(schema_path), std::move(name));
	}
	//! Return the qualification as a name in its own right.
	QualifiedName Parent() const {
		auto result = *this;
		if (!result.path.empty()) {
			result.path.pop_back();
		}
		return result;
	}

	//! The catalog is the first element of the path, but only when the path is fully qualified ([catalog,
	//! schema..., name])
	const Identifier &Catalog() const {
		return path.size() >= 3 ? path[0] : empty;
	}
	//! The schema is the element directly before the name (or empty if there is no schema)
	const Identifier &Schema() const {
		return path.size() >= 2 ? path[path.size() - 2] : empty;
	}
	//! The name is the last element of the path
	const Identifier &Name() const {
		return path.empty() ? empty : path.back();
	}
	//! The (possibly nested) schema qualification between the catalog and the name
	vector<Identifier> SchemaPath() const {
		vector<Identifier> result;
		for (idx_t i = path.size() >= 3 ? 1 : 0; i + 1 < path.size(); i++) {
			if (!path[i].empty()) {
				result.push_back(path[i]);
			}
		}
		return result;
	}
	//! The full underlying path. Most callers should use Catalog()/Schema()/Name(); this is for multi-level schema
	//! support (e.g. nested CREATE SCHEMA), where the qualification can be deeper than [catalog, schema].
	const vector<Identifier> &Path() const {
		return path;
	}

	//! Return a copy of this name with the name replaced, keeping the (possibly nested) catalog/schema qualification
	QualifiedName WithName(Identifier name) const {
		vector<Identifier> qualification;
		if (!path.empty()) {
			qualification.insert(qualification.end(), path.begin(), path.end() - 1);
		}
		return QualifiedName(std::move(qualification), std::move(name));
	}
	//! Return a copy of this name with the catalog/schema qualification replaced by the given path, keeping the name
	QualifiedName WithQualification(vector<Identifier> schema_path) const {
		return QualifiedName(std::move(schema_path), Name());
	}
	//! Drop the catalog component (if any), keeping the (possibly nested) schema path and the name
	void StripCatalog() {
		if (path.size() < 3) {
			return;
		}
		path.erase(path.begin());
		if (path.size() >= 2 && path[0].empty()) {
			// the name was catalog-qualified without a schema - drop the empty schema placeholder as well
			path.erase(path.begin());
		}
	}
	//! Return a copy of this name qualified with the given catalog, replacing the catalog component it already has.
	//! An empty catalog strips the catalog qualification.
	QualifiedName WithCatalog(Identifier catalog) const {
		if (catalog.empty()) {
			auto result = *this;
			result.StripCatalog();
			return result;
		}
		if (path.size() < 2) {
			return QualifiedName(std::move(catalog), Identifier(), Name());
		}
		vector<Identifier> qualification;
		qualification.push_back(std::move(catalog));
		// keep the (possibly nested) schema path, skipping the catalog component when the name is fully qualified
		for (idx_t i = path.size() >= 3 ? 1 : 0; i + 1 < path.size(); i++) {
			qualification.push_back(path[i]);
		}
		return QualifiedName(std::move(qualification), Name());
	}

	//! Parse the (optional) schema and a name from a string in the format of e.g. "schema"."table"; if there is no dot
	//! the schema will be set to INVALID_SCHEMA
	static QualifiedName Parse(const string &input);
	//! Parse a qualified name, returning false and setting the error if it is malformed or any component is empty
	static bool TryParse(const string &input, QualifiedName &result, string &error);
	static vector<Identifier> ParseComponents(const string &input);
	string ToString(QualifiedNameToStringMode mode = QualifiedNameToStringMode::DEFAULT) const;
	//! Render only the qualification (every component before the name), with a trailing "." after each component
	string QualificationToString(QualifiedNameToStringMode mode = QualifiedNameToStringMode::DEFAULT) const;

	hash_t Hash() const;
	bool operator==(const QualifiedName &rhs) const;
	bool operator!=(const QualifiedName &rhs) const;

	void Serialize(Serializer &serializer) const;
	static QualifiedName Deserialize(Deserializer &deserializer);

private:
	//! The full path (catalog/schema/name). The name is always the last element; the catalog/schema components that
	//! are actually present precede it, and the schema part can be a nested chain ([catalog, s1, s2, ..., name]).
	vector<Identifier> path;
	//! Always-empty identifier, returned by the accessors when a catalog/schema/name component is absent
	Identifier empty;
};

struct QualifiedColumnName {
	QualifiedColumnName();
	QualifiedColumnName(Identifier column_p); // NOLINT: allow implicit conversion from string to column name
	QualifiedColumnName(Identifier table_p, Identifier column_p);
	QualifiedColumnName(const BindingAlias &alias, Identifier column_p);

	Identifier catalog;
	Identifier schema;
	Identifier table;
	Identifier column;

	static QualifiedColumnName Parse(string &input);

	string ToString() const;
	string ToDisplayString() const;

	void Serialize(Serializer &serializer) const;
	static QualifiedColumnName Deserialize(Deserializer &deserializer);

	bool IsQualified() const;

	bool operator==(const QualifiedColumnName &rhs) const;
};

} // namespace duckdb
