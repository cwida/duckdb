#include "duckdb/parser/expression/pattern_expression.hpp"

#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> PatternExpression::Deserialize(Deserializer &deserializer) {
	auto type = deserializer.Get<ExpressionType>();
	switch (type) {
	case ExpressionType::CONCATENATION:
		return ConcatenationExpression::Deserialize(deserializer);
	case ExpressionType::QUANTIFIER:
		return QuantifiedExpression::Deserialize(deserializer);
	case ExpressionType::ALTERNATION:
		return AlternationExpression::Deserialize(deserializer);
	case ExpressionType::ANCHOR:
		return AnchorExpression::Deserialize(deserializer);
	default:
		throw SerializationException("Unsupported pattern expression type %s", ExpressionTypeToString(type));
	}
}

//===--------------------------------------------------------------------===//
// Concatenation
//===--------------------------------------------------------------------===//
string ConcatenationExpression::ToString() const {
	// the parentheses keep a quantifier on the concatenation as a whole from binding to its last part
	return "(" +
	       StringUtil::Join(children, children.size(), " ",
	                        [](const unique_ptr<ParsedExpression> &expr) { return expr->ToString(); }) +
	       ")";
}

bool ConcatenationExpression::Equals(const ParsedExpression &other_p) const {
	if (!ParsedExpression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<ConcatenationExpression>();
	return ParsedExpression::ListEquals(children, other.children);
}

unique_ptr<ParsedExpression> ConcatenationExpression::Copy() const {
	vector<unique_ptr<ParsedExpression>> new_children;
	new_children.reserve(children.size());
	for (auto &child : children) {
		new_children.push_back(child->Copy());
	}
	auto copy = make_uniq<ConcatenationExpression>(std::move(new_children));
	copy->CopyBase(*this);
	return std::move(copy);
}

void ConcatenationExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(200, "children", children);
}

unique_ptr<ParsedExpression> ConcatenationExpression::Deserialize(Deserializer &deserializer) {
	auto children = deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(200, "children");
	return make_uniq_base<ParsedExpression, ConcatenationExpression>(std::move(children));
}

//===--------------------------------------------------------------------===//
// Quantifier
//===--------------------------------------------------------------------===//
QuantifiedExpression::QuantifiedExpression(unique_ptr<ParsedExpression> child_p, optional_idx min_count_p,
                                           optional_idx max_count_p, bool excluded_p, bool reluctant_p)
    : PatternExpression(ExpressionType::QUANTIFIER), child(std::move(child_p)), min_count(min_count_p),
      max_count(max_count_p), excluded(excluded_p), reluctant(reluctant_p) {
	if (min_count.IsValid() && max_count.IsValid() && min_count.GetIndex() > max_count.GetIndex()) {
		throw ParserException("Min count cannot be larger than max count");
	}
}

string QuantifiedExpression::QuantifierString(optional_idx min_count, optional_idx max_count, bool reluctant) {
	const char *suffix = reluctant ? "?" : "";
	if (!min_count.IsValid() && !max_count.IsValid()) {
		// nothing to be reluctant about: the part matches exactly once either way
		return "";
	}
	if (min_count.IsValid() && min_count.GetIndex() == 0 && !max_count.IsValid()) {
		return "*" + string(suffix);
	}
	if (min_count.IsValid() && min_count.GetIndex() == 1 && !max_count.IsValid()) {
		return "+" + string(suffix);
	}
	if (min_count.IsValid() && min_count.GetIndex() == 0 && max_count.IsValid() && max_count.GetIndex() == 1) {
		return "?" + string(suffix);
	}
	return StringUtil::Format("{%s,%s}%s", min_count.IsValid() ? to_string(min_count.GetIndex()) : "",
	                          max_count.IsValid() ? to_string(max_count.GetIndex()) : "", suffix);
}

string QuantifiedExpression::ToString() const {
	auto inner = child->ToString();
	if (child->GetExpressionType() == ExpressionType::QUANTIFIER) {
		// two quantifiers written back to back would be read as a single token
		inner = "(" + inner + ")";
	}
	auto quantified = excluded ? "{- " + inner + " -}" : inner;
	return quantified + QuantifierString(min_count, max_count, reluctant);
}

