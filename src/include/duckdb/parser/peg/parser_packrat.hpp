//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parser_packrat.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/arena_containers/arena_unordered_map.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"

namespace duckdb {
class Matcher;
class ParseResult;

struct ParserPackratKey {
	idx_t matcher_id;
	idx_t token_index;

	bool operator==(const ParserPackratKey &other) const {
		return matcher_id == other.matcher_id && token_index == other.token_index;
	}
};

struct ParserPackratKeyHash {
	size_t operator()(const ParserPackratKey &key) const;
};

struct ParserPackratEntry {
	bool success = false;
	idx_t token_index_after = 0;
	idx_t max_token_index_seen = 0;
	optional_ptr<ParseResult> result;
};

class ParserPackratCache {
public:
	explicit ParserPackratCache(ArenaAllocator &arena);
	//! Index keys within a known input interval; other keys retain the unbounded map behavior.
	ParserPackratCache(ArenaAllocator &arena, idx_t token_start, idx_t token_limit);
	~ParserPackratCache();

	optional_ptr<const ParserPackratEntry> Lookup(const Matcher &matcher, idx_t token_index) const;
	void Store(const Matcher &matcher, idx_t token_index, ParserPackratEntry entry);

private:
	struct Node;
	bool IsIndexed(idx_t token_index) const {
		return token_index >= token_start && token_index < token_limit;
	}

private:
	ArenaAllocator &arena;
	arena_vector<optional_ptr<Node>> token_entries;
	arena_unordered_map<ParserPackratKey, ParserPackratEntry, ParserPackratKeyHash> entries;
	idx_t token_start;
	idx_t token_limit;
};

} // namespace duckdb
