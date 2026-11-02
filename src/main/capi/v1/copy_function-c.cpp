#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/capi/capi_internal_table.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/catalog/catalog.hpp"

//----------------------------------------------------------------------------------------------------------------------
// Common Copy Function Info
//----------------------------------------------------------------------------------------------------------------------

namespace duckdb {
namespace {

struct CCopyFunctionInfo : public CopyFunctionInfo {
	~CCopyFunctionInfo() override {
		if (extra_info && delete_callback) {
			delete_callback(extra_info);
		}
		extra_info = nullptr;
		delete_callback = nullptr;
	}

	duckdb_copy_function_bind_t bind_to = nullptr;
	duckdb_copy_function_global_init_t global_init = nullptr;
	duckdb_copy_function_sink_t sink = nullptr;
	duckdb_copy_function_finalize_t finalize = nullptr;

	void *extra_info = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

Value MakeValueFromCopyOptions(const identifier_map_t<vector<Value>> &options) {
	child_list_t<duckdb::Value> option_list;
	for (auto &[option_name, values] : options) {
		// Uppercase the option name, to make it simpler for users
		auto name = StringUtil::Upper(option_name.GetIdentifierName());

		if (values.empty()) {
			// Null!
			option_list.emplace_back(std::move(name), Value());
			continue;
		}
		if (values.size() == 1) {
			// Single value
			option_list.emplace_back(std::move(name), values[0]);
			continue;
		}

		auto is_same_type = true;
		auto first_type = values[0].type();
		for (auto &val : values) {
			if (val.type() != first_type) {
				// Different types, cannot unify
				is_same_type = false;
				break;
			}
		}

		// Is same type: create a list of that type
		if (is_same_type) {
			option_list.emplace_back(std::move(name), Value::LIST(first_type, values));
			continue;
		}

		// Different types: create an unnamed TUPLE
		vector<Value> children;
		for (auto &val : values) {
			children.push_back(val);
		}
		option_list.emplace_back(std::move(name), Value::TUPLE(std::move(children)));
	}

	if (option_list.empty()) {
		// No options
		return Value();
	}

	// Return a struct of all options
	return Value::STRUCT(std::move(option_list));
}

} // namespace
} // namespace duckdb

duckdb_copy_function duckdb_create_copy_function() {
	auto function = new duckdb::CopyFunction("");

	function->function_info = duckdb::make_shared_ptr<duckdb::CCopyFunctionInfo>();

	return reinterpret_cast<duckdb_copy_function>(function);
}

void duckdb_copy_function_set_name(duckdb_copy_function copy_function, const char *name) {
	if (!copy_function || !name) {
		return;
	}
	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);
	copy_function_ref.name = duckdb::Identifier(name);
}

void duckdb_destroy_copy_function(duckdb_copy_function *copy_function) {
	if (copy_function && *copy_function) {
		auto function = reinterpret_cast<duckdb::CopyFunction *>(*copy_function);
		delete function;
		*copy_function = nullptr;
	}
}

void duckdb_copy_function_set_extra_info(duckdb_copy_function function, void *extra_info,
                                         duckdb_delete_callback_t destroy) {
	if (!function) {
		return;
	}
	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(function);
	auto &info = copy_function_ref.function_info->Cast<duckdb::CCopyFunctionInfo>();
	info.extra_info = extra_info;
	info.delete_callback = destroy;
}