bool QuantifiedExpression::Equals(const ParsedExpression &other_p) const {
	if (!ParsedExpression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<QuantifiedExpression>();
	if (min_count != other.min_count || max_count != other.max_count || excluded != other.excluded ||
	    reluctant != other.reluctant) {
		return false;
	}
	return ParsedExpression::Equals(child, other.child);
}

unique_ptr<ParsedExpression> QuantifiedExpression::Copy() const {
	auto copy = make_uniq<QuantifiedExpression>(child->Copy(), min_count, max_count, excluded, reluctant);
	copy->CopyBase(*this);
	return std::move(copy);
}

void QuantifiedExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(200, "child", child);
	serializer.WritePropertyWithDefault<optional_idx>(201, "min_count", min_count, optional_idx());
	serializer.WritePropertyWithDefault<optional_idx>(202, "max_count", max_count, optional_idx());
	serializer.WritePropertyWithDefault<bool>(203, "excluded", excluded, false);
	serializer.WritePropertyWithDefault<bool>(204, "reluctant", reluctant, false);
}

unique_ptr<ParsedExpression> QuantifiedExpression::Deserialize(Deserializer &deserializer) {
	auto child = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(200, "child");
	auto min_count = deserializer.ReadPropertyWithExplicitDefault<optional_idx>(201, "min_count", optional_idx());
	auto max_count = deserializer.ReadPropertyWithExplicitDefault<optional_idx>(202, "max_count", optional_idx());
	auto excluded = deserializer.ReadPropertyWithExplicitDefault<bool>(203, "excluded", false);
	auto reluctant = deserializer.ReadPropertyWithExplicitDefault<bool>(204, "reluctant", false);
	return make_uniq_base<ParsedExpression, QuantifiedExpression>(std::move(child), min_count, max_count, excluded,
	                                                              reluctant);
}

//===--------------------------------------------------------------------===//
// Alternation
//===--------------------------------------------------------------------===//
string AlternationExpression::ToString() const {
	// the parentheses keep a quantifier from binding to the right hand side alone, and the spaces keep
	// the bar out of a neighbouring quantifier
	return StringUtil::Format("(%s | %s)", child_left->ToString(), child_right->ToString());
}

bool AlternationExpression::Equals(const ParsedExpression &other_p) const {
	if (!ParsedExpression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<AlternationExpression>();
	return ParsedExpression::Equals(child_left, other.child_left) &&
	       ParsedExpression::Equals(child_right, other.child_right);
}

unique_ptr<ParsedExpression> AlternationExpression::Copy() const {
	auto copy = make_uniq<AlternationExpression>(child_left->Copy(), child_right->Copy());
	copy->CopyBase(*this);
	return std::move(copy);
}

void AlternationExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(200, "child_left", child_left);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(201, "child_right", child_right);
}

unique_ptr<ParsedExpression> AlternationExpression::Deserialize(Deserializer &deserializer) {
	auto child_left = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(200, "child_left");
	auto child_right = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(201, "child_right");
	return make_uniq_base<ParsedExpression, AlternationExpression>(std::move(child_left), std::move(child_right));
}

//===--------------------------------------------------------------------===//
// Anchor
//===--------------------------------------------------------------------===//
string AnchorExpression::ToString() const {
	return at_end ? "$" : "^";
}

bool AnchorExpression::Equals(const ParsedExpression &other_p) const {
	if (!ParsedExpression::Equals(other_p)) {
		return false;
	}
	return at_end == other_p.Cast<AnchorExpression>().at_end;
}

unique_ptr<ParsedExpression> AnchorExpression::Copy() const {
	auto copy = make_uniq<AnchorExpression>(at_end);
	copy->CopyBase(*this);
	return std::move(copy);
}

void AnchorExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<bool>(200, "at_end", at_end, false);
}

unique_ptr<ParsedExpression> AnchorExpression::Deserialize(Deserializer &deserializer) {
	auto at_end = deserializer.ReadPropertyWithExplicitDefault<bool>(200, "at_end", false);
	return make_uniq_base<ParsedExpression, AnchorExpression>(at_end);
}

} // namespace duckdb
