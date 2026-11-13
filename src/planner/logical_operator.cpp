#include "duckdb/planner/logical_operator.hpp"

#include "duckdb/planner/expression_iterator.hpp"

#include "duckdb/original/std/sstream.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_order.hpp"

namespace duckdb {

LogicalOperator::LogicalOperator(LogicalOperatorType type)
    : type(type), estimated_cardinality(0), has_estimated_cardinality(false) {
}

LogicalOperator::LogicalOperator(LogicalOperatorType type, vector<unique_ptr<Expression>> expressions)
    : type(type), expressions(std::move(expressions)), estimated_cardinality(0), has_estimated_cardinality(false) {
}

LogicalOperator::~LogicalOperator() {
}

vector<ColumnBinding> LogicalOperator::GetColumnBindings() {
	return {ColumnBinding(TableIndex(0), ProjectionIndex(0))};
}

TableIndex LogicalOperator::GetRootIndex() {
	auto bindings = GetColumnBindings();
	if (bindings.empty()) {
		throw InternalException("Empty bindings in GetRootIndex");
	}
	auto root_index = bindings[0].table_index;
	for (idx_t i = 1; i < bindings.size(); i++) {
		if (bindings[i].table_index != root_index) {
			throw InternalException("GetRootIndex - multiple column bindings found");
		}
	}
	return root_index;
}
void LogicalOperator::SetParamsEstimatedCardinality(InsertionOrderPreservingMap<string> &result) const {
	if (has_estimated_cardinality) {
		result[RenderTreeNode::ESTIMATED_CARDINALITY] = StringUtil::Format("%llu", estimated_cardinality);
	}
}

void LogicalOperator::SetEstimatedCardinality(idx_t _estimated_cardinality) {
	estimated_cardinality = _estimated_cardinality;
	has_estimated_cardinality = true;
}

// LCOV_EXCL_START
string LogicalOperator::ColumnBindingsToString(const vector<ColumnBinding> &bindings) {
	string result = "{";
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (i != 0) {
			result += ", ";
		}
		result += bindings[i].ToString();
	}
	return result + "}";
}

void LogicalOperator::PrintColumnBindings() {
	Printer::Print(ColumnBindingsToString(GetColumnBindings()));
}
// LCOV_EXCL_STOP

string LogicalOperator::GetName() const {
	return LogicalOperatorToString(type);
}

InsertionOrderPreservingMap<string> LogicalOperator::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string expressions_info;
	for (idx_t i = 0; i < expressions.size(); i++) {
		if (i > 0) {
			expressions_info += "\n";
		}
		expressions_info += expressions[i]->GetName();
	}
	result["Expressions"] = expressions_info;
	SetParamsEstimatedCardinality(result);
	return result;
}

vector<ExplainSubPlan> LogicalOperator::GetExplainSubPlans() const {
	return {};
}

bool LogicalOperator::HasSideEffects() const {
	switch (type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		return true;
	default:
		break;
	}
	for (auto &child : children) {
		if (child && child->HasSideEffects()) {
			return true;
		}
	}
	return false;
}

bool LogicalOperator::HasVolatileExpressions() const {
	bool is_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(*this, [&](const unique_ptr<Expression> *expression) {
		if ((*expression)->IsVolatile()) {
			is_volatile = true;
		}
	});
	if (is_volatile) {
		return true;
	}
	for (auto &child : children) {
		if (child && child->HasVolatileExpressions()) {
			return true;
		}
	}
	return false;
}

void LogicalOperator::ResolveOperatorTypes() {
	types.clear();
	// first resolve child types
	for (auto &child : children) {
		child->ResolveOperatorTypes();
	}
	// now resolve the types for this operator
	ResolveTypes();
	D_ASSERT(types.size() == GetColumnBindings().size());
}

vector<ColumnBinding> LogicalOperator::GenerateColumnBindings(TableIndex table_idx, idx_t column_count) {
	vector<ColumnBinding> result;
	result.reserve(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		result.emplace_back(table_idx, ProjectionIndex(i));
	}
	return result;
}

vector<LogicalType> LogicalOperator::MapTypes(const vector<LogicalType> &types,
                                              const vector<ProjectionIndex> &projection_map) {
	if (projection_map.empty()) {
		return types;
	} else {
		vector<LogicalType> result_types;
		result_types.reserve(projection_map.size());
		for (auto index : projection_map) {
			result_types.push_back(types[index]);
		}
		return result_types;
	}
}

