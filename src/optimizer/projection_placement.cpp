#include "duckdb/optimizer/projection_placement.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {

constexpr double MINIMUM_COST_RATIO = 0.75;

struct PathStep {
	reference<LogicalOperator> op;
	idx_t child_index;
};

struct ExpressionPlacement {
	idx_t expression_index = 0;
	vector<PathStep> path;
	unique_ptr<Expression> expression;
	double evaluation_cardinality = 0;
	double output_transport_cardinality = 0;
	double input_transport_savings_cardinality = 0;
};

struct PlacementGroup {
	vector<PathStep> path;
	vector<ExpressionPlacement> placements;
};

struct ExpressionBindingWidth {
	ColumnBinding binding;
	LogicalType type;
	idx_t width;
};

struct LiftPathStep {
	reference<LogicalOperator> op;
	idx_t child_index;
	vector<ColumnBinding> output_bindings;
	column_binding_set_t result_bindings;
};

struct LiftOption {
	idx_t path_size;
	double evaluation_cardinality;
	double input_transport_cardinality;
	column_binding_set_t result_bindings;
	vector<ColumnBinding> target_bindings;
	vector<LogicalType> target_types;
};

struct LiftPlacement {
	optional_ptr<LogicalProjection> source;
	idx_t expression_index;
	ColumnBinding source_binding;
	vector<ExpressionBindingWidth> dependencies;
	vector<LiftPathStep> path;
	column_binding_set_t result_bindings;
	vector<ColumnBinding> target_bindings;
	vector<LogicalType> target_types;
};

struct LiftDependency {
	ColumnBinding original_binding;
	ColumnBinding current_binding;
	LogicalType type;
};

static unique_ptr<LogicalOperator> &GetPathTarget(const vector<PathStep> &path) {
	D_ASSERT(!path.empty());
	auto &last_step = path.back();
	return last_step.op.get().children[last_step.child_index];
}

static void CopyStatistics(column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                           const ColumnBinding &source, const ColumnBinding &target) {
	auto entry = statistics_map.find(source);
	if (entry == statistics_map.end()) {
		return;
	}
	unique_ptr<BaseStatistics> statistics;
	if (entry->second) {
		statistics = entry->second->Copy().ToUnique();
	}
	statistics_map.erase(target);
	if (statistics) {
		statistics_map.emplace(target, std::move(statistics));
	}
}

static bool GetBindingWidth(const ColumnBinding &binding, const LogicalType &type,
                            const column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map, idx_t &width) {
	auto physical_type = type.InternalType();
	if (TypeIsConstantSize(physical_type)) {
		width = GetTypeIdSize(physical_type);
		return true;
	}
	if (physical_type != PhysicalType::VARCHAR) {
		return false;
	}
	// Variable-width values are only costed when statistics provide a safe upper bound.
	auto entry = statistics_map.find(binding);
	if (entry == statistics_map.end() || !entry->second ||
	    entry->second->GetStatsType() != StatisticsType::STRING_STATS ||
	    !StringStats::HasMaxStringLength(*entry->second)) {
		return false;
	}
	width = GetTypeIdSize(physical_type) + StringStats::MaxStringLength(*entry->second);
	return true;
}

static bool GetExpressionWidth(const Expression &expression,
                               const column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                               idx_t &input_width, vector<ExpressionBindingWidth> &binding_widths) {
	vector<pair<ColumnBinding, LogicalType>> bindings;
	bool valid = true;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expression, [&](const BoundColumnRefExpression &colref) {
		    if (colref.Depth() != 0) {
			    valid = false;
			    return;
		    }
		    for (auto &entry : bindings) {
			    if (entry.first == colref.Binding()) {
				    return;
			    }
		    }
		    bindings.emplace_back(colref.Binding(), colref.GetReturnType());
	    });
	if (!valid || bindings.empty()) {
		return false;
	}
	input_width = 0;
	for (auto &entry : bindings) {
		idx_t binding_width;
		if (!GetBindingWidth(entry.first, entry.second, statistics_map, binding_width)) {
			return false;
		}
		input_width += binding_width;
		binding_widths.push_back({entry.first, entry.second, binding_width});
	}
	return true;
}

//! The number of expressions of the projection that read each binding
static column_binding_map_t<idx_t> CountReadingExpressions(const LogicalProjection &projection) {
	column_binding_map_t<idx_t> result;
	for (auto &expression : projection.expressions) {
		column_binding_set_t read_bindings;
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *expression, [&](const BoundColumnRefExpression &colref) {
			    if (colref.Depth() == 0 && read_bindings.insert(colref.Binding()).second) {
				    result[colref.Binding()]++;
			    }
		    });
	}
	return result;
}

//! The width of the bindings that no other expression of the projection reads
static idx_t GetExclusiveInputWidth(const column_binding_map_t<idx_t> &reading_expressions,
                                    const vector<ExpressionBindingWidth> &binding_widths) {
	idx_t result = 0;
	for (auto &binding_width : binding_widths) {
		auto entry = reading_expressions.find(binding_width.binding);
		if (entry != reading_expressions.end() && entry->second > 1) {
			continue;
		}
		result += binding_width.width;
	}
	return result;
}

