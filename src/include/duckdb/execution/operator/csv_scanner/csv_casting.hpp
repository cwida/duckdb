//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_casting.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {
struct CastParameters;
class CSVCast {
	template <class OP, class T>
	static bool TemplatedTryCastFloatingVector(const CSVReaderOptions &options, Vector &input_vector,
	                                           Vector &result_vector, idx_t count, CastParameters &parameters,
	                                           idx_t &line_error) {
		D_ASSERT(input_vector.GetType().id() == LogicalTypeId::VARCHAR);
		bool all_converted = true;
		idx_t row = 0;
		UnaryExecutor::Execute<string_t, T>(input_vector, result_vector, count, [&](string_t input) {
			T result;
			if (!OP::Operation(input, result, parameters)) {
				line_error = row;
				all_converted = false;
			} else {
				row++;
			}
			return result;
		});
		return all_converted;
	}

	template <class OP, class T>
	static bool TemplatedTryCastDecimalVector(const CSVReaderOptions &options, Vector &input_vector,
	                                          Vector &result_vector, idx_t count, CastParameters &parameters,
	                                          uint32_t width, uint32_t scale, idx_t &line_error) {
		D_ASSERT(input_vector.GetType().id() == LogicalTypeId::VARCHAR);
		bool all_converted = true;
		auto &validity_mask = FlatVector::ValidityMutable(result_vector);
		idx_t cur_line = 0;
		UnaryExecutor::Execute<string_t, T>(input_vector, result_vector, count, [&](string_t input) {
			T result;
			if (!OP::Operation(input, result, parameters, width, scale)) {
				if (all_converted) {
					line_error = cur_line;
				}
				validity_mask.SetInvalid(cur_line);
				all_converted = false;
				cur_line++;
			} else {
				cur_line++;
			}
			return result;
		});
		return all_converted;
	}

	struct TryCastDateOperator {
		static bool Operation(const map<LogicalTypeId, CSVOption<StrpTimeFormat>> &options, string_t input,
		                      date_t &result, string &error_message) {
			return options.at(LogicalTypeId::DATE).GetValue().TryParseDate(input, result, error_message);
		}
	};

	struct TryCastTimestampOperator {
		static bool Operation(const map<LogicalTypeId, CSVOption<StrpTimeFormat>> &options, string_t input,
		                      timestamp_t &result, string &error_message) {
			return options.at(LogicalTypeId::TIMESTAMP).GetValue().TryParseTimestamp(input, result, error_message);
		}
	};

	template <class OP, class T>
	static bool TemplatedTryCastDateVector(const map<LogicalTypeId, CSVOption<StrpTimeFormat>> &options,
	                                       Vector &input_vector, Vector &result_vector, idx_t count,
	                                       CastParameters &parameters, idx_t &line_error, bool nullify_error) {
		D_ASSERT(input_vector.GetType().id() == LogicalTypeId::VARCHAR);
		bool all_converted = true;
		idx_t cur_line = 0;
		auto &validity_mask = FlatVector::ValidityMutable(result_vector);
		UnaryExecutor::Execute<string_t, T>(input_vector, result_vector, count, [&](string_t input) {
			T result;
			if (!OP::Operation(options, input, result, *parameters.error_message)) {
				if (all_converted) {
					line_error = cur_line;
				}
				if (nullify_error) {
					validity_mask.SetInvalid(cur_line);
				}
				all_converted = false;
			}
			cur_line++;
			return result;
		});
		return all_converted;
	}

public:
	static bool TryCastDateVector(const map<LogicalTypeId, CSVOption<StrpTimeFormat>> &options, Vector &input_vector,
	                              Vector &result_vector, idx_t count, CastParameters &parameters, idx_t &line_error,
	                              bool nullify_error = false) {
		return TemplatedTryCastDateVector<TryCastDateOperator, date_t>(options, input_vector, result_vector, count,
		                                                               parameters, line_error, nullify_error);
	}
	static bool TryCastTimestampVector(const map<LogicalTypeId, CSVOption<StrpTimeFormat>> &options,
	                                   Vector &input_vector, Vector &result_vector, idx_t count,
	                                   CastParameters &parameters, bool nullify_error = false) {
		idx_t line_error;
		return TemplatedTryCastDateVector<TryCastTimestampOperator, timestamp_t>(
		    options, input_vector, result_vector, count, parameters, line_error, nullify_error);
	}
	static bool TryCastFloatingVectorCommaSeparated(const CSVReaderOptions &options, Vector &input_vector,
	                                                Vector &result_vector, idx_t count, CastParameters &parameters,
	                                                const LogicalType &result_type, idx_t &line_error) {
		switch (result_type.InternalType()) {
		case PhysicalType::DOUBLE:
			return TemplatedTryCastFloatingVector<TryCastErrorMessageCommaSeparated, double>(
			    options, input_vector, result_vector, count, parameters, line_error);
		case PhysicalType::FLOAT:
			return TemplatedTryCastFloatingVector<TryCastErrorMessageCommaSeparated, float>(
			    options, input_vector, result_vector, count, parameters, line_error);
		default:
			throw InternalException("Unimplemented physical type for floating");
		}
	}

	static bool TryCastDecimalVectorCommaSeparated(const CSVReaderOptions &options, Vector &input_vector,
	                                               Vector &result_vector, idx_t count, CastParameters &parameters,
	                                               const LogicalType &result_type, idx_t &line_error) {
		auto width = DecimalType::GetWidth(result_type);
		auto scale = DecimalType::GetScale(result_type);
		switch (result_type.InternalType()) {
		case PhysicalType::INT16:
			return TemplatedTryCastDecimalVector<TryCastToDecimalCommaSeparated, int16_t>(
			    options, input_vector, result_vector, count, parameters, width, scale, line_error);
		case PhysicalType::INT32:
			return TemplatedTryCastDecimalVector<TryCastToDecimalCommaSeparated, int32_t>(
			    options, input_vector, result_vector, count, parameters, width, scale, line_error);
		case PhysicalType::INT64:
			return TemplatedTryCastDecimalVector<TryCastToDecimalCommaSeparated, int64_t>(
			    options, input_vector, result_vector, count, parameters, width, scale, line_error);
		case PhysicalType::INT128:
			return TemplatedTryCastDecimalVector<TryCastToDecimalCommaSeparated, hugeint_t>(
			    options, input_vector, result_vector, count, parameters, width, scale, line_error);
		default:
			throw InternalException("Unimplemented physical type for decimal");
		}
	}
};
} // namespace duckdb
