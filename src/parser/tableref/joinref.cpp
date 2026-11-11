#include "duckdb/parser/tableref/joinref.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

//! True if the join type is expressible with standard SQL JOIN syntax (e.g. INNER JOIN, LEFT JOIN).
//! Internal join types (MARK, SINGLE, ...) instead use the JOIN BY (TYPE <type>) form.
static bool JoinTypeUsesStandardSyntax(JoinType type) {
	switch (type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::OUTER:
	case JoinType::SEMI:
	case JoinType::ANTI:
		return true;
	default:
		return false;
	}
}

string JoinRef::ToString() const {
	string result;
	if (!is_implicit) {
		result += "(";
	}
	result += left->ToString() + " ";
	switch (ref_type) {
	case JoinRefType::REGULAR:
		if (JoinTypeUsesStandardSyntax(type)) {
			result += EnumUtil::ToString(type) + " JOIN ";
		} else {
			result += "JOIN BY (TYPE " + EnumUtil::ToString(type) + ") ";
		}
		break;
	case JoinRefType::NATURAL:
		result += "NATURAL ";
		result += EnumUtil::ToString(type) + " JOIN ";
		break;
	case JoinRefType::ASOF:
		result += "ASOF ";
		result += EnumUtil::ToString(type) + " JOIN ";
		break;
	case JoinRefType::CROSS:
		result += is_implicit ? ", " : "CROSS JOIN ";
		break;
	case JoinRefType::POSITIONAL:
		result += "POSITIONAL JOIN ";
		break;
	case JoinRefType::DEPENDENT:
		result += "DEPENDENT JOIN ";
		break;
	case JoinRefType::NEAREST:
		result += EnumUtil::ToString(type) + " JOIN ";
		break;
	}
	result += right->ToString();
	if (ref_type == JoinRefType::NEAREST) {
		result += nearest_approx ? " APPROX" : " EXACT";
		result += " NEAREST " + to_string(nearest_count);
		result += nearest_order_type == OrderType::DESCENDING ? " BY SIMILARITY " : " BY DISTANCE ";
		result += ranking_expression->ToString();
		if (!is_implicit) {
			result += ")";
		}
		return result;
	}
	if (condition) {
		D_ASSERT(using_columns.empty());
		result += " ON (";
		result += condition->ToString();
		result += ")";
	} else if (!using_columns.empty()) {
		result += " USING (";
		for (idx_t i = 0; i < using_columns.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += SQLIdentifier(using_columns[i]);
		}
		result += ")";
	}
	if (!is_implicit) {
		result += ")";
	}
	return result;
}

bool JoinRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<JoinRef>();
	if (using_columns.size() != other.using_columns.size()) {
		return false;
	}
	for (idx_t i = 0; i < using_columns.size(); i++) {
		if (using_columns[i] != other.using_columns[i]) {
			return false;
		}
	}
	if (nearest_count != other.nearest_count || nearest_order_type != other.nearest_order_type ||
	    nearest_approx != other.nearest_approx) {
		return false;
	}
	return left->Equals(*other.left) && right->Equals(*other.right) &&
	       ParsedExpression::Equals(condition, other.condition) &&
	       ParsedExpression::Equals(ranking_expression, other.ranking_expression) && type == other.type;
}

unique_ptr<TableRef> JoinRef::Copy() {
	auto copy = make_uniq<JoinRef>(ref_type);
	copy->left = left->Copy();
	copy->right = right->Copy();
	if (condition) {
		copy->condition = condition->Copy();
	}
	copy->type = type;
	copy->ref_type = ref_type;
	copy->using_columns = using_columns;
	copy->delim_flipped = delim_flipped;
	for (auto &col : duplicate_eliminated_columns) {
		copy->duplicate_eliminated_columns.emplace_back(col->Copy());
	}
	copy->is_implicit = is_implicit;
	if (ranking_expression) {
		copy->ranking_expression = ranking_expression->Copy();
	}
	copy->nearest_count = nearest_count;
	copy->nearest_order_type = nearest_order_type;
	copy->nearest_approx = nearest_approx;
	CopyProperties(*copy);
	return std::move(copy);
}

JoinRefType JoinRef::SerializedRefType(Serializer &serializer) const {
	// The NEAREST properties are only written from v2.0.0 onwards - rather than silently dropping them (and turning
	// the join into something else entirely), refuse to serialize to an older storage version
	if (ref_type == JoinRefType::NEAREST && !serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		throw SerializationException(
		    "Cannot serialize a NEAREST BY join to storage version \"%s\" - NEAREST BY requires storage version "
		    "\"v2.0.0\" or newer",
		    serializer.GetOptions().storage_compatibility.duckdb_version);
	}
	return ref_type;
}

} // namespace duckdb
