#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "core_functions/scalar/struct_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

//! The result field that every argument after the struct writes to
struct StructUpdateBindData : public FunctionData {
	StructUpdateBindData(LogicalType stype_p, vector<idx_t> field_indexes_p)
	    : stype(std::move(stype_p)), field_indexes(std::move(field_indexes_p)) {
	}

	LogicalType stype;
	//! Indexed by argument index minus one
	vector<idx_t> field_indexes;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StructUpdateBindData>(stype, field_indexes);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<StructUpdateBindData>();
		return stype == other.stype && field_indexes == other.field_indexes;
	}

	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const BoundScalarFunction &function) {
		auto &info = bind_data->Cast<StructUpdateBindData>();
		serializer.WriteProperty(100, "variable_return_type", info.stype);
		serializer.WritePropertyWithDefault(101, "field_indexes", info.field_indexes);
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &bound_function) {
		auto stype = deserializer.ReadProperty<LogicalType>(100, "variable_return_type");
		auto field_indexes = deserializer.ReadPropertyWithDefault<vector<idx_t>>(101, "field_indexes");
		return make_uniq<StructUpdateBindData>(std::move(stype), std::move(field_indexes));
	}
};

//! For every result field, the argument that provides it - fields without one keep the value of the source struct
static vector<optional_idx> FieldArguments(const StructUpdateBindData &info, idx_t field_count) {
	vector<optional_idx> field_arguments(field_count);
	for (idx_t arg_idx = 1; arg_idx <= info.field_indexes.size(); arg_idx++) {
		field_arguments[info.field_indexes[arg_idx - 1]] = arg_idx;
	}
	return field_arguments;
}

static void StructUpdateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &starting_vec = args.data[0];
	starting_vec.Verify();

	auto &starting_child_entries = StructVector::GetEntries(starting_vec);
	auto &result_child_entries = StructVector::GetEntries(result);
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<StructUpdateBindData>();
	auto field_arguments = FieldArguments(info, result_child_entries.size());

	for (idx_t field_idx = 0; field_idx < result_child_entries.size(); field_idx++) {
		auto &argument = field_arguments[field_idx];
		if (argument.IsValid()) {
			result_child_entries[field_idx].Reference(args.data[argument.GetIndex()]);
		} else {
			result_child_entries[field_idx].Reference(starting_child_entries[field_idx]);
		}
	}
}

static unique_ptr<FunctionData> StructUpdateBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		throw InvalidInputException("Missing required arguments for struct_update function.");
	}
	if (LogicalTypeId::STRUCT != arguments[0]->GetReturnType().id()) {
		throw InvalidInputException("The first argument to struct_update must be a STRUCT");
	}
	if (arguments.size() < 2) {
		throw InvalidInputException("Can't update nothing into a STRUCT");
	}

	child_list_t<LogicalType> new_children;
	auto &existing_children = StructType::GetChildTypes(arguments[0]->GetReturnType());

	auto incoming_children = identifier_tree_t<idx_t>();
	auto is_new_field = vector<bool>(arguments.size(), true);
	vector<idx_t> field_indexes(arguments.size() - 1);

	// Record the names of the fields that are updated
	for (idx_t arg_idx = 1; arg_idx < arguments.size(); arg_idx++) {
		incoming_children.emplace(input.GetArgumentName(arg_idx), arg_idx);
	}

	for (idx_t field_idx = 0; field_idx < existing_children.size(); field_idx++) {
		auto &existing_child = existing_children[field_idx];
		auto update = incoming_children.find(existing_child.first);
		if (update == incoming_children.end()) {
			// No update provided for the named value
			new_children.push_back(make_pair(existing_child.first, existing_child.second));
		} else {
			// Update the struct with the new data of the same name
			auto arg_idx = update->second;
			new_children.emplace_back(make_pair(input.GetArgumentName(arg_idx), arguments[arg_idx]->GetReturnType()));
			is_new_field[arg_idx] = false;
			field_indexes[arg_idx - 1] = field_idx;
		}
	}

	// Loop through the additional arguments (name/value pairs)
	for (idx_t arg_idx = 1; arg_idx < arguments.size(); arg_idx++) {
		if (is_new_field[arg_idx]) {
			field_indexes[arg_idx - 1] = new_children.size();
			new_children.emplace_back(make_pair(input.GetArgumentName(arg_idx), arguments[arg_idx]->GetReturnType()));
		}
	}

	bound_function.SetReturnType(LogicalType::STRUCT(new_children));
	return make_uniq<StructUpdateBindData>(bound_function.GetReturnType(), std::move(field_indexes));
}

static unique_ptr<BaseStatistics> StructUpdateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto &info = input.bind_data->Cast<StructUpdateBindData>();

	auto new_stats = StructStats::CreateUnknown(expr.GetReturnType());
	new_stats.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);

	auto existing_stats = StructStats::GetChildStats(child_stats[0]);
	auto field_count = StructType::GetChildCount(expr.GetReturnType());
	auto field_arguments = FieldArguments(info, field_count);
	for (idx_t field_idx = 0; field_idx < field_count; field_idx++) {
		auto &argument = field_arguments[field_idx];
		if (argument.IsValid()) {
			StructStats::SetChildStats(new_stats, field_idx, child_stats[argument.GetIndex()]);
		} else {
			StructStats::SetChildStats(new_stats, field_idx, existing_stats[field_idx]);
		}
	}
	return new_stats.ToUnique();
}

static unique_ptr<ParsedExpression> StructUpdateUnbind(FunctionUnbindInput &input) {
	auto &info = input.expression.BindInfo()->Cast<StructUpdateBindData>();
	vector<FunctionArgument> arguments;
	for (idx_t i = 0; i < input.children.size(); i++) {
		auto name = i == 0 ? Identifier() : StructType::GetChildName(info.stype, info.field_indexes[i - 1]);
		arguments.emplace_back(std::move(name), std::move(input.children[i]));
	}
	return make_uniq<FunctionExpression>(input.expression.Function().GetDefinition()->GetQualifiedName(),
	                                     std::move(arguments));
}

ScalarFunction StructUpdateFun::GetFunction() {
	ScalarFunction fun({}, LogicalTypeId::STRUCT, StructUpdateFunction, StructUpdateBind, StructUpdateStats);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.GetSignature().AddParameter("struct", LogicalType::ANY).AddKwargs("kwargs", LogicalType::ANY);
	fun.GetProperties().SetRequiresExpressionNames(true);
	fun.SetUnbindCallback(StructUpdateUnbind);
	fun.SetSerializeCallback(StructUpdateBindData::Serialize);
	fun.SetDeserializeCallback(StructUpdateBindData::Deserialize);
	return fun;
}

} // namespace duckdb
