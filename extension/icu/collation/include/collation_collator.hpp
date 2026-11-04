//===----------------------------------------------------------------------===//
//                         DuckDB
//
// collation_collator.hpp
//
// Implementation of the Unicode Collation Algorithm over the Unicode Consortium
// collation data, generating sort keys that are compatible with ICU.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
namespace collation {

enum class CollationStrength : uint8_t { PRIMARY = 0, SECONDARY = 1, TERTIARY = 2, QUATERNARY = 3, IDENTICAL = 4 };

enum class CaseFirst : uint8_t { DEFAULT = 0, LOWER_FIRST = 1, UPPER_FIRST = 2 };

struct CollationSettings {
	CollationStrength strength = CollationStrength::TERTIARY;
	//! Whether to add a separate case level after the secondary level
	bool case_level = false;
	CaseFirst case_first = CaseFirst::DEFAULT;
	//! Whether variable (punctuation, symbol, ...) weights are shifted to the quaternary level
	bool alternate_shifted = false;
	//! Whether secondary weights are compared backwards, as in Canadian French
	bool backward_secondary = false;
	//! Whether text that is not in canonical order is normalized before it is collated
	bool normalization = false;
};

struct CollationTailoring;

//! An index of the code points a locale tailors: a bitmap that rejects the characters it
//! does not tailor, and the range of the tailoring each block of code points uses
struct TailoredBlocks {
	static constexpr uint32_t BITMAP_SHIFT = 6;
	static constexpr uint32_t BITMAP_COUNT = 0x110000 >> BITMAP_SHIFT;
	static constexpr uint32_t BOUND_SHIFT = 8;
	static constexpr uint32_t BOUND_COUNT = 0x110000 >> BOUND_SHIFT;

	//! Indexes the sorted code points of a tailoring
	void Build(const uint32_t *codepoints, uint32_t count);

	bool Contains(uint32_t codepoint) const {
		if (codepoint >= 0x110000) {
			return false;
		}
		auto block = codepoint >> BITMAP_SHIFT;
		return (bitmap[block / 64] & (1ULL << (block % 64))) != 0;
	}
	//! The part of the tailoring that can hold the code point
	void GetRange(uint32_t codepoint, uint32_t &lower, uint32_t &upper) const {
		if (codepoint >= 0x110000) {
			lower = 0;
			upper = 0;
			return;
		}
		lower = bounds[codepoint >> BOUND_SHIFT];
		upper = bounds[(codepoint >> BOUND_SHIFT) + 1];
	}

	uint64_t bitmap[BITMAP_COUNT / 64] = {};
	vector<uint32_t> bounds;
};

//! The buffers a collator works in, reused across the strings of a vector so that
//! generating sort keys does not allocate for every string
struct CollationBuffer {
	//! the code points of the string
	vector<uint32_t> text;
	//! the collation elements of the string
	vector<uint64_t> elements;
	//! the sort key, terminated by a null byte
	vector<uint8_t> key;
	//! the levels of the sort key that are written after the primary level
	vector<uint8_t> levels[4];
};

//! A collator is immutable once it is created, so that it can be shared between threads.
//! The buffers it works in are passed in by the caller.
class Collator {
public:
	explicit Collator(CollationSettings settings_p) : settings(settings_p) {
	}
	//! Creates a collator for a collation name such as "de" or "fr_ca", falling back to the
	//! root collation when the locale is unknown
	explicit Collator(const string &collation);

	//! Whether a collation with this name exists
	static bool HasCollation(const string &collation);
	//! The names of all collations, in sorted order
	static vector<string> GetCollations();

	//! Writes the sort key of the given UTF-8 string into buffer.key, terminated by a null byte
	void GetSortKey(const char *data, idx_t size, CollationBuffer &buffer) const;

	//! The settings of this collator
	const CollationSettings &GetSettings() const {
		return settings;
	}

private:
	//! Fills the table of ASCII characters that map to a single collation element
	void BuildFastPath();
	//! Writes the sort key of a string of simple characters, returns false when the string
	//! contains a character that needs the general path
	template <bool UPPER_FIRST>
	bool GetFastSortKey(const char *data, idx_t size, CollationBuffer &buffer) const;
	//! Collects the elements of a string of simple characters, for the settings that the
	//! fast path does not write sort keys for
	bool GetFastElements(const char *data, idx_t size, CollationBuffer &buffer) const;

private:
	CollationSettings settings;
	//! The tailoring of the collation, or nullptr when it uses the root collation
	const CollationTailoring *tailoring = nullptr;
	//! The characters below this limit are collated without decomposing anything, they
	//! cannot carry a combining class
	static constexpr uint32_t FAST_LIMIT = 0x300;
	//! The elements of every character below the limit, the first one is 0 when the
	//! character needs the general path and the second one is 0 when it has only one
	uint64_t fast_elements[FAST_LIMIT] = {};
	uint64_t fast_expansions[FAST_LIMIT] = {};
	//! The code point blocks the locale tailors
	TailoredBlocks tailored;
	//! The lead byte permutation of the locale, or nullptr when it does not reorder
	const uint8_t *reorder_table = nullptr;
	//! Whether the levels of the sort key are written the way the root collation writes them
	bool simple_settings = false;
};

} // namespace collation
} // namespace duckdb
