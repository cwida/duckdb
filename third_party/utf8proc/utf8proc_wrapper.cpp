#include "utf8proc_wrapper.hpp"
#include "utf8proc.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

// This function efficiently checks if a string is valid UTF8.
// It was originally written by Sjoerd Mullender.

// Here is the table that makes it work:

// B 		= Number of Bytes in UTF8 encoding
// C_MIN 	= First Unicode code point
// C_MAX 	= Last Unicode code point
// B1 		= First Byte Prefix

// 	B	C_MIN		C_MAX		B1
//	1	U+000000	U+00007F		0xxxxxxx
//	2	U+000080	U+0007FF		110xxxxx
//	3	U+000800	U+00FFFF		1110xxxx
//	4	U+010000	U+10FFFF		11110xxx

static void AssignInvalidUTF8Reason(UnicodeInvalidReason *invalid_reason, size_t *invalid_pos, size_t pos,
                                    UnicodeInvalidReason reason) {
	if (invalid_reason) {
		*invalid_reason = reason;
	}
	if (invalid_pos) {
		*invalid_pos = pos;
	}
}

template <const int nextra_bytes, const int mask>
static inline UnicodeType UTF8ExtraByteLoop(const int first_pos_seq, int utf8char, size_t &i, const char *s,
                                            const size_t len, UnicodeInvalidReason *invalid_reason,
                                            size_t *invalid_pos) {
	if ((len - i) < (nextra_bytes + 1)) {
		/* incomplete byte sequence */
		AssignInvalidUTF8Reason(invalid_reason, invalid_pos, first_pos_seq, UnicodeInvalidReason::BYTE_MISMATCH);
		return UnicodeType::INVALID;
	}
	for (size_t j = 0; j < nextra_bytes; j++) {
		int c = (int)s[++i];
		/* now validate the extra bytes */
		if ((c & 0xC0) != 0x80) {
			/* extra byte is not in the format 10xxxxxx */
			AssignInvalidUTF8Reason(invalid_reason, invalid_pos, i, UnicodeInvalidReason::BYTE_MISMATCH);
			return UnicodeType::INVALID;
		}
		utf8char = (utf8char << 6) | (c & 0x3F);
	}
	if ((utf8char & mask) == 0) {
		/* invalid UTF-8 codepoint, not shortest possible */
		AssignInvalidUTF8Reason(invalid_reason, invalid_pos, first_pos_seq, UnicodeInvalidReason::INVALID_UNICODE);
		return UnicodeType::INVALID;
	}
	if (utf8char > 0x10FFFF) {
		/* value not representable by Unicode */
		AssignInvalidUTF8Reason(invalid_reason, invalid_pos, first_pos_seq, UnicodeInvalidReason::INVALID_UNICODE);
		return UnicodeType::INVALID;
	}
	if ((utf8char & 0x1FFF800) == 0xD800) {
		/* Unicode characters from U+D800 to U+DFFF are surrogate characters used by UTF-16 which are invalid in UTF-8
		 */
		AssignInvalidUTF8Reason(invalid_reason, invalid_pos, first_pos_seq, UnicodeInvalidReason::INVALID_UNICODE);
		return UnicodeType::INVALID;
	}
	return UnicodeType::UTF8;
}

