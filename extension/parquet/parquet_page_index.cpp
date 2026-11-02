#include "parquet_page_index.hpp"

#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_statistics.hpp"
#include "thrift_tools.hpp"

namespace duckdb {

bool ParquetPageIndexUtils::HasColumnIndex(const duckdb_parquet::ColumnChunk &column_chunk) {
	return column_chunk.__isset.column_index_offset && column_chunk.__isset.column_index_length &&
	       column_chunk.column_index_offset > 0 && column_chunk.column_index_length > 0;
}

bool ParquetPageIndexUtils::HasOffsetIndex(const duckdb_parquet::ColumnChunk &column_chunk) {
	return column_chunk.__isset.offset_index_offset && column_chunk.__isset.offset_index_length &&
	       column_chunk.offset_index_offset > 0 && column_chunk.offset_index_length > 0;
}

static void ValidateIndexLocation(const ThriftFileTransport &transport, int64_t offset, int32_t length,
                                  const char *index_name) {
	if (offset < 0 || length <= 0 || NumericCast<idx_t>(offset) + NumericCast<idx_t>(length) > transport.GetSize()) {
		throw InvalidInputException("Parquet file is corrupt: %s at offset %lld with length %d lies outside the file",
		                            index_name, offset, length);
	}
}

void ParquetPageIndexUtils::ReadColumnIndex(duckdb_apache::thrift::protocol::TProtocol &protocol,
                                            const duckdb_parquet::ColumnChunk &column_chunk,
                                            duckdb_parquet::ColumnIndex &result) {
	D_ASSERT(HasColumnIndex(column_chunk));
	auto &transport = reinterpret_cast<ThriftFileTransport &>(*protocol.getTransport());
	ValidateIndexLocation(transport, column_chunk.column_index_offset, column_chunk.column_index_length, "ColumnIndex");
	transport.SetLocation(NumericCast<idx_t>(column_chunk.column_index_offset));
	result.read(&protocol);
}

void ParquetPageIndexUtils::ReadOffsetIndex(duckdb_apache::thrift::protocol::TProtocol &protocol,
                                            const duckdb_parquet::ColumnChunk &column_chunk,
                                            duckdb_parquet::OffsetIndex &result) {
	D_ASSERT(HasOffsetIndex(column_chunk));
	auto &transport = reinterpret_cast<ThriftFileTransport &>(*protocol.getTransport());
	ValidateIndexLocation(transport, column_chunk.offset_index_offset, column_chunk.offset_index_length, "OffsetIndex");
	transport.SetLocation(NumericCast<idx_t>(column_chunk.offset_index_offset));
	result.read(&protocol);
}

bool ParquetPageIndexUtils::OffsetIndexIsValid(const duckdb_parquet::OffsetIndex &offset_index,
                                               idx_t row_group_num_rows, idx_t file_size,
                                               const duckdb_parquet::ColumnChunk &column_chunk) {
	auto &locations = offset_index.page_locations;
	if (locations.empty() || locations[0].first_row_index != 0 || !column_chunk.__isset.meta_data) {
		return false;
	}
	// every page has to lie inside the byte range of its own column chunk
	auto &meta = column_chunk.meta_data;
	int64_t chunk_begin = meta.data_page_offset;
	if (meta.__isset.dictionary_page_offset && meta.dictionary_page_offset >= 4) {
		chunk_begin = MinValue(chunk_begin, meta.dictionary_page_offset);
	}
	if (chunk_begin < 0 || meta.total_compressed_size < 0) {
		return false;
	}
	const auto chunk_end = NumericCast<idx_t>(chunk_begin) + NumericCast<idx_t>(meta.total_compressed_size);
	for (idx_t page_idx = 0; page_idx < locations.size(); page_idx++) {
		auto &location = locations[page_idx];
		if (location.offset <= 0 || location.compressed_page_size <= 0 || location.first_row_index < 0) {
			return false;
		}
		if (NumericCast<idx_t>(location.offset) + NumericCast<idx_t>(location.compressed_page_size) > file_size) {
			return false;
		}
		if (NumericCast<idx_t>(location.offset) < NumericCast<idx_t>(chunk_begin) ||
		    NumericCast<idx_t>(location.offset) + NumericCast<idx_t>(location.compressed_page_size) > chunk_end) {
			return false;
		}
		if (NumericCast<idx_t>(location.first_row_index) >= row_group_num_rows) {
			return false;
		}
		if (page_idx > 0) {
			// pages are stored in row order, so both the rows and the bytes have to increase
			auto &previous = locations[page_idx - 1];
			if (location.first_row_index <= previous.first_row_index) {
				return false;
			}
			if (NumericCast<idx_t>(location.offset) <
			    NumericCast<idx_t>(previous.offset) + NumericCast<idx_t>(previous.compressed_page_size)) {
				return false;
			}
		}
	}
	return true;
}

bool ParquetPageIndexUtils::ColumnIndexIsValid(const duckdb_parquet::ColumnIndex &column_index, idx_t page_count) {
	if (column_index.null_pages.size() != page_count || column_index.min_values.size() != page_count ||
	    column_index.max_values.size() != page_count) {
		return false;
	}
	if (column_index.__isset.null_counts && column_index.null_counts.size() != page_count) {
		return false;
	}
	return true;
}

idx_t ParquetPageIndexUtils::PageRowCount(const duckdb_parquet::OffsetIndex &offset_index, idx_t page_idx,
                                          idx_t row_group_num_rows) {
	auto &locations = offset_index.page_locations;
	const auto begin = NumericCast<idx_t>(locations[page_idx].first_row_index);
	const auto end = page_idx + 1 < locations.size() ? NumericCast<idx_t>(locations[page_idx + 1].first_row_index)
	                                                 : row_group_num_rows;
	return end - begin;
}

void ParquetPageIndexUtils::FilterPages(const ParquetColumnSchema &schema,
                                        const duckdb_parquet::ColumnIndex &column_index, const TableFilter &filter,
                                        bool can_have_nan, vector<bool> &result) {
	const auto page_count = column_index.null_pages.size();
	D_ASSERT(ColumnIndexIsValid(column_index, page_count));
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "ParquetPageIndexUtils::FilterPages");
	result.assign(page_count, true);
	ArenaAllocator stats_arena(Allocator::DefaultAllocator());
	for (idx_t page_idx = 0; page_idx < page_count; page_idx++) {
		unique_ptr<BaseStatistics> stats;
		const bool has_null_count = column_index.__isset.null_counts;
		if (column_index.null_pages[page_idx]) {
			// a page that holds only NULL values
			stats = BaseStatistics::CreateUnknown(schema.type).ToUnique();
			stats->Set(StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES);
			stats->Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
		} else {
			// the bounds of a page are stored like the bounds of a column chunk, but may be truncated
			duckdb_parquet::Statistics page_stats(stats_arena);
			page_stats.__set_min_value(column_index.min_values[page_idx]);
			page_stats.__set_max_value(column_index.max_values[page_idx]);
			if (has_null_count) {
				page_stats.__set_null_count(column_index.null_counts[page_idx]);
			}
			stats = ParquetStatisticsUtils::TransformParquetStatistics(schema.type, schema, page_stats, can_have_nan);
			if (!stats) {
				continue;
			}
			stats->Set(StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES);
			if (has_null_count && column_index.null_counts[page_idx] == 0) {
				stats->Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
			}
		}
		auto prune_result = expr_filter.CheckStatistics(*stats);
		if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			result[page_idx] = false;
		}
	}
}

