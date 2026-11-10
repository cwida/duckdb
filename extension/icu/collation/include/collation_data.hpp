//===----------------------------------------------------------------------===//
//                         DuckDB
//
// collation_data.hpp
//
// Layout of the collation tables that are generated from the Unicode Consortium
// collation data by extension/icu/scripts/generate_collation_data.py.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace duckdb {
namespace collation {

//! Collation elements are stored as (primary << 32) | (secondary << 16) | tertiary,
//! where every weight is left-aligned in its field
typedef uint64_t collation_element_t;

//! Code points are mapped to a 32-bit trie value: the top two bits are the tag,
//! the remaining bits are an index into the array selected by the tag
enum CollationTag : uint32_t {
	COLLATION_TAG_NONE = 0,      // no mapping, the weight is computed
	COLLATION_TAG_SINGLE = 1,    // a single collation element
	COLLATION_TAG_EXPANSION = 2, // multiple collation elements
	COLLATION_TAG_CONTEXTS = 3   // the mapping depends on the surrounding characters
};

static constexpr uint32_t COLLATION_TAG_SHIFT = 30;
static constexpr uint32_t COLLATION_INDEX_MASK = (1U << COLLATION_TAG_SHIFT) - 1;

//! Two-stage trie: value = stage2[stage1[cp >> COLLATION_TRIE_SHIFT] + (cp & COLLATION_TRIE_MASK)]
static constexpr uint32_t COLLATION_TRIE_SHIFT = 6;
static constexpr uint32_t COLLATION_TRIE_MASK = (1U << COLLATION_TRIE_SHIFT) - 1;

//! The Han ranges are indexed by blocks of code points, the index of a block holds the
//! first and the last range that can contain a code point of that block
static constexpr uint32_t HAN_BLOCK_SHIFT = 8;

enum CollationContextType : uint32_t {
	COLLATION_CONTEXT_CONTRACTION = 0, // the context follows the character
	COLLATION_CONTEXT_PREFIX = 1       // the context precedes the character, stored in reverse order
};

struct CollationExpansion {
	uint32_t ce_offset;
	uint32_t ce_count;
};

struct CollationEntry {
	uint32_t ce_offset; // the elements to use when no context matches
	uint32_t ce_count;
	uint32_t context_offset;
	uint32_t context_count;
};

struct CollationContext {
	uint32_t chars_offset;
	uint32_t chars_length;
	uint32_t type;
	uint32_t ce_offset;
	uint32_t ce_count;
};

struct CollationTable {
	const uint32_t *trie_stage1;
	const uint32_t *trie_stage2;
	const collation_element_t *ces;
	const CollationExpansion *expansions;
	const CollationEntry *entries;
	const CollationContext *contexts;
	const uint32_t *context_chars;
};

//! A locale that tailors the root collation. Only the tailored code points are stored,
//! everything else falls back to the root table.
struct CollationTailoring {
	CollationTable table;
	const uint32_t *codepoints; // sorted
	const uint32_t *values;     // the trie value of each code point
	uint32_t count;
	//! Permutation of the primary lead bytes for [reorder ...], or nullptr
	const uint8_t *reorder_table;
};

//! The root collation, which every collation falls back to
struct CollationRoot {
	CollationTable table;
	//! Whether the primary weights of a lead byte are compressed in a sort key
	const uint8_t *compressible_lead_byte;
	//! Han characters are ordered by radical/stroke, the order is stored as runs of code
	//! points that are indexed by code point block
	const uint32_t *han_block_lower;
	const uint32_t *han_block_upper;
	const uint32_t *han_range_start;
	const uint32_t *han_range_index;
	const uint16_t *han_range_length;
};

//! The canonical decompositions and combining classes, stored as sorted tables
struct CollationNormalization {
	const uint32_t *decomposition_chars;
	const uint32_t *decomposition_codepoint;
	const uint32_t *decomposition_offset;
	const uint8_t *decomposition_length;
	uint32_t decomposition_count;
	const uint32_t *combining_class_start;
	const uint32_t *combining_class_end;
	const uint8_t *combining_class_value;
	uint32_t combining_class_count;
	const uint32_t *fcd_start;
	const uint32_t *fcd_end;
	const uint16_t *fcd_value;
	uint32_t fcd_count;
};

//! A compressed group of tables, decompressed the first time it is used
struct CollationUnit {
	const uint8_t *data;
	uint32_t compressed_size;
	uint32_t size;
};

//! A collation that can be used in a query, and the unit that holds its tailoring
struct CollationInfo {
	const char *name;
	uint32_t unit;
	uint8_t strength;
	bool case_level;
	uint8_t case_first;
	bool alternate_shifted;
	bool backward_secondary;
	bool normalization;
};

// the generated tables
extern const uint8_t collation_dictionary[];
extern const uint32_t collation_dictionary_size;
extern const CollationUnit collation_units[];
extern const uint32_t collation_unit_count;
extern const CollationInfo collation_infos[];
extern const uint32_t collation_count;
//! The last variable primary for each MaxVariable group
extern const uint32_t variable_top_primaries[];

//! The units that hold the tables every collation uses
static constexpr uint32_t COLLATION_ROOT_UNIT = 0;
static constexpr uint32_t COLLATION_NORMALIZATION_UNIT = 1;

//! Decompresses the tables of the root collation, only the first call does the work
const CollationRoot &GetCollationRoot();
//! Decompresses the normalization tables, only the first call does the work
const CollationNormalization &GetCollationNormalization();
//! Decompresses the tailoring of a collation, only the first call for a unit does the work
const CollationTailoring &GetCollationTailoring(uint32_t unit);

} // namespace collation
} // namespace duckdb
