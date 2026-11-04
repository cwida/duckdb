#include "duckdb/common/vector/union_vector.hpp"
#include "core_functions/scalar/union_functions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"

namespace duckdb {

namespace {

struct UnionExtractBindData : public FunctionData {
	UnionExtractBindData(const Identifier &key, idx_t index, LogicalType type)
	    : key(key), index(index), type(std::move(type)) {
	}

	string key;
	idx_t index;
	LogicalType type;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<UnionExtractBindData>(Identifier(key), index, type);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<UnionExtractBindData>();
		return key == other.key && index == other.index && type == other.type;
	}
};

void UnionExtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<UnionExtractBindData>();

	// this should be guaranteed by the binder
	const auto &vec = args.data[0];
	vec.Verify();

	D_ASSERT(info.index < UnionType::GetMemberCount(vec.GetType()));
	auto &member = UnionVector::GetMember(vec, info.index);
	result.Reference(member);
	result.Verify();
}

//! Validates the union argument - the extracted member and the return type are resolved in the bind
void UnionExtractResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	D_ASSERT(bound_function.GetArguments().size() == 2);
	auto &union_type = input.GetArgumentType(0);
	if (union_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	if (union_type.id() != LogicalTypeId::UNION) {
		throw BinderException("union_extract can only take a union parameter");
	}
	if (UnionType::GetMemberCount(union_type) == 0) {
		throw InternalException("Can't extract something from an empty union");
	}
	bound_function.GetArguments()[0] = union_type;
}

unique_ptr<FunctionData> UnionExtractBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	idx_t union_member_count = UnionType::GetMemberCount(arguments[0]->GetReturnType());

	auto key_val = input.GetNonNullConstant(1);
	D_ASSERT(key_val.type().id() == LogicalTypeId::VARCHAR);
	auto &key_str = StringValue::Get(key_val);
	if (key_str.empty()) {
		throw BinderException("Key name for union_extract must not be empty");
	}
	auto key = Identifier(key_str);

	LogicalType return_type;
	idx_t key_index = 0;
	bool found_key = false;

	for (size_t i = 0; i < union_member_count; i++) {
		auto &member_name = UnionType::GetMemberName(arguments[0]->GetReturnType(), i);
		if (member_name == key) {
			found_key = true;
			key_index = i;
			return_type = UnionType::GetMemberType(arguments[0]->GetReturnType(), i);
			break;
		}
	}

	if (!found_key) {
		// LCOV_EXCL_START
		vector<string> candidates;
		candidates.reserve(union_member_count);
		for (idx_t i = 0; i < union_member_count; i++) {
			candidates.emplace_back(UnionType::GetMemberName(arguments[0]->GetReturnType(), i));
		}
		auto closest_settings = StringUtil::TopNJaroWinkler(candidates, key);
		auto message = StringUtil::CandidatesMessage(closest_settings, "Candidate Entries");
		throw BinderException("Could not find key %s in union\n%s", key, message);
		// LCOV_EXCL_STOP
	}

	bound_function.SetReturnType(return_type);
	return make_uniq<UnionExtractBindData>(key, key_index, return_type);
}

} // namespace

ScalarFunction UnionExtractFun::GetFunction() {
	// the arguments and return types are actually set in the binder function
	ScalarFunction fun({}, LogicalType::ANY, UnionExtractFunction, UnionExtractBind, nullptr, nullptr);
	fun.GetSignature().AddParameter("union", LogicalTypeId::UNION).AddParameter("tag", LogicalType::VARCHAR);
	fun.SetResolveTypesCallback(UnionExtractResolveTypes);
	return fun;
}

} // namespace duckdb