static bool BindingsBelongToChild(const Expression &expression, LogicalOperator &child) {
	auto child_bindings = child.GetColumnBindings();
	bool found = false;
	bool valid = true;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expression, [&](const BoundColumnRefExpression &colref) {
		    found = true;
		    if (colref.Depth() != 0 ||
		        std::find(child_bindings.begin(), child_bindings.end(), colref.Binding()) == child_bindings.end()) {
			    valid = false;
		    }
	    });
	return found && valid;
}

static bool RewriteThroughProjection(unique_ptr<Expression> &expression, LogicalProjection &projection) {
	bool valid = true;
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expression, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
		    if (!valid || colref.Depth() != 0 || colref.Binding().table_index != projection.table_index ||
		        colref.Binding().column_index.GetIndex() >= projection.expressions.size()) {
			    valid = false;
			    return;
		    }
		    auto &source = *projection.expressions[colref.Binding().column_index.GetIndex()];
		    if (source.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		        source.GetReturnType() != colref.GetReturnType()) {
			    valid = false;
			    return;
		    }
		    auto &source_colref = source.Cast<BoundColumnRefExpression>();
		    if (source_colref.Depth() != 0) {
			    valid = false;
			    return;
		    }
		    colref.BindingMutable() = source_colref.Binding();
	    });
	return valid;
}

static bool IsEligibleJoin(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.type != LogicalOperatorType::LOGICAL_ANY_JOIN) {
		return false;
	}
	return op.Cast<LogicalJoin>().join_type == JoinType::INNER;
}

static bool ReferencesAnyBinding(const Expression &expression, const column_binding_set_t &bindings) {
	bool result = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expression, [&](const BoundColumnRefExpression &colref) {
		    if (colref.Depth() == 0 && bindings.find(colref.Binding()) != bindings.end()) {
			    result = true;
		    }
	    });
	return result;
}

static bool OperatorReferencesAnyBinding(LogicalOperator &op, const column_binding_set_t &bindings) {
	bool result = false;
	const auto &constant_op = op;
	LogicalOperatorVisitor::EnumerateExpressions(constant_op, [&](const auto &expression) {
		if (!result && ReferencesAnyBinding(**expression, bindings)) {
			result = true;
		}
	});
	return result;
}

static column_binding_set_t RetainedBindings(LogicalOperator &op, const column_binding_set_t &bindings) {
	column_binding_set_t result;
	for (auto &binding : op.GetColumnBindings()) {
		if (bindings.find(binding) != bindings.end()) {
			result.insert(binding);
		}
	}
	return result;
}

static bool TraceProjectionResult(LogicalProjection &projection, const column_binding_set_t &input_bindings,
                                  column_binding_set_t &output_bindings) {
	for (idx_t expression_idx = 0; expression_idx < projection.expressions.size(); expression_idx++) {
		auto &expression = *projection.expressions[expression_idx];
		if (!ReferencesAnyBinding(expression, input_bindings)) {
			continue;
		}
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &colref = expression.Cast<BoundColumnRefExpression>();
		if (colref.Depth() != 0 || input_bindings.find(colref.Binding()) == input_bindings.end()) {
			return false;
		}
		output_bindings.insert(ColumnBinding(projection.table_index, ProjectionIndex(expression_idx)));
	}
	return !output_bindings.empty();
}

