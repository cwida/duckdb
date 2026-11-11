#include "duckdb/common/helper.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"
#include "json_schema_inference.hpp"
#include "json_transform.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace duckdb {

static inline LogicalType RemoveDuplicateStructKeys(const LogicalType &type, const bool ignore_errors) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		case_insensitive_set_t child_names;
		child_list_t<LogicalType> child_types;
		for (auto &child_type : StructType::GetChildTypes(type)) {
			auto insert_success = child_names.insert(child_type.first.GetIdentifierName()).second;
			if (!insert_success) {
				if (ignore_errors) {
					continue;
				}
				throw NotImplementedException(
				    "Duplicate name %s in struct auto-detected in JSON, try ignore_errors=true", child_type.first);
			} else {
				child_types.emplace_back(child_type.first, RemoveDuplicateStructKeys(child_type.second, ignore_errors));
			}
		}
		return LogicalType::STRUCT(child_types);
	}
	case LogicalTypeId::MAP:
		return LogicalType::MAP(RemoveDuplicateStructKeys(MapType::KeyType(type), ignore_errors),
		                        RemoveDuplicateStructKeys(MapType::ValueType(type), ignore_errors));
	case LogicalTypeId::LIST:
		return LogicalType::LIST(RemoveDuplicateStructKeys(ListType::GetChildType(type), ignore_errors));
	default:
		return type;
	}
}

struct AutoDetectState {
	AutoDetectState(ClientContext &context_p, JSONScanData &json_data_p, const vector<OpenFileInfo> &files,
	                vector<shared_ptr<JSONReader>> &sampled_readers_p, MutableDateFormatMap &date_format_map)
	    : context(context_p), json_data(json_data_p), files(files), sampled_readers(sampled_readers_p),
	      date_format_map(date_format_map), files_scanned(0), tuples_scanned(0), bytes_scanned(0), total_file_size(0) {
	}

	ClientContext &context;
	JSONScanData &json_data;
	const vector<OpenFileInfo> &files;
	vector<shared_ptr<JSONReader>> &sampled_readers;
	MutableDateFormatMap &date_format_map;
	atomic<idx_t> files_scanned;
	atomic<idx_t> tuples_scanned;
	atomic<idx_t> bytes_scanned;
	atomic<idx_t> total_file_size;
};

class JSONSchemaTask : public BaseExecutorTask {
public:
	JSONSchemaTask(TaskExecutor &executor, AutoDetectState &auto_detect_state, JSONStructureNode &node_p,
	               const idx_t file_idx_start_p, const idx_t file_idx_end_p)
	    : BaseExecutorTask(executor), auto_detect_state(auto_detect_state), node(node_p),
	      file_idx_start(file_idx_start_p), file_idx_end(file_idx_end_p),
	      allocator(BufferAllocator::Get(auto_detect_state.context)), string_vector(LogicalType::VARCHAR) {
	}

