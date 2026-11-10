#include "duckdb/parser/tableref.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/list.hpp"

namespace duckdb {

string TableRef::BaseToString(string result) const {
	vector<Identifier> column_name_alias;
	return BaseToString(std::move(result), column_name_alias);
}

string TableRef::AliasToString(const vector<Identifier> &column_name_alias) const {
	string result;
	if (!alias.empty()) {
		result += StringUtil::Format(" AS %s", SQLIdentifier(alias));
	}
	if (!column_name_alias.empty()) {
		D_ASSERT(!alias.empty());
		result += "(";
		for (idx_t i = 0; i < column_name_alias.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += SQLIdentifier(column_name_alias[i]);
		}
		result += ")";
	}
	return result;
}

string TableRef::SampleToString() const {
	string result;
	if (sample) {
		result += " TABLESAMPLE " + EnumUtil::ToString(sample->method);
		result += "(" + sample->sample_size.ToString() + " " + string(sample->is_percentage ? "PERCENT" : "ROWS") + ")";
		if (sample->seed.IsValid()) {
			result += "REPEATABLE (" + to_string(sample->seed.GetIndex()) + ")";
		}
	}
	return result;
}

string TableRef::BaseToString(string result, const vector<Identifier> &column_name_alias) const {
	result += AliasToString(column_name_alias);
	result += SampleToString();
	return result;
}

bool TableRef::Equals(const TableRef &other) const {
	return type == other.type && alias == other.alias && SampleOptions::Equals(sample.get(), other.sample.get());
}

void TableRef::CopyProperties(TableRef &target) const {
	D_ASSERT(type == target.type);
	target.alias = alias;
	target.query_location = query_location;
	target.sample = sample ? sample->Copy() : nullptr;
	target.external_dependency = external_dependency;
}

void TableRef::Print() {
	Printer::Print(ToString());
}

bool TableRef::Equals(const unique_ptr<TableRef> &left, const unique_ptr<TableRef> &right) {
	if (left.get() == right.get()) {
		return true;
	}
	if (!left || !right) {
		return false;
	}
	return left->Equals(*right);
}

void TableRef::VerifyDeserialized() const {
	auto invalid = [&](const string &reason) {
		return SerializationException("Failed to deserialize %s table reference: %s", EnumUtil::ToString(type), reason);
	};
	switch (type) {
	case TableReferenceType::JOIN: {
		auto &join_ref = Cast<JoinRef>();
		if (!join_ref.left || !join_ref.right) {
			throw invalid("missing join side");
		}
		if (join_ref.ref_type == JoinRefType::REGULAR && !join_ref.condition && join_ref.using_columns.empty()) {
			throw invalid("a regular join requires a condition or USING columns");
		}
		break;
	}
	case TableReferenceType::SUBQUERY: {
		auto &subquery_ref = Cast<SubqueryRef>();
		if (!subquery_ref.subquery || !subquery_ref.subquery->node) {
			throw invalid("missing subquery");
		}
		break;
	}
	case TableReferenceType::PIVOT: {
		auto &pivot_ref = Cast<PivotRef>();
		if (!pivot_ref.source) {
			throw invalid("missing source");
		}
		if (pivot_ref.pivots.empty()) {
			throw invalid("no PIVOT or UNPIVOT columns");
		}
		break;
	}
	case TableReferenceType::BASE_TABLE: {
		auto &at_clause = Cast<BaseTableRef>().at_clause;
		if (at_clause && !at_clause->Expression()) {
			throw invalid("missing AT expression");
		}
		break;
	}
	case TableReferenceType::TABLE_FUNCTION: {
		auto &function = Cast<TableFunctionRef>().function;
		if (!function || function->GetExpressionClass() != ExpressionClass::FUNCTION) {
			throw invalid("the table function must be a function expression");
		}
		break;
	}
	case TableReferenceType::EXPRESSION_LIST:
		for (auto &row : Cast<ExpressionListRef>().values) {
			for (auto &value : row) {
				if (!value) {
					throw invalid("missing value");
				}
			}
		}
		break;
	default:
		break;
	}
}

} // namespace duckdb
