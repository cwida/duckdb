//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/pattern_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! The nodes a PATTERN is built from. They share ExpressionClass::PATTERN and are told apart by their
//! expression type.
class PatternExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::PATTERN;

public:
	explicit PatternExpression(ExpressionType type) : ParsedExpression(type, ExpressionClass::PATTERN) {
	}

public:
	//! One entry point for the three, which share an expression class
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);
};

//! A B: the parts match one after the other
class ConcatenationExpression : public PatternExpression {
public:
	explicit ConcatenationExpression(vector<unique_ptr<ParsedExpression>> children_p)
	    : PatternExpression(ExpressionType::CONCATENATION), children(std::move(children_p)) {
	}

public:
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

	vector<unique_ptr<ParsedExpression>> children;
};

//! A*, A+, A{2,4}: an unset bound means "unbounded" in that direction
class QuantifiedExpression : public PatternExpression {
public:
	QuantifiedExpression(unique_ptr<ParsedExpression> child_p, optional_idx min_count_p, optional_idx max_count_p,
	                     bool excluded_p = false, bool reluctant_p = false);

public:
	//! The quantifier as it is written after the part it applies to, empty when both bounds are unset
	static string QuantifierString(optional_idx min_count, optional_idx max_count, bool reluctant);

	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

	unique_ptr<ParsedExpression> child;

	optional_idx min_count;
	optional_idx max_count;
	//! {- ... -}: the rows this matches take part in the match but are left out of the output
	bool excluded;
	//! A trailing ?: prefer the fewest repetitions rather than the most
	bool reluctant;
};

//! ^ and $: holds at the start or the end of the partition, and takes no row
class AnchorExpression : public PatternExpression {
public:
	explicit AnchorExpression(bool at_end_p) : PatternExpression(ExpressionType::ANCHOR), at_end(at_end_p) {
	}

public:
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

	bool at_end;
};

//! A | B: either side matches
class AlternationExpression : public PatternExpression {
public:
	AlternationExpression(unique_ptr<ParsedExpression> child_left_p, unique_ptr<ParsedExpression> child_right_p)
	    : PatternExpression(ExpressionType::ALTERNATION), child_left(std::move(child_left_p)),
	      child_right(std::move(child_right_p)) {
	}

public:
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

	unique_ptr<ParsedExpression> child_left;
	unique_ptr<ParsedExpression> child_right;
};

} // namespace duckdb