	static idx_t ExecuteInternal(AutoDetectState &auto_detect_state, JSONStructureNode &node, const idx_t file_idx,
	                             ArenaAllocator &allocator, Vector &string_vector, idx_t remaining) {
		auto &context = auto_detect_state.context;
		auto &files = auto_detect_state.files;
		auto &json_data = auto_detect_state.json_data;
		auto json_reader = make_shared_ptr<JSONReader>(context, json_data.options, files[file_idx]);
		if (auto_detect_state.sampled_readers[file_idx]) {
			throw InternalException("Reader for file %llu was already created", file_idx);
		}
		auto &reader = *json_reader;
		auto_detect_state.sampled_readers[file_idx] = std::move(json_reader);

		// the sampled readers keep their first buffer for the scan, so it has to count against the memory limit
		auto &global_allocator = BufferAllocator::Get(context);
		idx_t buffer_capacity = json_data.options.maximum_object_size * 2;
		JSONReaderScanState scan_state(context, global_allocator, buffer_capacity);
		auto &options = json_data.options;
		// Read and detect schema
		idx_t total_tuple_count = 0;
		idx_t total_read_size = 0;

		reader.Initialize(global_allocator, buffer_capacity);
		reader.InitializeScan(scan_state, JSONFileReadType::SCAN_ENTIRE_FILE);

		auto file_size = reader.GetFileHandle().GetHandle().GetFileSize();
		while (remaining != 0) {
			allocator.Reset();
			auto buffer_offset_before = scan_state.buffer_offset;
			auto read_count = reader.Scan(scan_state);
			if (read_count == 0) {
				break;
			}
			total_read_size += scan_state.buffer_offset - buffer_offset_before;
			total_tuple_count += read_count;

			const auto next = MinValue<idx_t>(read_count, remaining);
			for (idx_t i = 0; i < next; i++) {
				const auto &val = scan_state.values[i];
				if (val) {
					JSONStructure::ExtractStructure(val, node, true, options.geojson.value_or(false));
				}
			}
			remaining -= next;
			if (!node.ContainsVarchar()) { // Can't refine non-VARCHAR types
				continue;
			}
			// options.timestamp_format is only ever set from the user-supplied timestampformat= parameter,
			// so non-emptiness is exactly "the user specified a timestamp format"
			const bool user_specified_timestamp_format = !options.timestamp_format.empty();
			node.InitializeCandidateTypes(options.max_depth, options.convert_strings_to_integers,
			                              user_specified_timestamp_format);
			node.RefineCandidateTypes(scan_state.values, next, string_vector, allocator,
			                          auto_detect_state.date_format_map, user_specified_timestamp_format);
		}
		// release the current buffer, otherwise the sampled reader keeps it alive after the bind
		scan_state.ClearBufferHandle();
		auto_detect_state.total_file_size += file_size;
		auto_detect_state.bytes_scanned += total_read_size;
		auto_detect_state.tuples_scanned += total_tuple_count;
		++auto_detect_state.files_scanned;

		return remaining;
	}

	void ExecuteTask() override {
		auto &options = auto_detect_state.json_data.options;
		for (idx_t file_idx = file_idx_start; file_idx < file_idx_end; file_idx++) {
			ExecuteInternal(auto_detect_state, node, file_idx, allocator, string_vector, options.sample_size);
		}
	}

	string TaskType() const override {
		return "JSONSchemaTask";
	}

private:
	AutoDetectState &auto_detect_state;
	JSONStructureNode &node;
	const idx_t file_idx_start;
	const idx_t file_idx_end;

	ArenaAllocator allocator;
	Vector string_vector;
};

//! A GeoJSON Feature: {"type": "Feature", "geometry": ..., "properties": {...}}, optionally with "id" and "bbox".
//! Only these members are allowed, so that ordinary objects that happen to have a "geometry" are not unnested.
static bool IsGeoJSONFeatureType(const LogicalType &type) {
	if (type.id() != LogicalTypeId::STRUCT) {
		return false;
	}
	bool has_type = false;
	bool has_geometry = false;
	bool has_properties = false;
	for (const auto &child : StructType::GetChildTypes(type)) {
		if (child.first == "type") {
			has_type = true;
		} else if (child.first == "geometry") {
			has_geometry = true;
		} else if (child.first == "properties") {
			has_properties = true;
		} else if (child.first != "id" && child.first != "bbox") {
			return false;
		}
	}
	return has_type && has_geometry && has_properties;
}