//----------------------------------------------------------------------------------------------------------------------
// Copy To Bind
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {
struct CCopyToBindInfo : FunctionData {
	shared_ptr<CopyFunctionInfo> function_info;
	void *bind_data = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;

	unique_ptr<FunctionData> Copy() const override {
		throw InternalException("CCopyToBindInfo cannot be copied");
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CCopyToBindInfo>();
		return bind_data == other.bind_data && delete_callback == other.delete_callback;
	}

	~CCopyToBindInfo() override {
		if (bind_data && delete_callback) {
			delete_callback(bind_data);
		}
		bind_data = nullptr;
		delete_callback = nullptr;
	}
};

struct CCopyFunctionToInternalBindInfo {
	CCopyFunctionToInternalBindInfo(ClientContext &context, CopyFunctionBindInput &input,
	                                const vector<LogicalType> &sql_types, const vector<Identifier> &names,
	                                const CCopyFunctionInfo &function_info)
	    : context(context), input(input), sql_types(sql_types), names(names), function_info(function_info),
	      success(true) {
	}

	ClientContext &context;
	CopyFunctionBindInput &input;
	const vector<LogicalType> &sql_types;
	const vector<Identifier> &names;
	const CCopyFunctionInfo &function_info;
	bool success;
	string error;

	// Supplied by the user
	void *bind_data = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

unique_ptr<FunctionData> CCopyToBind(ClientContext &context, CopyFunctionBindInput &input,
                                     const vector<Identifier> &names, const vector<LogicalType> &sql_types) {
	auto &info = input.function_info->Cast<CCopyFunctionInfo>();

	auto result = make_uniq<CCopyToBindInfo>();
	result->function_info = input.function_info;

	if (info.bind_to) {
		// Call the user-defined bind function
		CCopyFunctionToInternalBindInfo bind_info(context, input, sql_types, names, info);
		info.bind_to(reinterpret_cast<duckdb_copy_function_bind_info>(&bind_info));

		// Pass on user bind data to the result
		result->bind_data = bind_info.bind_data;
		result->delete_callback = bind_info.delete_callback;

		if (!bind_info.success) {
			throw BinderException(bind_info.error);
		}
	}
	return std::move(result);
}

} // namespace
} // namespace duckdb

void duckdb_copy_function_set_bind(duckdb_copy_function copy_function, duckdb_copy_function_bind_t bind) {
	if (!copy_function || !bind) {
		return;
	}

	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);
	auto &info = copy_function_ref.function_info->Cast<duckdb::CCopyFunctionInfo>();

	// Set C bind callback
	info.bind_to = bind;
}

void duckdb_copy_function_bind_set_error(duckdb_copy_function_bind_info info, const char *error) {
	if (!info || !error) {
		return;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);

	// Set the error message
	info_ref.error = error;
	info_ref.success = false;
}

void *duckdb_copy_function_bind_get_extra_info(duckdb_copy_function_bind_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);
	return info_ref.function_info.extra_info;
}

duckdb_client_context duckdb_copy_function_bind_get_client_context(duckdb_copy_function_bind_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);
	auto wrapper = new duckdb::CClientContextWrapper(info_ref.context);
	return reinterpret_cast<duckdb_client_context>(wrapper);
}

idx_t duckdb_copy_function_bind_get_column_count(duckdb_copy_function_bind_info info) {
	if (!info) {
		return 0;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);
	return info_ref.sql_types.size();
}

duckdb_logical_type duckdb_copy_function_bind_get_column_type(duckdb_copy_function_bind_info info, idx_t col_idx) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);
	if (col_idx >= info_ref.sql_types.size()) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(info_ref.sql_types[col_idx]));
}

duckdb_value duckdb_copy_function_bind_get_options(duckdb_copy_function_bind_info info) {
	if (!info) {
		return nullptr;
	}

	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);
	auto &options = info_ref.input.info.options;

	// return as struct of options
	auto options_value = duckdb::MakeValueFromCopyOptions(options);
	return reinterpret_cast<duckdb_value>(new duckdb::Value(options_value));
}

