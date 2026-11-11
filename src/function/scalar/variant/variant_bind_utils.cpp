#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

VariantPathBindData::VariantPathBindData() : FunctionData() {
}
VariantPathBindData::VariantPathBindData(const string &input_path) : FunctionData() {
	if (input_path.empty()) {
		paths.emplace_back();
	} else {
		paths.push_back({VariantPathComponent(input_path)});
	}
}
VariantPathBindData::VariantPathBindData(const vector<string> &input_paths) : FunctionData() {
	for (const auto &path : input_paths) {
		if (path.empty()) {
			paths.emplace_back();
		} else {
			paths.push_back({VariantPathComponent(path)});
		}
	}
}

unique_ptr<FunctionData> VariantPathBindData::Copy() const {
	return make_uniq<VariantPathBindData>(*this);
}

bool VariantPathBindData::Equals(const FunctionData &other) const {
	auto &bind_data = other.Cast<VariantPathBindData>();
	if (paths.size() != bind_data.paths.size()) {
		return false;
	}

	for (idx_t i = 0; i < paths.size(); i++) {
		if (paths[i].size() != bind_data.paths[i].size()) {
			return false;
		}
		for (idx_t j = 0; j < paths[i].size(); j++) {
			if (paths[i][j] != bind_data.paths[i][j]) {
				return false;
			}
		}
	}
	return true;
}

static vector<string> CollectPaths(const Value &constant_arg, const string &function_name) {
	vector<string> paths;
	const auto &children = ListValue::GetChildren(constant_arg);
	for (const auto &child : children) {
		if (child.IsNull()) {
			throw BinderException("'%s' does not accept NULL paths", function_name);
		}
		paths.push_back(child.GetValue<string>());
	}

	return paths;
}

unique_ptr<FunctionData> VariantBindUtils::VariantPathBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	const auto &function_name = input.GetBoundFunction().GetName();

	if (arguments.size() != 1 && arguments.size() != 2) {
		throw BinderException("%s expects either one VARIANT column argument, or two VARIANT column and "
		                      "VARCHAR path arguments",
		                      function_name);
	}

	if (arguments.size() == 1) {
		// No path supplied, execute function over the root of the variant
		return make_uniq<VariantPathBindData>();
	}

	const auto &path_expr = *arguments[1];
	const auto &return_type = path_expr.GetReturnType();
	if (return_type.id() != LogicalTypeId::VARCHAR && return_type.id() != LogicalTypeId::LIST) {
		throw BinderException("%s expects the second argument to be of type VARCHAR or VARCHAR[], not %s",
		                      function_name, return_type.ToString());
	}
	if (return_type.id() == LogicalTypeId::LIST) {
		const auto child_type_id = ListType::GetChildType(return_type).id();
		if (child_type_id != LogicalTypeId::VARCHAR && child_type_id != LogicalTypeId::SQLNULL) {
			throw BinderException("%s expects the second argument to be of type VARCHAR or VARCHAR[], not %s",
			                      function_name, return_type.ToString());
		}
	}

	auto constant_arg = input.GetNonNullConstant(1);

	const auto path_type_id = constant_arg.type().id();
	if (path_type_id == LogicalTypeId::VARCHAR) {
		return make_uniq<VariantPathBindData>(constant_arg.GetValue<string>());
	}
	if (path_type_id == LogicalTypeId::LIST) {
		return make_uniq<VariantPathBindData>(CollectPaths(constant_arg, function_name.GetIdentifierName()));
	}

	throw BinderException("%s received an unexpected type for the second argument", function_name);
}

} // namespace duckdb
