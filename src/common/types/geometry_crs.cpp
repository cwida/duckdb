#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/common/json_document.hpp"
#include "duckdb/catalog/catalog_entry/coordinate_system_catalog_entry.hpp"
#include "fast_float/fast_float.h"

#include <limits>

namespace duckdb {

//----------------------------------------------------------------------------------------------------------------------
// WKT2:2019 Parsing
//----------------------------------------------------------------------------------------------------------------------
namespace {
class WKTKeyword;
class WKTNumber;
class WKTString;

class WKTValue {
public:
	enum class Type { KEYWORD, NUMBER, STRING } type;
	virtual ~WKTValue() = default;

	template <class T>
	T &As() {
		D_ASSERT(T::TYPE == type);
		return static_cast<T &>(*this);
	}

	template <class T>
	const T &As() const {
		D_ASSERT(T::TYPE == type);
		return static_cast<const T &>(*this);
	}

	bool IsKeyword() const {
		return type == Type::KEYWORD;
	}
	bool IsNumber() const {
		return type == Type::NUMBER;
	}
	bool IsString() const {
		return type == Type::STRING;
	}

protected:
	explicit WKTValue(Type type_p) : type(type_p) {
	}
};

class WKTKeyword final : public WKTValue {
public:
	static constexpr auto TYPE = Type::KEYWORD;

	const string &GetName() const {
		return name;
	}

	const vector<unique_ptr<WKTValue>> &GetChildren() const {
		return children;
	}

	bool Match(const char *keyword) const {
		return StringUtil::CIEquals(name, keyword);
	}

	explicit WKTKeyword(string name, vector<unique_ptr<WKTValue>> children_p)
	    : WKTValue(TYPE), name(std::move(name)), children(std::move(children_p)) {
	}

private:
	string name;
	vector<unique_ptr<WKTValue>> children;
};

class WKTNumber final : public WKTValue {
public:
	static constexpr auto TYPE = Type::NUMBER;

	double GetValue() const {
		return value;
	}

	explicit WKTNumber(double value_p) : WKTValue(TYPE), value(value_p) {
	}

private:
	double value;
};

class WKTString final : public WKTValue {
public:
	static constexpr auto TYPE = Type::STRING;

	const string &GetValue() const {
		return value;
	}

	explicit WKTString(string value) : WKTValue(TYPE), value(std::move(value)) {
	}

private:
	string value;
};

class WKTParser {
public:
	static unique_ptr<WKTValue> Parse(const string &wkt) {
		WKTParser parser(wkt.c_str(), wkt.size());

		// Skip initial whitespace
		parser.SkipWhitespace();

		// Parse the root node
		auto node = parser.ParseNode();

		return node;
	}

private:
	const char *beg;
	const char *end;
	const char *pos;
	uint32_t depth;

private:
	WKTParser(const char *text, size_t size) : beg(text), end(text + size), pos(text), depth(0) {
	}

	bool TryMatch(char c) {
		if (pos < end && *pos == c) {
			pos++;
			SkipWhitespace(); // remove trailing whitespace
			return true;
		}
		return false; // not matched
	}

	void Match(char c) {
		if (!TryMatch(c)) {
			if (pos == end) {
				throw InvalidInputException("Expected '%c' but got end of input at position %zu", c, pos - beg);
			}
			throw InvalidInputException("Expected '%c' but got '%c' at position %zu", c, *pos, pos - beg);
		}
	}

	bool TryMatchText(string &result) {
		const auto start = pos;
		// First character must be alphabetic or underscore
		if (pos < end && (StringUtil::CharacterIsAlpha(*pos) || *pos == '_')) {
			pos++;
			// Subsequent characters can also include digits
			while (pos < end && (StringUtil::CharacterIsAlphaNumeric(*pos) || *pos == '_')) {
				pos++;
			}
		}
		if (pos == start) {
			// Didnt match any text
			return false;
		}
		result = string(start, UnsafeNumericCast<size_t>(pos - start));

		SkipWhitespace();
		return true;
	}

