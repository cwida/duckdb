#include "duckdb/common/helper.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"
#include "json_schema_inference.hpp"
#include "json_transform.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace duckdb {

//! Deduplicate a name against the ones we already have: drop it if "ignore_errors", otherwise rename it with an
//! incrementing suffix until it no longer collides. Returns whether the name should be kept.
static bool ResolveDuplicateName(identifier_map_t<idx_t> &name_collision_count, Identifier &name,
                                 const bool ignore_errors, const bool rename_duplicates = true) {
	if (name_collision_count.find(name) == name_collision_count.end()) {
		name_collision_count[name] = 0;
		return true;
	}
	if (ignore_errors) {
		return false;
	}
	if (!rename_duplicates) {
		throw NotImplementedException("Duplicate name %s in struct auto-detected in JSON, try ignore_errors=true",
		                              name);
	}
	while (name_collision_count.find(name) != name_collision_count.end()) {
		name_collision_count[name] += 1;
		name = Identifier(name.GetIdentifierName() + "_" + to_string(name_collision_count[name]));
	}
	name_collision_count[name] = 0;
	return true;
}

static inline LogicalType RemoveDuplicateStructKeys(const LogicalType &type, const bool ignore_errors,
                                                    const bool rename_duplicates = true) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		identifier_map_t<idx_t> name_collision_count;
		child_list_t<LogicalType> child_types;
		for (auto &child_type : StructType::GetChildTypes(type)) {
			auto child_name = child_type.first;
			if (!ResolveDuplicateName(name_collision_count, child_name, ignore_errors, rename_duplicates)) {
				continue;
			}
			child_types.emplace_back(child_name,
			                         RemoveDuplicateStructKeys(child_type.second, ignore_errors, rename_duplicates));
		}
		return LogicalType::STRUCT(child_types);
	}
	case LogicalTypeId::MAP:
		return LogicalType::MAP(RemoveDuplicateStructKeys(MapType::KeyType(type), ignore_errors, false),
		                        RemoveDuplicateStructKeys(MapType::ValueType(type), ignore_errors, false));
	case LogicalTypeId::LIST:
		return LogicalType::LIST(
		    RemoveDuplicateStructKeys(ListType::GetChildType(type), ignore_errors, rename_duplicates));
	default:
		return type;
	}
}

//! Build the JSON keys that a (nested) type is read from, mirroring RemoveDuplicateStructKeys so the tree lines up
//! with the renamed struct fields. Dropped duplicates (ignore_errors) are dropped here as well.
static shared_ptr<const JSONKeyTree> BuildKeyTree(const JSONStructureNode &node, const bool ignore_errors) {
	if (node.descriptions.size() != 1) {
		return nullptr;
	}
	auto &desc = node.descriptions[0];
	switch (desc.type) {
	case LogicalTypeId::STRUCT: {
		auto result = make_shared_ptr<JSONKeyTree>();
		identifier_map_t<idx_t> name_collision_count;
		for (auto &child : desc.children) {
			auto child_name = Identifier(*child.key);
			if (!ResolveDuplicateName(name_collision_count, child_name, ignore_errors)) {
				continue;
			}
			result->keys.push_back(*child.key);
			result->children.push_back(BuildKeyTree(child, ignore_errors));
		}
		return result;
	}
	case LogicalTypeId::LIST: {
		auto result = make_shared_ptr<JSONKeyTree>();
		if (!desc.children.empty()) {
			result->children.push_back(BuildKeyTree(desc.children[0], ignore_errors));
		}
		return result;
	}
	default:
		return nullptr;
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
                                  vector<Identifier> &names, vector<shared_ptr<const JSONKeyTree>> &key_trees) {
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
			D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].children.size() == child_types.size());
			const auto &child_nodes = node.descriptions[0].children;
			return_types.reserve(child_types.size());
			names.reserve(child_types.size());
			key_trees.reserve(child_types.size());
			for (idx_t i = 0; i < child_types.size(); i++) {
				auto &child_type = child_types[i];
				return_types.emplace_back(RemoveDuplicateStructKeys(child_type.second, options.ignore_errors));
				names.emplace_back(child_type.first);
				key_trees.emplace_back(BuildKeyTree(child_nodes[i], options.ignore_errors));
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
		key_trees.emplace_back(BuildKeyTree(node, options.ignore_errors));
	}
}

void JSONScan::AutoDetect(ClientContext &context, JSONScanData &json_data, const vector<OpenFileInfo> &files,
                          vector<shared_ptr<JSONReader>> &sampled_readers, vector<LogicalType> &return_types,
                          vector<Identifier> &names) {
	json_data.record_type_auto_detected = json_data.options.record_type == JSONRecordType::AUTO_DETECT;
	auto node = DetectStructure(context, json_data, files, sampled_readers);
	StructureToColumns(context, json_data.options, *node, json_data.feature_columns, return_types, names,
	                   json_data.key_trees);
	if (json_data.keep_structure) {
		// keep the structure around so it can be combined with the structures of other files later on
		json_data.structure = std::move(node);
	}
}

} // namespace duckdb