static bool FindLiftPlacement(LogicalProjection &projection, idx_t expression_index, const vector<PathStep> &ancestors,
                              const column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                              const column_binding_map_t<idx_t> &reading_expressions, LiftPlacement &result) {
	auto &expression = *projection.expressions[expression_index];
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF || expression.IsVolatile() ||
	    expression.CanThrow() || !projection.children[0]->has_estimated_cardinality) {
		return false;
	}

	idx_t input_width;
	vector<ExpressionBindingWidth> dependencies;
	auto source_binding = ColumnBinding(projection.table_index, ProjectionIndex(expression_index));
	if (!GetExpressionWidth(expression, statistics_map, input_width, dependencies)) {
		return false;
	}
	auto exclusive_input_width = GetExclusiveInputWidth(reading_expressions, dependencies);

	column_binding_set_t current_bindings;
	current_bindings.insert(source_binding);
	vector<LiftPathStep> path;
	vector<LiftOption> options;
	double transport_cardinality = 0;

	for (idx_t ancestor_idx = ancestors.size(); ancestor_idx > 0; ancestor_idx--) {
		auto &ancestor = ancestors[ancestor_idx - 1];
		auto &op = ancestor.op.get();
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			column_binding_set_t projected_bindings;
			if (!TraceProjectionResult(op.Cast<LogicalProjection>(), current_bindings, projected_bindings)) {
				break;
			}
			current_bindings = std::move(projected_bindings);
		} else if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			if (op.children.size() != 1 || OperatorReferencesAnyBinding(op, current_bindings)) {
				break;
			}
			current_bindings = RetainedBindings(op, current_bindings);
			if (current_bindings.empty()) {
				break;
			}
		} else if (IsEligibleJoin(op)) {
			if (!op.has_estimated_cardinality || OperatorReferencesAnyBinding(op, current_bindings)) {
				break;
			}
			current_bindings = RetainedBindings(op, current_bindings);
			if (current_bindings.empty()) {
				break;
			}
		} else {
			break;
		}

		op.ResolveOperatorTypes();
		LiftPathStep step {op, ancestor.child_index, op.GetColumnBindings(), current_bindings};
		path.push_back(std::move(step));
		if (!IsEligibleJoin(op)) {
			continue;
		}
		transport_cardinality += static_cast<double>(op.estimated_cardinality);
		options.push_back({path.size(), static_cast<double>(op.estimated_cardinality), transport_cardinality,
		                   current_bindings, op.GetColumnBindings(), op.types});
	}

	if (options.empty()) {
		return false;
	}
	idx_t output_width;
	bool has_output_width = GetBindingWidth(source_binding, expression.GetReturnType(), statistics_map, output_width);
	for (auto &option : options) {
		for (auto &binding : option.result_bindings) {
			if (!has_output_width) {
				has_output_width = GetBindingWidth(binding, expression.GetReturnType(), statistics_map, output_width);
			}
		}
	}
	if (!has_output_width) {
		return false;
	}
	auto evaluation_width = static_cast<double>(input_width) + static_cast<double>(output_width) +
	                        static_cast<double>(ExpressionHeuristics::Cost(expression));
	const auto source_cardinality = static_cast<double>(projection.children[0]->estimated_cardinality);
	const auto current_cost =
	    source_cardinality * evaluation_width + transport_cardinality * static_cast<double>(output_width);
	if (current_cost == 0) {
		return false;
	}
	double best_cost = current_cost * MINIMUM_COST_RATIO;
	optional_idx best_option;
	for (idx_t option_idx = 0; option_idx < options.size(); option_idx++) {
		auto &option = options[option_idx];
		auto remaining_output_transport = transport_cardinality - option.input_transport_cardinality;
		auto candidate_cost = option.evaluation_cardinality * evaluation_width +
		                      option.input_transport_cardinality * static_cast<double>(exclusive_input_width) +
		                      remaining_output_transport * static_cast<double>(output_width);
		if (candidate_cost < best_cost) {
			best_cost = candidate_cost;
			best_option = optional_idx(option_idx);
		}
	}
	if (!best_option.IsValid()) {
		return false;
	}

	auto &option = options[best_option.GetIndex()];
	result.source = projection;
	result.expression_index = expression_index;
	result.source_binding = source_binding;
	result.dependencies = std::move(dependencies);
	result.path.reserve(option.path_size);
	for (idx_t path_idx = 0; path_idx < option.path_size; path_idx++) {
		result.path.push_back(path[path_idx]);
	}
	result.result_bindings = option.result_bindings;
	result.target_bindings = option.target_bindings;
	result.target_types = option.target_types;
	return true;
}

struct ProjectionRewrite {
	vector<ColumnBinding> old_bindings;
	vector<ColumnBinding> new_bindings;
};

