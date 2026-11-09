//===----------------------------------------------------------------------===//
//                         DuckDB
//
// utf8proc_wrapper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <cassert>
#include <cstring>
#include <cstdint>

namespace duckdb {
class GraphemeIterator;

enum class UnicodeType { INVALID, ASCII, UTF8 };
enum class UnicodeInvalidReason { BYTE_MISMATCH, INVALID_UNICODE };

class Utf8Proc {
public:
	//! Distinguishes ASCII, Valid UTF8 and Invalid UTF8 strings
	static UnicodeType Analyze(const char *s, size_t len, UnicodeInvalidReason *invalid_reason = nullptr, size_t *invalid_pos = nullptr);
	//! Performs UTF NFC normalization of string, return value needs to be free'd
	static char* Normalize(const char* s, size_t len);
	//! Returns whether or not the UTF8 string is valid
	static bool IsValid(const char *s, size_t len);
	//! Replaces each maximal invalid subpart in place with special_flag and returns the new, possibly shorter, length
	[[nodiscard]] static size_t MakeValid(char *s, size_t len, char special_flag = '?');
	//! Creates a new string with invalid UTF-8 characters removed
	static std::string RemoveInvalid(const char *s, size_t len);
	//! Returns the position (in bytes) of the next grapheme cluster
	static size_t NextGraphemeCluster(const char *s, size_t len, size_t pos);
	//! Returns the position (in bytes) of the previous grapheme cluster
	static size_t PreviousGraphemeCluster(const char *s, size_t len, size_t pos);

	//! Find the next legal-UTF8 string
	static bool FindNextLegalUTF8(std::string &str);

	//! Given a string that may end with a truncated (incomplete) UTF-8 sequence,
	//! compute the smallest valid UTF-8 string that is strictly greater than every
	//! string that shares the same byte prefix.  Returns false when no finite upper
	//! bound exists (e.g. the string is empty or consists entirely of U+10FFFF).
	static bool ValidUpperBound(const std::string &str, std::string &result);

	//! Given a string that may contain an invalid or truncated UTF-8 sequence,
	//! compute a valid UTF-8 lower bound by replacing the first invalid byte (and
	//! everything after it) with a NUL byte.
	static bool ValidLowerBound(const std::string &str, std::string &result);

	//! Transform a codepoint to utf8 and writes it to "c", sets "sz" to the size of the codepoint
	static bool CodepointToUtf8(int cp, int &sz, char *c);
	//! Returns the codepoint length in bytes when encoded in UTF8
	static int CodepointLength(int cp);
	//! Transform a UTF8 string to a codepoint; returns the codepoint and writes the length of the codepoint (in UTF8) to sz
	//! length is the number of bytes available at c, used to reject truncated multi-byte sequences
	static int32_t UTF8ToCodepoint(const char *c, int &sz, size_t length);
	//! Returns the render width of a single character in a string
	static size_t RenderWidth(const char *s, size_t len, size_t pos);
	static size_t RenderWidth(const std::string &str);

	static int32_t CodepointToUpper(int32_t codepoint);
	static int32_t CodepointToLower(int32_t codepoint);

	//! Constructs a class that can be iterated over to fetch grapheme clusters in a string
	static GraphemeIterator GraphemeClusters(const char *s, size_t len);

	//! Returns the number of grapheme clusters in a string
	static size_t GraphemeCount(const char *s, size_t len);


};

struct GraphemeCluster {
	size_t start;
	size_t end;
};

class GraphemeIterator {
public:
	GraphemeIterator(const char *s, size_t len);

private:
	const char *s;
	size_t len;

private:
	class GraphemeClusterIterator {
	public:
		GraphemeClusterIterator(const char *s, size_t len);

		const char *s;
		size_t len;
		GraphemeCluster cluster;

	public:
		void Next();
		void SetInvalid();
		bool IsInvalid() const;

		GraphemeClusterIterator &operator++();
		bool operator!=(const GraphemeClusterIterator &other) const;
		GraphemeCluster operator*() const;
	};

public:
	GraphemeClusterIterator begin() { // NOLINT: match stl API
		return GraphemeClusterIterator(s, len);
	}
	GraphemeClusterIterator end() { // NOLINT: match stl API
		return GraphemeClusterIterator(nullptr, 0);
	}
};


}
