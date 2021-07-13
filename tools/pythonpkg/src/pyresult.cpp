#include "duckdb_python/array_wrapper.hpp"
#include "duckdb_python/pyresult.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/arrow.hpp"

#include "datetime.h" // from Python

namespace duckdb {

void DuckDBPyResult::Initialize(py::handle &m) {
	py::class_<DuckDBPyResult>(m, "DuckDBPyResult")
	    .def("close", &DuckDBPyResult::Close)
	    .def("fetchone", &DuckDBPyResult::Fetchone)
	    .def("fetchall", &DuckDBPyResult::Fetchall)
	    .def("fetchnumpy", &DuckDBPyResult::FetchNumpy)
	    .def("fetchdf", &DuckDBPyResult::FetchDF)
	    .def("fetch_df", &DuckDBPyResult::FetchDF)
	    .def("fetch_df_chunk", &DuckDBPyResult::FetchDFChunk)
	    .def("fetch_arrow_table", &DuckDBPyResult::FetchArrowTable)
	    .def("arrow", &DuckDBPyResult::FetchArrowTable)
	    .def("df", &DuckDBPyResult::FetchDF);

	PyDateTime_IMPORT;
}

py::object GetValueToPython(Value &val, const LogicalType &type) {
	if (val.is_null) {
		return py::none();
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return py::cast(val.GetValue<bool>());
	case LogicalTypeId::TINYINT:
		return py::cast(val.GetValue<int8_t>());
	case LogicalTypeId::SMALLINT:
		return py::cast(val.GetValue<int16_t>());
	case LogicalTypeId::INTEGER:
		return py::cast(val.GetValue<int32_t>());
	case LogicalTypeId::BIGINT:
		return py::cast(val.GetValue<int64_t>());
	case LogicalTypeId::UTINYINT:
		return py::cast(val.GetValue<uint8_t>());
	case LogicalTypeId::USMALLINT:
		return py::cast(val.GetValue<uint16_t>());
	case LogicalTypeId::UINTEGER:
		return py::cast(val.GetValue<uint32_t>());
	case LogicalTypeId::UBIGINT:
		return py::cast(val.GetValue<uint64_t>());
	case LogicalTypeId::HUGEINT:
		return py::cast<py::object>(PyLong_FromString((char *)val.GetValue<string>().c_str(), nullptr, 10));
	case LogicalTypeId::FLOAT:
		return py::cast(val.GetValue<float>());
	case LogicalTypeId::DOUBLE:
		return py::cast(val.GetValue<double>());
	case LogicalTypeId::DECIMAL: {
		py::object decimal_py = py::module_::import("decimal").attr("Decimal");
		return decimal_py(val.ToString());
	}
	case LogicalTypeId::VARCHAR:
		return py::cast(val.GetValue<string>());
	case LogicalTypeId::BLOB:
		return py::bytes(val.GetValueUnsafe<string>());
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		D_ASSERT(type.InternalType() == PhysicalType::INT64);
		auto timestamp = val.GetValueUnsafe<timestamp_t>();
		if (type.id() == LogicalTypeId::TIMESTAMP_MS) {
			timestamp = Timestamp::FromEpochMs(timestamp.value);
		} else if (type.id() == LogicalTypeId::TIMESTAMP_NS) {
			timestamp = Timestamp::FromEpochNanoSeconds(timestamp.value);
		} else if (type.id() == LogicalTypeId::TIMESTAMP_SEC) {
			timestamp = Timestamp::FromEpochSeconds(timestamp.value);
		}
		int32_t year, month, day, hour, min, sec, micros;
		date_t date;
		dtime_t time;
		Timestamp::Convert(timestamp, date, time);
		Date::Convert(date, year, month, day);
		Time::Convert(time, hour, min, sec, micros);
		return py::cast<py::object>(PyDateTime_FromDateAndTime(year, month, day, hour, min, sec, micros));
	}
	case LogicalTypeId::TIME: {
		D_ASSERT(type.InternalType() == PhysicalType::INT64);

		int32_t hour, min, sec, microsec;
		auto time = val.GetValueUnsafe<dtime_t>();
		duckdb::Time::Convert(time, hour, min, sec, microsec);
		return py::cast<py::object>(PyTime_FromTime(hour, min, sec, microsec));
	}
	case LogicalTypeId::DATE: {
		D_ASSERT(type.InternalType() == PhysicalType::INT32);

		auto date = val.GetValueUnsafe<date_t>();
		int32_t year, month, day;
		duckdb::Date::Convert(date, year, month, day);
		return py::cast<py::object>(PyDate_FromDate(year, month, day));
	}
	case LogicalTypeId::LIST: {
		py::list list;
		for (auto list_elem : val.list_value) {
			list.append(GetValueToPython(list_elem, ListType::GetChildType(type)));
		}
		return std::move(list);
	}
	case LogicalTypeId::MAP:
	case LogicalTypeId::STRUCT: {
		py::dict py_struct;
		auto &child_types = StructType::GetChildTypes(type);
		for (idx_t i = 0; i < val.struct_value.size(); i++) {
			auto &child_entry = child_types[i];
			auto &child_name = child_entry.first;
			auto &child_type = child_entry.second;
			py_struct[child_name.c_str()] = GetValueToPython(val.struct_value[i], child_type);
		}
		return std::move(py_struct);
	}
	default:
		throw std::runtime_error("unsupported type: " + type.ToString());
	}
}