static optional_ptr<unique_ptr<LogicalOperator>> FindOperator(unique_ptr<LogicalOperator> &op,
                                                              const LogicalOperator &target) {
	if (op.get() == &target) {
		return op;
	}
	for (auto &child : op->children) {
		auto result = FindOperator(child, target);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

static ColumnBinding ResolveBinding(const ColumnBinding &binding,
                                    const vector<pair<ColumnBinding, ColumnBinding>> &replacements) {
	auto result = binding;
	for (idx_t iteration = 0; iteration <= replacements.size(); iteration++) {
		bool replaced = false;
		for (auto &replacement : replacements) {
			if (replacement.first != result) {
				continue;
			}
			result = replacement.second;
			replaced = true;
			break;
		}
		if (!replaced) {
			return result;
		}
	}
	throw InternalException("Projection placement encountered a cyclic binding replacement");
}

static void RemapParentProjectionMap(optional_ptr<LogicalOperator> parent, idx_t child_index,
                                     const vector<ColumnBinding> &old_bindings,
                                     const vector<ColumnBinding> &new_bindings) {
	if (!parent) {
		return;
	}
	auto projection_map = LogicalOperatorVisitor::GetProjectionMap(*parent, child_index);
	if (!projection_map || projection_map->empty() || old_bindings == new_bindings) {
		return;
	}
	vector<ProjectionIndex> remapped;
	remapped.reserve(projection_map->size());
	for (auto projection_index : *projection_map) {
		auto &desired_binding = old_bindings[projection_index.GetIndex()];
		auto entry = std::find(new_bindings.begin(), new_bindings.end(), desired_binding);
		if (entry == new_bindings.end()) {
			projection_map->clear();
			return;
		}
		remapped.emplace_back(NumericCast<idx_t>(entry - new_bindings.begin()));
	}
	*projection_map = std::move(remapped);
}

static void ExposeBinding(LogicalOperator &op, idx_t child_index, const ColumnBinding &binding) {
	auto &child = *op.children[child_index];
	auto child_bindings = child.GetColumnBindings();
	auto entry = std::find(child_bindings.begin(), child_bindings.end(), binding);
	if (entry == child_bindings.end()) {
		throw InternalException("Projection placement lost binding %s", binding.ToString());
	}
	auto projection_map = LogicalOperatorVisitor::GetProjectionMap(op, child_index);
	if (!projection_map || projection_map->empty()) {
		return;
	}
	auto projection_index = ProjectionIndex(NumericCast<idx_t>(entry - child_bindings.begin()));
	if (std::find(projection_map->begin(), projection_map->end(), projection_index) == projection_map->end()) {
		projection_map->push_back(projection_index);
	}
}

static ProjectionRewrite RebuildProjection(Optimizer &optimizer,
                                           column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                                           unique_ptr<LogicalOperator> &root, LogicalProjection &projection,
                                           const column_binding_set_t &removed_bindings,
                                           vector<LiftDependency> &dependencies,
                                           vector<pair<ColumnBinding, ColumnBinding>> &binding_replacements,
                                           optional_ptr<LogicalOperator> &lowest_rebuilt) {
	auto projection_ptr = FindOperator(root, projection);
	if (!projection_ptr) {
		throw InternalException("Projection placement lost a projection while lifting an expression");
	}

	auto old_bindings = projection.GetColumnBindings();
	vector<unique_ptr<Expression>> expressions;
	vector<optional_idx> dependency_indexes(dependencies.size());
	vector<pair<ColumnBinding, idx_t>> retained_bindings;
	for (idx_t expression_idx = 0; expression_idx < projection.expressions.size(); expression_idx++) {
		auto old_binding = old_bindings[expression_idx];
		if (removed_bindings.find(old_binding) != removed_bindings.end()) {
			continue;
		}
		auto new_index = expressions.size();
		auto &expression = projection.expressions[expression_idx];
		if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &colref = expression->Cast<BoundColumnRefExpression>();
			if (colref.Depth() == 0) {
				// the expressions still refer to the bindings of projections rebuilt below us: the binding replacer
				// only runs once the whole placement is in place (see ApplyLiftPlacement)
				auto current_binding = ResolveBinding(colref.Binding(), binding_replacements);
				for (idx_t dependency_idx = 0; dependency_idx < dependencies.size(); dependency_idx++) {
					if (!dependency_indexes[dependency_idx].IsValid() &&
					    current_binding == dependencies[dependency_idx].current_binding &&
					    colref.GetReturnType() == dependencies[dependency_idx].type) {
						dependency_indexes[dependency_idx] = optional_idx(new_index);
					}
				}
			}
		}
		expressions.push_back(std::move(expression));
		retained_bindings.emplace_back(old_binding, new_index);
	}

	for (idx_t dependency_idx = 0; dependency_idx < dependencies.size(); dependency_idx++) {
		if (dependency_indexes[dependency_idx].IsValid()) {
			continue;
		}
		dependency_indexes[dependency_idx] = optional_idx(expressions.size());
		expressions.push_back(make_uniq<BoundColumnRefExpression>(dependencies[dependency_idx].type,
		                                                          dependencies[dependency_idx].current_binding));
	}

	auto table_index = optimizer.binder.GenerateTableIndex();
	auto replacement = make_uniq<LogicalProjection>(table_index, std::move(expressions));
	if (projection.has_estimated_cardinality) {
		replacement->SetEstimatedCardinality(projection.estimated_cardinality);
	}
	replacement->children.push_back(std::move(projection.children[0]));
	optional_ptr<LogicalOperator> replacement_ptr = *replacement;
	*projection_ptr = std::move(replacement);

	for (auto &retained_binding : retained_bindings) {
		auto new_binding = ColumnBinding(table_index, ProjectionIndex(retained_binding.second));
		binding_replacements.emplace_back(retained_binding.first, new_binding);
		CopyStatistics(statistics_map, retained_binding.first, new_binding);
	}
	for (idx_t dependency_idx = 0; dependency_idx < dependencies.size(); dependency_idx++) {
		auto old_binding = dependencies[dependency_idx].current_binding;
		auto new_binding = ColumnBinding(table_index, ProjectionIndex(dependency_indexes[dependency_idx].GetIndex()));
		CopyStatistics(statistics_map, old_binding, new_binding);
		dependencies[dependency_idx].current_binding = new_binding;
	}
	if (!lowest_rebuilt) {
		lowest_rebuilt = replacement_ptr;
	}
	// The references to the old bindings are replaced later, in one go, once the lifted expression has been placed:
	// the projection maps of the operators above us are only made consistent with the new column layout step by
	// step (RemapParentProjectionMap, ExposeBinding, and finally the restore projection), and walking the plan with
	// a ColumnBindingReplacer before that resolves the column bindings of every operator through these stale maps.
	return {std::move(old_bindings), replacement_ptr->GetColumnBindings()};
}

static void RewriteLiftedExpression(unique_ptr<Expression> &expression, const vector<LiftDependency> &dependencies) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expression, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
		    if (colref.Depth() != 0) {
			    return;
		    }
		    for (auto &dependency : dependencies) {
			    if (colref.Binding() == dependency.original_binding) {
				    colref.BindingMutable() = dependency.current_binding;
				    return;
			    }
		    }
	    });
}

