//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_executors.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "json_functions.hpp"
#include "yyjson_memory.hpp"

namespace duckdb {

template <class T>
using json_function_t = std::function<optional<T>(yyjson_val *, yyjson_alc *, Vector &)>;

struct JSONExecutors {
public:
	//! Single-argument JSON read function, i.e. json_type('[1, 2, 3]')
	template <class T>
	static void UnaryExecute(DataChunk &args, ExpressionState &state, Vector &result, const json_function_t<T> fun) {
		auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
		auto alc = lstate.json_allocator->GetYYAlc();

		const auto &inputs = args.data[0];
		UnaryExecutor::Execute<string_t, T>(inputs, result, [&](string_t input) -> optional<T> {
			auto doc = JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc);
			return fun(doc->root, alc, result);
		});

		JSONAllocator::AddBuffer(result, alc);
	}

	//! Two-argument JSON read function (with path query), i.e. json_type('[1, 2, 3]', '$[0]')
	template <class T, bool SET_NULL_IF_NOT_FOUND = true>
	static void BinaryExecute(DataChunk &args, ExpressionState &state, Vector &result, const json_function_t<T> fun) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.BindInfo()->Cast<JSONReadFunctionData>();
		auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
		auto alc = lstate.json_allocator->GetYYAlc();

		const auto &inputs = args.data[0];
		if (info.constant) { // Constant path
			const char *ptr = info.ptr;
			const idx_t &len = info.len;
			if (info.path_type == JSONCommon::JSONPathType::REGULAR) {
				UnaryExecutor::Execute<string_t, T>(inputs, result, [&](string_t input) -> optional<T> {
					auto doc = JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc);
					// Use the path elements parsed at bind time when available
					auto val = info.use_elements ? JSONCommon::GetPathElements(doc->root, info.elements)
					                             : JSONCommon::GetUnsafe(doc->root, ptr, len);
					if (SET_NULL_IF_NOT_FOUND && !val) {
						return nullopt;
					} else {
						return fun(val, alc, result);
					}
				});
			} else {
				D_ASSERT(info.path_type == JSONCommon::JSONPathType::WILDCARD);
				vector<yyjson_val *> vals;
				UnaryExecutor::Execute<string_t, list_entry_t>(inputs, result, [&](string_t input) {
					vals.clear();

					auto doc = JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc);
					JSONCommon::GetWildcardPath(doc->root, ptr, len, vals);

					auto current_size = ListVector::GetListSize(result);
					auto new_size = current_size + vals.size();
					if (ListVector::GetListCapacity(result) < new_size) {
						ListVector::Reserve(result, new_size);
					}

					auto &child_entry = ListVector::GetChildMutable(result);
					auto child_vals = FlatVector::GetDataMutable<T>(child_entry);
					auto &child_validity = FlatVector::ValidityMutable(child_entry);
					for (idx_t i = 0; i < vals.size(); i++) {
						auto &val = vals[i];
						D_ASSERT(val != nullptr); // Wildcard extract shouldn't give back nullptrs
						auto fun_result = fun(val, alc, child_entry);
						if (fun_result.has_value()) {
							child_vals[current_size + i] = fun_result.value();
						} else {
							child_validity.SetInvalid(current_size + i);
						}
					}

					ListVector::SetListSize(result, new_size);

					return list_entry_t {current_size, vals.size()};
				});
			}
		} else { // Columnref path
			D_ASSERT(info.path_type == JSONCommon::JSONPathType::REGULAR);
			unique_ptr<Vector> casted_paths;
			if (args.data[1].GetType().id() == LogicalTypeId::VARCHAR) {
				casted_paths = make_uniq<Vector>(Vector::Ref(args.data[1]));
			} else {
				casted_paths = make_uniq<Vector>(LogicalTypeId::VARCHAR);
				VectorOperations::DefaultCast(args.data[1], *casted_paths, args.size(), true);
			}
			BinaryExecutor::Execute<string_t, string_t, T>(
			    inputs, *casted_paths, result, args.size(), [&](string_t input, string_t path) -> optional<T> {
				    auto doc = JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc);
				    auto val = JSONCommon::Get(doc->root, path, args.data[1].GetType().IsIntegral());
				    if (SET_NULL_IF_NOT_FOUND && !val) {
					    return nullopt;
				    } else {
					    return fun(val, alc, result);
				    }
			    });
		}
		JSONAllocator::AddBuffer(result, alc);
	}

	//! JSON read function with list of path queries, i.e. json_type('[1, 2, 3]', ['$[0]', '$[1]'])
	template <class T, bool SET_NULL_IF_NOT_FOUND = true>
	static void ExecuteMany(DataChunk &args, ExpressionState &state, Vector &result, const json_function_t<T> fun) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		const auto &info = func_expr.BindInfo()->Cast<JSONReadManyFunctionData>();
		auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
		auto alc = lstate.json_allocator->GetYYAlc();
		D_ASSERT(info.ptrs.size() == info.lens.size());

		const auto count = args.size();
		const idx_t num_paths = info.ptrs.size();
		const idx_t list_size = count * num_paths;

		UnifiedVectorFormat input_data;
		auto &input_vector = args.data[0];
		input_vector.ToUnifiedFormat(input_data);
		auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

		ListVector::Reserve(result, list_size);
		auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
		auto &list_validity = FlatVector::ValidityMutable(result);

		auto &child = ListVector::GetChildMutable(result);
		auto child_data = FlatVector::GetDataMutable<T>(child);
		auto &child_validity = FlatVector::ValidityMutable(child);

		idx_t offset = 0;
		yyjson_val *val;
		for (idx_t i = 0; i < count; i++) {
			auto idx = input_data.sel->get_index(i);
			if (!input_data.validity.RowIsValid(idx)) {
				list_validity.SetInvalid(i);
				continue;
			}

			auto doc = JSONCommon::ReadDocument(inputs[idx], JSONCommon::READ_FLAG, alc);
			for (idx_t path_i = 0; path_i < num_paths; path_i++) {
				auto child_idx = offset + path_i;
				val = info.use_elements[path_i]
				          ? JSONCommon::GetPathElements(doc->root, info.elements[path_i])
				          : JSONCommon::GetUnsafe(doc->root, info.ptrs[path_i], info.lens[path_i]);
				if (SET_NULL_IF_NOT_FOUND && !val) {
					child_validity.SetInvalid(child_idx);
				} else {
					auto fun_result = fun(val, alc, child);
					if (fun_result.has_value()) {
						child_data[child_idx] = fun_result.value();
					} else {
						child_validity.SetInvalid(child_idx);
					}
				}
			}

			list_entries[i].offset = offset;
			list_entries[i].length = num_paths;
			offset += num_paths;
		}
		ListVector::SetListSize(result, offset);
		JSONAllocator::AddBuffer(result, alc);
	}
};

} // namespace duckdb
