//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// arrow/arrow_test_helper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <utility>

#include "test_helpers.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/common/arrow/arrow_format.hpp"

class ArrowStreamTestFactory : public duckdb::ArrowScanFactory {
public:
	explicit ArrowStreamTestFactory(ArrowArrayStream &stream_p) : stream(stream_p) {
	}
	duckdb::unique_ptr<duckdb::ArrowArrayStreamWrapper>
	ProduceStream(duckdb::ArrowStreamParameters &parameters) override;
	void GetSchema(ArrowSchema &schema) override;

private:
	duckdb::reference<ArrowArrayStream> stream;
};

namespace duckdb {
class ArrowTestFactory : public ArrowScanFactory {
public:
	ArrowTestFactory(vector<LogicalType> types_p, vector<string> names_p, duckdb::unique_ptr<QueryResult> result_p,
	                 ClientProperties options, ClientContext &context)
	    : types(std::move(types_p)), names(std::move(names_p)), result(std::move(result_p)),
	      options(std::move(options)), context(context) {
	}

	vector<LogicalType> types;
	vector<string> names;
	//! Served as the Arrow arrays the engine produced when the query ran in the Arrow format, and
	//! converted chunk by chunk otherwise
	duckdb::unique_ptr<QueryResult> result;
	ClientProperties options;
	ClientContext &context;

	struct ArrowArrayStreamData {
		explicit ArrowArrayStreamData(ArrowTestFactory &factory, ClientProperties options)
		    : factory(factory), options(options) {
		}

		ArrowTestFactory &factory;
		ClientProperties options;
	};

	static int ArrowArrayStreamGetSchema(struct ArrowArrayStream *stream, struct ArrowSchema *out);

	static int ArrowArrayStreamGetNext(struct ArrowArrayStream *stream, struct ArrowArray *out);

	static const char *ArrowArrayStreamGetLastError(struct ArrowArrayStream *stream);

	static void ArrowArrayStreamRelease(struct ArrowArrayStream *stream);

	duckdb::unique_ptr<duckdb::ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override;
	void GetSchema(ArrowSchema &schema) override;

	void ToArrowSchema(struct ArrowSchema *out);
};

class ArrowTestHelper {
public:
	//! Used in the Arrow Roundtrip Tests
	static bool RunArrowComparison(Connection &con, const string &query, bool big_result = false);
	//! Used in the ADBC Testing
	static bool RunArrowComparison(Connection &con, const string &query, ArrowArrayStream &arrow_stream);

private:
	static bool CompareResults(Connection &con, shared_ptr<Relation> arrow_tbl, const string &query);

public:
	static shared_ptr<ArrowScanFactory> ConstructArrowScan(ArrowTestFactory &factory);
	static shared_ptr<ArrowScanFactory> ConstructArrowScan(ArrowArrayStream &stream);
	static unique_ptr<QueryResult> ScanArrowObject(Connection &con, shared_ptr<ArrowScanFactory> factory);
};
} // namespace duckdb