py::object DuckDBPyResult::Fetchone() {
	if (!result) {
		throw std::runtime_error("result closed");
	}
	if (!current_chunk || chunk_offset >= current_chunk->size()) {
		current_chunk = result->Fetch();
		chunk_offset = 0;
	}
	if (!current_chunk || current_chunk->size() == 0) {
		return py::none();
	}
	py::tuple res(result->types.size());

	for (idx_t col_idx = 0; col_idx < result->types.size(); col_idx++) {
		auto &mask = FlatVector::Validity(current_chunk->data[col_idx]);
		if (!mask.RowIsValid(chunk_offset)) {
			res[col_idx] = py::none();
			continue;
		}
		auto val = current_chunk->data[col_idx].GetValue(chunk_offset);
		res[col_idx] = GetValueToPython(val, result->types[col_idx]);
	}
	chunk_offset++;
	return move(res);
}

py::list DuckDBPyResult::Fetchall() {
	py::list res;
	while (true) {
		auto fres = Fetchone();
		if (fres.is_none()) {
			break;
		}
		res.append(fres);
	}
	return res;
}

py::dict DuckDBPyResult::FetchNumpy(bool stream, idx_t vectors_per_chunk) {
	if (!result) {
		throw std::runtime_error("result closed");
	}

	// iterate over the result to materialize the data needed for the NumPy arrays
	idx_t initial_capacity = STANDARD_VECTOR_SIZE * 2;
	if (result->type == QueryResultType::MATERIALIZED_RESULT) {
		// materialized query result: we know exactly how much space we need
		auto &materialized = (MaterializedQueryResult &)*result;
		initial_capacity = materialized.collection.Count();
	}

	NumpyResultConversion conversion(result->types, initial_capacity);
	if (result->type == QueryResultType::MATERIALIZED_RESULT) {
		auto &materialized = (MaterializedQueryResult &)*result;
		if (!stream) {
			for (auto &chunk : materialized.collection.Chunks()) {
				conversion.Append(*chunk);
			}
			materialized.collection.Reset();
		} else {
			for (idx_t count_vec = 0; count_vec < vectors_per_chunk; count_vec++) {
				auto chunk = materialized.Fetch();
				if (!chunk || chunk->size() == 0) {
					//! finished
					break;
				}
				conversion.Append(*chunk);
			}
		}
	} else {
		if (!stream) {
			while (true) {
				auto chunk = result->FetchRaw();
				if (!chunk || chunk->size() == 0) {
					//! finished
					break;
				}
				conversion.Append(*chunk);
			}
		} else {
			auto stream_result = (StreamQueryResult *)result.get();
			for (idx_t count_vec = 0; count_vec < vectors_per_chunk; count_vec++) {
				if (!stream_result->is_open) {
					break;
				}
				auto chunk = stream_result->FetchRaw();
				if (!chunk || chunk->size() == 0) {
					//! finished
					break;
				}
				conversion.Append(*chunk);
			}
		}
	}

	// now that we have materialized the result in contiguous arrays, construct the actual NumPy arrays
	py::dict res;
	for (idx_t col_idx = 0; col_idx < result->types.size(); col_idx++) {
		res[result->names[col_idx].c_str()] = conversion.ToArray(col_idx);
	}
	return res;
}