static void ApplyLiftPlacement(Optimizer &optimizer, column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                               unique_ptr<LogicalOperator> &root, LiftPlacement &placement) {
	D_ASSERT(placement.source);
	D_ASSERT(!placement.path.empty());
	auto &source = *placement.source;
	auto expression = std::move(source.expressions[placement.expression_index]);
	vector<LiftDependency> dependencies;
	for (auto &dependency : placement.dependencies) {
		dependencies.push_back({dependency.binding, dependency.binding, dependency.type});
	}
	vector<pair<ColumnBinding, ColumnBinding>> binding_replacements;

	column_binding_set_t source_binding;
	source_binding.insert(placement.source_binding);
	optional_ptr<LogicalOperator> lowest_rebuilt;
	auto source_rewrite = RebuildProjection(optimizer, statistics_map, root, source, source_binding, dependencies,
	                                        binding_replacements, lowest_rebuilt);
	RemapParentProjectionMap(placement.path[0].op.get(), placement.path[0].child_index, source_rewrite.old_bindings,
	                         source_rewrite.new_bindings);

	for (idx_t path_idx = 0; path_idx < placement.path.size(); path_idx++) {
		auto &step = placement.path[path_idx];
		ProjectionRewrite rewrite {step.output_bindings, step.output_bindings};
		if (step.op.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
			rewrite = RebuildProjection(optimizer, statistics_map, root, step.op.get().Cast<LogicalProjection>(),
			                            step.result_bindings, dependencies, binding_replacements, lowest_rebuilt);
		} else {
			for (auto &dependency : dependencies) {
				ExposeBinding(step.op.get(), step.child_index, dependency.current_binding);
			}
			rewrite.new_bindings = step.op.get().GetColumnBindings();
		}
		if (path_idx + 1 < placement.path.size()) {
			RemapParentProjectionMap(placement.path[path_idx + 1].op.get(), placement.path[path_idx + 1].child_index,
			                         rewrite.old_bindings, rewrite.new_bindings);
		}
	}

	RewriteLiftedExpression(expression, dependencies);
	auto &target_step = placement.path.back();
	auto target_ptr = FindOperator(root, target_step.op.get());
	if (!target_ptr) {
		throw InternalException("Projection placement lost its target join");
	}
	auto &target = **target_ptr;
	target.ResolveOperatorTypes();
	auto current_target_bindings = target.GetColumnBindings();

	vector<unique_ptr<Expression>> compute_expressions;
	vector<ColumnBinding> compute_inputs;
	vector<idx_t> restore_indexes;
	restore_indexes.reserve(placement.target_bindings.size());
	for (idx_t target_idx = 0; target_idx < placement.target_bindings.size(); target_idx++) {
		auto &old_binding = placement.target_bindings[target_idx];
		if (placement.result_bindings.find(old_binding) != placement.result_bindings.end()) {
			restore_indexes.push_back(DConstants::INVALID_INDEX);
			continue;
		}
		auto current_binding = ResolveBinding(old_binding, binding_replacements);
		auto current_entry = std::find(current_target_bindings.begin(), current_target_bindings.end(), current_binding);
		if (current_entry == current_target_bindings.end()) {
			throw InternalException("Projection placement lost target binding %s", current_binding.ToString());
		}
		auto compute_entry = std::find(compute_inputs.begin(), compute_inputs.end(), current_binding);
		idx_t compute_idx;
		if (compute_entry == compute_inputs.end()) {
			compute_idx = compute_inputs.size();
			compute_inputs.push_back(current_binding);
			auto type_idx = NumericCast<idx_t>(current_entry - current_target_bindings.begin());
			compute_expressions.push_back(make_uniq<BoundColumnRefExpression>(target.types[type_idx], current_binding));
		} else {
			compute_idx = NumericCast<idx_t>(compute_entry - compute_inputs.begin());
		}
		restore_indexes.push_back(compute_idx);
	}

	auto candidate_idx = compute_expressions.size();
	compute_expressions.push_back(std::move(expression));
	auto compute_table_index = optimizer.binder.GenerateTableIndex();
	auto compute_projection = make_uniq<LogicalProjection>(compute_table_index, std::move(compute_expressions));
	if (target.has_estimated_cardinality) {
		compute_projection->SetEstimatedCardinality(target.estimated_cardinality);
	}
	compute_projection->children.push_back(std::move(*target_ptr));

	vector<unique_ptr<Expression>> restore_expressions;
	restore_expressions.reserve(placement.target_bindings.size());
	for (idx_t target_idx = 0; target_idx < placement.target_bindings.size(); target_idx++) {
		auto compute_idx = restore_indexes[target_idx];
		if (compute_idx == DConstants::INVALID_INDEX) {
			compute_idx = candidate_idx;
		}
		restore_expressions.push_back(make_uniq<BoundColumnRefExpression>(
		    placement.target_types[target_idx], ColumnBinding(compute_table_index, ProjectionIndex(compute_idx))));
	}
	auto restore_table_index = optimizer.binder.GenerateTableIndex();
	auto restore_projection = make_uniq<LogicalProjection>(restore_table_index, std::move(restore_expressions));
	if (target.has_estimated_cardinality) {
		restore_projection->SetEstimatedCardinality(target.estimated_cardinality);
	}
	restore_projection->children.push_back(std::move(compute_projection));
	optional_ptr<LogicalOperator> restore_ptr = *restore_projection;
	*target_ptr = std::move(restore_projection);

	for (idx_t compute_idx = 0; compute_idx < compute_inputs.size(); compute_idx++) {
		CopyStatistics(statistics_map, compute_inputs[compute_idx],
		               ColumnBinding(compute_table_index, ProjectionIndex(compute_idx)));
	}
	CopyStatistics(statistics_map, placement.source_binding,
	               ColumnBinding(compute_table_index, ProjectionIndex(candidate_idx)));

	// now that the restore projection reproduces the target's original column layout, every projection map in the
	// plan is consistent again: replace the references to the bindings of the rebuilt projections (resolved through
	// the chain of rebuilds) everywhere above the lowest rebuilt projection
	ColumnBindingReplacer rebuild_replacer;
	for (auto &replacement : binding_replacements) {
		auto final_binding = ResolveBinding(replacement.first, binding_replacements);
		if (final_binding != replacement.first) {
			rebuild_replacer.replacement_bindings.emplace_back(replacement.first, final_binding);
		}
	}
	rebuild_replacer.stop_operator = lowest_rebuilt;
	rebuild_replacer.VisitOperator(*root);

	ColumnBindingReplacer replacer;
	for (idx_t target_idx = 0; target_idx < placement.target_bindings.size(); target_idx++) {
		auto restore_binding = ColumnBinding(restore_table_index, ProjectionIndex(target_idx));
		auto &old_binding = placement.target_bindings[target_idx];
		replacer.replacement_bindings.emplace_back(old_binding, restore_binding);
		if (placement.result_bindings.find(old_binding) == placement.result_bindings.end()) {
			auto current_binding = ResolveBinding(old_binding, binding_replacements);
			if (current_binding != old_binding) {
				replacer.replacement_bindings.emplace_back(current_binding, restore_binding);
			}
		}
		auto compute_idx = restore_indexes[target_idx];
		if (compute_idx == DConstants::INVALID_INDEX) {
			compute_idx = candidate_idx;
		}
		CopyStatistics(statistics_map, ColumnBinding(compute_table_index, ProjectionIndex(compute_idx)),
		               restore_binding);
	}
	replacer.stop_operator = restore_ptr;
	replacer.VisitOperator(*root);
}