UnicodeType Utf8Proc::Analyze(const char *s, size_t len, UnicodeInvalidReason *invalid_reason, size_t *invalid_pos) {
	UnicodeType type = UnicodeType::ASCII;

	static constexpr uint64_t MASK = 0x8080808080808080U;
	size_t i = 0;
	if (len >= 64) {
		// long input: check 32 bytes at a time with a single branch while everything is ASCII
		for (; i + 4 * sizeof(uint64_t) <= len; i += 4 * sizeof(uint64_t)) {
			auto acc = Load<uint64_t>(const_data_ptr_cast(s + i)) | Load<uint64_t>(const_data_ptr_cast(s + i + 8)) |
			           Load<uint64_t>(const_data_ptr_cast(s + i + 16)) | Load<uint64_t>(const_data_ptr_cast(s + i + 24));
			if (acc & MASK) {
				break;
			}
		}
	}
	for (; i < len;) {
		// Check 8 bytes at a time until we hit non-ASCII
		for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
			if (Load<uint64_t>(const_data_ptr_cast(s + i)) & MASK) {
				break; // Non-ASCII in the next 8 bytes
			}
		}
		// Check 1 byte at a time for the next 8 bytes
		const auto end = MinValue(i + sizeof(uint64_t), len);
		for (; i < end; i++) {
			int c = (int)s[i];
			if ((c & 0x80) == 0) {
				continue;
			}
			int first_pos_seq = i;

			if ((c & 0xE0) == 0xC0) {
				/* 2 byte sequence */
				int utf8char = c & 0x1F;
				type = UTF8ExtraByteLoop<1, 0x000780>(first_pos_seq, utf8char, i, s, len, invalid_reason, invalid_pos);
			} else if ((c & 0xF0) == 0xE0) {
				/* 3 byte sequence */
				int utf8char = c & 0x0F;
				type = UTF8ExtraByteLoop<2, 0x00F800>(first_pos_seq, utf8char, i, s, len, invalid_reason, invalid_pos);
			} else if ((c & 0xF8) == 0xF0) {
				/* 4 byte sequence */
				int utf8char = c & 0x07;
				type = UTF8ExtraByteLoop<3, 0x1F0000>(first_pos_seq, utf8char, i, s, len, invalid_reason, invalid_pos);
			} else {
				/* invalid UTF-8 start byte */
				AssignInvalidUTF8Reason(invalid_reason, invalid_pos, i, UnicodeInvalidReason::BYTE_MISMATCH);
				return UnicodeType::INVALID;
			}
			if (type == UnicodeType::INVALID) {
				return type;
			}
		}
	}
	return type;
}

void Utf8Proc::MakeValid(char *s, size_t len, char special_flag) {
	D_ASSERT(special_flag <= 127);
	UnicodeType type = UnicodeType::ASCII;
	for (size_t i = 0; i < len; i++) {
		int c = (int)s[i];
		if ((c & 0x80) == 0) {
			continue;
		}
		int first_pos_seq = i;
		if ((c & 0xE0) == 0xC0) {
			/* 2 byte sequence */
			int utf8char = c & 0x1F;
			type = UTF8ExtraByteLoop<1, 0x000780>(first_pos_seq, utf8char, i, s, len, nullptr, nullptr);
		} else if ((c & 0xF0) == 0xE0) {
			/* 3 byte sequence */
			int utf8char = c & 0x0F;
			type = UTF8ExtraByteLoop<2, 0x00F800>(first_pos_seq, utf8char, i, s, len, nullptr, nullptr);
		} else if ((c & 0xF8) == 0xF0) {
			/* 4 byte sequence */
			int utf8char = c & 0x07;
			type = UTF8ExtraByteLoop<3, 0x1F0000>(first_pos_seq, utf8char, i, s, len, nullptr, nullptr);
		} else {
			/* invalid UTF-8 start byte */
			s[i] = special_flag; // Rewrite invalid byte
		}
		if (type == UnicodeType::INVALID) {
			for (size_t j = first_pos_seq; j <= i; j++) {
				s[j] = special_flag; // Rewrite each byte of the invalid sequence
			}
			type = UnicodeType::ASCII;
		}
	}
	D_ASSERT(Utf8Proc::IsValid(s, len));
}

char *Utf8Proc::Normalize(const char *s, size_t len) {
	assert(s);
	assert(Utf8Proc::Analyze(s, len) != UnicodeType::INVALID);
	return (char *)utf8proc_NFC((const utf8proc_uint8_t *)s, len);
}

bool Utf8Proc::IsValid(const char *s, size_t len) {
	return Utf8Proc::Analyze(s, len) != UnicodeType::INVALID;
}