py::object DuckDBPyResult::FetchDF() {
	return py::module::import("pandas").attr("DataFrame").attr("from_dict")(FetchNumpy());
}

py::object DuckDBPyResult::FetchDFChunk(idx_t num_of_vectors) {
	return py::module::import("pandas").attr("DataFrame").attr("from_dict")(FetchNumpy(true, num_of_vectors));
}

py::object DuckDBPyResult::FetchArrowTable() {
	if (!result) {
		throw std::runtime_error("result closed");
	}
	py::gil_scoped_acquire acquire;
	auto pyarrow_lib_module = py::module::import("pyarrow").attr("lib");

	auto batch_import_func = pyarrow_lib_module.attr("RecordBatch").attr("_import_from_c");
	auto from_batches_func = pyarrow_lib_module.attr("Table").attr("from_batches");
	auto schema_import_func = pyarrow_lib_module.attr("Schema").attr("_import_from_c");
	ArrowSchema schema;
	result->ToArrowSchema(&schema);
	auto schema_obj = schema_import_func((uint64_t)&schema);

	py::list batches;
	while (true) {
		auto data_chunk = result->Fetch();
		if (!data_chunk || data_chunk->size() == 0) {
			break;
		}
		ArrowArray data;
		data_chunk->ToArrowArray(&data);
		ArrowSchema arrow_schema;
		result->ToArrowSchema(&arrow_schema);
		batches.append(batch_import_func((uint64_t)&data, (uint64_t)&arrow_schema));
	}
	return from_batches_func(batches, schema_obj);
}

py::str GetTypeToPython(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return py::str("bool");
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL: {
		return py::str("NUMBER");
	}
	case LogicalTypeId::VARCHAR:
		return py::str("STRING");
	case LogicalTypeId::BLOB:
		return py::str("BINARY");
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		return py::str("DATETIME");
	}
	case LogicalTypeId::TIME: {
		return py::str("Time");
	}
	case LogicalTypeId::DATE: {
		return py::str("Date");
	}
	case LogicalTypeId::MAP:
	case LogicalTypeId::STRUCT:
		return py::str("dict");
	case LogicalTypeId::LIST: {
		return py::str("list");
	}
	default:
		throw std::runtime_error("unsupported type: " + type.ToString());
	}
}

py::list DuckDBPyResult::Description() {
	py::list desc(result->names.size());
	for (idx_t col_idx = 0; col_idx < result->names.size(); col_idx++) {
		py::tuple col_desc(7);
		col_desc[0] = py::str(result->names[col_idx]);
		col_desc[1] = GetTypeToPython(result->types[col_idx]);
		col_desc[2] = py::none();
		col_desc[3] = py::none();
		col_desc[4] = py::none();
		col_desc[5] = py::none();
		col_desc[6] = py::none();
		desc[col_idx] = col_desc;
	}
	return desc;
}

void DuckDBPyResult::Close() {
	result = nullptr;
}

} // namespace duckdb