static bool LiftFirstProfitableExpression(Optimizer &optimizer,
                                          column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                                          unique_ptr<LogicalOperator> &root, unique_ptr<LogicalOperator> &op,
                                          vector<PathStep> &ancestors) {
	for (idx_t child_idx = 0; child_idx < op->children.size(); child_idx++) {
		ancestors.push_back({*op, child_idx});
		auto changed =
		    LiftFirstProfitableExpression(optimizer, statistics_map, root, op->children[child_idx], ancestors);
		ancestors.pop_back();
		if (changed) {
			return true;
		}
	}
	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	auto &projection = op->Cast<LogicalProjection>();
	auto reading_expressions = CountReadingExpressions(projection);
	for (idx_t expression_idx = 0; expression_idx < projection.expressions.size(); expression_idx++) {
		LiftPlacement placement;
		if (!FindLiftPlacement(projection, expression_idx, ancestors, statistics_map, reading_expressions, placement)) {
			continue;
		}
		ApplyLiftPlacement(optimizer, statistics_map, root, placement);
		return true;
	}
	return false;
}

static optional_idx GetExpressionChild(const Expression &expression, LogicalOperator &op) {
	const bool references_left = BindingsBelongToChild(expression, *op.children[0]);
	const bool references_right = BindingsBelongToChild(expression, *op.children[1]);
	if (references_left == references_right) {
		return optional_idx();
	}
	return optional_idx(references_right ? 1 : 0);
}

