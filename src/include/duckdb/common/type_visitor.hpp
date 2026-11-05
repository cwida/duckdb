//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/type_visitor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"

#include <algorithm>

namespace duckdb {

struct TypeVisitor {
	template <class F>
	static bool Contains(const LogicalType &type, F &&predicate);

	static bool Contains(const LogicalType &type, LogicalTypeId type_id);

	template <class F>
	static LogicalType VisitReplace(const LogicalType &type, F &&func);

private:
	struct VisitorFrame {
		LogicalType original_type;
		idx_t expected_children_count;
	};

	static idx_t PushChildren(const LogicalType &type, vector<reference<const LogicalType>> &worklist);
	static LogicalType PopType(vector<LogicalType> &result_stack);
	static LogicalType RebuildType(const VisitorFrame &frame, vector<LogicalType> &result_stack);
};

inline idx_t TypeVisitor::PushChildren(const LogicalType &type, vector<reference<const LogicalType>> &worklist) {
	if (!type.HasParameters()) {
		return 0;
	}

	auto initial_size = worklist.size();
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		for (const auto &child : StructType::GetChildTypes(type)) {
			worklist.push_back(child.second);
		}
		break;
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			worklist.push_back(UnionType::GetMemberType(type, i));
		}
		break;
	case LogicalTypeId::MAP:
		worklist.push_back(MapType::KeyType(type));
		worklist.push_back(MapType::ValueType(type));
		break;
	case LogicalTypeId::LIST:
		worklist.push_back(ListType::GetChildType(type));
		break;
	case LogicalTypeId::ARRAY:
		worklist.push_back(ArrayType::GetChildType(type));
		break;
	default:
		break;
	}
	return worklist.size() - initial_size;
}

inline LogicalType TypeVisitor::PopType(vector<LogicalType> &result_stack) {
	auto type = std::move(result_stack.back());
	result_stack.pop_back();
	return type;
}

inline LogicalType TypeVisitor::RebuildType(const VisitorFrame &frame, vector<LogicalType> &result_stack) {
	D_ASSERT(frame.expected_children_count > 0);
	D_ASSERT(result_stack.size() >= frame.expected_children_count);
	const auto &type = frame.original_type;
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::UNION: {
		auto children =
		    type.id() == LogicalTypeId::UNION ? UnionType::CopyMemberTypes(type) : StructType::GetChildTypes(type);
		for (idx_t i = children.size(); i > 0; i--) {
			children[i - 1].second = PopType(result_stack);
		}
		if (type.id() == LogicalTypeId::STRUCT) {
			return LogicalType::STRUCT(std::move(children));
		}
		if (type.id() == LogicalTypeId::TUPLE) {
			return LogicalType::TUPLE(std::move(children));
		}
		return LogicalType::UNION(std::move(children));
	}
	case LogicalTypeId::MAP: {
		auto value_type = PopType(result_stack);
		auto key_type = PopType(result_stack);
		return LogicalType::MAP(std::move(key_type), std::move(value_type));
	}
	case LogicalTypeId::LIST:
		return LogicalType::LIST(PopType(result_stack));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(PopType(result_stack), ArrayType::GetSize(type));
	default:
		throw InternalException("Cannot rebuild children of a non-nested type");
	}
}

template <class F>
inline LogicalType TypeVisitor::VisitReplace(const LogicalType &type, F &&func) {
	vector<VisitorFrame> postfix_stack;
	vector<reference<const LogicalType>> worklist {type};

	while (!worklist.empty()) {
		const auto &current = worklist.back().get();
		worklist.pop_back();
		auto child_count = PushChildren(current, worklist);
		postfix_stack.push_back({current, child_count});
	}

	vector<LogicalType> result_stack;
	while (!postfix_stack.empty()) {
		auto frame = std::move(postfix_stack.back());
		postfix_stack.pop_back();

		if (frame.expected_children_count == 0) {
			result_stack.push_back(func(frame.original_type));
		} else {
			result_stack.push_back(func(RebuildType(frame, result_stack)));
		}
	}
	D_ASSERT(result_stack.size() == 1);
	return result_stack.back();
}

template <class F>
inline bool TypeVisitor::Contains(const LogicalType &type, F &&predicate) {
	vector<reference<const LogicalType>> worklist {type};

	while (!worklist.empty()) {
		const auto &current = worklist.back().get();
		worklist.pop_back();

		if (predicate(current)) {
			return true;
		}
		auto child_count = PushChildren(current, worklist);
		std::reverse(worklist.end() - NumericCast<int64_t>(child_count), worklist.end());
	}
	return false;
}

inline bool TypeVisitor::Contains(const LogicalType &type, LogicalTypeId type_id) {
	return Contains(type, [&](const LogicalType &ty) { return ty.id() == type_id; });
}

} // namespace duckdb