std::string Utf8Proc::RemoveInvalid(const char *s, size_t len) {
	std::string result;
	result.reserve(len); // Reserve the maximum possible size

	for (size_t i = 0; i < len; i++) {
		int c = (int)s[i];
		if ((c & 0x80) == 0) {
			// ASCII character - always valid
			result.push_back(s[i]);
			continue;
		}

		int first_pos_seq = i;
		UnicodeType type = UnicodeType::ASCII;
		UnicodeInvalidReason invalid_reason = UnicodeInvalidReason::BYTE_MISMATCH;
		size_t invalid_pos = first_pos_seq;

		if ((c & 0xE0) == 0xC0) {
			/* 2 byte sequence */
			int utf8char = c & 0x1F;
			type = UTF8ExtraByteLoop<1, 0x000780>(first_pos_seq, utf8char, i, s, len, &invalid_reason, &invalid_pos);
		} else if ((c & 0xF0) == 0xE0) {
			/* 3 byte sequence */
			int utf8char = c & 0x0F;
			type = UTF8ExtraByteLoop<2, 0x00F800>(first_pos_seq, utf8char, i, s, len, &invalid_reason, &invalid_pos);
		} else if ((c & 0xF8) == 0xF0) {
			/* 4 byte sequence */
			int utf8char = c & 0x07;
			type = UTF8ExtraByteLoop<3, 0x1F0000>(first_pos_seq, utf8char, i, s, len, &invalid_reason, &invalid_pos);
		} else {
			// invalid, do not write to output
			continue;
		}

		if (type == UnicodeType::INVALID) {
			// A bad continuation byte (not a truncated tail, not a bad codepoint after a fully well-formed sequence) 
			// means the byte at invalid_pos was never actually consumed as part of this sequence 
			// rewind so it gets reprocessed on its own next iteration
			if (invalid_reason == UnicodeInvalidReason::BYTE_MISMATCH && i > first_pos_seq) {
				i = invalid_pos - 1;
			}
			continue;
		}

		// If we get here, the sequence is valid, so add all bytes of the sequence to result
		for (size_t j = first_pos_seq; j <= i; j++) {
			result.push_back(s[j]);
		}
	}

	D_ASSERT(Utf8Proc::IsValid(result.c_str(), result.size()));
	return result;
}

size_t Utf8Proc::NextGraphemeCluster(const char *s, size_t len, size_t cpos) {
	int sz;
	auto prev_codepoint = Utf8Proc::UTF8ToCodepoint(s + cpos, sz, len - cpos);
	utf8proc_int32_t state = 0;
	while (true) {
		cpos += sz;
		if (cpos >= len) {
			return cpos;
		}
		auto next_codepoint = Utf8Proc::UTF8ToCodepoint(s + cpos, sz, len - cpos);
		if (utf8proc_grapheme_break_stateful(prev_codepoint, next_codepoint, &state)) {
			// found a grapheme break here
			return cpos;
		}
		// not a grapheme break, move on to next codepoint
		prev_codepoint = next_codepoint;
	}
}

size_t Utf8Proc::GraphemeCount(const char *input_data, size_t input_size) {
	size_t num_characters = 0;
	for (auto cluster : Utf8Proc::GraphemeClusters(input_data, input_size)) {
		(void)cluster;
		num_characters++;
	}
	return num_characters;
}

int32_t Utf8Proc::CodepointToUpper(int32_t codepoint) {
	return utf8proc_toupper(codepoint);
}

int32_t Utf8Proc::CodepointToLower(int32_t codepoint) {
	return utf8proc_tolower(codepoint);
}

GraphemeIterator::GraphemeIterator(const char *s, size_t len) : s(s), len(len) {
}

GraphemeIterator Utf8Proc::GraphemeClusters(const char *s, size_t len) {
	return GraphemeIterator(s, len);
}

GraphemeIterator::GraphemeClusterIterator::GraphemeClusterIterator(const char *s_p, size_t len_p) : s(s_p), len(len_p) {
	if (s) {
		cluster.start = 0;
		cluster.end = 0;
		Next();
	} else {
		SetInvalid();
	}
}

void GraphemeIterator::GraphemeClusterIterator::SetInvalid() {
	s = nullptr;
	len = 0;
	cluster.start = 0;
	cluster.end = 0;
}

bool GraphemeIterator::GraphemeClusterIterator::IsInvalid() const {
	return !s;
}

void GraphemeIterator::GraphemeClusterIterator::Next() {
	if (IsInvalid()) {
		throw std::runtime_error("Grapheme cluster out of bounds!");
	}
	if (cluster.end >= len) {
		// out of bounds
		SetInvalid();
		return;
	}
	size_t next_pos = Utf8Proc::NextGraphemeCluster(s, len, cluster.end);
	cluster.start = cluster.end;
	cluster.end = next_pos;
}

GraphemeIterator::GraphemeClusterIterator &GraphemeIterator::GraphemeClusterIterator::operator++() {
	Next();
	return *this;
}
bool GraphemeIterator::GraphemeClusterIterator::operator!=(const GraphemeClusterIterator &other) const {
	return !(len == other.len && s == other.s && cluster.start == other.cluster.start &&
	         cluster.end == other.cluster.end);
}

GraphemeCluster GraphemeIterator::GraphemeClusterIterator::operator*() const {
	if (IsInvalid()) {
		throw std::runtime_error("Grapheme cluster out of bounds!");
	}
	return cluster;
}

