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
#include "duckdb/common/types/bignum_decimal.hpp"
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

	//! Parses wide (BIGNUM-backed) decimals written with a comma as the decimal separator
	static bool TryCastWideDecimalVectorCommaSeparated(Vector &input_vector, Vector &result_vector, idx_t count,
	                                                   uint32_t width, uint32_t scale, idx_t &line_error) {
		D_ASSERT(input_vector.GetType().id() == LogicalTypeId::VARCHAR);
		bool all_converted = true;
		auto &validity_mask = FlatVector::ValidityMutable(result_vector);
		idx_t cur_line = 0;
		UnaryExecutor::Execute<string_t, string_t>(input_vector, result_vector, count, [&](string_t input) {
			string blob;
			if (!TryParseWideDecimal(input.GetData(), input.GetSize(), ',', width, scale, blob)) {
				if (all_converted) {
					line_error = cur_line;
				}
				validity_mask.SetInvalid(cur_line);
				all_converted = false;
				cur_line++;
				return string_t();
			}
			cur_line++;
			return StringVector::AddStringOrBlob(result_vector, blob.data(), blob.size());
		});
		return all_converted;
	}

public:
	//! Parses a wide (BIGNUM-backed) decimal from text written with the given decimal separator
	static bool TryParseWideDecimal(const char *value_ptr, idx_t size, char decimal_separator, uint32_t width,
	                                uint32_t scale, string &result) {
		// like the fixed-size decimals: a period is not a valid character when the comma separates
		return BignumDecimal::TryParse(value_ptr, size, width, scale, result, decimal_separator);
	}

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
		case PhysicalType::VARCHAR:
			return TryCastWideDecimalVectorCommaSeparated(input_vector, result_vector, count, width, scale, line_error);
		default:
			throw InternalException("Unimplemented physical type for decimal");
		}
	}
};
} // namespace duckdb