	unique_ptr<WKTValue> ParseStringNode() {
		if (!TryMatch('"')) {
			return nullptr;
		}
		const char *start = pos;
		string result;
		while (pos < end) {
			if (*pos == '"') {
				// Check for escaped quote (doubled quote)
				if (pos + 1 < end && *(pos + 1) == '"') {
					// Append everything up to and including one quote
					result.append(start, UnsafeNumericCast<size_t>(pos - start + 1));
					pos += 2; // Skip both quotes
					start = pos;
				} else {
					// End of string
					break;
				}
			} else {
				pos++;
			}
		}
		if (pos == end) {
			throw InvalidInputException("Unterminated string starting at position %zu", start - beg);
		}
		// Append any remaining content before the closing quote
		result.append(start, UnsafeNumericCast<size_t>(pos - start));

		Match('"');
		SkipWhitespace();

		return make_uniq<WKTString>(result);
	}

	unique_ptr<WKTValue> ParseKeywordNode() {
		string name;
		vector<unique_ptr<WKTValue>> children;

		if (!TryMatchText(name)) {
			return nullptr;
		}

		if (TryMatch('[')) {
			do {
				auto child = ParseNode();
				if (!child) {
					break;
				}
				children.push_back(std::move(child));
			} while (TryMatch(','));

			Match(']');
			return make_uniq<WKTKeyword>(std::move(name), std::move(children));
		}

		if (TryMatch('(')) {
			do {
				auto child = ParseNode();
				if (!child) {
					break;
				}
				children.push_back(std::move(child));
			} while (TryMatch(','));
			Match(')');
			return make_uniq<WKTKeyword>(std::move(name), std::move(children));
		}

		// Nodes don't have to have children
		return make_uniq<WKTKeyword>(std::move(name), std::move(children));
	}

	unique_ptr<WKTValue> ParseNumberNode() {
		if (pos != end) {
			double num;
			const auto res = duckdb_fast_float::from_chars(pos, end, num);
			if (res.ec != std::errc()) {
				return nullptr;
			}
			pos = res.ptr;    // update position to the end of the parsed number
			SkipWhitespace(); // remove trailing whitespace
			return make_uniq<WKTNumber>(num);
		}

		return nullptr;
	}

	unique_ptr<WKTValue> ParseNode() {
		// Increment depth to avoid stack overflow on malicious input
		if (depth++ > 1000) {
			throw InvalidInputException("WKT input is too deeply nested to parse");
		}

		unique_ptr<WKTValue> node = nullptr;

		node = ParseStringNode();
		if (node) {
			return node;
		}

		node = ParseKeywordNode();
		if (node) {
			return node;
		}

		node = ParseNumberNode();
		if (node) {
			return node;
		}

		return nullptr;
	}