void duckdb_copy_function_bind_set_bind_data(duckdb_copy_function_bind_info info, void *bind_data,
                                             duckdb_delete_callback_t destructor) {
	if (!info) {
		return;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyFunctionToInternalBindInfo *>(info);

	// Store the bind data and destructor
	info_ref.bind_data = bind_data;
	info_ref.delete_callback = destructor;
}

//----------------------------------------------------------------------------------------------------------------------
// Copy To Global Initialize
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {

struct CCopyToGlobalState : GlobalFunctionData {
	void *global_state = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;

	~CCopyToGlobalState() override {
		if (global_state && delete_callback) {
			delete_callback(global_state);
		}
		global_state = nullptr;
		delete_callback = nullptr;
	}
};

struct CCopyToGlobalInitInfo {
	CCopyToGlobalInitInfo(ClientContext &context, FunctionData &bind_data, const string &file_path)
	    : context(context), bind_data(bind_data), file_path(file_path) {
	}

	ClientContext &context;
	FunctionData &bind_data;
	const string &file_path;

	string error;
	bool success = true;

	void *global_state = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

unique_ptr<GlobalFunctionData> CCopyToGlobalInit(ClientContext &context, FunctionData &bind_data,
                                                 const string &file_path) {
	auto &bind_info = bind_data.Cast<CCopyToBindInfo>();
	auto &function_info = bind_info.function_info->Cast<CCopyFunctionInfo>();

	auto result = make_uniq<CCopyToGlobalState>();

	if (function_info.global_init) {
		// Call the user-defined global init function
		CCopyToGlobalInitInfo global_init_info(context, bind_data, file_path);
		function_info.global_init(reinterpret_cast<duckdb_copy_function_global_init_info>(&global_init_info));

		// Pass on user global state to the result
		result->global_state = global_init_info.global_state;
		result->delete_callback = global_init_info.delete_callback;

		if (!global_init_info.success) {
			throw InvalidInputException(global_init_info.error);
		}
	}

	return std::move(result);
}

} // namespace
} // namespace duckdb

void duckdb_copy_function_set_global_init(duckdb_copy_function copy_function, duckdb_copy_function_global_init_t init) {
	if (!copy_function || !init) {
		return;
	}
	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);
	auto &info = copy_function_ref.function_info->Cast<duckdb::CCopyFunctionInfo>();

	// Set C global init callback
	info.global_init = init;
}

void duckdb_copy_function_global_init_set_error(duckdb_copy_function_global_init_info info, const char *error) {
	if (!info || !error) {
		return;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToGlobalInitInfo *>(info);

	// Set the error message
	info_ref.error = error;
	info_ref.success = false;
}

void *duckdb_copy_function_global_init_get_extra_info(duckdb_copy_function_global_init_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToGlobalInitInfo *>(info);
	return info_ref.bind_data.Cast<duckdb::CCopyToBindInfo>()
	    .function_info->Cast<duckdb::CCopyFunctionInfo>()
	    .extra_info;
}

duckdb_client_context duckdb_copy_function_global_init_get_client_context(duckdb_copy_function_global_init_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToGlobalInitInfo *>(info);
	auto wrapper = new duckdb::CClientContextWrapper(info_ref.context);
	return reinterpret_cast<duckdb_client_context>(wrapper);
}

void *duckdb_copy_function_global_init_get_bind_data(duckdb_copy_function_global_init_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToGlobalInitInfo *>(info);
	auto &bind_info = info_ref.bind_data.Cast<duckdb::CCopyToBindInfo>();

	return bind_info.bind_data;
}

void duckdb_copy_function_global_init_set_global_state(duckdb_copy_function_global_init_info info, void *global_state,
                                                       duckdb_delete_callback_t destructor) {
	if (!info) {
		return;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToGlobalInitInfo *>(info);
	info_ref.global_state = global_state;
	info_ref.delete_callback = destructor;
}

const char *duckdb_copy_function_global_init_get_file_path(duckdb_copy_function_global_init_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToGlobalInitInfo *>(info);
	return info_ref.file_path.c_str();
}

//----------------------------------------------------------------------------------------------------------------------
// Copy To Local Initialize
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {

unique_ptr<LocalFunctionData> CCopyToLocalInit(ExecutionContext &context, FunctionData &bind_data) {
	// This isnt exposed to the C-API yet, so we just return empty local function data
	return make_uniq<LocalFunctionData>();
}

} // namespace
} // namespace duckdb
//----------------------------------------------------------------------------------------------------------------------
// Copy To Sink
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {

struct CCopyToSinkInfo {
	CCopyToSinkInfo(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate)
	    : context(context), bind_data(bind_data), gstate(gstate) {
	}

	ClientContext &context;
	FunctionData &bind_data;
	GlobalFunctionData &gstate;
	string error;
	bool success = true;
};

void CCopyToSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                 LocalFunctionData &lstate, DataChunk &input) {
	auto &bind_info = bind_data.Cast<CCopyToBindInfo>();
	auto &function_info = bind_info.function_info->Cast<CCopyFunctionInfo>();

	// Flatten input (we dont support compressed execution yet!)
	// TODO: Dont flatten!
	input.Flatten();
	for (auto &vector : input.data) {
		PrepareCAPIVector(vector);
	}

	CCopyToSinkInfo copy_to_sink_info(context.client, bind_data, gstate);

	// Sink is required!
	function_info.sink(reinterpret_cast<duckdb_copy_function_sink_info>(&copy_to_sink_info),
	                   reinterpret_cast<duckdb_data_chunk>(&input));

	if (!copy_to_sink_info.success) {
		throw InvalidInputException(copy_to_sink_info.error);
	}
}

} // namespace
} // namespace duckdb

