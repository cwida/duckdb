#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

SourceResultType PhysicalDummyScan::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                    OperatorSourceInput &input) const {
	// return a single row on the first call to the dummy scan
	FlatVector::SetNull(chunk.data[0], 0, true);
	FlatVector::SetSize(chunk.data[0], 1);

	return SourceResultType::FINISHED;
}

} // namespace duckdb
