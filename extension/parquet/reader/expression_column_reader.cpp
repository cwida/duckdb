#include "reader/expression_column_reader.hpp"

#include <utility>

#include "parquet_reader.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb_apache {
namespace thrift {
namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache
namespace duckdb_parquet {
class ColumnChunk;
} // namespace duckdb_parquet

namespace duckdb {
class ClientContext;

//===--------------------------------------------------------------------===//
// Expression Column Reader
//===--------------------------------------------------------------------===//
ExpressionColumnReader::ExpressionColumnReader(ClientContext &context_p,
                                               vector<unique_ptr<ColumnReader>> child_readers_p,
                                               unique_ptr<Expression> expr_p, const ParquetColumnSchema &schema_p)
    : ColumnReader(child_readers_p[0]->Reader(), schema_p), child_readers(std::move(child_readers_p)),
      expr(std::move(expr_p)), executor(context_p, expr.get()), context(context_p) {
	if (child_readers.empty()) {
		throw InternalException("Can't instantiate an ExpressionColumnReader with 0 children");
	}
	for (auto &child : child_readers) {
		if (child) {
			child->SetParent(*this);
		}
	}
	InitializeChunk();
}

ExpressionColumnReader::ExpressionColumnReader(ClientContext &context_p,
                                               vector<unique_ptr<ColumnReader>> child_readers_p,
                                               unique_ptr<Expression> expr_p,
                                               unique_ptr<ParquetColumnSchema> owned_schema_p)
    : ColumnReader(child_readers_p[0]->Reader(), *owned_schema_p), child_readers(std::move(child_readers_p)),
      expr(std::move(expr_p)), executor(context_p, expr.get()), owned_schema(std::move(owned_schema_p)),
      context(context_p) {
	if (child_readers.empty()) {
		throw InternalException("Can't instantiate an ExpressionColumnReader with 0 children");
	}
	for (auto &child : child_readers) {
		if (child) {
			child->SetParent(*this);
		}
	}
	InitializeChunk();
}

void ExpressionColumnReader::InitializeChunk() {
	vector<LogicalType> intermediate_types;
	for (auto &child_reader : child_readers) {
		intermediate_types.push_back(child_reader->Type());
	};
	intermediate_chunk.Initialize(reader.allocator, intermediate_types);
}

void ExpressionColumnReader::InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows,
                                            const duckdb_parquet::ColumnChunkList &columns, TProtocol &protocol_p) {
	for (auto &child_reader : child_readers) {
		child_reader->InitializeRead(row_group_idx_p, row_group_num_rows, columns, protocol_p);
	}
}

unique_ptr<BaseStatistics> ExpressionColumnReader::Stats(idx_t row_group_idx_p,
                                                         const duckdb_parquet::ColumnChunkList &columns) {
	if (Schema().schema_type != ParquetColumnSchemaType::EXPRESSION) {
		return ColumnReader::Stats(row_group_idx_p, columns);
	}
	vector<BaseStatistics> input_stats;
	input_stats.reserve(child_readers.size());
	for (auto &child_reader : child_readers) {
		auto child_stats = child_reader->Stats(row_group_idx_p, columns);
		if (!child_stats) {
			return nullptr;
		}
		input_stats.push_back(child_stats->Copy());
	}
	return ExpressionFilter::TryGetExpressionStatistics(
	    context, *expr, array_ptr<const BaseStatistics>(input_stats.data(), input_stats.size()));
}

static void ReverseSelectionVector(const SelectionVector &input, SelectionVector &output, idx_t input_count,
                                   idx_t result_count) {
	//! For an input selection vector: [5, 10],
	//! produce a new selection vector: [-, -, -, -, 0, -, -, -, -, 1]
	idx_t result_index = 0;
	idx_t last_index = 0;
	for (idx_t i = 0; i < input_count; i++) {
		idx_t value = input[i];
		last_index = i;
		if (value >= result_count) {
			throw InternalException("Not enough room in the resulting selection vector (%d) to reverse the selection "
			                        "vector, encountered value: %d, at index: %d",
			                        input_count, value, i);
		}
		for (; result_index <= value; result_index++) {
			output[result_index] = last_index;
		}
	}
	//! Fill the remainder of the selection vector, to remove any uninitialized values
	for (; result_index < result_count; result_index++) {
		output[result_index] = last_index;
	}
}

void ExpressionColumnReader::Select(ColumnReaderInput &input, Vector &result, const SelectionVector &sel,
                                    idx_t approved_tuple_count) {
	intermediate_chunk.Reset();
	for (idx_t i = 0; i < child_readers.size(); i++) {
		auto &child_reader = child_readers[i];
		child_reader->Select(input, intermediate_chunk.data[i], sel, approved_tuple_count);
	}
	intermediate_chunk.SetChildCardinality(input.num_values);
	//! This executes the expression *and* applies the selection vector in the process
	executor.ExecuteExpression(intermediate_chunk, result, sel, approved_tuple_count);
	if (input.num_values != approved_tuple_count) {
		//! Since the caller expects the rows to be in the spot they would have been in the input chunk
		//! We now have to reverse this selection ..
		SelectionVector inverted_sel(input.num_values);
		ReverseSelectionVector(sel, inverted_sel, approved_tuple_count, input.num_values);
		result.Slice(inverted_sel, input.num_values);
	}
}

idx_t ExpressionColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	intermediate_chunk.Reset();

	optional_idx amount;
	for (idx_t i = 0; i < child_readers.size(); i++) {
		auto &intermediate_vector = intermediate_chunk.data[i];
		auto &child_reader = child_readers[i];

		auto res = child_reader->Read(input, intermediate_vector);
		if (amount.IsValid() && res != amount.GetIndex()) {
			throw InternalException("ExpressionColumnReader children Read calls produced differing amounts (%d and %d)",
			                        res, amount.GetIndex());
		}
		amount = res;
	}

	// Execute the expression
	intermediate_chunk.SetChildCardinality(amount.GetIndex());
	executor.ExecuteExpression(intermediate_chunk, result);
	return amount.GetIndex();
}

void ExpressionColumnReader::Skip(idx_t num_values) {
	for (auto &child_reader : child_readers) {
		child_reader->Skip(num_values);
	}
}

idx_t ExpressionColumnReader::GroupRowsAvailable() {
	return child_readers[0]->GroupRowsAvailable();
}

} // namespace duckdb