static bool FindPlacement(LogicalProjection &projection, idx_t expression_index,
                          const column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                          const column_binding_map_t<idx_t> &reading_expressions, ExpressionPlacement &result) {
	auto &expression = *projection.expressions[expression_index];
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF || expression.IsVolatile() ||
	    expression.CanThrow() || !projection.children[0]->has_estimated_cardinality) {
		return false;
	}

	idx_t input_width;
	vector<ExpressionBindingWidth> input_bindings;
	if (!GetExpressionWidth(expression, statistics_map, input_width, input_bindings)) {
		return false;
	}
	auto exclusive_input_width = GetExclusiveInputWidth(reading_expressions, input_bindings);
	idx_t output_width;
	if (!GetBindingWidth(ColumnBinding(projection.table_index, ProjectionIndex(expression_index)),
	                     expression.GetReturnType(), statistics_map, output_width)) {
		return false;
	}

	const double evaluation_width = static_cast<double>(input_width) + static_cast<double>(output_width) +
	                                static_cast<double>(ExpressionHeuristics::Cost(expression));
	const double current_evaluation_cost =
	    static_cast<double>(projection.children[0]->estimated_cardinality) * evaluation_width;
	if (current_evaluation_cost == 0) {
		return false;
	}

	auto current_expression = expression.Copy();
	reference<unique_ptr<LogicalOperator>> current = projection.children[0];
	vector<PathStep> path;
	double transport_cardinality = 0;
	double input_transport_savings_cardinality = 0;
	// Beyond the first direct join, the source can be live for another path expression or join condition.
	bool can_credit_input_transport = true;
	vector<ExpressionPlacement> candidates;

	// Compare every position on the dependency path instead of making a greedy per-join decision.
	while (current.get()) {
		auto &op = *current.get();
		if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			if (op.children.size() != 1 || !BindingsBelongToChild(*current_expression, op)) {
				break;
			}
			path.push_back({op, 0});
			can_credit_input_transport = false;
			current = op.children[0];
			continue;
		}
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &lower_projection = op.Cast<LogicalProjection>();
			if (!RewriteThroughProjection(current_expression, lower_projection)) {
				break;
			}
			path.push_back({op, 0});
			current = op.children[0];
			continue;
		}
		if (!IsEligibleJoin(op) || !op.has_estimated_cardinality) {
			break;
		}
		auto child_index = GetExpressionChild(*current_expression, op);
		if (!child_index.IsValid()) {
			break;
		}
		auto &child = op.children[child_index.GetIndex()];
		if (!child->has_estimated_cardinality) {
			break;
		}
		column_binding_set_t dependency_bindings;
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *current_expression, [&](const BoundColumnRefExpression &colref) {
			    if (colref.Depth() == 0) {
				    dependency_bindings.insert(colref.Binding());
			    }
		    });
		if (OperatorReferencesAnyBinding(op, dependency_bindings)) {
			can_credit_input_transport = false;
		}
		path.push_back({op, child_index.GetIndex()});
		transport_cardinality += static_cast<double>(op.estimated_cardinality);
		if (can_credit_input_transport) {
			input_transport_savings_cardinality += static_cast<double>(op.estimated_cardinality);
		}
		can_credit_input_transport = false;
		ExpressionPlacement candidate;
		candidate.expression_index = expression_index;
		candidate.path = path;
		candidate.expression = current_expression->Copy();
		candidate.evaluation_cardinality = static_cast<double>(child->estimated_cardinality);
		candidate.output_transport_cardinality = transport_cardinality;
		candidate.input_transport_savings_cardinality = input_transport_savings_cardinality;
		candidates.push_back(std::move(candidate));
		current = child;
	}

	const double current_cost =
	    current_evaluation_cost + input_transport_savings_cardinality * static_cast<double>(exclusive_input_width);
	double best_cost = current_cost * MINIMUM_COST_RATIO;
	bool found = false;
	for (auto &candidate : candidates) {
		const double remaining_input_transport =
		    input_transport_savings_cardinality - candidate.input_transport_savings_cardinality;
		const double candidate_cost = candidate.evaluation_cardinality * evaluation_width +
		                              candidate.output_transport_cardinality * static_cast<double>(output_width) +
		                              remaining_input_transport * static_cast<double>(exclusive_input_width);
		if (candidate_cost < best_cost) {
			best_cost = candidate_cost;
			result = std::move(candidate);
			found = true;
		}
	}
	return found;
}

static void ExposeBinding(PathStep &step, const ColumnBinding &binding) {
	ExposeBinding(step.op.get(), step.child_index, binding);
}