void duckdb_copy_function_set_sink(duckdb_copy_function copy_function, duckdb_copy_function_sink_t function) {
	if (!copy_function || !function) {
		return;
	}
	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);
	auto &info = copy_function_ref.function_info->Cast<duckdb::CCopyFunctionInfo>();

	// Set C sink callback
	info.sink = function;
}

void duckdb_copy_function_sink_set_error(duckdb_copy_function_sink_info info, const char *error) {
	if (!info || !error) {
		return;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToSinkInfo *>(info);
	// Set the error message
	info_ref.error = error;
	info_ref.success = false;
}

void *duckdb_copy_function_sink_get_extra_info(duckdb_copy_function_sink_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToSinkInfo *>(info);
	return info_ref.bind_data.Cast<duckdb::CCopyToBindInfo>()
	    .function_info->Cast<duckdb::CCopyFunctionInfo>()
	    .extra_info;
}

duckdb_client_context duckdb_copy_function_sink_get_client_context(duckdb_copy_function_sink_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToSinkInfo *>(info);
	auto wrapper = new duckdb::CClientContextWrapper(info_ref.context);
	return reinterpret_cast<duckdb_client_context>(wrapper);
}

void *duckdb_copy_function_sink_get_bind_data(duckdb_copy_function_sink_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToSinkInfo *>(info);
	auto &bind_info = info_ref.bind_data.Cast<duckdb::CCopyToBindInfo>();

	return bind_info.bind_data;
}

void *duckdb_copy_function_sink_get_global_state(duckdb_copy_function_sink_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToSinkInfo *>(info);
	auto &gstate = info_ref.gstate.Cast<duckdb::CCopyToGlobalState>();

	return gstate.global_state;
}

//----------------------------------------------------------------------------------------------------------------------
// Copy To Combine
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {

void CCopyToCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                    LocalFunctionData &lstate) {
	// Do nothing for now (this isnt exposed to the C-API yet)
}

} // namespace
} // namespace duckdb

//----------------------------------------------------------------------------------------------------------------------
// Copy To Finalize
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {

struct CCopyToFinalizeInfo {
	CCopyToFinalizeInfo(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate)
	    : context(context), bind_data(bind_data), gstate(gstate) {
	}

	ClientContext &context;
	FunctionData &bind_data;
	GlobalFunctionData &gstate;

	string error;
	bool success = true;
};

void CCopyToFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &bind_info = bind_data.Cast<CCopyToBindInfo>();
	auto &function_info = bind_info.function_info->Cast<CCopyFunctionInfo>();

	// Finalize is optional
	if (function_info.finalize) {
		CCopyToFinalizeInfo copy_to_finalize_info(context, bind_data, gstate);
		function_info.finalize(reinterpret_cast<duckdb_copy_function_finalize_info>(&copy_to_finalize_info));

		if (!copy_to_finalize_info.success) {
			throw InvalidInputException(copy_to_finalize_info.error);
		}
	}
}

} // namespace
} // namespace duckdb

void duckdb_copy_function_set_finalize(duckdb_copy_function copy_function, duckdb_copy_function_finalize_t finalize) {
	if (!copy_function || !finalize) {
		return;
	}

	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);
	auto &info = copy_function_ref.function_info->Cast<duckdb::CCopyFunctionInfo>();

	// Set C finalize callback
	info.finalize = finalize;
}

void duckdb_copy_function_finalize_set_error(duckdb_copy_function_finalize_info info, const char *error) {
	if (!info || !error) {
		return;
	}

	auto &info_ref = *reinterpret_cast<duckdb::CCopyToFinalizeInfo *>(info);
	// Set the error message
	info_ref.error = error;
	info_ref.success = false;
}

