//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/json_document.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include <functional>

//! Forward declaration of the yyjson types - the actual definitions live in the yyjson headers, which are only
//! included in the implementation. This keeps yyjson an internal detail of the wrapper.
namespace duckdb_yyjson { // NOLINT
struct yyjson_doc;
struct yyjson_val;
struct yyjson_mut_doc;
struct yyjson_mut_val;
} // namespace duckdb_yyjson

namespace duckdb {

//! The type of a JSON value
//! NOTE: no explicit storage type so that EnumUtil ToString/FromString code is not generated for it
enum class JSONValueType {
	INVALID,          //!< Not a valid value (e.g. the result of a failed lookup)
	JSON_NULL,        //!< JSON null
	BOOLEAN,          //!< true / false
	UNSIGNED_INTEGER, //!< an integer that fits in a uint64_t
	SIGNED_INTEGER,   //!< an integer that fits in an int64_t
	DOUBLE,           //!< a floating point number
	STRING,           //!< a string
	ARRAY,            //!< a JSON array
	OBJECT,           //!< a JSON object
	RAW               //!< raw (unparsed) number, e.g. a bignum
};

//! Flags controlling how JSON is parsed
enum class JSONReadFlags {
	NONE = 0,
	ALLOW_INVALID_UNICODE = 1 << 0,
	ALLOW_INF_AND_NAN = 1 << 1,
	ALLOW_TRAILING_COMMAS = 1 << 2,
	BIGNUM_AS_RAW = 1 << 3
};

constexpr JSONReadFlags operator|(JSONReadFlags a, JSONReadFlags b) {
	return static_cast<JSONReadFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

//! Flags controlling how JSON is written
enum class JSONWriteFlags { NONE = 0, ALLOW_INVALID_UNICODE = 1 << 0, ALLOW_INF_AND_NAN = 1 << 1, PRETTY = 1 << 2 };

constexpr JSONWriteFlags operator|(JSONWriteFlags a, JSONWriteFlags b) {
	return static_cast<JSONWriteFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

//! A read-only handle to a single value inside a JSONDocument.
//! NOTE: a JSONValue is only valid for as long as the JSONDocument it originates from is alive.
class JSONValue {
	friend class JSONDocument;
	friend class JSONWriter;

public:
	//! Constructs an invalid value
	JSONValue();

	//! Whether or not this refers to an actual value
	bool IsValid() const;
	//! The type of this value
	JSONValueType GetType() const;

	bool IsNull() const;
	bool IsString() const;
	bool IsArray() const;
	bool IsObject() const;
	//! Whether or not this is an integer (signed or unsigned)
	bool IsInteger() const;
	//! Whether or not this is a number
	bool IsNumber() const;

	//! Get the value as a string (only valid if IsString())
	string GetString() const;
	//! Get the value as a boolean
	bool GetBoolean() const;
	//! Get the value as an unsigned integer
	uint64_t GetUnsignedInteger() const;
	//! Get the value as a signed integer
	int64_t GetSignedInteger() const;
	//! Get the value as a double
	double GetDouble() const;
	//! Get any number as a double
	double GetNumber() const;

	//! Look up a member of an object by key - returns an invalid value if this is not an object or the key is absent
	JSONValue GetMember(const string &key) const;

	//! Iterate over the elements of an array
	void IterateArray(const std::function<void(JSONValue)> &callback) const;
	//! Iterate over the key/value pairs of an object
	void IterateObject(const std::function<void(const string &, JSONValue)> &callback) const;

	//! Serialize this value to a string. Throws bad_alloc when memory runs out.
	string ToString(JSONWriteFlags flags = JSONWriteFlags::NONE) const;

private:
	explicit JSONValue(duckdb_yyjson::yyjson_val *val);

private:
	//! The wrapped yyjson value - owned by the originating JSONDocument
	duckdb_yyjson::yyjson_val *val;
};

//! Error information from parsing a JSON document
struct JSONParseError {
	//! Whether or not an error occurred
	bool HasError() const {
		return has_error;
	}

	bool has_error = false;
	//! Byte position of the error in the input
	idx_t position = 0;
	//! Human-readable error message
	string message;
};

//! Owns an immutable, parsed JSON document. Values obtained from it are valid for as long as the document is alive.
class JSONDocument {
public:
	JSONDocument();
	~JSONDocument();
	// non-copyable, movable
	JSONDocument(const JSONDocument &) = delete;
	JSONDocument &operator=(const JSONDocument &) = delete;
	JSONDocument(JSONDocument &&other) noexcept;
	JSONDocument &operator=(JSONDocument &&other) noexcept;

	//! Parse the given input. Throws an InvalidInputException on failure.
	static unique_ptr<JSONDocument> Parse(const char *data, idx_t len, JSONReadFlags flags = JSONReadFlags::NONE);
	//! Parse the given input. Returns nullptr on failure, in which case "error" is populated.
	//! Throws bad_alloc when memory runs out.
	static unique_ptr<JSONDocument> TryParse(const char *data, idx_t len, JSONParseError &error,
	                                         JSONReadFlags flags = JSONReadFlags::NONE);

	//! The root value of the document
	JSONValue GetRoot() const;

	//! Serialize the (immutable) document to a string
	string ToString(JSONWriteFlags flags = JSONWriteFlags::NONE) const;

private:
	//! The wrapped yyjson document
	duckdb_yyjson::yyjson_doc *doc;
};

//! A handle to a value being built inside a JSONWriter. The value is owned by the JSONWriter that created it - it must
//! not outlive that writer.
class JSONMutableValue {
	friend class JSONWriter;

public:
	//! Constructs an invalid value
	JSONMutableValue();

	bool IsValid() const;

	//! Add a key/value pair to this (object) value
	void Add(const string &key, JSONMutableValue value);
	//! Add a string key/value pair to this (object) value
	void AddString(const string &key, const string &value);

	//! Append a value to this (array) value
	void Append(JSONMutableValue value);
	//! Append a string value to this (array) value
	void AppendString(const string &value);

private:
	JSONMutableValue(duckdb_yyjson::yyjson_mut_doc *doc, duckdb_yyjson::yyjson_mut_val *val);

private:
	//! The document this value belongs to - not owned
	duckdb_yyjson::yyjson_mut_doc *doc;
	//! The wrapped mutable yyjson value - owned by "doc"
	duckdb_yyjson::yyjson_mut_val *val;
};

//! Builds a JSON document out of JSONMutableValues and serializes it to a string.
class JSONWriter {
public:
	JSONWriter();
	~JSONWriter();
	// non-copyable, movable
	JSONWriter(const JSONWriter &) = delete;
	JSONWriter &operator=(const JSONWriter &) = delete;
	JSONWriter(JSONWriter &&other) noexcept;
	JSONWriter &operator=(JSONWriter &&other) noexcept;

	//! Create values that belong to this document
	JSONMutableValue CreateObject();
	JSONMutableValue CreateArray();
	JSONMutableValue CreateString(const string &value);
	JSONMutableValue CreateNull();
	JSONMutableValue CreateBoolean(bool value);
	JSONMutableValue CreateUnsignedInteger(uint64_t value);
	JSONMutableValue CreateSignedInteger(int64_t value);
	JSONMutableValue CreateDouble(double value);
	//! Create a (deep) copy of an immutable value (e.g. from a parsed JSONDocument) belonging to this document
	JSONMutableValue CreateCopy(const JSONValue &value);

	//! Set the root value of the document
	void SetRoot(JSONMutableValue value);

	//! Serialize the document to a string
	string ToString(JSONWriteFlags flags = JSONWriteFlags::NONE) const;

private:
	//! The wrapped mutable yyjson document
	duckdb_yyjson::yyjson_mut_doc *doc;
};

} // namespace duckdb
