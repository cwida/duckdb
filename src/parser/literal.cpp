#include "duckdb/parser/literal.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

Literal::Literal(LiteralKind kind_p, string text_p) : kind(kind_p), text(std::move(text_p)) {
}

Literal Literal::Null() {
	return Literal(LiteralKind::NULL_LITERAL, string());
}

Literal Literal::Boolean(bool value) {
	return Literal(LiteralKind::BOOLEAN, value ? "true" : "false");
}

Literal Literal::Integer(int64_t value) {
	return Literal(LiteralKind::INTEGER, std::to_string(value));
}

Literal Literal::Number(string text) {
	for (auto c : text) {
		if (c == '.' || c == 'e' || c == 'E') {
			return Literal(LiteralKind::NUMERIC, std::move(text));
		}
	}
	return Literal(LiteralKind::INTEGER, std::move(text));
}

Literal Literal::String(string text) {
	return Literal(LiteralKind::STRING, std::move(text));
}

Literal Literal::Hex(string text) {
	if (text.size() % 2 != 0) {
		throw ParserException("Hex string literal must have an even number of hex digits");
	}
	for (auto c : text) {
		if (!StringUtil::CharacterIsHex(c)) {
			throw ParserException("Hex string literal contains a non-hexadecimal character '%c'", c);
		}
	}
	return Literal(LiteralKind::HEX, std::move(text));
}

Literal Literal::Bit(string text) {
	for (auto c : text) {
		if (c != '0' && c != '1') {
			throw ParserException("Bit string literal contains a non-binary character '%c'", c);
		}
	}
	return Literal(LiteralKind::BIT, std::move(text));
}

Literal Literal::Pointer(uintptr_t address) {
	return Literal(LiteralKind::POINTER, Value::POINTER(address).ToString());
}

bool Literal::IsNumeric() const {
	return kind == LiteralKind::INTEGER || kind == LiteralKind::NUMERIC;
}

bool Literal::IsNull() const {
	return kind == LiteralKind::NULL_LITERAL;
}

bool Literal::IsPointer() const {
	return kind == LiteralKind::POINTER;
}

bool Literal::TryGetInt64(int64_t &result) const {
	if (kind != LiteralKind::INTEGER) {
		return false;
	}
	return TryCast::Operation<string_t, int64_t>(string_t(text), result);
}

Literal Literal::Negate() const {
	D_ASSERT(IsNumeric());
	if (!text.empty() && text[0] == '-') {
		return Literal(kind, text.substr(1));
	}
	return Literal(kind, "-" + text);
}

static Value NumberToValue(const string &val) {
	string_t str_val(val);
	bool try_cast_as_integer = true;
	bool try_cast_as_decimal = true;
	optional_idx decimal_position = optional_idx::Invalid();
	idx_t num_underscores = 0;
	idx_t num_integer_underscores = 0;
	for (idx_t i = 0; i < str_val.GetSize(); i++) {
		if (val[i] == '.') {
			// decimal point: cast as either decimal or double
			try_cast_as_integer = false;
			decimal_position = i;
		}
		if (val[i] == 'e' || val[i] == 'E') {
			// found exponent, cast as double
			try_cast_as_integer = false;
			try_cast_as_decimal = false;
		}
		if (val[i] == '_') {
			num_underscores++;
			if (!decimal_position.IsValid()) {
				num_integer_underscores++;
			}
		}
	}
	if (try_cast_as_integer) {
		int32_t int_value;
		if (TryCast::Operation<string_t, int32_t>(str_val, int_value)) {
			// the magnitude of INT32_MIN does not fit an INTEGER, so -2147483648 is typed as BIGINT
			if (int_value != NumericLimits<int32_t>::Minimum()) {
				return Value::INTEGER(int_value);
			}
		}
		int64_t bigint_value;
		if (TryCast::Operation<string_t, int64_t>(str_val, bigint_value)) {
			return Value::BIGINT(bigint_value);
		}
		hugeint_t hugeint_value;
		if (TryCast::Operation<string_t, hugeint_t>(str_val, hugeint_value)) {
			return Value::HUGEINT(hugeint_value);
		}
		uhugeint_t uhugeint_value;
		if (TryCast::Operation<string_t, uhugeint_t>(str_val, uhugeint_value)) {
			return Value::UHUGEINT(uhugeint_value);
		}
		// integers beyond uhugeint keep their precision as a bignum
		try {
			auto bignum_str = Bignum::VarcharToBignum(str_val);
			return Value::BIGNUM(bignum_str);
		} catch (const ConversionException &) {
			// not a valid bignum either: fall through to the decimal/double path
		}
	}
	idx_t decimal_offset = val[0] == '-' ? 3 : 2;
	if (try_cast_as_decimal && decimal_position.IsValid() &&
	    str_val.GetSize() - num_underscores < Decimal::MAX_WIDTH_DECIMAL + decimal_offset) {
		// figure out the width/scale based on the decimal position
		auto width = NumericCast<uint8_t>(str_val.GetSize() - 1 - num_underscores);
		// a well-formed decimal has a single '.', so its position never sits past the width; a second (e.g. trailing)
		// dot pushes decimal_position beyond width and would underflow the scale computation below
		if (decimal_position.GetIndex() > NumericCast<idx_t>(width) + num_integer_underscores) {
			throw ParserException("Could not convert string \"%s\" to a decimal value", val);
		}
		auto scale = NumericCast<uint8_t>(width - decimal_position.GetIndex() + num_integer_underscores);
		if (val[0] == '-') {
			width--;
		}
		if (width <= Decimal::MAX_WIDTH_DECIMAL) {
			return Value(str_val).DefaultCastAs(LogicalType::DECIMAL(width, scale));
		}
	}
	// there is an exponent, or the value is too wide for a decimal
	double dbl_value = Cast::Operation<string_t, double>(str_val);
	return Value::DOUBLE(dbl_value);
}