static void ApplyPlacementGroup(Optimizer &optimizer, column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                                LogicalProjection &upper_projection, PlacementGroup &group) {
	auto old_upper_child_bindings = upper_projection.children[0]->GetColumnBindings();
	vector<vector<ColumnBinding>> old_path_child_bindings;
	old_path_child_bindings.reserve(group.path.size());
	for (auto &step : group.path) {
		old_path_child_bindings.push_back(step.op.get().children[step.child_index]->GetColumnBindings());
	}

	auto &target = GetPathTarget(group.path);
	auto old_bindings = target->GetColumnBindings();
	auto lower_index = optimizer.binder.GenerateTableIndex();
	auto lower_projection = LogicalProjection::CreateIdentity(lower_index, std::move(target));
	for (auto &placement : group.placements) {
		lower_projection->expressions.push_back(std::move(placement.expression));
	}
	if (lower_projection->children[0]->has_estimated_cardinality) {
		lower_projection->SetEstimatedCardinality(lower_projection->children[0]->estimated_cardinality);
	}
	target = std::move(lower_projection);

	BindingReplacementGraph replacements;
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		auto new_binding = ColumnBinding(lower_index, ProjectionIndex(i));
		replacements.Add(old_bindings[i], new_binding);
		CopyStatistics(statistics_map, old_bindings[i], new_binding);
	}

	vector<ColumnBinding> computed_bindings;
	computed_bindings.reserve(group.placements.size());
	for (idx_t i = 0; i < group.placements.size(); i++) {
		auto binding = ColumnBinding(lower_index, ProjectionIndex(old_bindings.size() + i));
		computed_bindings.push_back(binding);
		CopyStatistics(
		    statistics_map,
		    ColumnBinding(upper_projection.table_index, ProjectionIndex(group.placements[i].expression_index)),
		    binding);
	}

	for (idx_t path_idx = group.path.size(); path_idx > 0; path_idx--) {
		auto &step = group.path[path_idx - 1];
		auto step_ptr = FindOperator(upper_projection.children[0], step.op.get());
		if (!step_ptr) {
			throw InternalException("Projection placement lost a path operator");
		}
		ColumnBindingRewrite::ApplyToChild(*step_ptr, step.child_index,
		                                   std::move(old_path_child_bindings[path_idx - 1]), replacements);
		if (step.op.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = step.op.get().Cast<LogicalProjection>();
			for (idx_t i = 0; i < computed_bindings.size(); i++) {
				auto type = upper_projection.expressions[group.placements[i].expression_index]->GetReturnType();
				projection.expressions.push_back(make_uniq<BoundColumnRefExpression>(type, computed_bindings[i]));
				auto new_binding =
				    ColumnBinding(projection.table_index, ProjectionIndex(projection.expressions.size() - 1));
				CopyStatistics(statistics_map, computed_bindings[i], new_binding);
				computed_bindings[i] = new_binding;
			}
		} else {
			for (auto &binding : computed_bindings) {
				ExposeBinding(step, binding);
			}
		}
	}
	ColumnBindingRewrite::ApplyToOperatorBindings(upper_projection, replacements);
	ColumnBindingRewrite::ValidateOutput(old_upper_child_bindings, upper_projection.children[0]->GetColumnBindings(),
	                                     replacements);

	for (idx_t i = 0; i < group.placements.size(); i++) {
		auto expression_index = group.placements[i].expression_index;
		auto alias = upper_projection.expressions[expression_index]->GetAlias();
		auto replacement = make_uniq<BoundColumnRefExpression>(
		    upper_projection.expressions[expression_index]->GetReturnType(), computed_bindings[i]);
		replacement->SetAlias(std::move(alias));
		upper_projection.expressions[expression_index] = std::move(replacement);
	}
}

static void OptimizeOperator(Optimizer &optimizer, column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map,
                             unique_ptr<LogicalOperator> &op) {
	for (auto &child : op->children) {
		OptimizeOperator(optimizer, statistics_map, child);
	}
	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}
	auto &projection = op->Cast<LogicalProjection>();
	while (true) {
		PlacementGroup group;
		auto reading_expressions = CountReadingExpressions(projection);
		for (idx_t expression_index = 0; expression_index < projection.expressions.size(); expression_index++) {
			ExpressionPlacement placement;
			if (!FindPlacement(projection, expression_index, statistics_map, reading_expressions, placement)) {
				continue;
			}
			if (group.placements.empty()) {
				group.path = placement.path;
			}
			if (RefersToSameObject(*GetPathTarget(group.path), *GetPathTarget(placement.path))) {
				group.placements.push_back(std::move(placement));
			}
		}
		if (group.placements.empty()) {
			break;
		}
		ApplyPlacementGroup(optimizer, statistics_map, projection, group);
	}
}

} // namespace

ProjectionPlacementOptimizer::ProjectionPlacementOptimizer(
    Optimizer &optimizer_p, column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map_p)
    : optimizer(optimizer_p), statistics_map(statistics_map_p) {
}

void ProjectionPlacementOptimizer::Optimize(unique_ptr<LogicalOperator> &plan) {
	vector<PathStep> ancestors;
	while (LiftFirstProfitableExpression(optimizer, statistics_map, plan, plan, ancestors)) {
		plan->ResolveOperatorTypes();
	}
	OptimizeOperator(optimizer, statistics_map, plan);
	plan->ResolveOperatorTypes();
}

} // namespace duckdb