void *duckdb_copy_function_finalize_get_extra_info(duckdb_copy_function_finalize_info info) {
	if (!info) {
		return nullptr;
	}

	auto &info_ref = *reinterpret_cast<duckdb::CCopyToFinalizeInfo *>(info);
	return info_ref.bind_data.Cast<duckdb::CCopyToBindInfo>()
	    .function_info->Cast<duckdb::CCopyFunctionInfo>()
	    .extra_info;
}

duckdb_client_context duckdb_copy_function_finalize_get_client_context(duckdb_copy_function_finalize_info info) {
	if (!info) {
		return nullptr;
	}
	auto &info_ref = *reinterpret_cast<duckdb::CCopyToFinalizeInfo *>(info);
	auto wrapper = new duckdb::CClientContextWrapper(info_ref.context);
	return reinterpret_cast<duckdb_client_context>(wrapper);
}

void *duckdb_copy_function_finalize_get_bind_data(duckdb_copy_function_finalize_info info) {
	if (!info) {
		return nullptr;
	}

	auto &info_ref = *reinterpret_cast<duckdb::CCopyToFinalizeInfo *>(info);
	auto &bind_info = info_ref.bind_data.Cast<duckdb::CCopyToBindInfo>();
	return bind_info.bind_data;
}

void *duckdb_copy_function_finalize_get_global_state(duckdb_copy_function_finalize_info info) {
	if (!info) {
		return nullptr;
	}

	auto &info_ref = *reinterpret_cast<duckdb::CCopyToFinalizeInfo *>(info);
	auto &gstate = info_ref.gstate.Cast<duckdb::CCopyToGlobalState>();
	return gstate.global_state;
}

//----------------------------------------------------------------------------------------------------------------------
// Copy FROM
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
namespace {

unique_ptr<FunctionData> CCopyFromBind(ClientContext &context, CopyFromFunctionBindInput &info,
                                       vector<Identifier> &expected_names, vector<LogicalType> &expected_types) {
	auto &tf_info = info.tf.function_info->Cast<CTableFunctionInfo>();
	auto result = make_uniq<CTableBindData>(tf_info);

	named_argument_map_t named_parameters;

	// Turn all options into named parameters
	for (auto opt : info.info.options) {
		auto option_schema = info.tf.GetSignature().GetTypedKwargs();
		if (!option_schema || !option_schema->Find(Identifier(opt.first))) {
			// Option not found in the table function's named parameters
			throw BinderException("'%s' is not a supported option for copy function '%s'", opt.first.c_str(),
			                      info.tf.name.c_str());
		}

		// Try to convert a list of values into a single Value, either by extracting or unifying into a list
		Value param_value;
		if (opt.second.empty()) {
			continue;
		}
		if (opt.second.size() == 1) {
			param_value = opt.second[0];
		} else {
			auto first_type = opt.second[0].type();
			auto is_same_type = true;
			for (auto &val : opt.second) {
				if (val.type() != first_type) {
					is_same_type = false;
					break;
				}
			}
			if (is_same_type) {
				param_value = Value::LIST(first_type, opt.second);
			} else {
				throw BinderException("Cannot pass multiple values of different types for copy option '%s'",
				                      opt.first.c_str());
			}
		}

		// Assign the option as a named parameter
		named_parameters[Identifier(opt.first)] = param_value;
	}

	// Also pass file path as a regular parameter
	vector<Value> parameters;
	parameters.push_back(Value(info.info.file_path));

	// Now bind, using the normal table function bind mechanism
	CTableInternalBindInfo bind_info(context, parameters, named_parameters, expected_types, expected_names, *result,
	                                 tf_info);
	tf_info.bind(reinterpret_cast<duckdb_bind_info>(&bind_info));
	if (!bind_info.success) {
		throw BinderException(bind_info.error);
	}

	return std::move(result);
}

} // namespace
} // namespace duckdb