vector<ColumnBinding> LogicalOperator::MapBindings(const vector<ColumnBinding> &bindings,
                                                   const vector<ProjectionIndex> &projection_map) {
	if (projection_map.empty()) {
		return bindings;
	} else {
		vector<ColumnBinding> result_bindings;
		result_bindings.reserve(projection_map.size());
		for (auto index : projection_map) {
			result_bindings.push_back(bindings[index]);
		}
		return result_bindings;
	}
}

string LogicalOperator::ToString(optional_ptr<ClientContext> context, const ProfilerPrintFormat &format) const {
	auto renderer = context ? TreeRenderer::CreateRenderer(*context, format) : TreeRenderer::CreateRenderer(format);
	if (!renderer) {
		// formats without output (e.g. "no_output") render nothing
		return string();
	}
	StringTreeRenderer ss;
	auto tree = RenderTree::CreateRenderTree(*this);
	renderer->ToStream(*tree, ss);
	return ss.str();
}

void LogicalOperator::Verify(ClientContext &context) {
#ifdef DEBUG
	// verify expressions
	for (idx_t expr_idx = 0; expr_idx < expressions.size(); expr_idx++) {
		auto str = expressions[expr_idx]->ToString();
		// verify that we can (correctly) copy this expression
		auto copy = expressions[expr_idx]->Copy();
		auto original_hash = expressions[expr_idx]->Hash();
		auto copy_hash = copy->Hash();
		// copy should be identical to original
		D_ASSERT(expressions[expr_idx]->ToString() == copy->ToString());
		D_ASSERT(original_hash == copy_hash);
		D_ASSERT(Expression::Equals(expressions[expr_idx], copy));

		for (idx_t other_idx = 0; other_idx < expr_idx; other_idx++) {
			// comparison with other expressions
			auto other_hash = expressions[other_idx]->Hash();
			bool expr_equal = Expression::Equals(expressions[expr_idx], expressions[other_idx]);
			if (original_hash != other_hash) {
				// if the hashes are not equal the expressions should not be equal either
				D_ASSERT(!expr_equal);
			}
		}
		D_ASSERT(!str.empty());

		// verify that serialization + deserialization round-trips correctly
		if (expressions[expr_idx]->HasParameter()) {
			continue;
		}
		MemoryStream stream(Allocator::Get(context));
		// We are serializing a query plan
		try {
			auto &config = DBConfig::GetConfig(context);
			SerializationOptions options;
			if (config.options.storage_compatibility.manually_set) {
				options.storage_compatibility = config.options.storage_compatibility;
			} else {
				options.storage_compatibility = StorageCompatibility::Latest();
			}

			BinarySerializer::Serialize(*expressions[expr_idx], stream, options);
		} catch (NotImplementedException &ex) {
			// ignore for now (FIXME)
			continue;
		}
		// Rewind the stream
		stream.Rewind();

		bound_parameter_map_t parameters;
		auto deserialized_expression = BinaryDeserializer::Deserialize<Expression>(stream, context, parameters);

		// FIXME: expressions might not be equal yet because of statistics propagation
		continue;
		D_ASSERT(Expression::Equals(expressions[expr_idx], deserialized_expression));
		D_ASSERT(expressions[expr_idx]->Hash() == deserialized_expression->Hash());
	}
	D_ASSERT(!ToString().empty());
	for (auto &child : children) {
		child->Verify(context);
	}
#endif
}

void LogicalOperator::AddChild(unique_ptr<LogicalOperator> child) {
	D_ASSERT(child);
	children.push_back(std::move(child));
}

idx_t LogicalOperator::EstimateCardinality(ClientContext &context) {
	// simple estimator, just take the max of the children
	if (has_estimated_cardinality) {
		return estimated_cardinality;
	}
	idx_t max_cardinality = 0;
	for (auto &child : children) {
		max_cardinality = MaxValue(child->EstimateCardinality(context), max_cardinality);
	}
	has_estimated_cardinality = true;
	estimated_cardinality = max_cardinality;
	return estimated_cardinality;
}

void LogicalOperator::Print() {
	Printer::Print(ToString());
}

vector<TableIndex> LogicalOperator::GetTableIndex() const {
	return vector<TableIndex> {};
}

unique_ptr<LogicalOperator> LogicalOperator::Copy(ClientContext &context) const {
	MemoryStream stream(Allocator::Get(context));
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::Latest();
	BinarySerializer serializer(stream, options);
	try {
		serializer.Begin();
		this->Serialize(serializer);
		serializer.End();
	} catch (NotImplementedException &ex) {
		ErrorData error(ex);
		throw NotImplementedException("Logical Operator Copy requires the logical operator and all of its children to "
		                              "be serializable: " +
		                              error.RawMessage());
	}
	stream.Rewind();
	bound_parameter_map_t parameters;
	auto op_copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
	return op_copy;
}

} // namespace duckdb
