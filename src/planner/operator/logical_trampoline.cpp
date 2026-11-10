#include "duckdb/planner/operator/logical_trampoline.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

LogicalTrampoline::LogicalTrampoline() : LogicalCTE(LogicalOperatorType::LOGICAL_TRAMPOLINE), selector_idx(0) {
}

LogicalTrampoline::LogicalTrampoline(Identifier ctename_p, TableIndex table_index_p, idx_t column_count_p,
                                     idx_t selector_idx, unique_ptr<LogicalOperator> seed,
                                     vector<unique_ptr<LogicalOperator>> branches)
    : LogicalCTE(LogicalOperatorType::LOGICAL_TRAMPOLINE), selector_idx(selector_idx) {
	ctename = std::move(ctename_p);
	table_index = table_index_p;
	column_count = column_count_p;
	children.push_back(std::move(seed));
	for (auto &branch : branches) {
		children.push_back(std::move(branch));
	}
}

InsertionOrderPreservingMap<string> LogicalTrampoline::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	result["Branches"] = StringUtil::Format("%llu", BranchCount());
	SetParamsEstimatedCardinality(result);
	return result;
}

vector<TableIndex> LogicalTrampoline::GetTableIndex() const {
	return vector<TableIndex> {table_index};
}

string LogicalTrampoline::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", table_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

bool LogicalTrampoline::TypeUsesPhysicalDistinctEquality(ClientContext &context, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR: {
		auto collation = StringType::GetCollation(type);
		if (collation.empty()) {
			collation = Settings::Get<DefaultCollationSetting>(context);
		}
		collation = StringUtil::Lower(collation);
		return collation.empty() || collation == "binary" || collation == "c" || collation == "posix";
	}
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::UNION: {
		for (auto &entry : StructType::GetChildTypes(type)) {
			if (!TypeUsesPhysicalDistinctEquality(context, entry.second)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return TypeUsesPhysicalDistinctEquality(context, ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return TypeUsesPhysicalDistinctEquality(context, ArrayType::GetChildType(type));
	default:
		return true;
	}
}

void LogicalTrampoline::ResolveTypes() {
	types = children[0]->types;
}

} // namespace duckdb