//! Flatten a Feature into columns: the geometry and the "id"/"bbox" members come from the Feature itself, and
//! every member of "properties" becomes a column of its own.
//! The Feature's own members are emitted first, so that a property of the same name is the one that gets
//! deduplicated by the usual column name collision handling.
static void UnnestGeoJSONFeature(const LogicalType &type, const bool ignore_errors, vector<LogicalType> &return_types,
                                 vector<Identifier> &names, vector<JSONFeatureColumn> &feature_columns) {
	const auto add_column = [&](const Identifier &name, const LogicalType &column_type, const bool from_properties,
	                            string key) {
		names.emplace_back(name);
		return_types.emplace_back(RemoveDuplicateStructKeys(column_type, ignore_errors));
		feature_columns.push_back(JSONFeatureColumn {from_properties, std::move(key)});
	};

	optional_ptr<const LogicalType> geometry_type;
	optional_ptr<const LogicalType> id_type;
	optional_ptr<const LogicalType> bbox_type;
	optional_ptr<const LogicalType> properties_type;
	for (const auto &child : StructType::GetChildTypes(type)) {
		if (child.first == "geometry") {
			geometry_type = child.second;
		} else if (child.first == "id") {
			id_type = child.second;
		} else if (child.first == "bbox") {
			bbox_type = child.second;
		} else if (child.first == "properties") {
			properties_type = child.second;
		}
	}
	D_ASSERT(geometry_type && properties_type);

	add_column("geometry", *geometry_type, false, "geometry");
	if (id_type) {
		add_column("id", *id_type, false, "id");
	}
	if (bbox_type) {
		add_column("bbox", *bbox_type, false, "bbox");
	}
	if (properties_type->id() == LogicalTypeId::STRUCT) {
		for (const auto &property : StructType::GetChildTypes(*properties_type)) {
			add_column(property.first, property.second, true, property.first.GetIdentifierName());
		}
	} else {
		// Properties were not inferred as a struct (e.g. they were too heterogeneous and became a MAP), so we
		// cannot give each one its own column - keep them together instead
		add_column("properties", *properties_type, false, "properties");
	}
}

//! COPY ... FROM takes its columns from the target table, so their names decide what is read from the Feature
//! itself and what is looked up in its properties. Only members the Features actually have are bound to the
//! Feature, so that e.g. a column named "id" reads an "id" property when there is no Feature-level id - which is
//! where auto-detection would have found it too.
static void BindGeoJSONFeatureColumns(const LogicalType &type, const vector<Identifier> &names,
                                      vector<JSONFeatureColumn> &feature_columns) {
	case_insensitive_set_t feature_members;
	for (const auto &child : StructType::GetChildTypes(type)) {
		if (child.first == "geometry" || child.first == "id" || child.first == "bbox") {
			feature_members.insert(child.first.GetIdentifierName());
		}
	}
	for (const auto &name : names) {
		const auto from_properties = feature_members.find(name.GetIdentifierName()) == feature_members.end();
		feature_columns.push_back(JSONFeatureColumn {from_properties, name.GetIdentifierName()});
	}
}

unique_ptr<JSONStructureNode> JSONScan::DetectStructure(ClientContext &context, JSONScanData &json_data,
                                                        const vector<OpenFileInfo> &files,
                                                        vector<shared_ptr<JSONReader>> &sampled_readers) {
	MutableDateFormatMap date_format_map(*json_data.date_format_map);
	auto &options = json_data.options;
	auto node = make_uniq<JSONStructureNode>(options.case_insensitive_keys);
	auto file_count = MinValue<idx_t>(options.maximum_sample_files, files.size());
	sampled_readers.resize(files.empty() ? 0 : files.size());

	AutoDetectState auto_detect_state(context, json_data, files, sampled_readers, date_format_map);
	const auto num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	const auto files_per_task = (file_count + num_threads - 1) / num_threads;
	const auto num_tasks = (file_count + files_per_task - 1) / files_per_task;
	vector<JSONStructureNode> task_nodes;
	task_nodes.reserve(num_tasks);
	for (idx_t task_idx = 0; task_idx < num_tasks; task_idx++) {
		task_nodes.emplace_back(options.case_insensitive_keys);
	}

	// Same idea as in union_by_name.hpp
	TaskExecutor executor(context);
	for (idx_t task_idx = 0; task_idx < num_tasks; task_idx++) {
		const auto file_idx_start = task_idx * files_per_task;
		const auto file_idx_end = MinValue(file_idx_start + files_per_task, file_count);
		auto task =
		    make_uniq<JSONSchemaTask>(executor, auto_detect_state, task_nodes[task_idx], file_idx_start, file_idx_end);
		executor.ScheduleTask(std::move(task));
	}
	executor.WorkOnTasks();

	// Merge task nodes into one
	for (auto &task_node : task_nodes) {
		JSONStructure::MergeNodes(*node, task_node);
	}

	// set the max threads/estimated per-file cardinality
	if (auto_detect_state.files_scanned > 0 && auto_detect_state.tuples_scanned > 0) {
		auto average_tuple_size =
		    MaxValue<idx_t>(auto_detect_state.bytes_scanned / auto_detect_state.tuples_scanned, 1);
		json_data.estimated_cardinality_per_file = auto_detect_state.total_file_size / average_tuple_size;
		if (auto_detect_state.files_scanned == 1) {
			json_data.max_threads =
			    MaxValue<idx_t>(auto_detect_state.total_file_size / json_data.options.maximum_object_size, 1);
		}
	}
	return node;
}

