#include "duckdb/parser/peg/parser_packrat.hpp"

#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

size_t ParserPackratKeyHash::operator()(const ParserPackratKey &key) const {
	return std::hash<idx_t>()(key.matcher_id) ^ (std::hash<idx_t>()(key.token_index) << 1);
}

struct ParserPackratCache::Node {
	Node(idx_t matcher_id, const ParserPackratEntry &value, optional_ptr<Node> next)
	    : matcher_id(matcher_id), value(value), next(next) {
	}
	idx_t matcher_id;
	ParserPackratEntry value;
	optional_ptr<Node> next;
};

ParserPackratCache::ParserPackratCache(ArenaAllocator &arena) : ParserPackratCache(arena, 0, 0) {
}

ParserPackratCache::ParserPackratCache(ArenaAllocator &arena, idx_t token_start, idx_t token_limit)
    : arena(arena), token_entries(arena), entries(arena), token_start(token_start), token_limit(token_limit) {
}

ParserPackratCache::~ParserPackratCache() = default;

optional_ptr<const ParserPackratEntry> ParserPackratCache::Lookup(const Matcher &matcher, idx_t token_index) const {
	D_ASSERT(matcher.IsPackratMemoized());
	auto packrat_id = matcher.GetPackratId();
	D_ASSERT(packrat_id.IsValid());
	auto matcher_id = packrat_id.GetIndex();
	if (IsIndexed(token_index)) {
		auto index = token_index - token_start;
		if (index >= token_entries.size()) {
			return nullptr;
		}
		auto node = token_entries[index];
		while (node) {
			if (node->matcher_id == matcher_id) {
				return &node->value;
			}
			node = node->next;
		}
		return nullptr;
	}
	ParserPackratKey key {matcher_id, token_index};
	auto entry = entries.find(key);
	if (entry == entries.end()) {
		return nullptr;
	}
	return optional_ptr<const ParserPackratEntry>(&entry->second);
}

void ParserPackratCache::Store(const Matcher &matcher, idx_t token_index, ParserPackratEntry entry) {
	D_ASSERT(matcher.IsPackratMemoized());
	auto packrat_id = matcher.GetPackratId();
	D_ASSERT(packrat_id.IsValid());
	auto matcher_id = packrat_id.GetIndex();
	if (IsIndexed(token_index)) {
		auto index = token_index - token_start;
		if (index >= token_entries.size()) {
			auto capacity = MaxValue<idx_t>(index + 1, MaxValue<idx_t>(16, token_entries.size() * 2));
			token_entries.resize(MinValue(capacity, token_limit - token_start));
		}
		auto node = token_entries[index];
		while (node) {
			if (node->matcher_id == matcher_id) {
				return;
			}
			node = node->next;
		}
		token_entries[index] = arena.Make<Node>(matcher_id, entry, token_entries[index]);
		return;
	}
	ParserPackratKey key {matcher_id, token_index};
	entries.insert(make_pair(key, entry));
}

} // namespace duckdb