size_t Utf8Proc::PreviousGraphemeCluster(const char *s, size_t len, size_t cpos) {
	if (!Utf8Proc::IsValid(s, len)) {
		return cpos - 1;
	}
	size_t current_pos = 0;
	while (true) {
		size_t new_pos = NextGraphemeCluster(s, len, current_pos);
		if (new_pos <= current_pos || new_pos >= cpos) {
			return current_pos;
		}
		current_pos = new_pos;
	}
}

bool Utf8Proc::CodepointToUtf8(int cp, int &sz, char *c) {
	if (cp <= 0x7F) {
		sz = 1;
		c[0] = cp;
	} else if (cp <= 0x7FF) {
		sz = 2;
		c[0] = (cp >> 6) + 192;
		c[1] = (cp & 63) + 128;
	} else if (0xd800 <= cp && cp <= 0xdfff) {
		sz = -1;
		// invalid block of utf
		return false;
	} else if (cp <= 0xFFFF) {
		sz = 3;
		c[0] = (cp >> 12) + 224;
		c[1] = ((cp >> 6) & 63) + 128;
		c[2] = (cp & 63) + 128;
	} else if (cp <= 0x10FFFF) {
		sz = 4;
		c[0] = (cp >> 18) + 240;
		c[1] = ((cp >> 12) & 63) + 128;
		c[2] = ((cp >> 6) & 63) + 128;
		c[3] = (cp & 63) + 128;
	} else {
		sz = -1;
		return false;
	}
	return true;
}

int Utf8Proc::CodepointLength(int cp) {
	if (cp <= 0x7F) {
		return 1;
	}
	 if (cp <= 0x7FF) {
		return 2;
	}
	 if (0xd800 <= cp && cp <= 0xdfff) {
	 	throw InternalException("invalid code point detected in Utf8Proc::CodepointLength (0xd800 to 0xdfff), likely due to invalid UTF-8");
	}
	 if (cp <= 0xFFFF) {
		return 3;
	}
	 if (cp <= 0x10FFFF) {
		return 4;
	}
	throw InternalException("invalid code point detected in Utf8Proc::CodepointLength, likely due to invalid UTF-8");
}

int32_t Utf8Proc::UTF8ToCodepoint(const char *u_input, int &sz, const size_t length) {
	// from http://www.zedwood.com/article/cpp-utf8-char-to-codepoint
	auto u = reinterpret_cast<const unsigned char *>(u_input);
	unsigned char u0 = u[0];
	if (u0 <= 127) {
		sz = 1;
		return u0;
	}
	// work out the sequence length from the lead byte and make sure the continuation bytes are present before
	// reading them, otherwise a truncated trailing sequence reads past the end of the buffer
	size_t needed;
	if (u0 >= 240 && u0 <= 247) {
		needed = 4;
	} else if (u0 >= 224 && u0 <= 239) {
		needed = 3;
	} else if (u0 >= 192 && u0 <= 223) {
		needed = 2;
	} else {
		throw InvalidInputException("invalid lead byte detected in Utf8Proc::UTF8ToCodepoint, likely due to invalid UTF-8");
	}
	if (length < needed) {
		throw InvalidInputException("truncated UTF-8 sequence detected in Utf8Proc::UTF8ToCodepoint");
	}
	unsigned char u1 = u[1];
	if (needed == 2) {
		sz = 2;
		return (u0 - 192) * 64 + (u1 - 128);
	}
	if (u0 == 0xed && (u1 & 0xa0) == 0xa0) {
		throw InvalidInputException("invalid code point detected in Utf8Proc::UTF8ToCodepoint (0xd800 to 0xdfff), likely due to invalid UTF-8");
	}
	unsigned char u2 = u[2];
	if (needed == 3) {
		sz = 3;
		return (u0 - 224) * 4096 + (u1 - 128) * 64 + (u2 - 128);
	}
	unsigned char u3 = u[3];
	sz = 4;
	return (u0 - 240) * 262144 + (u1 - 128) * 4096 + (u2 - 128) * 64 + (u3 - 128);
}

size_t Utf8Proc::RenderWidth(const char *s, size_t len, size_t pos) {
	int sz;
	auto codepoint = Utf8Proc::UTF8ToCodepoint(s + pos, sz, len - pos);
	auto properties = duckdb::utf8proc_get_property(codepoint);
	return properties->charwidth;
}

