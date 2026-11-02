#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/expression_util.hpp"
#include "duckdb/common/limits.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// PivotColumn
//===--------------------------------------------------------------------===//
string PivotColumn::ToString() const {
	string result;
	if (!unpivot_names.empty()) {
		D_ASSERT(pivot_expressions.empty());
		// unpivot
		if (unpivot_names.size() == 1) {
			result += SQLIdentifier(unpivot_names[0]);
		} else {
			result += "(";
			for (idx_t n = 0; n < unpivot_names.size(); n++) {
				if (n > 0) {
					result += ", ";
				}
				result += SQLIdentifier(unpivot_names[n]);
			}
			result += ")";
		}
	} else if (!pivot_expressions.empty()) {
		// pivot
		result += "(";
		for (idx_t n = 0; n < pivot_expressions.size(); n++) {
			if (n > 0) {
				result += ", ";
			}
			result += pivot_expressions[n]->ToString();
		}
		result += ")";
	}
	result += " IN ";
	if (pivot_enum.empty()) {
		result += "(";
		for (idx_t e = 0; e < entries.size(); e++) {
			auto &entry = entries[e];
			if (e > 0) {
				result += ", ";
			}
			if (entry.expr) {
				D_ASSERT(entry.values.empty());
				result += entry.expr->ToString();
			} else if (entry.values.size() == 1) {
				result += entry.values[0].ToSQLString();
			} else {
				result += "(";
				for (idx_t v = 0; v < entry.values.size(); v++) {
					if (v > 0) {
						result += ", ";
					}
					result += entry.values[v].ToSQLString();
				}
				result += ")";
			}
			if (!entry.alias.empty()) {
				result += " AS " + SQLIdentifier(entry.alias);
			}
		}
		result += ")";
	} else {
		result += SQLIdentifier(pivot_enum);
	}
	return result;
}

bool PivotColumnEntry::Equals(const PivotColumnEntry &other) const {
	if (alias != other.alias) {
		return false;
	}
	if (values.size() != other.values.size()) {
		return false;
	}
	for (idx_t i = 0; i < values.size(); i++) {
		if (!Value::NotDistinctFrom(values[i], other.values[i])) {
			return false;
		}
	}
	return true;
}

bool PivotColumn::Equals(const PivotColumn &other) const {
	if (!ExpressionUtil::ListEquals(pivot_expressions, other.pivot_expressions)) {
		return false;
	}
	if (other.unpivot_names != unpivot_names) {
		return false;
	}
	if (other.pivot_enum != pivot_enum) {
		return false;
	}
	if (other.entries.size() != entries.size()) {
		return false;
	}
	for (idx_t i = 0; i < entries.size(); i++) {
		if (!entries[i].Equals(other.entries[i])) {
			return false;
		}
	}
	return true;
}

PivotColumn PivotColumn::Copy() const {
	PivotColumn result;
	for (auto &expr : pivot_expressions) {
		result.pivot_expressions.push_back(expr->Copy());
	}
	result.unpivot_names = unpivot_names;
	for (auto &entry : entries) {
		result.entries.push_back(entry.Copy());
	}
	result.pivot_enum = pivot_enum;
	return result;
}

//===--------------------------------------------------------------------===//
// PivotColumnEntry
//===--------------------------------------------------------------------===//
PivotColumnEntry PivotColumnEntry::Copy() const {
	PivotColumnEntry result;
	result.values = values;
	result.expr = expr ? expr->Copy() : nullptr;
	result.alias = alias;
	return result;
}