void JSONScan::StructureToColumns(ClientContext &context, JSONReaderOptions &options, const JSONStructureNode &node,
                                  vector<JSONFeatureColumn> &feature_columns, vector<LogicalType> &return_types,
                                  vector<Identifier> &names) {
	// Convert structure to logical type
	auto type = JSONStructure::StructureToType(context, node, options.max_depth, options.field_appearance_threshold,
	                                           options.map_inference_threshold);

	// Auto-detect record type
	if (options.record_type == JSONRecordType::AUTO_DETECT) {
		if (options.geojson.value_or(false) && IsGeoJSONFeatureType(type)) {
			options.record_type = JSONRecordType::FEATURES;
		} else if (type.id() == LogicalTypeId::STRUCT) {
			options.record_type = JSONRecordType::RECORDS;
		} else {
			options.record_type = JSONRecordType::VALUES;
		}
	}

	if (options.record_type == JSONRecordType::FEATURES) {
		if (names.empty()) {
			UnnestGeoJSONFeature(type, options.ignore_errors, return_types, names, feature_columns);
		} else {
			// COPY - we already have names/types, but we still need to know where each of them is read from
			BindGeoJSONFeatureColumns(type, names, feature_columns);
		}
		return;
	}

	if (!names.empty()) {
		// COPY - we already have names/types
		return;
	}

	// Auto-detect columns
	if (options.record_type == JSONRecordType::RECORDS) {
		if (type.id() == LogicalTypeId::STRUCT) {
			const auto &child_types = StructType::GetChildTypes(type);
			return_types.reserve(child_types.size());
			names.reserve(child_types.size());
			for (auto &child_type : child_types) {
				return_types.emplace_back(RemoveDuplicateStructKeys(child_type.second, options.ignore_errors));
				names.emplace_back(child_type.first);
			}
		} else {
			throw BinderException("json_read expected records, but got non-record JSON instead."
			                      "\n Try setting records='auto' or records='false'.");
		}
	} else {
		D_ASSERT(options.record_type == JSONRecordType::VALUES);
		return_types.emplace_back(RemoveDuplicateStructKeys(type, options.ignore_errors));
		// Newline-delimited bare geometries produce a single GEOMETRY column, which "json" would be a poor name for
		names.emplace_back(type.id() == LogicalTypeId::GEOMETRY ? "geometry" : "json");
	}
}

void JSONScan::AutoDetect(ClientContext &context, JSONScanData &json_data, const vector<OpenFileInfo> &files,
                          vector<shared_ptr<JSONReader>> &sampled_readers, vector<LogicalType> &return_types,
                          vector<Identifier> &names) {
	json_data.record_type_auto_detected = json_data.options.record_type == JSONRecordType::AUTO_DETECT;
	auto node = DetectStructure(context, json_data, files, sampled_readers);
	StructureToColumns(context, json_data.options, *node, json_data.feature_columns, return_types, names);
	if (json_data.keep_structure) {
		// keep the structure around so it can be combined with the structures of other files later on
		json_data.structure = std::move(node);
	}
}

} // namespace duckdb
