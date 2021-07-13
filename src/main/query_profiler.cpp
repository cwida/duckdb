#include "duckdb/main/query_profiler.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/fstream.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/helper/physical_execute.hpp"
#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <utility>
#include <algorithm>

namespace duckdb {

void QueryProfiler::StartQuery(string query) {
	if (!enabled) {
		return;
	}
	this->running = true;
	this->query = move(query);
	tree_map.clear();
	root = nullptr;
	phase_timings.clear();
	phase_stack.clear();

	main_query.Start();
}

bool QueryProfiler::OperatorRequiresProfiling(PhysicalOperatorType op_type) {
	switch (op_type) {
	case PhysicalOperatorType::ORDER_BY:
	case PhysicalOperatorType::RESERVOIR_SAMPLE:
	case PhysicalOperatorType::STREAMING_SAMPLE:
	case PhysicalOperatorType::LIMIT:
	case PhysicalOperatorType::TOP_N:
	case PhysicalOperatorType::WINDOW:
	case PhysicalOperatorType::UNNEST:
	case PhysicalOperatorType::SIMPLE_AGGREGATE:
	case PhysicalOperatorType::HASH_GROUP_BY:
	case PhysicalOperatorType::FILTER:
	case PhysicalOperatorType::PROJECTION:
	case PhysicalOperatorType::COPY_TO_FILE:
	case PhysicalOperatorType::TABLE_SCAN:
	case PhysicalOperatorType::CHUNK_SCAN:
	case PhysicalOperatorType::DELIM_SCAN:
	case PhysicalOperatorType::EXPRESSION_SCAN:
	case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
	case PhysicalOperatorType::NESTED_LOOP_JOIN:
	case PhysicalOperatorType::HASH_JOIN:
	case PhysicalOperatorType::CROSS_PRODUCT:
	case PhysicalOperatorType::PIECEWISE_MERGE_JOIN:
	case PhysicalOperatorType::DELIM_JOIN:
	case PhysicalOperatorType::UNION:
	case PhysicalOperatorType::RECURSIVE_CTE:
	case PhysicalOperatorType::EMPTY_RESULT:
		return true;
	default:
		return false;
	}
}

void QueryProfiler::EndQuery() {
	if (!enabled || !running) {
		return;
	}

	main_query.End();
	this->running = false;
	// print or output the query profiling after termination, if this is enabled
	if (automatic_print_format != ProfilerPrintFormat::NONE) {
		// check if this query should be output based on the operator types
		string query_info;
		if (automatic_print_format == ProfilerPrintFormat::JSON) {
			query_info = ToJSON();
		} else if (automatic_print_format == ProfilerPrintFormat::QUERY_TREE) {
			query_info = ToString();
		} else if (automatic_print_format == ProfilerPrintFormat::QUERY_TREE_OPTIMIZER) {
			query_info = ToString(true);
		}

		if (save_location.empty()) {
			Printer::Print(query_info);
			Printer::Print("\n");
		} else {
			WriteToFile(save_location.c_str(), query_info);
		}
	}
}

void QueryProfiler::StartPhase(string new_phase) {
	if (!enabled || !running) {
		return;
	}

	if (!phase_stack.empty()) {
		// there are active phases
		phase_profiler.End();
		// add the timing to all phases prior to this one
		string prefix = "";
		for (auto &phase : phase_stack) {
			phase_timings[phase] += phase_profiler.Elapsed();
			prefix += phase + " > ";
		}
		// when there are previous phases, we prefix the current phase with those phases
		new_phase = prefix + new_phase;
	}

	// start a new phase
	phase_stack.push_back(new_phase);
	// restart the timer
	phase_profiler.Start();
}

void QueryProfiler::EndPhase() {
	if (!enabled || !running) {
		return;
	}
	D_ASSERT(phase_stack.size() > 0);

	// end the timer
	phase_profiler.End();
	// add the timing to all currently active phases
	for (auto &phase : phase_stack) {
		phase_timings[phase] += phase_profiler.Elapsed();
	}
	// now remove the last added phase
	phase_stack.pop_back();

	if (!phase_stack.empty()) {
		phase_profiler.Start();
	}
}

void QueryProfiler::Initialize(PhysicalOperator *root_op) {
	if (!enabled || !running) {
		return;
	}
	this->query_requires_profiling = false;
	this->root = CreateTree(root_op);
	if (!query_requires_profiling) {
		// query does not require profiling: disable profiling for this query
		this->running = false;
		tree_map.clear();
		root = nullptr;
		phase_timings.clear();
		phase_stack.clear();
	}
}

OperatorProfiler::OperatorProfiler(bool enabled_p) : enabled(enabled_p) {
	execution_stack = std::stack<const PhysicalOperator *>();
}

void OperatorProfiler::StartOperator(const PhysicalOperator *phys_op) {
	if (!enabled) {
		return;
	}

	if (!execution_stack.empty()) {
		// add timing for the previous element
		op.End();

		AddTiming(execution_stack.top(), op.Elapsed(), 0);
	}

	execution_stack.push(phys_op);

	// start timing for current element
	op.Start();
}

void OperatorProfiler::EndOperator(DataChunk *chunk) {
	if (!enabled) {
		return;
	}

	// finish timing for the current element
	op.End();

	AddTiming(execution_stack.top(), op.Elapsed(), chunk ? chunk->size() : 0);

	D_ASSERT(!execution_stack.empty());
	execution_stack.pop();

	// start timing again for the previous element, if any
	if (!execution_stack.empty()) {
		op.Start();
	}
}

void OperatorProfiler::AddTiming(const PhysicalOperator *op, double time, idx_t elements) {
	if (!enabled) {
		return;
	}
	if (!Value::DoubleIsValid(time)) {
		return;
	}
	auto entry = timings.find(op);
	if (entry == timings.end()) {
		// add new entry
		timings[op] = OperatorInformation(time, elements);
	} else {
		// add to existing entry
		entry->second.time += time;
		entry->second.elements += elements;
	}
}
void OperatorProfiler::Flush(const PhysicalOperator *phys_op, ExpressionExecutor *expression_executor,
                             const string &name, int id) {
	auto entry = timings.find(phys_op);
	if (entry == timings.end()) {
		return;
	}
	auto &operator_timing = timings.find(phys_op)->second;
	if (int(operator_timing.executors_info.size()) <= id) {
		operator_timing.executors_info.resize(id + 1);
	}
	operator_timing.executors_info[id] = make_unique<ExpressionExecutorInfo>(*expression_executor, name, id);
	operator_timing.name = phys_op->GetName();
}

void QueryProfiler::Flush(OperatorProfiler &profiler) {
	if (!enabled || !running) {
		return;
	}
	lock_guard<mutex> guard(flush_lock);
	for (auto &node : profiler.timings) {
		auto entry = tree_map.find(node.first);
		D_ASSERT(entry != tree_map.end());

		entry->second->info.time += node.second.time;
		entry->second->info.elements += node.second.elements;
		if (!detailed_enabled) {
			continue;
		}
		for (auto &info : node.second.executors_info) {
			if (!info) {
				continue;
			}
			if (int(entry->second->info.executors_info.size()) <= info->id) {
				entry->second->info.executors_info.resize(info->id + 1);
			}
			entry->second->info.executors_info[info->id] = move(info);
		}
	}
}

static string DrawPadded(const string &str, idx_t width) {
	if (str.size() > width) {
		return str.substr(0, width);
	} else {
		width -= str.size();
		int half_spaces = width / 2;
		int extra_left_space = width % 2 != 0 ? 1 : 0;
		return string(half_spaces + extra_left_space, ' ') + str + string(half_spaces, ' ');
	}
}

static string RenderTitleCase(string str) {
	str = StringUtil::Lower(str);
	str[0] = toupper(str[0]);
	for (idx_t i = 0; i < str.size(); i++) {
		if (str[i] == '_') {
			str[i] = ' ';
			if (i + 1 < str.size()) {
				str[i + 1] = toupper(str[i + 1]);
			}
		}
	}
	return str;
}

static string RenderTiming(double timing) {
	string timing_s;
	if (timing >= 1) {
		timing_s = StringUtil::Format("%.2f", timing);
	} else if (timing >= 0.1) {
		timing_s = StringUtil::Format("%.3f", timing);
	} else {
		timing_s = StringUtil::Format("%.4f", timing);
	}
	return timing_s + "s";
}

string QueryProfiler::ToString(bool print_optimizer_output) const {
	std::stringstream str;
	ToStream(str, print_optimizer_output);
	return str.str();
}

void QueryProfiler::ToStream(std::ostream &ss, bool print_optimizer_output) const {
	if (!enabled) {
		ss << "Query profiling is disabled. Call "
		      "Connection::EnableProfiling() to enable profiling!";
		return;
	}
	ss << "┌─────────────────────────────────────┐\n";
	ss << "│┌───────────────────────────────────┐│\n";
	ss << "││    Query Profiling Information    ││\n";
	ss << "│└───────────────────────────────────┘│\n";
	ss << "└─────────────────────────────────────┘\n";
	ss << StringUtil::Replace(query, "\n", " ") + "\n";
	if (query.empty()) {
		return;
	}

	constexpr idx_t TOTAL_BOX_WIDTH = 39;
	ss << "┌─────────────────────────────────────┐\n";
	ss << "│┌───────────────────────────────────┐│\n";
	string total_time = "Total Time: " + RenderTiming(main_query.Elapsed());
	ss << "││" + DrawPadded(total_time, TOTAL_BOX_WIDTH - 4) + "││\n";
	ss << "│└───────────────────────────────────┘│\n";
	ss << "└─────────────────────────────────────┘\n";
	// print phase timings
	if (print_optimizer_output) {
		bool has_previous_phase = false;
		for (const auto &entry : GetOrderedPhaseTimings()) {
			if (!StringUtil::Contains(entry.first, " > ")) {
				// primary phase!
				if (has_previous_phase) {
					ss << "│└───────────────────────────────────┘│\n";
					ss << "└─────────────────────────────────────┘\n";
				}
				ss << "┌─────────────────────────────────────┐\n";
				ss << "│" +
				          DrawPadded(RenderTitleCase(entry.first) + ": " + RenderTiming(entry.second),
				                     TOTAL_BOX_WIDTH - 2) +
				          "│\n";
				ss << "│┌───────────────────────────────────┐│\n";
				has_previous_phase = true;
			} else {
				string entry_name = StringUtil::Split(entry.first, " > ")[1];
				ss << "││" +
				          DrawPadded(RenderTitleCase(entry_name) + ": " + RenderTiming(entry.second),
				                     TOTAL_BOX_WIDTH - 4) +
				          "││\n";
			}
		}
		if (has_previous_phase) {
			ss << "│└───────────────────────────────────┘│\n";
			ss << "└─────────────────────────────────────┘\n";
		}
	}
	// render the main operator tree
	if (root) {
		Render(*root, ss);
	}
}

// Print a row
static void PrintRow(std::ostream &ss, const string &annotation, int id, const string &name, double time,
                     int sample_counter, int tuple_counter, string extra_info, int depth) {
	ss << string(depth * 3, ' ') << " {\n";
	ss << string(depth * 3, ' ') << "   \"annotation\": \"" + annotation + "\",\n";
	ss << string(depth * 3, ' ') << "   \"id\": " + to_string(id) + ",\n";
	ss << string(depth * 3, ' ') << "   \"name\": \"" + name + "\",\n";
#if defined(RDTSC)
	ss << string(depth * 3, ' ') << "   \"timing\": \"NULL\" ,\n";
	ss << string(depth * 3, ' ') << "   \"cycles_per_tuple\": " + StringUtil::Format("%.4f", time) + ",\n";
#else
	ss << string(depth * 3, ' ') << "   \"timing\":" + to_string(time) + ",\n";
	ss << string(depth * 3, ' ') << "   \"cycles_per_tuple\": \"NULL\" ,\n";
#endif
	ss << string(depth * 3, ' ') << "   \"sample_size\": " << to_string(sample_counter) + ",\n";
	ss << string(depth * 3, ' ') << "   \"input_size\": " << to_string(tuple_counter) + ",\n";
	ss << string(depth * 3, ' ') << "   \"extra_info\": \""
	   << StringUtil::Replace(std::move(extra_info), "\n", "\\n") + "\"\n";
	ss << string(depth * 3, ' ') << " },\n";
}

static void ExtractFunctions(std::ostream &ss, ExpressionInfo &info, int &fun_id, int depth) {
	if (info.hasfunction) {
		D_ASSERT(info.sample_tuples_count != 0);
		PrintRow(ss, "Function", fun_id++, info.function_name,
		         int(info.function_time) / double(info.sample_tuples_count), info.sample_tuples_count,
		         info.tuples_count, "", depth);
	}
	if (info.children.empty()) {
		return;
	}
	// extract the children of this node
	for (auto &child : info.children) {
		ExtractFunctions(ss, *child, fun_id, depth);
	}
}

static void ToJSONRecursive(QueryProfiler::TreeNode &node, std::ostream &ss, int depth = 1) {
	ss << string(depth * 3, ' ') << " {\n";
	ss << string(depth * 3, ' ') << "   \"name\": \"" + node.name + "\",\n";
	ss << string(depth * 3, ' ') << "   \"timing\":" + to_string(node.info.time) + ",\n";
	ss << string(depth * 3, ' ') << "   \"cardinality\":" + to_string(node.info.elements) + ",\n";
	ss << string(depth * 3, ' ')
	   << "   \"extra_info\": \"" + StringUtil::Replace(node.extra_info, "\n", "\\n") + "\",\n";
	ss << string(depth * 3, ' ') << "   \"timings\": [";
	int32_t function_counter = 1;
	int32_t expression_counter = 1;
	ss << "\n ";
	for (auto &expr_executor : node.info.executors_info) {
		// For each Expression tree
		if (!expr_executor) {
			continue;
		}
		for (auto &expr_timer : expr_executor->roots) {
			D_ASSERT(expr_timer->sample_tuples_count != 0);
			PrintRow(ss, "ExpressionRoot", expression_counter++, expr_timer->name,
			         int(expr_timer->time) / double(expr_timer->sample_tuples_count), expr_timer->sample_tuples_count,
			         expr_timer->tuples_count, expr_timer->extra_info, depth + 1);
			// Extract all functions inside the tree
			ExtractFunctions(ss, *expr_timer->root, function_counter, depth + 1);
		}
	}
	ss.seekp(-2, ss.cur);
	ss << "\n";
	ss << string(depth * 3, ' ') << "   ],\n";
	ss << string(depth * 3, ' ') << "   \"children\": [\n";
	if (node.children.empty()) {
		ss << string(depth * 3, ' ') << "   ]\n";
	} else {
		for (idx_t i = 0; i < node.children.size(); i++) {
			if (i > 0) {
				ss << ",\n";
			}
			ToJSONRecursive(*node.children[i], ss, depth + 1);
		}
		ss << string(depth * 3, ' ') << "   ]\n";
	}
	ss << string(depth * 3, ' ') << " }\n";
}

string QueryProfiler::ToJSON() const {
	if (!enabled) {
		return "{ \"result\": \"disabled\" }\n";
	}
	if (query.empty()) {
		return "{ \"result\": \"empty\" }\n";
	}
	if (!root) {
		return "{ \"result\": \"error\" }\n";
	}
	std::stringstream ss;
	ss << "{\n";
	ss << "   \"name\":  \"Query\", \n";
	ss << "   \"result\": " + to_string(main_query.Elapsed()) + ",\n";
	ss << "   \"timing\": " + to_string(main_query.Elapsed()) + ",\n";
	ss << "   \"cardinality\": " + to_string(root->info.elements) + ",\n";
	// JSON cannot have literal control characters in string literals
	string extra_info = StringUtil::Replace(query, "\t", "\\t");
	extra_info = StringUtil::Replace(extra_info, "\n", "\\n");
	ss << "   \"extra-info\": \"" + extra_info + "\", \n";
	// print the phase timings
	ss << "   \"timings\": [\n";
	const auto &ordered_phase_timings = GetOrderedPhaseTimings();
	for (idx_t i = 0; i < ordered_phase_timings.size(); i++) {
		if (i > 0) {
			ss << ",\n";
		}
		ss << "   {\n";
		ss << "   \"annotation\": \"" + ordered_phase_timings[i].first + "\", \n";
		ss << "   \"timing\": " + to_string(ordered_phase_timings[i].second) + "\n";
		ss << "   }";
	}
	ss << "\n";
	ss << "   ],\n";
	// recursively print the physical operator tree
	ss << "   \"children\": [\n";
	ToJSONRecursive(*root, ss);
	ss << "   ]\n";
	ss << "}";
	return ss.str();
}

void QueryProfiler::WriteToFile(const char *path, string &info) const {
	ofstream out(path);
	out << info;
	out.close();
	// throw an IO exception if it fails to write the file
	if (out.fail()) {
		throw IOException(strerror(errno));
	}
}

unique_ptr<QueryProfiler::TreeNode> QueryProfiler::CreateTree(PhysicalOperator *root, idx_t depth) {
	if (OperatorRequiresProfiling(root->type)) {
		this->query_requires_profiling = true;
	}
	auto node = make_unique<QueryProfiler::TreeNode>();
	node->name = root->GetName();
	node->extra_info = root->ParamsToString();
	node->depth = depth;
	tree_map[root] = node.get();
	for (auto &child : root->children) {
		auto child_node = CreateTree(child.get(), depth + 1);
		node->children.push_back(move(child_node));
	}
	switch (root->type) {
	case PhysicalOperatorType::DELIM_JOIN: {
		auto &delim_join = (PhysicalDelimJoin &)*root;
		auto child_node = CreateTree((PhysicalOperator *)delim_join.join.get(), depth + 1);
		node->children.push_back(move(child_node));
		child_node = CreateTree((PhysicalOperator *)delim_join.distinct.get(), depth + 1);
		node->children.push_back(move(child_node));
		break;
	}
	case PhysicalOperatorType::EXECUTE: {
		auto &execute = (PhysicalExecute &)*root;
		auto child_node = CreateTree((PhysicalOperator *)execute.plan, depth + 1);
		node->children.push_back(move(child_node));
		break;
	}
	default:
		break;
	}
	return node;
}

void QueryProfiler::Render(const QueryProfiler::TreeNode &node, std::ostream &ss) const {
	TreeRenderer renderer;
	if (IsDetailedEnabled()) {
		renderer.EnableDetailed();
	} else {
		renderer.EnableStandard();
	}
	renderer.Render(node, ss);
}

void QueryProfiler::Print() {
	Printer::Print(ToString());
}

vector<QueryProfiler::PhaseTimingItem> QueryProfiler::GetOrderedPhaseTimings() const {
	vector<PhaseTimingItem> result;
	// first sort the phases alphabetically
	vector<string> phases;
	for (auto &entry : phase_timings) {
		phases.push_back(entry.first);
	}
	std::sort(phases.begin(), phases.end());
	for (const auto &phase : phases) {
		auto entry = phase_timings.find(phase);
		D_ASSERT(entry != phase_timings.end());
		result.emplace_back(entry->first, entry->second);
	}
	return result;
}
void QueryProfiler::Propagate(QueryProfiler &qp) {
	this->automatic_print_format = qp.automatic_print_format;
	this->save_location = qp.save_location;
	this->enabled = qp.enabled;
	this->detailed_enabled = qp.detailed_enabled;
}

void ExpressionInfo::ExtractExpressionsRecursive(unique_ptr<ExpressionState> &state) {
	if (state->child_states.empty()) {
		return;
	}
	// extract the children of this node
	for (auto &child : state->child_states) {
		auto expr_info = make_unique<ExpressionInfo>();
		if (child->expr.expression_class == ExpressionClass::BOUND_FUNCTION) {
			expr_info->hasfunction = true;
			expr_info->function_name = ((BoundFunctionExpression &)child->expr).function.ToString();
			expr_info->function_time = child->profiler.time;
			expr_info->sample_tuples_count = child->profiler.sample_tuples_count;
			expr_info->tuples_count = child->profiler.tuples_count;
		}
		expr_info->ExtractExpressionsRecursive(child);
		children.push_back(move(expr_info));
	}
	return;
}

ExpressionExecutorInfo::ExpressionExecutorInfo(ExpressionExecutor &executor, const string &name, int id) : id(id) {
	// Extract Expression Root Information from ExpressionExecutorStats
	for (auto &state : executor.GetStates()) {
		roots.push_back(make_unique<ExpressionRootInfo>(*state, name));
	}
}

ExpressionRootInfo::ExpressionRootInfo(ExpressionExecutorState &state, string name)
    : current_count(state.profiler.current_count), sample_count(state.profiler.sample_count),
      sample_tuples_count(state.profiler.sample_tuples_count), tuples_count(state.profiler.tuples_count),
      name(state.name), time(state.profiler.time) {
	// Use the name of expression-tree as extra-info
	extra_info = move(name);
	auto expression_info_p = make_unique<ExpressionInfo>();
	// Maybe root has a function
	if (state.root_state->expr.expression_class == ExpressionClass::BOUND_FUNCTION) {
		expression_info_p->hasfunction = true;
		expression_info_p->function_name = ((BoundFunctionExpression &)state.root_state->expr).function.name;
		expression_info_p->function_time = state.root_state->profiler.time;
		expression_info_p->sample_tuples_count = state.root_state->profiler.sample_tuples_count;
		expression_info_p->tuples_count = state.root_state->profiler.tuples_count;
	}
	expression_info_p->ExtractExpressionsRecursive(state.root_state);
	root = move(expression_info_p);
}
} // namespace duckdb
