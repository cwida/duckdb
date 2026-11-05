//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/extra_operator_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"

namespace duckdb {

class ExtraOperatorInfo {
public:
	//! Table index of retained file-filter bindings, reserved above any binder-generated index
	static constexpr idx_t FILE_FILTER_TABLE_INDEX = DConstants::INVALID_INDEX - 1;

	ExtraOperatorInfo() : file_filters(""), sample_options(nullptr) {
	}
	ExtraOperatorInfo(ExtraOperatorInfo &&extra_info) noexcept = default;
	ExtraOperatorInfo &operator=(ExtraOperatorInfo &&extra_info) noexcept = default;

	bool operator==(const ExtraOperatorInfo &other) const {
		return file_filters == other.file_filters && total_files == other.total_files &&
		       filtered_files == other.filtered_files && sample_options == other.sample_options;
	}

	//! Filters that have been pushed down into the main file list
	string file_filters;
	//! Consumed file predicates with bindings in the unprojected source column space.
	optional<vector<unique_ptr<Expression>>> file_filter_expressions;
	//! Total size of file list
	optional_idx total_files;
	//! Size of file list after applying filters
	optional_idx filtered_files;
	//! Sample options that have been pushed down into the table scan
	unique_ptr<SampleOptions> sample_options;

	void Serialize(Serializer &serializer) const;
	static ExtraOperatorInfo Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
