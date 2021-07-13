#include "duckdb/common/enums/join_type.hpp"

namespace duckdb {

string JoinTypeToString(JoinType type) {
	switch (type) {
	case JoinType::LEFT:
		return "LEFT";
	case JoinType::RIGHT:
		return "RIGHT";
	case JoinType::INNER:
		return "INNER";
	case JoinType::OUTER:
		return "OUTER";
	case JoinType::SEMI:
		return "SEMI";
	case JoinType::ANTI:
		return "ANTI";
	case JoinType::SINGLE:
		return "SINGLE";
	case JoinType::MARK:
		return "MARK";
	case JoinType::INVALID:
	default:
		return "INVALID";
	}
}

bool IsOuterJoin(JoinType type) {
	return type == JoinType::LEFT || type == JoinType::OUTER || type == JoinType::RIGHT;
}

bool IsLeftOuterJoin(JoinType type) {
	return type == JoinType::LEFT || type == JoinType::OUTER;
}

bool IsRightOuterJoin(JoinType type) {
	return type == JoinType::OUTER || type == JoinType::RIGHT;
}

bool IsInnerJoin(JoinType type) {
	return type == JoinType::INNER;
}

} // namespace duckdb
