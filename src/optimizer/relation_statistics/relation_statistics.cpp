#include "duckdb/optimizer/relation_statistics/relation_statistics.hpp"

namespace duckdb {

DistinctCount::DistinctCount(idx_t distinct_count, DistinctCountSource source)
    : distinct_count(distinct_count), source(source) {
}

void SemiAntiJoinDomainEvidence::TightenFilterDomainBound(idx_t bound) {
	if (!filter_domain_bound.IsValid() || bound < filter_domain_bound.GetIndex()) {
		filter_domain_bound = optional_idx(bound);
	}
}

void SemiAntiJoinDomainEvidence::MarkUnique() {
	is_unique = true;
}

void SemiAntiJoinDomainEvidence::Invalidate() {
	filter_domain_bound = optional_idx();
	is_unique = false;
}

optional_idx SemiAntiJoinDomainEvidence::GetFilterDomainBound() const {
	return filter_domain_bound;
}

optional_idx SemiAntiJoinDomainEvidence::GetSupportedDomainSize(idx_t current_domain) const {
	optional_idx result;
	if (is_unique) {
		result = optional_idx(current_domain);
	}
	if (filter_domain_bound.IsValid()) {
		result = optional_idx(result.IsValid() ? MinValue(result.GetIndex(), filter_domain_bound.GetIndex())
		                                       : filter_domain_bound.GetIndex());
	}
	return result;
}

RelationColumnStats::RelationColumnStats(ColumnBinding binding, DistinctCount domain, Identifier name)
    : RelationColumnStats(binding, domain, domain, std::move(name)) {
}

RelationColumnStats::RelationColumnStats(ColumnBinding binding, DistinctCount total_domain,
                                         DistinctCount current_domain, Identifier name,
                                         SemiAntiJoinDomainEvidence semi_anti_join_domain_evidence)
    : binding(binding), total_domain(total_domain), current_domain(current_domain),
      semi_anti_join_domain_evidence(semi_anti_join_domain_evidence), name(std::move(name)) {
}

optional_idx RelationColumnStats::GetSupportedSemiAntiDomainSize() const {
	return semi_anti_join_domain_evidence.GetSupportedDomainSize(current_domain.distinct_count);
}

RelationStats::RelationStats() : cardinality(1), row_retention(1), stats_initialized(false) {
}

optional_idx RelationStats::FindColumn(ColumnBinding binding) const {
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		if (columns[column_idx].binding == binding) {
			return column_idx;
		}
	}
	return {};
}

optional_ptr<const RelationColumnStats> RelationStats::GetColumnStats(ColumnBinding binding) const {
	auto column_idx = FindColumn(binding);
	if (!column_idx.IsValid()) {
		return nullptr;
	}
	return columns[column_idx.GetIndex()];
}

bool RelationStats::MatchesBindings(const vector<ColumnBinding> &bindings) const {
	if (columns.size() != bindings.size()) {
		return false;
	}
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		if (columns[column_idx].binding != bindings[column_idx]) {
			return false;
		}
	}
	return true;
}

void RelationStats::CapCurrentDomainsToCardinality() {
	for (auto &column : columns) {
		column.current_domain.distinct_count = MinValue(column.current_domain.distinct_count, cardinality);
	}
}

RelationColumnStatsLookup::RelationColumnStatsLookup(const RelationStats &stats) {
	AddRelation(stats);
}

void RelationColumnStatsLookup::AddRelation(const RelationStats &stats) {
	for (auto &column : stats.columns) {
		columns.emplace(column.binding, column);
	}
}

optional_ptr<const RelationColumnStats> RelationColumnStatsLookup::Find(ColumnBinding binding) const {
	auto entry = columns.find(binding);
	if (entry == columns.end()) {
		return nullptr;
	}
	return entry->second.get();
}

void RelationStats::Verify(const vector<ColumnBinding> &bindings) const {
	D_ASSERT(!stats_initialized || MatchesBindings(bindings));
	if (!stats_initialized) {
		return;
	}
	D_ASSERT(row_retention >= 0 && row_retention <= 1);
	for (auto &column : columns) {
		(void)column;
		D_ASSERT(column.current_domain.distinct_count <= column.total_domain.distinct_count);
		D_ASSERT(column.current_domain.distinct_count <= cardinality);
	}
}

} // namespace duckdb