static bool TryFoldForBackwardsCompatibility(const unique_ptr<ParsedExpression> &expr, vector<Value> &values) {
	if (!expr) {
		return true;
	}

	switch (expr->GetExpressionType()) {
	case ExpressionType::COLUMN_REF: {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			return false;
		}
		values.emplace_back(colref.GetColumnName());
		return true;
	}
	case ExpressionType::FUNCTION: {
		auto &function = expr->Cast<FunctionExpression>();
		if (function.FunctionName() != "row") {
			return false;
		}
		for (auto &child : function.GetArgumentsMutable()) {
			if (!TryFoldForBackwardsCompatibility(child.GetExpressionMutable(), values)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionType::VALUE_CONSTANT: {
		values.push_back(expr->Cast<ConstantExpression>().GetLiteral().ToValue());
		return true;
	}
	default:
		return false;
	}
}

vector<PivotColumnEntry> PivotColumn::GetEntriesForSerialization(Serializer &serializer) const {
	vector<PivotColumnEntry> result;

	if (serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
		// Latest version, serialize as is.
		// Unfortunately, we have to make a deep copy to return vector by value.
		for (auto &entry : entries) {
			result.push_back(entry.Copy());
		}
		return result;
	}

	// For PIVOT, We need to potentially transform entries to deal with older serialization versions.
	const auto is_unpivot = !unpivot_names.empty();

	for (auto &entry : entries) {
		auto result_entry = entry.Copy();
		if (!entry.expr) {
			// No expression, just values, so we can serialize as-is
			result.push_back(std::move(result_entry));
			continue;
		}

		// Try to constant-fold the expression back into values
		vector<Value> folded_values;
		if (TryFoldForBackwardsCompatibility(result_entry.expr, folded_values)) {
			// Set the folded values and clear the expression for serialization
			result_entry.values = std::move(folded_values);
			result_entry.expr.reset();
			result.push_back(std::move(result_entry));
			continue;
		}

		// UNPIVOT always supported expressions, so if we didn't fold, that's fine, we can serialize the expr as-is
		if (is_unpivot) {
			result.push_back(std::move(result_entry));
			continue;
		}

		// Otherwise this is a PIVOT with an expression we could not fold.
		// Older versions of DuckDB do not support this, so throw an exception.
		const auto target_version = serializer.GetOptions().storage_compatibility.duckdb_version;

		throw SerializationException(
		    "Cannot serialize non-constant expression '%s' in pivot list when targeting database storage version '%s'",
		    entry.expr->ToString(), target_version);
	}

	return result;
}

//===--------------------------------------------------------------------===//
// PivotRef
//===--------------------------------------------------------------------===//
string PivotRef::ToString() const {
	string result;
	result = source->ToString();
	if (!aggregates.empty()) {
		// pivot
		result += " PIVOT (";
		for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
			if (aggr_idx > 0) {
				result += ", ";
			}
			result += aggregates[aggr_idx]->ToString();
			if (!aggregates[aggr_idx]->GetAlias().empty()) {
				result += " AS " + SQLIdentifier(aggregates[aggr_idx]->GetAlias());
			}
		}
	} else {
		// unpivot
		result += " UNPIVOT ";
		if (include_nulls) {
			result += "INCLUDE NULLS ";
		}
		result += "(";
		if (unpivot_names.size() == 1) {
			result += SQLIdentifier(unpivot_names[0]);
		} else {
			result += "(";
			for (idx_t n = 0; n < unpivot_names.size(); n++) {
				if (n > 0) {
					result += ", ";
				}
				result += SQLIdentifier(unpivot_names[n]);
			}
			result += ")";
		}
	}
	result += " FOR";
	for (auto &pivot : pivots) {
		result += " ";
		result += pivot.ToString();
	}
	if (!groups.empty()) {
		result += " GROUP BY ";
		for (idx_t i = 0; i < groups.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += SQLIdentifier(groups[i]);
		}
	}
	result += ")";
	if (!alias.empty()) {
		result += " AS " + SQLIdentifier(alias);
		if (!column_name_alias.empty()) {
			result += "(";
			for (idx_t i = 0; i < column_name_alias.size(); i++) {
				if (i > 0) {
					result += ", ";
				}
				result += SQLIdentifier(column_name_alias[i]);
			}
			result += ")";
		}
	}
	return result;
}

bool PivotRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<PivotRef>();
	if (!source->Equals(*other.source)) {
		return false;
	}
	if (!ParsedExpression::ListEquals(aggregates, other.aggregates)) {
		return false;
	}
	if (pivots.size() != other.pivots.size()) {
		return false;
	}
	for (idx_t i = 0; i < pivots.size(); i++) {
		if (!pivots[i].Equals(other.pivots[i])) {
			return false;
		}
	}
	if (unpivot_names != other.unpivot_names) {
		return false;
	}
	if (alias != other.alias) {
		return false;
	}
	if (groups != other.groups) {
		return false;
	}
	if (include_nulls != other.include_nulls) {
		return false;
	}
	return true;
}

unique_ptr<TableRef> PivotRef::Copy() {
	auto copy = make_uniq<PivotRef>();
	copy->source = source->Copy();
	for (auto &aggr : aggregates) {
		copy->aggregates.push_back(aggr->Copy());
	}
	copy->unpivot_names = unpivot_names;
	for (auto &entry : pivots) {
		copy->pivots.push_back(entry.Copy());
	}
	copy->groups = groups;
	copy->column_name_alias = column_name_alias;
	copy->include_nulls = include_nulls;
	copy->alias = alias;
	return std::move(copy);
}

} // namespace duckdb
