#include "duckdb/common/identifier.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"

#include <ostream>

namespace duckdb {

Identifier GenerateInternalName(const char *prefix) {
	return Identifier(prefix + UUID::ToString(UUID::GenerateRandomUUID()));
}

bool Identifier::StartsWith(const string &prefix) const {
	return StringUtil::CIStartsWith(value, prefix);
}

bool Identifier::EndsWith(const string &suffix) const {
	return StringUtil::CIEndsWith(value, suffix);
}

hash_t Identifier::Hash() const {
	return StringUtil::CIHash(value);
}

bool operator==(const Identifier &a, const Identifier &b) {
	return StringUtil::CIEquals(a.GetIdentifierName(), b.GetIdentifierName());
}
bool operator==(const Identifier &a, const string &b) {
	return StringUtil::CIEquals(a.GetIdentifierName(), b);
}
bool operator==(const string &a, const Identifier &b) {
	return StringUtil::CIEquals(a, b.GetIdentifierName());
}
bool operator==(const Identifier &a, const char *b) {
	return StringUtil::CIEquals(a.GetIdentifierName(), b);
}
bool operator==(const char *a, const Identifier &b) {
	return StringUtil::CIEquals(a, b.GetIdentifierName());
}

bool operator<(const Identifier &a, const Identifier &b) {
	return StringUtil::CILessThan(a.GetIdentifierName(), b.GetIdentifierName());
}

std::ostream &operator<<(std::ostream &os, const Identifier &id) {
	return os << id.GetIdentifierName();
}

} // namespace duckdb
