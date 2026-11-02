
//===----------------------------------------------------------------------===//
//                         DuckDB
//
// direct_file_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"

namespace duckdb {

class DirectFileReader : public BaseFileReader {
public:
	explicit DirectFileReader(OpenFileInfo file_p, const LogicalType &type);
	~DirectFileReader() override;

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, const Identifier &name) override;

	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	AsyncResult Scan(ClientContext &context, GlobalTableFunctionState &global_state,
	                 LocalTableFunctionState &local_state, DataChunk &chunk) override;
	void FinishFile(ClientContext &context, GlobalTableFunctionState &gstate) override;
	double GetProgressInFile(ClientContext &context) override;

	string GetReaderType() const override {
		return "File";
	};

private:
	atomic<bool> done;
	LogicalType type;
};

} // namespace duckdb