	void SkipWhitespace() {
		while (pos < end && StringUtil::CharacterIsSpace(*pos)) {
			pos++;
		}
	}
};

static string TryExtractIDFromWKTNode(const WKTKeyword &keyword) {
	auto &children = keyword.GetChildren();
	for (const auto &child : children) {
		if (!child->IsKeyword()) {
			continue;
		}

		const auto &child_keyword = child->As<WKTKeyword>();
		if (!child_keyword.Match("ID")) {
			continue;
		}

		auto &id_children = child_keyword.GetChildren();
		if (id_children.size() < 2) {
			continue;
		}

		const auto &first = id_children[0];
		const auto &second = id_children[1];

		if (!first->IsString()) {
			continue;
		}

		const auto code_auth = first->As<WKTString>().GetValue();
		if (second->IsNumber()) {
			const auto number = second->As<WKTNumber>().GetValue();
			if (number < 0 || number >= static_cast<double>(std::numeric_limits<idx_t>::max())) {
				continue;
			}
			return code_auth + ":" + StringUtil::Format("%llu", static_cast<idx_t>(number));
		}
		if (second->IsString()) {
			return code_auth + ":" + second->As<WKTString>().GetValue();
		}
		return string();
	}

	return string();
}

} // namespace

bool CoordinateReferenceSystem::TryParseWKT2(const string &text, CoordinateReferenceSystem &result) {
	const auto node = WKTParser::Parse(text);

	if (!node) {
		// Not a valid WKT2 string
		return false;
	}

	if (!node->IsKeyword()) {
		return false;
	}

	const auto &keyword = node->As<WKTKeyword>();

	// Check that this is a valid CRS keyword
	auto found = false;

	const auto crs_keywords = {
	    // WKT2019 keywords
	    "BOUNDCRS",     "COMPOUNDCRS", "ENGCRS",        "ENGINEERINGCRS", "GEODCRS",
	    "GEODETICCRS",  "GEOGCRS",     "GEOGRAPHICCRS", "PARAMETRICCRS",  "PROJCRS",
	    "PROJECTEDCRS", "TIMECRS",     "VERTCRS",       "VERTICALCRS"
	    // Older WKT1 keywords (we dont support these for now)
	    // "COMPDCS",
	    // "FITTED_CS",
	    // "GEOCCS",
	    // "GEOGCS",
	    // "LOCAL_CS",
	    // "PROJCS",
	    // "VERT_CS",
	};

	// Check if we match any of the CRS keywords
	for (auto &kw : crs_keywords) {
		if (keyword.Match(kw)) {
			found = true;
			break;
		}
	}

	if (!found) {
		// Not a valid CRS keyword
		return false;
	}

	auto &children = keyword.GetChildren();
	if (children.empty()) {
		// Needs a name
		return false;
	}

	// Special case for BOUNDCRS, use the name of the transform instead
	if (keyword.Match("BOUNDCRS")) {
		if (children.size() < 3) {
			// Needs at least source CRS, target CRS and transform
			return false;
		}

		const auto &transform_node = children[2];
		if (!transform_node->IsKeyword()) {
			// Transform needs to be a keyword
			return false;
		}

		const auto &transform_keyword = transform_node->As<WKTKeyword>();
		auto &transform_children = transform_keyword.GetChildren();
		if (transform_children.empty()) {
			// Transform needs a name
			return false;
		}

		const auto &first_transform_child = transform_children[0];
		if (!first_transform_child->IsString()) {
			// First child of transform needs to be a string (the name)
			return false;
		}

		// TODO: Parse "ID" subnode to get the code
		auto name = TryExtractIDFromWKTNode(transform_keyword);
		if (name.empty()) {
			// Pick name as fallback instead of code
			name = first_transform_child->As<WKTString>().GetValue();
		}

		result.type = CoordinateReferenceSystemType::WKT2_2019;
		result.identifier = name;
		result.definition = text;

		// Also trim text
		// TODO: Normalize WKT Input
		StringUtil::Trim(result.definition);

		return true;
	}

	// Otherwise, the name is the first child
	const auto &first = children[0];
	if (!first->IsString()) {
		// Pick name as fallback instead of code
		return false;
	}

	auto name = TryExtractIDFromWKTNode(keyword);
	if (name.empty()) {
		// Pick name as fallback
		name = first->As<WKTString>().GetValue();
	}

	result.type = CoordinateReferenceSystemType::WKT2_2019;
	result.identifier = name;
	result.definition = text;

	// Also trim text
	// TODO: Normalize WKT Input
	StringUtil::Trim(result.definition);

	return true;
}

//----------------------------------------------------------------------------------------------------------------------
// PROJJSON Parsing
//----------------------------------------------------------------------------------------------------------------------
bool CoordinateReferenceSystem::TryParsePROJJSON(const string &text, CoordinateReferenceSystem &result) {
	JSONParseError error;
	auto doc = JSONDocument::TryParse(text.c_str(), text.size(), error);
	if (!doc) {
		// Not a valid JSON
		return false;
	}

	auto root = doc->GetRoot();
	if (!root.IsObject()) {
		// The root is not an object
		return false;
	}

	// Get the "type" field from the root object
	auto type_val = root.GetMember("type");
	if (!type_val.IsString()) {
		return false;
	}

	// Check that the type is one of the PROJJSON CRS types
	// There are other (derived CRS) types, but they can not be used as root CRS definitions
	const string type_str = type_val.GetString();
	const auto projjson_crs_types = {"GeographicCRS", "GeodeticCRS",    "ProjectedCRS", "CompoundCRS",  "BoundCRS",
	                                 "VerticalCRS",   "EngineeringCRS", "TemporalCRS",  "ParametricCRS"};

	auto found = false;
	for (auto &kw : projjson_crs_types) {
		if (StringUtil::CIEquals(type_str, kw)) {
			found = true;
			break;
		}
	}

	if (!found) {
		return false;
	}

	// Start out with the root object
	auto target_val = root;

	// Special case for BoundCRS, use the name of the transformation instead
	if (StringUtil::CIEquals(type_str, "BoundCRS")) {
		auto trans_val = root.GetMember("transformation");
		if (!trans_val.IsObject()) {
			return false;
		}

		// Switch to the transformation object
		target_val = trans_val;
	}

	// Try to get the "name" field from the target object
	auto name_val = target_val.GetMember("name");
	if (name_val.IsString()) {
		result.identifier = name_val.GetString();
	}

	// Try to get the "id" field from the target object
	auto id_val = target_val.GetMember("id");
	if (id_val.IsObject()) {
		auto auth_val = id_val.GetMember("authority");
		if (auth_val.IsString()) {
			result.identifier = auth_val.GetString();

			auto code_val = id_val.GetMember("code");
			if (code_val.IsInteger()) {
				result.identifier += ":" + StringUtil::Format("%d", code_val.GetSignedInteger());
			}
			if (code_val.IsString()) {
				result.identifier += ":" + code_val.GetString();
			}
		}
	}

	result.type = CoordinateReferenceSystemType::PROJJSON;

	// Print the PROJJSON back to a string to normalize it
	// TODO: We should actually normalize the PROJJSON here (e.g. sort fields) to ensure consistent equality checks
	result.definition = doc->ToString(JSONWriteFlags::NONE);

	return true;
}

//----------------------------------------------------------------------------------------------------------------------
// AUTH:CODE Parsing
//----------------------------------------------------------------------------------------------------------------------
bool CoordinateReferenceSystem::TryParseAuthCode(const string &text, CoordinateReferenceSystem &result) {
	auto beg = text.c_str();
	auto end = beg + text.size();

	// Remove whitespace
	while (beg != end) {
		if (!StringUtil::CharacterIsSpace(*beg)) {
			break;
		}
		beg++;
	}
	while (end != beg) {
		if (!StringUtil::CharacterIsSpace(*(end - 1))) {
			break;
		}
		end--;
	}

	for (auto colon_pos = beg; colon_pos != end; colon_pos++) {
		if (*colon_pos == ':') {
			bool auth_valid = true;
			for (auto ptr = beg; ptr != colon_pos; ptr++) {
				if (!StringUtil::CharacterIsAlpha(*ptr)) {
					auth_valid = false;
					break;
				}
			}

			bool code_valid = true;
			for (auto ptr = colon_pos + 1; ptr != end; ptr++) {
				if (!StringUtil::CharacterIsAlphaNumeric(*ptr)) {
					code_valid = false;
					break;
				}
			}

			if (auth_valid && code_valid) {
				// Valid AUTH:CODE
				result.type = CoordinateReferenceSystemType::AUTH_CODE;
				result.definition = string(beg, UnsafeNumericCast<size_t>(end - beg));
				return true;
			}
			break;
		}
	}
	return false;
}

//----------------------------------------------------------------------------------------------------------------------
// Coordinate Reference System Parsing
//----------------------------------------------------------------------------------------------------------------------
void CoordinateReferenceSystem::ParseDefinition(const string &definition, CoordinateReferenceSystem &result) {
	if (definition.empty()) {
		result.type = CoordinateReferenceSystemType::INVALID;
		return;
	}

	// Check if the text is all whitespace
	auto all_space = true;
	for (const auto c : definition) {
		if (!StringUtil::CharacterIsSpace(c)) {
			all_space = false;
			break;
		}
	}

	if (all_space) {
		result.type = CoordinateReferenceSystemType::INVALID;
		return;
	}

	if (TryParsePROJJSON(definition, result)) {
		return;
	}

	if (TryParseAuthCode(definition, result)) {
		return;
	}

	// TODO: Also strip formatting
	if (TryParseWKT2(definition, result)) {
		return;
	}

	// Otherwise, treat this as an opaque identifier, and don't set an explicit id
	result.type = CoordinateReferenceSystemType::SRID;
	result.definition = definition;
}

void CoordinateReferenceSystem::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<string>(100, "definition", definition, string());
}