void duckdb_copy_function_set_copy_from_function(duckdb_copy_function copy_function,
                                                 duckdb_table_function table_function) {
	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);
	if (!copy_function || !table_function) {
		return;
	}
	auto &tf = *reinterpret_cast<duckdb::TableFunction *>(table_function);
	auto &tf_info = tf.function_info->Cast<duckdb::CTableFunctionInfo>();

	if (tf.name.empty()) {
		// Take the name from the copy function if not set
		tf.name = copy_function_ref.name;
	}

	if (!tf_info.bind || !tf_info.init || !tf_info.function) {
		return;
	}
	for (auto &param : tf.GetSignature().GetParameters()) {
		// a variadic parameter carries no type of its own
		if (param.IsVariadic()) {
			continue;
		}
		if (duckdb::TypeVisitor::Contains(param.GetType(), duckdb::LogicalTypeId::INVALID)) {
			return;
		}
	}
	auto option_schema = tf.GetSignature().GetTypedKwargs();
	if (option_schema) {
		for (auto &option : option_schema->GetOptions()) {
			if (duckdb::TypeVisitor::Contains(option.type, duckdb::LogicalTypeId::INVALID)) {
				return;
			}
		}
	}

	// Set the bind callback to mark this as a "copy from" capable function
	copy_function_ref.copy_from_bind = duckdb::CCopyFromBind;
	copy_function_ref.copy_from_function = tf;
}

idx_t duckdb_table_function_bind_get_result_column_count(duckdb_bind_info bind_info) {
	if (!bind_info) {
		return 0;
	}
	auto &bind_info_ref = *reinterpret_cast<duckdb::CTableInternalBindInfo *>(bind_info);
	return bind_info_ref.return_types.size();
}

duckdb_logical_type duckdb_table_function_bind_get_result_column_type(duckdb_bind_info bind_info, idx_t col_idx) {
	if (!bind_info) {
		return nullptr;
	}
	auto &bind_info_ref = *reinterpret_cast<duckdb::CTableInternalBindInfo *>(bind_info);
	if (col_idx >= bind_info_ref.return_types.size()) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(bind_info_ref.return_types[col_idx]));
}

const char *duckdb_table_function_bind_get_result_column_name(duckdb_bind_info bind_info, idx_t col_idx) {
	if (!bind_info) {
		return nullptr;
	}
	auto &bind_info_ref = *reinterpret_cast<duckdb::CTableInternalBindInfo *>(bind_info);
	if (col_idx >= bind_info_ref.names.size()) {
		return nullptr;
	}
	return bind_info_ref.names[col_idx].c_str();
}

//----------------------------------------------------------------------------------------------------------------------
// Register
//----------------------------------------------------------------------------------------------------------------------
duckdb_state duckdb_register_copy_function(duckdb_connection connection, duckdb_copy_function copy_function) {
	if (!connection || !copy_function) {
		return DuckDBError;
	}

	auto &copy_function_ref = *reinterpret_cast<duckdb::CopyFunction *>(copy_function);

	// Check that the copy function has a valid name
	if (copy_function_ref.name.empty()) {
		return DuckDBError;
	}

	auto &info = copy_function_ref.function_info->Cast<duckdb::CCopyFunctionInfo>();

	auto is_copy_to = false;
	auto is_copy_from = copy_function_ref.copy_from_bind != nullptr;

	if (info.sink) {
		// Set the copy function callbacks
		is_copy_to = true;
		copy_function_ref.copy_to_bind = duckdb::CCopyToBind;
		copy_function_ref.copy_to_initialize_global = duckdb::CCopyToGlobalInit;
		copy_function_ref.copy_to_initialize_local = duckdb::CCopyToLocalInit;
		copy_function_ref.copy_to_sink = duckdb::CCopyToSink;
		copy_function_ref.copy_to_combine = duckdb::CCopyToCombine;
		copy_function_ref.copy_to_finalize = duckdb::CCopyToFinalize;
	}

	if (!is_copy_to && !is_copy_from) {
		// At least one of copy to or copy from must be implemented
		return DuckDBError;
	}

	auto &conn = *reinterpret_cast<duckdb::Connection *>(connection);
	try {
		conn.context->RunFunctionInTransaction([&]() {
			auto &catalog = duckdb::Catalog::GetSystemCatalog(*conn.context);
			duckdb::CreateCopyFunctionInfo cp_info(copy_function_ref);
			cp_info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			catalog.CreateCopyFunction(*conn.context, cp_info);
		});
	} catch (...) { // LCOV_EXCL_START
		return DuckDBError;
	} // LCOV_EXCL_STOP
	return DuckDBSuccess;
}