Value Literal::ToValue() const {
	switch (kind) {
	case LiteralKind::NULL_LITERAL:
		return Value();
	case LiteralKind::BOOLEAN:
		return Value::BOOLEAN(text == "true");
	case LiteralKind::INTEGER:
	case LiteralKind::NUMERIC:
		return NumberToValue(text);
	case LiteralKind::STRING:
		return Value(text);
	case LiteralKind::HEX: {
		string bytes;
		bytes.reserve(text.size() / 2);
		for (idx_t i = 0; i + 1 < text.size(); i += 2) {
			auto byte = StringUtil::GetHexValue(text[i]) * 16 + StringUtil::GetHexValue(text[i + 1]);
			bytes.push_back(static_cast<char>(byte));
		}
		return Value::BLOB_RAW(bytes);
	}
	case LiteralKind::BIT:
		return Value::BIT(text);
	case LiteralKind::POINTER:
		return Value::POINTER(CastToPointer::Operation<string_t, uintptr_t>(string_t(text)));
	default:
		throw InternalException("Cannot convert an invalid literal to a value");
	}
}

void Literal::Verify() const {
	// validates that text conforms to kind so a (potentially deserialized) literal never renders arbitrary SQL text
	auto all_of = [&](const char *allowed) {
		return text.find_first_not_of(allowed) == string::npos;
	};
	auto has_digit = [&]() {
		return text.find_first_of("0123456789") != string::npos;
	};
	switch (kind) {
	case LiteralKind::NULL_LITERAL:
		if (!text.empty()) {
			throw SerializationException("Invalid NULL literal: text must be empty");
		}
		return;
	case LiteralKind::BOOLEAN:
		if (text != "true" && text != "false") {
			throw SerializationException("Invalid BOOLEAN literal text '%s'", text);
		}
		return;
	case LiteralKind::INTEGER:
		if (!has_digit() || !all_of("0123456789_-")) {
			throw SerializationException("Invalid INTEGER literal text '%s'", text);
		}
		return;
	case LiteralKind::NUMERIC:
		if (!has_digit() || !all_of("0123456789_.eE+-")) {
			throw SerializationException("Invalid NUMERIC literal text '%s'", text);
		}
		return;
	case LiteralKind::STRING:
		// any text; rendered through SQLString quoting
		return;
	case LiteralKind::HEX:
		if (text.size() % 2 != 0 || !all_of("0123456789abcdefABCDEF")) {
			throw SerializationException("Invalid HEX literal text '%s'", text);
		}
		return;
	case LiteralKind::BIT:
		if (!all_of("01")) {
			throw SerializationException("Invalid BIT literal text '%s'", text);
		}
		return;
	case LiteralKind::POINTER:
		if (text.empty() || !all_of("0123456789abcdefABCDEFxX")) {
			throw SerializationException("Invalid POINTER literal text '%s'", text);
		}
		return;
	default:
		throw SerializationException("Invalid literal kind");
	}
}

string Literal::ToString() const {
	switch (kind) {
	case LiteralKind::NULL_LITERAL:
		return "NULL";
	case LiteralKind::BOOLEAN:
		return text;
	case LiteralKind::INTEGER:
	case LiteralKind::NUMERIC:
		return text;
	case LiteralKind::STRING:
		return SQLString::ToString(text);
	case LiteralKind::HEX:
		return "X'" + text + "'";
	case LiteralKind::BIT:
		return "B'" + text + "'";
	case LiteralKind::POINTER:
		return text;
	default:
		throw InternalException("Cannot render an invalid literal");
	}
}

hash_t Literal::Hash() const {
	auto hash = duckdb::Hash<uint8_t>(static_cast<uint8_t>(kind));
	return CombineHash(hash, duckdb::Hash(text.c_str(), text.size()));
}

bool Literal::operator==(const Literal &other) const {
	return kind == other.kind && text == other.text;
}

bool Literal::operator!=(const Literal &other) const {
	return !(*this == other);
}

} // namespace duckdb