CoordinateReferenceSystem CoordinateReferenceSystem::Deserialize(Deserializer &deserializer) {
	string definition;
	deserializer.ReadPropertyWithExplicitDefault<string>(100, "definition", definition, string());

	// If this fails for whatever reason, just return an invalid CRS
	CoordinateReferenceSystem result;
	try {
		ParseDefinition(definition, result);
	} catch (...) {
		result.definition = "";
		result.type = CoordinateReferenceSystemType::INVALID;
	}
	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// Coordinate Reference System Manager
//----------------------------------------------------------------------------------------------------------------------

unique_ptr<CoordinateReferenceSystem> CoordinateReferenceSystem::TryConvert(ClientContext &context,
                                                                            const CoordinateReferenceSystem &source_crs,
                                                                            CoordinateReferenceSystemType target_type) {
	if (source_crs.type == target_type && source_crs.IsComplete()) {
		// No conversion needed
		return make_uniq<CoordinateReferenceSystem>(source_crs);
	}

	auto &catalog = Catalog::GetSystemCatalog(context);
	auto entry = catalog.GetEntry(context, CatalogType::COORDINATE_SYSTEM_ENTRY, Identifier::DefaultSchema(),
	                              Identifier(source_crs.GetIdentifier()), OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		return nullptr;
	}

	auto &crs_entry = entry->Cast<CoordinateSystemCatalogEntry>();

	switch (target_type) {
	case CoordinateReferenceSystemType::AUTH_CODE: {
		if (crs_entry.authority.empty() || crs_entry.code.empty()) {
			return nullptr;
		}
		return make_uniq<CoordinateReferenceSystem>(crs_entry.authority + ":" + crs_entry.code);
	}
	case CoordinateReferenceSystemType::SRID: {
		return make_uniq<CoordinateReferenceSystem>(crs_entry.name.GetIdentifierName());
	}
	case CoordinateReferenceSystemType::PROJJSON: {
		if (crs_entry.projjson_definition.empty()) {
			return nullptr;
		}
		return make_uniq<CoordinateReferenceSystem>(crs_entry.projjson_definition);
	}
	case CoordinateReferenceSystemType::WKT2_2019: {
		if (crs_entry.wkt2_2019_definition.empty()) {
			return nullptr;
		}
		return make_uniq<CoordinateReferenceSystem>(crs_entry.wkt2_2019_definition);
	}
	default:
		break;
	}

	return nullptr;
}

unique_ptr<CoordinateReferenceSystem> CoordinateReferenceSystem::TryConvert(ClientContext &context,
                                                                            const string &source_crs,
                                                                            CoordinateReferenceSystemType target_type) {
	const CoordinateReferenceSystem source(source_crs);
	return TryConvert(context, source, target_type);
}

unique_ptr<CoordinateReferenceSystem> CoordinateReferenceSystem::TryIdentify(ClientContext &context,
                                                                             const string &source_crs) {
	CoordinateReferenceSystem source(source_crs);

	// We couldnt even parse the CRS
	if (source.GetType() == CoordinateReferenceSystemType::INVALID) {
		return nullptr;
	}

	// We always want to identify the CRS as short as possible, so first check for AUTH:CODE
	auto auth_crs = TryConvert(context, source, CoordinateReferenceSystemType::AUTH_CODE);
	if (auth_crs) {
		return auth_crs;
	}

	// Next, check for SRID
	auto srid_crs = TryConvert(context, source, CoordinateReferenceSystemType::SRID);
	if (srid_crs) {
		return srid_crs;
	}

	// Otherwise, PROJJSON
	auto projjson_crs = TryConvert(context, source, CoordinateReferenceSystemType::PROJJSON);
	if (projjson_crs) {
		return projjson_crs;
	}

	// Finally, WKT2:2019
	auto wkt2_crs = TryConvert(context, source, CoordinateReferenceSystemType::WKT2_2019);
	if (wkt2_crs) {
		return wkt2_crs;
	}

	if (!source.IsComplete()) {
		return nullptr;
	}

	return make_uniq<CoordinateReferenceSystem>(std::move(source));
}

} // namespace duckdb