vector<ParquetPageRowRange> ParquetPageIndexUtils::PagesToRowRanges(const duckdb_parquet::OffsetIndex &offset_index,
                                                                    const vector<bool> &page_may_match,
                                                                    idx_t row_group_num_rows) {
	vector<ParquetPageRowRange> result;
	auto &locations = offset_index.page_locations;
	D_ASSERT(page_may_match.size() == locations.size());
	for (idx_t page_idx = 0; page_idx < locations.size(); page_idx++) {
		if (!page_may_match[page_idx]) {
			continue;
		}
		const auto begin = NumericCast<idx_t>(locations[page_idx].first_row_index);
		const auto end = begin + PageRowCount(offset_index, page_idx, row_group_num_rows);
		if (!result.empty() && result.back().end == begin) {
			// consecutive pages form one range
			result.back().end = end;
		} else {
			result.push_back({begin, end});
		}
	}
	return result;
}

vector<ParquetPageRowRange> ParquetPageIndexUtils::IntersectRowRanges(const vector<ParquetPageRowRange> &left,
                                                                      const vector<ParquetPageRowRange> &right) {
	vector<ParquetPageRowRange> result;
	idx_t left_idx = 0;
	idx_t right_idx = 0;
	while (left_idx < left.size() && right_idx < right.size()) {
		auto &l = left[left_idx];
		auto &r = right[right_idx];
		const auto begin = MaxValue(l.begin, r.begin);
		const auto end = MinValue(l.end, r.end);
		if (begin < end) {
			result.push_back({begin, end});
		}
		// advance the range that ends first
		if (l.end < r.end) {
			left_idx++;
		} else {
			right_idx++;
		}
	}
	return result;
}

idx_t ParquetPageIndexUtils::RowCount(const vector<ParquetPageRowRange> &ranges) {
	idx_t result = 0;
	for (auto &range : ranges) {
		result += range.end - range.begin;
	}
	return result;
}

bool ParquetPageIndexUtils::PageOverlapsRowRanges(const vector<ParquetPageRowRange> &ranges, idx_t begin, idx_t end) {
	// the first range that ends after the page begins is the only one that can overlap the page
	auto it = std::upper_bound(ranges.begin(), ranges.end(), begin,
	                           [](idx_t row, const ParquetPageRowRange &range) { return row < range.end; });
	return it != ranges.end() && it->begin < end;
}

} // namespace duckdb
