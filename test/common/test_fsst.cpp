#include "catch.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "fsst.h"

using namespace duckdb;

namespace {

struct FSSTEncoderDeleter {
	void operator()(duckdb_fsst_encoder_t *encoder) const {
		duckdb_fsst_destroy(encoder);
	}
};

vector<uint8_t> GreedyFSST(const vector<uint8_t> &input, const duckdb_fsst_decoder_t &decoder) {
	vector<uint8_t> result;
	for (idx_t offset = 0; offset < input.size();) {
		idx_t match_length = 0;
		uint8_t match_code = FSST_ESC;
		for (idx_t code = 0; code < FSST_ESC; code++) {
			auto length = decoder.len[code];
			if (length > match_length && length <= input.size() - offset &&
			    memcmp(input.data() + offset, &decoder.symbol[code], length) == 0) {
				match_length = length;
				match_code = NumericCast<uint8_t>(code);
			}
		}
		result.push_back(match_code);
		if (match_length == 0) {
			result.push_back(input[offset++]);
		} else {
			offset += match_length;
		}
	}
	return result;
}

} // namespace

TEST_CASE("FSST uses the longest symbol across former chunk boundaries", "[fsst]") {
	const string pattern = "abcdefgh";
	vector<uint8_t> training(32768);
	for (idx_t i = 0; i < training.size(); i++) {
		training[i] = static_cast<uint8_t>(pattern[i % pattern.size()]);
	}
	auto training_size = training.size();
	auto training_ptr = training.data();
	unique_ptr<duckdb_fsst_encoder_t, FSSTEncoderDeleter> encoder(
	    duckdb_fsst_create(1, &training_size, &training_ptr, 0));
	REQUIRE(encoder);
	auto decoder = duckdb_fsst_decoder(encoder.get());

	// Ensure a multi-byte symbol crosses byte 511 in the long inputs.
	bool has_full_symbol = false;
	for (idx_t code = 0; code < FSST_ESC; code++) {
		if (decoder.len[code] == pattern.size() && memcmp(&decoder.symbol[code], pattern.data(), pattern.size()) == 0) {
			has_full_symbol = true;
		}
	}
	REQUIRE(has_full_symbol);

	for (idx_t length : {0, 1, 7, 8, 9, 510, 511, 512, 513, 520, 528, 1022, 1023, 1024, 1025, 4096}) {
		for (idx_t start = 0; start < pattern.size(); start++) {
			CAPTURE(length, start);
			// Keep the address valid for an empty input, too.
			vector<uint8_t> input(length ? length : 1);
			for (idx_t i = 0; i < length; i++) {
				input[i] = static_cast<uint8_t>(pattern[(i + start) % pattern.size()]);
			}
			auto input_ptr = input.data();
			auto input_size = input.size();
			if (length == 0) {
				input_size = 0;
			}
			vector<uint8_t> compressed(2 * length + 7);
			auto compressed_size = compressed.size();
			uint8_t *compressed_ptr = nullptr;
			REQUIRE(duckdb_fsst_compress(encoder.get(), 1, &input_size, &input_ptr, compressed.size(),
			                             compressed.data(), &compressed_size, &compressed_ptr) == 1);
			input.resize(length);
			compressed.resize(compressed_size);
			REQUIRE(compressed == GreedyFSST(input, decoder));

			vector<uint8_t> decoded(length + 8);
			REQUIRE(duckdb_fsst_decompress(&decoder, compressed.size(), compressed.data(), decoded.size(),
			                               decoded.data()) == length);
			decoded.resize(length);
			REQUIRE(decoded == input);
		}
	}
}
