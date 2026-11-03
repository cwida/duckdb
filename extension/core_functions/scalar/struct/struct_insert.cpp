#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "core_functions/scalar/struct_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

static void StructInsertFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &starting_vec = args.data[0];
	starting_vec.Verify();

	auto &starting_child_entries = StructVector::GetEntries(starting_vec);
	auto &result_child_entries = StructVector::GetEntries(result);

	// Assign the original child entries to the STRUCT.
	for (idx_t i = 0; i < starting_child_entries.size(); i++) {
		auto &starting_child = starting_child_entries[i];
		result_child_entries[i].Reference(starting_child);
	}

	// Assign the new children to the result vector.
	for (idx_t i = 1; i < args.ColumnCount(); i++) {
		result_child_entries[starting_child_entries.size() + i - 1].Reference(args.data[i]);
	}
}

static unique_ptr<FunctionData> StructInsertBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	for (auto &argument : arguments) {
		if (argument->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}
	if (LogicalTypeId::STRUCT != arguments[0]->GetReturnType().id()) {
		throw InvalidInputException("The first argument to struct_insert must be a STRUCT");
	}
	if (arguments.size() < 2) {
		throw InvalidInputException("Can't insert nothing into a STRUCT");
	}

	identifier_set_t name_collision_set;
	child_list_t<LogicalType> new_children;
	auto &existing_children = StructType::GetChildTypes(arguments[0]->GetReturnType());

	for (idx_t i = 0; i < existing_children.size(); i++) {
		auto &child = existing_children[i];
		name_collision_set.insert(child.first);
		new_children.push_back(make_pair(child.first, child.second));
	}

	// Loop through the fields that are inserted
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto &name = input.GetArgumentName(i);
		if (name_collision_set.find(name) != name_collision_set.end()) {
			throw BinderException("Duplicate struct entry name %s", name);
		}
		name_collision_set.insert(name);
		new_children.emplace_back(make_pair(name, arguments[i]->GetReturnType()));
	}

	bound_function.SetReturnType(LogicalType::STRUCT(new_children));
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

static unique_ptr<BaseStatistics> StructInsertStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto new_stats = StructStats::CreateUnknown(expr.GetReturnType());
	new_stats.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);

	auto existing_count = StructType::GetChildCount(child_stats[0].GetType());
	auto existing_stats = StructStats::GetChildStats(child_stats[0]);
	for (idx_t i = 0; i < existing_count; i++) {
		StructStats::SetChildStats(new_stats, i, existing_stats[i]);
	}

	auto new_count = StructType::GetChildCount(expr.GetReturnType());
	auto offset = new_count - child_stats.size();
	for (idx_t i = 1; i < child_stats.size(); i++) {
		StructStats::SetChildStats(new_stats, offset + i, child_stats[i]);
	}
	return new_stats.ToUnique();
}

static unique_ptr<ParsedExpression> StructInsertUnbind(FunctionUnbindInput &input) {
	auto &function = input.expression.Function();
	auto &types = function.GetLogicalArguments();
	auto &return_type = function.GetLogicalReturnType();
	if (types.empty() || types[0].id() != LogicalTypeId::STRUCT || return_type.id() != LogicalTypeId::STRUCT ||
	    input.children.size() != types.size()) {
		return nullptr;
	}
	auto existing_count = StructType::GetChildCount(types[0]);
	auto &return_children = StructType::GetChildTypes(return_type);
	if (return_children.size() != existing_count + types.size() - 1) {
		return nullptr;
	}
	vector<FunctionArgument> arguments;
	arguments.emplace_back(std::move(input.children[0]));
	for (idx_t i = 1; i < input.children.size(); i++) {
		arguments.emplace_back(return_children[existing_count + i - 1].first, std::move(input.children[i]));
	}
	return make_uniq<FunctionExpression>(function.GetDefinition()->GetQualifiedName(), std::move(arguments));
}

ScalarFunction StructInsertFun::GetFunction() {
	ScalarFunction fun({}, LogicalTypeId::STRUCT, StructInsertFunction, StructInsertBind, StructInsertStats);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.GetSignature().AddParameter("struct", LogicalType::ANY).AddKwargs("kwargs", LogicalType::ANY);
	fun.GetProperties().SetRequiresExpressionNames(true);
	fun.SetSerializeCallback(VariableReturnBindData::Serialize);
	fun.SetDeserializeCallback(VariableReturnBindData::Deserialize);
	fun.SetUnbindCallback(StructInsertUnbind);
	return fun;
}

} // namespace duckdb
