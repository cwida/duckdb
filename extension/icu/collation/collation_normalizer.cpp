#include "collation_normalizer.hpp"

#include "collation_data.hpp"

namespace duckdb {
namespace collation {

static constexpr uint32_t REPLACEMENT_CHARACTER = 0xFFFD;

//! No character below these limits has a combining class or a canonical decomposition,
//! which the tables are checked against
static constexpr uint32_t FIRST_COMBINING_MARK = 0x300;
static constexpr uint32_t FIRST_DECOMPOSING_CHARACTER = 0xC0;

// Hangul syllables are decomposed algorithmically
static constexpr uint32_t HANGUL_S_BASE = 0xAC00;
static constexpr uint32_t HANGUL_L_BASE = 0x1100;
static constexpr uint32_t HANGUL_V_BASE = 0x1161;
static constexpr uint32_t HANGUL_T_BASE = 0x11A7;
static constexpr uint32_t HANGUL_V_COUNT = 21;
static constexpr uint32_t HANGUL_T_COUNT = 28;
static constexpr uint32_t HANGUL_N_COUNT = HANGUL_V_COUNT * HANGUL_T_COUNT;
static constexpr uint32_t HANGUL_S_COUNT = 19 * HANGUL_N_COUNT;

static bool IsHangulSyllable(uint32_t codepoint) {
	return codepoint >= HANGUL_S_BASE && codepoint < HANGUL_S_BASE + HANGUL_S_COUNT;
}

uint32_t Normalizer::Decode(const char *data, idx_t size, vector<uint32_t> &result) {
	result.clear();
	uint32_t flags = 0;
	auto bytes = reinterpret_cast<const uint8_t *>(data);
	idx_t pos = 0;
	while (pos < size) {
		auto lead = bytes[pos];
		uint32_t codepoint;
		idx_t length;
		if (lead < 0x80) {
			codepoint = lead;
			length = 1;
		} else if ((lead & 0xE0) == 0xC0) {
			codepoint = lead & 0x1F;
			length = 2;
		} else if ((lead & 0xF0) == 0xE0) {
			codepoint = lead & 0x0F;
			length = 3;
		} else if ((lead & 0xF8) == 0xF0) {
			codepoint = lead & 0x07;
			length = 4;
		} else {
			result.push_back(REPLACEMENT_CHARACTER);
			pos++;
			continue;
		}
		if (pos + length > size) {
			result.push_back(REPLACEMENT_CHARACTER);
			pos++;
			continue;
		}
		bool valid = true;
		for (idx_t i = 1; i < length; i++) {
			auto trail = bytes[pos + i];
			if ((trail & 0xC0) != 0x80) {
				valid = false;
				break;
			}
			codepoint = (codepoint << 6) | (trail & 0x3F);
		}
		if (!valid || codepoint > 0x10FFFF) {
			result.push_back(REPLACEMENT_CHARACTER);
			pos++;
			continue;
		}
		if (codepoint >= 0x80) {
			// ASCII characters never carry a combining class and never decompose
			flags |= TEXT_HAS_MARKS;
			if (IsHangulSyllable(codepoint)) {
				flags |= TEXT_HAS_HANGUL;
			}
		}
		result.push_back(codepoint);
		pos += length;
	}
	return flags;
}

//! Binary search in a range table
template <class T>
static const T *FindRange(uint32_t codepoint, const uint32_t *starts, const uint32_t *ends, const T *values,
                          uint32_t count) {
	uint32_t lower = 0;
	uint32_t upper = count;
	while (lower < upper) {
		auto middle = (lower + upper) / 2;
		if (codepoint < starts[middle]) {
			upper = middle;
		} else if (codepoint > ends[middle]) {
			lower = middle + 1;
		} else {
			return values + middle;
		}
	}
	return nullptr;
}

uint8_t Normalizer::CombiningClass(uint32_t codepoint) {
	if (codepoint < FIRST_COMBINING_MARK) {
		return 0;
	}
	auto &data = GetCollationNormalization();
	auto value = FindRange(codepoint, data.combining_class_start, data.combining_class_end, data.combining_class_value,
	                       data.combining_class_count);
	return value ? *value : 0;
}

//! The combining classes of the first and last character of the canonical decomposition
static uint16_t GetFCD(uint32_t codepoint) {
	if (codepoint < FIRST_DECOMPOSING_CHARACTER) {
		return 0;
	}
	if (codepoint >= HANGUL_S_BASE && codepoint < HANGUL_S_BASE + HANGUL_S_COUNT) {
		return 0;
	}
	auto &data = GetCollationNormalization();
	auto value = FindRange(codepoint, data.fcd_start, data.fcd_end, data.fcd_value, data.fcd_count);
	return value ? *value : 0;
}

//! The canonical decomposition of a code point, or nullptr if it has none
static const uint32_t *GetDecomposition(uint32_t codepoint, uint32_t &length) {
	auto &data = GetCollationNormalization();
	uint32_t lower = 0;
	uint32_t upper = data.decomposition_count;
	while (lower < upper) {
		auto middle = (lower + upper) / 2;
		if (codepoint < data.decomposition_codepoint[middle]) {
			upper = middle;
		} else if (codepoint > data.decomposition_codepoint[middle]) {
			lower = middle + 1;
		} else {
			length = data.decomposition_length[middle];
			return data.decomposition_chars + data.decomposition_offset[middle];
		}
	}
	return nullptr;
}

bool Normalizer::IsFCD(const vector<uint32_t> &text) {
	uint8_t previous_trailing = 0;
	for (auto codepoint : text) {
		auto fcd = GetFCD(codepoint);
		auto leading = static_cast<uint8_t>(fcd >> 8);
		if (leading != 0 && previous_trailing > leading) {
			// the combining marks are not in canonical order
			return false;
		}
		previous_trailing = static_cast<uint8_t>(fcd & 0xFF);
	}
	return true;
}

void Normalizer::DecomposeHangul(vector<uint32_t> &text) {
	bool has_syllable = false;
	for (auto codepoint : text) {
		if (IsHangulSyllable(codepoint)) {
			has_syllable = true;
			break;
		}
	}
	if (!has_syllable) {
		return;
	}
	vector<uint32_t> decomposed;
	decomposed.reserve(text.size() + 2);
	for (auto codepoint : text) {
		if (!IsHangulSyllable(codepoint)) {
			decomposed.push_back(codepoint);
			continue;
		}
		auto index = codepoint - HANGUL_S_BASE;
		decomposed.push_back(HANGUL_L_BASE + index / HANGUL_N_COUNT);
		decomposed.push_back(HANGUL_V_BASE + (index % HANGUL_N_COUNT) / HANGUL_T_COUNT);
		auto trailing = index % HANGUL_T_COUNT;
		if (trailing != 0) {
			decomposed.push_back(HANGUL_T_BASE + trailing);
		}
	}
	text = std::move(decomposed);
}

void Normalizer::Decompose(vector<uint32_t> &text) {
	vector<uint32_t> decomposed;
	decomposed.reserve(text.size() * 2);
	for (auto codepoint : text) {
		if (codepoint >= HANGUL_S_BASE && codepoint < HANGUL_S_BASE + HANGUL_S_COUNT) {
			auto index = codepoint - HANGUL_S_BASE;
			decomposed.push_back(HANGUL_L_BASE + index / HANGUL_N_COUNT);
			decomposed.push_back(HANGUL_V_BASE + (index % HANGUL_N_COUNT) / HANGUL_T_COUNT);
			auto trailing = index % HANGUL_T_COUNT;
			if (trailing != 0) {
				decomposed.push_back(HANGUL_T_BASE + trailing);
			}
			continue;
		}
		uint32_t length;
		auto decomposition = GetDecomposition(codepoint, length);
		if (!decomposition) {
			decomposed.push_back(codepoint);
			continue;
		}
		for (uint32_t i = 0; i < length; i++) {
			decomposed.push_back(decomposition[i]);
		}
	}
	// put the combining marks in canonical order
	for (idx_t i = 1; i < decomposed.size(); i++) {
		auto current = CombiningClass(decomposed[i]);
		if (current == 0) {
			continue;
		}
		for (idx_t j = i; j > 0; j--) {
			auto previous = CombiningClass(decomposed[j - 1]);
			if (previous == 0 || previous <= current) {
				break;
			}
			std::swap(decomposed[j - 1], decomposed[j]);
		}
	}
	text = std::move(decomposed);
}

} // namespace collation
} // namespace duckdb
