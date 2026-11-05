#include "duckdb/parser/peg/matcher/operator_matcher.hpp"

namespace duckdb {

bool OperatorMatcher::HasSpecialPrecedence(const string &operator_name) {
	switch (operator_name.size()) {
	case 1:
		switch (operator_name[0]) {
		case '=':
		case '<':
		case '>':
		case '~':
			return true;
		default:
			return false;
		}
	case 2:
		return operator_name == "->" || operator_name == "<=" || operator_name == ">=" || operator_name == "!=" ||
		       operator_name == "==" || operator_name == "<>" || operator_name == "~~" || operator_name == "~*" ||
		       operator_name == "!~";
	case 3:
		return operator_name == "~~*" || operator_name == "~~~" || operator_name == "!~~" || operator_name == "!~*";
	case 4:
		return operator_name == "!~~*";
	default:
		return false;
	}
}

} // namespace duckdb