size_t Utf8Proc::RenderWidth(const std::string &str) {
	size_t render_width = 0;
	for (auto cluster : Utf8Proc::GraphemeClusters(str.c_str(), str.size())) {
		// use the width of the first codepoint in the grapheme cluster
		// combining marks, ZWJ, variation selectors, etc. have charwidth 0
		// and multi-codepoint clusters (e.g. ZWJ emoji sequences) should only
		// count the base character's width, not the sum of all codepoints
		render_width += Utf8Proc::RenderWidth(str.c_str(), str.size(), cluster.start);
	}
	return render_width;
}

bool Utf8Proc::ValidUpperBound(const string &str, string &result) {
	if (str.empty()) {
		return false;
	}
	auto *data = reinterpret_cast<const uint8_t *>(str.data());
	idx_t pos = str.size();

	// Find the start of the rightmost (possibly incomplete) sequence.
	idx_t start = pos - 1;
	while (start > 0 && (data[start] & 0xC0) == 0x80) {
		start--;
	}

	uint8_t lead = data[start];
	int total_len;
	if ((lead & 0x80) == 0) {
		total_len = 1;
	} else if ((lead & 0xE0) == 0xC0) {
		total_len = 2;
	} else if ((lead & 0xF0) == 0xE0) {
		total_len = 3;
	} else if ((lead & 0xF8) == 0xF0) {
		total_len = 4;
	} else {
		// Invalid leading byte; strip it and let FindNextLegalUTF8 backtrack.
		result = string(str.data(), start);
		return FindNextLegalUTF8(result);
	}

	// Fill any missing continuation bytes with 0xBF so the sequence is complete,
	// then delegate the increment (and any backtracking) to FindNextLegalUTF8.
	int present = static_cast<int>(pos - start);
	result = str.substr(0, pos);
	for (int i = present; i < total_len; i++) {
		result += '\xBF';
	}
	return FindNextLegalUTF8(result);
}

bool Utf8Proc::ValidLowerBound(const string &str, string &result) {
	size_t invalid_pos;
	if (Analyze(str.c_str(), str.size(), nullptr, &invalid_pos) != UnicodeType::INVALID) {
		result = str;
		return true;
	}
	// Everything before invalid_pos is valid UTF-8.  A NUL byte is always less
	// than any byte a valid continuation or leading byte could produce (>= 0x80).
	result = str.substr(0, invalid_pos) + '\0';
	return true;
}

bool Utf8Proc::FindNextLegalUTF8(string &str) {
	while (!str.empty()) {
		// Find the start of the last codepoint.
		idx_t last_codepoint_start;
		for (last_codepoint_start = str.size(); last_codepoint_start > 0; last_codepoint_start--) {
			if (IsCharacter(str[last_codepoint_start - 1])) {
				break;
			}
		}
		if (last_codepoint_start == 0) {
			throw InvalidInputException("Invalid UTF8 found in string \"%s\"", str);
		}
		last_codepoint_start--;
		// Surrogates (U+D800–U+DFFF, encoded as ED A0 80–ED BF BF) cannot be decoded
		// with UTF8ToCodepoint, which throws for them.  This can happen when ValidUpperBound
		// fills a truncated ED-prefixed sequence with 0xBF bytes.  Jump straight to U+E000.
		auto *seq = reinterpret_cast<const uint8_t *>(str.c_str() + last_codepoint_start);
		if (seq[0] == 0xED && (seq[1] & 0xE0) == 0xA0) {
			char cp_bytes[4];
			int cp_size;
			Utf8Proc::CodepointToUtf8(0xE000, cp_size, cp_bytes);
			str = str.substr(0, last_codepoint_start) + string(cp_bytes, static_cast<idx_t>(cp_size));
			return true;
		}
		int codepoint_size;
		auto codepoint =
		    Utf8Proc::UTF8ToCodepoint(str.c_str() + last_codepoint_start, codepoint_size, str.size() - last_codepoint_start) +
		    1;
		if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
			// incremented codepoint falls within surrogate range; skip to next valid character
			codepoint = 0xE000;
		}
		char next_codepoint_text[4];
		int next_codepoint_size;
		if (Utf8Proc::CodepointToUtf8(codepoint, next_codepoint_size, next_codepoint_text)) {
			auto s = static_cast<idx_t>(next_codepoint_size);
			str = str.substr(0, last_codepoint_start) + string(next_codepoint_text, s);
			return true;
		}
		// Last codepoint was U+10FFFF; pop it and try the preceding one.
		str = str.substr(0, last_codepoint_start);
	}
	return false;
}

} // namespace duckdb
