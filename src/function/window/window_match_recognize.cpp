#include "duckdb/function/window/window_match_recognize.hpp"

#include "duckdb/function/window/window_aggregate_states.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

#include "duckdb/function/match_recognize.hpp"
#include "duckdb/function/window/match_recognize_functions.hpp"
#include "duckdb/function/window/match_recognize_matcher.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

//! The result's list child, in the order ResultType() declares its fields
using SpanStruct = VectorStructType<string_t, uint64_t, bool, bool, uint64_t, uint64_t, bool, bool, uint64_t>;

//! One membership of a row in a match: there is one per (row, match) pair, not one per row
struct MatchRecognizeSpan {
	//! A row of a match, classified as the variable it matched
	static MatchRecognizeSpan Row(idx_t symbol, idx_t match_number, idx_t match_start, idx_t match_end, bool excluded) {
		return MatchRecognizeSpan {symbol, match_number, match_start, match_end, excluded, false};
	}
	//! An empty match covers no rows at all, so this only marks the row where one happened
	static MatchRecognizeSpan Empty(idx_t match_number, idx_t row) {
		return MatchRecognizeSpan {0, match_number, row, row, false, true};
	}

	idx_t symbol;
	idx_t match_number;
	idx_t match_start;
	idx_t match_end;
	bool excluded;
	bool empty;
};

//! The memberships of one row, in the order the matches were found
struct MatchRecognizeRowSpans {
	struct Node {
		MatchRecognizeSpan span;
		Node *next;
	};

	Node *first = nullptr;
	Node *last = nullptr;
	idx_t count = 0;
};

//! Appends memberships on behalf of one thread. The arena is that thread's own but owned by the global
//! state, so the memberships outlive the walk however it ends, and it allocates through the buffer
//! manager so that they count against the memory limit.
struct MatchRecognizeSpanWriter {
	explicit MatchRecognizeSpanWriter(ArenaAllocator &arena_p) : arena(arena_p) {
	}

	void Append(MatchRecognizeRowSpans &row, const MatchRecognizeSpan &span) {
		auto node = reinterpret_cast<MatchRecognizeRowSpans::Node *>(
		    arena.AllocateAligned(sizeof(MatchRecognizeRowSpans::Node)));
		node->span = span;
		node->next = nullptr;
		if (row.count++ == 0) {
			row.first = node;
		} else {
			row.last->next = node;
		}
		row.last = node;
	}

	ArenaAllocator &arena;
};

struct WindowMatchRecognizeGlobalState : WindowExecutorGlobalState {
	WindowMatchRecognizeGlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
	                                const ValidityMask &partition_mask, const ValidityMask &order_mask)
	    : WindowExecutorGlobalState(client, executor, payload_count, partition_mask, order_mask),
	      row_spans(payload_count) {
		auto &config = executor.wexpr.BindInfo()->Cast<MatchRecognizeFunctionData>();
		condition_values.resize(config.conditions.size());
		for (auto &values : condition_values) {
			values.assign(payload_count, 0);
		}
		classifiers.resize(payload_count);
		excluded_rows.resize(payload_count);
		// the name a symbol reports is the same for every row it classifies, so it is spelled once
		for (auto &symbol : config.symbols) {
			classifier_names.push_back(MatchRecognizeSymbolName(symbol));
		}
		D_ASSERT(executor.wexpr.GetReturnType().id() == LogicalTypeId::LIST);
	}

	//! An arena for a thread to fill, owned here so that what it holds survives a cut-short walk
	ArenaAllocator &SpanArena(ClientContext &client) {
		lock_guard<mutex> guard(state_lock);
		span_arenas.push_back(make_uniq<ArenaAllocator>(BufferManager::GetBufferManager(client).GetBufferAllocator()));
		return *span_arenas.back();
	}

	mutex state_lock;
	//! Set up once; the threads then take partitions from the cursor below
	bool prepared = false;
	//! Partitions are independent, so the threads that reach Finalize share them out
	vector<pair<idx_t, idx_t>> partitions;
	atomic<idx_t> next_partition {0};
	//! The variable that classified each row, written only by the thread that owns the partition
	vector<idx_t> classifiers;
	//! Whether the pattern matched each row inside a {- -}, written alongside the classifier above
	vector<uint8_t> excluded_rows;
	//! One boolean per symbol per row, filled by Sink over disjoint ranges
	vector<vector<uint8_t>> condition_values;

	//! What each symbol reports as its classifier
	vector<string> classifier_names;
	//! Where each row's memberships start, and how many there are
	vector<MatchRecognizeRowSpans> row_spans;
	//! The arenas the memberships above live in, one per thread that wrote any
	vector<unique_ptr<ArenaAllocator>> span_arenas;
};

LogicalType WindowMatchRecognizeExecutor::ResultType() {
	// One entry per match a row takes part in; the plan unnests the list, which drops the rows that
	// matched nothing
	return LogicalType::LIST(LogicalType::STRUCT({{"classifier", LogicalType::VARCHAR},
	                                              {"match_number", LogicalType::UBIGINT},
	                                              {"is_match_start", LogicalType::BOOLEAN},
	                                              {"is_match_end", LogicalType::BOOLEAN},
	                                              {"match_start", LogicalType::UBIGINT},
	                                              {"match_end", LogicalType::UBIGINT},
	                                              {"is_excluded", LogicalType::BOOLEAN},
	                                              {"is_empty", LogicalType::BOOLEAN},
	                                              // the row's place in the partition, which gives a measure's
	                                              // window a total order even when the ORDER BY has ties
	                                              {"row_index", LogicalType::UBIGINT}}));
}

//===--------------------------------------------------------------------===//
// Binding
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> WindowMatchRecognizeExecutor::Bind(BindWindowFunctionInput &input) {
	// the MATCH_RECOGNIZE binder hands its configuration over as bind data, so nothing reaching here
	// came from a MATCH_RECOGNIZE clause
	throw BinderException("%s is how the MATCH_RECOGNIZE clause is planned rather than a function to call, so it "
	                      "cannot be used directly",
	                      MatchRecognizeFun::Name);
}

//===--------------------------------------------------------------------===//
// Serialization
//===--------------------------------------------------------------------===//
void MatchRecognizePattern::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "type", type);
	serializer.WritePropertyWithDefault<idx_t>(101, "symbol", symbol);
	serializer.WritePropertyWithDefault(102, "at_end", at_end);
	serializer.WritePropertyWithDefault(103, "min_count", min_count);
	serializer.WritePropertyWithDefault(104, "max_count", max_count);
	serializer.WritePropertyWithDefault(105, "excluded", excluded);
	serializer.WritePropertyWithDefault(106, "reluctant", reluctant);
	serializer.WriteList(107, "children", children.size(), [&](Serializer::List &list, idx_t i) {
		list.WriteObject([&](Serializer &child) { children[i]->Serialize(child); });
	});
}

unique_ptr<MatchRecognizePattern> MatchRecognizePattern::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<MatchRecognizePattern>(deserializer.ReadProperty<MatchRecognizePatternType>(100, "type"));
	result->symbol = deserializer.ReadPropertyWithDefault<idx_t>(101, "symbol");
	result->at_end = deserializer.ReadPropertyWithDefault<bool>(102, "at_end");
	result->min_count = deserializer.ReadPropertyWithDefault<optional_idx>(103, "min_count");
	result->max_count = deserializer.ReadPropertyWithDefault<optional_idx>(104, "max_count");
	result->excluded = deserializer.ReadPropertyWithDefault<bool>(105, "excluded");
	result->reluctant = deserializer.ReadPropertyWithDefault<bool>(106, "reluctant");
	deserializer.ReadList(107, "children", [&](Deserializer::List &list, idx_t i) {
		list.ReadObject([&](Deserializer &child) { result->children.push_back(Deserialize(child)); });
	});
	return result;
}

void WindowMatchRecognizeExecutor::Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                             const BoundWindowFunction &function) {
	auto &config = bind_data->Cast<MatchRecognizeFunctionData>();
	serializer.WriteObject(100, "pattern", [&](Serializer &child) { config.pattern->Serialize(child); });
	serializer.WriteProperty(101, "conditions", config.conditions);
	serializer.WriteProperty(102, "symbols", config.symbols);
	serializer.WriteProperty(103, "after_match", config.after_match);
	serializer.WriteProperty(104, "after_match_variable", config.after_match_variable);
	serializer.WriteProperty(105, "depends_on_match_number", config.depends_on_match_number);
	serializer.WritePropertyWithDefault<idx_t>(108, "match_number_field", config.match_number_field);
	serializer.WriteProperty(106, "row_scoped", config.row_scoped);
	serializer.WriteList(107, "navigations", config.navigations.size(), [&](Serializer::List &list, idx_t i) {
		auto &navigation = config.navigations[i];
		list.WriteObject([&](Serializer &child) {
			child.WriteProperty(100, "last", navigation.last);
			child.WriteProperty(101, "symbol", navigation.symbol);
			child.WriteProperty(102, "field", navigation.field);
			child.WriteProperty(103, "offset", navigation.offset);
			child.WritePropertyWithDefault<bool>(104, "classifier", navigation.classifier);
			child.WritePropertyWithDefault<int64_t>(105, "step", navigation.step);
		});
	});
	serializer.WriteList(109, "subsets", config.subsets.size(), [&](Serializer::List &list, idx_t i) {
		auto &subset = config.subsets[i];
		list.WriteObject([&](Serializer &child) {
			child.WriteProperty(100, "name", subset.name);
			child.WriteProperty(101, "members", subset.members);
		});
	});
	serializer.WriteList(110, "aggregates", config.aggregates.size(), [&](Serializer::List &list, idx_t i) {
		auto &aggregate = config.aggregates[i];
		list.WriteObject([&](Serializer &child) {
			child.WriteProperty(100, "symbol", aggregate.symbol);
			child.WritePropertyWithDefault<optional_idx>(101, "operand", aggregate.operand);
			child.WriteProperty(102, "field", aggregate.field);
			child.WriteProperty(103, "expression", aggregate.expression);
		});
	});
}

unique_ptr<FunctionData> WindowMatchRecognizeExecutor::Deserialize(Deserializer &deserializer,
                                                                   BoundWindowFunction &function) {
	auto result = make_uniq<MatchRecognizeFunctionData>();
	deserializer.ReadObject(100, "pattern",
	                        [&](Deserializer &child) { result->pattern = MatchRecognizePattern::Deserialize(child); });
	deserializer.ReadProperty(101, "conditions", result->conditions);
	deserializer.ReadProperty(102, "symbols", result->symbols);
	deserializer.ReadProperty(103, "after_match", result->after_match);
	deserializer.ReadProperty(104, "after_match_variable", result->after_match_variable);
	deserializer.ReadProperty(105, "depends_on_match_number", result->depends_on_match_number);
	result->match_number_field = deserializer.ReadPropertyWithDefault<idx_t>(108, "match_number_field");
	deserializer.ReadProperty(106, "row_scoped", result->row_scoped);
	deserializer.ReadList(107, "navigations", [&](Deserializer::List &list, idx_t i) {
		list.ReadObject([&](Deserializer &child) {
			MatchRecognizeFunctionData::Navigation navigation;
			navigation.last = child.ReadProperty<bool>(100, "last");
			navigation.symbol = child.ReadProperty<string>(101, "symbol");
			navigation.field = child.ReadProperty<idx_t>(102, "field");
			navigation.offset = child.ReadProperty<idx_t>(103, "offset");
			navigation.classifier = child.ReadPropertyWithDefault<bool>(104, "classifier");
			navigation.step = child.ReadPropertyWithDefault<int64_t>(105, "step");
			result->navigations.push_back(navigation);
		});
	});
	deserializer.ReadList(109, "subsets", [&](Deserializer::List &list, idx_t i) {
		list.ReadObject([&](Deserializer &child) {
			MatchRecognizeFunctionData::Subset subset;
			subset.name = child.ReadProperty<string>(100, "name");
			subset.members = child.ReadProperty<vector<string>>(101, "members");
			result->subsets.push_back(std::move(subset));
		});
	});
	deserializer.ReadList(110, "aggregates", [&](Deserializer::List &list, idx_t i) {
		list.ReadObject([&](Deserializer &child) {
			MatchRecognizeFunctionData::Aggregate aggregate;
			aggregate.symbol = child.ReadProperty<string>(100, "symbol");
			aggregate.operand = child.ReadPropertyWithDefault<optional_idx>(101, "operand");
			aggregate.field = child.ReadProperty<idx_t>(102, "field");
			aggregate.expression = child.ReadProperty<unique_ptr<Expression>>(103, "expression");
			result->aggregates.push_back(std::move(aggregate));
		});
	});
	function.SetReturnType(ResultType());
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// WindowMatchRecognizeExecutor
//===--------------------------------------------------------------------===//
void WindowMatchRecognizeExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	// matching spans a whole partition rather than a frame, so no frame boundaries are needed
}

void WindowMatchRecognizeExecutor::GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
	auto &config = executor.wexpr.BindInfo()->Cast<MatchRecognizeFunctionData>();
	// the conditions are evaluated per chunk as the rows arrive
	for (auto &child : executor.wexpr.GetChildren()) {
		executor.child_idx.emplace_back(shared.RegisterSink(child));
	}
	// a condition settled per candidate row reads arbitrary rows, so the group has to stay
	auto per_row = !config.navigations.empty();
	for (auto scoped : config.row_scoped) {
		per_row = per_row || scoped;
	}
	if (per_row) {
		for (auto &child : executor.wexpr.GetChildren()) {
			executor.aux_idx.emplace_back(shared.RegisterCollection(child, false));
		}
	}
}

unique_ptr<GlobalSinkState> WindowMatchRecognizeExecutor::GetGlobal(ClientContext &client,
                                                                    const WindowExecutor &executor,
                                                                    const idx_t payload_count,
                                                                    const ValidityMask &partition_mask,
                                                                    const ValidityMask &order_mask) {
	return make_uniq<WindowMatchRecognizeGlobalState>(client, executor, payload_count, partition_mask, order_mask);
}

//! Holds the per thread machinery Sink needs to evaluate the conditions
class MatchRecognizeLocalState : public WindowExecutorLocalState {
public:
	MatchRecognizeLocalState(ExecutionContext &context, const WindowMatchRecognizeGlobalState &gstate)
	    : WindowExecutorLocalState(context, gstate) {
		auto &config = gstate.executor.wexpr.BindInfo()->Cast<MatchRecognizeFunctionData>();
		for (idx_t i = 0; i < config.conditions.size(); i++) {
			// a condition that depends on the match being assembled has no answer yet
			if (config.row_scoped[i]) {
				continue;
			}
			auto copied = config.conditions[i]->Copy();
			types.push_back(copied->GetReturnType());
			conditions.push_back(std::move(copied));
			condition_index.push_back(i);
		}
		if (!conditions.empty()) {
			executor = make_uniq<ExpressionExecutor>(context.client, conditions);
			result.Initialize(context.client, types);
		}
	}

	//! The columns the conditions read, in the order the window was handed them. They are referenced
	//! rather than copied, so this is the same chunk pointed at a new sink chunk each time.
	DataChunk &ConditionColumns(DataChunk &sink_chunk, const vector<column_t> &child_idx) {
		if (columns.ColumnCount() != child_idx.size()) {
			vector<LogicalType> column_types;
			for (auto column_idx : child_idx) {
				column_types.push_back(sink_chunk.data[column_idx].GetType());
			}
			columns.InitializeEmpty(column_types);
		}
		for (idx_t col = 0; col < child_idx.size(); col++) {
			columns.data[col].Reference(sink_chunk.data[child_idx[col]]);
		}
		columns.SetCardinalityUnsafe(sink_chunk.size());
		return columns;
	}

	vector<unique_ptr<Expression>> conditions;
	//! The condition each of the above decides, since the ones settled per row are left out
	vector<idx_t> condition_index;
	vector<LogicalType> types;
	unique_ptr<ExpressionExecutor> executor;
	DataChunk result;
	DataChunk columns;
};

unique_ptr<LocalSinkState> WindowMatchRecognizeExecutor::GetLocal(ExecutionContext &context,
                                                                  const GlobalSinkState &gstate) {
	return make_uniq<MatchRecognizeLocalState>(context, gstate.Cast<WindowMatchRecognizeGlobalState>());
}

void WindowMatchRecognizeExecutor::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                        idx_t input_idx, OperatorSinkInput &sink) {
	auto &gstate = sink.global_state.Cast<WindowMatchRecognizeGlobalState>();
	auto &lstate = sink.local_state.Cast<MatchRecognizeLocalState>();
	if (!lstate.executor) {
		return;
	}

	lstate.result.Reset();
	lstate.executor->Execute(lstate.ConditionColumns(sink_chunk, gstate.executor.child_idx), lstate.result);
	for (idx_t i = 0; i < lstate.conditions.size(); i++) {
		auto &values = gstate.condition_values[lstate.condition_index[i]];
		for (const auto &entry : lstate.result.data[i].Values<bool>()) {
			values[input_idx + entry.GetIndex()] = entry.IsValid() && entry.GetValueUnsafe() ? 1 : 0;
		}
	}
}

static bool Contains(const vector<idx_t> &symbols, idx_t symbol) {
	return std::find(symbols.begin(), symbols.end(), symbol) != symbols.end();
}

//! Where to resume scanning after a match spanning [match_start, match_end]. A union variable's
//! rows are those of any of its members, so the target is a set of symbols.
static idx_t SkipTo(const MatchRecognizeFunctionData &config, const vector<idx_t> &skip_symbols, idx_t match_start,
                    idx_t match_end, const vector<idx_t> &classifiers) {
	auto resume = match_end + 1;
	switch (config.after_match) {
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_NEXT_ROW:
		resume = match_start + 1;
		break;
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_FIRST_VAR:
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_VAR: {
		const auto first = config.after_match == MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_FIRST_VAR;
		// walked from the end the wanted row is nearer, so either way it is the first one found
		optional_idx target;
		for (idx_t step = 0; step <= match_end - match_start; step++) {
			const auto row = first ? match_start + step : match_end - step;
			if (Contains(skip_symbols, classifiers[row])) {
				target = row;
				break;
			}
		}
		// the symbol carries the internal prefix, which is no help to whoever wrote the query
		const auto variable = MatchRecognizeSymbolName(config.after_match_variable);
		if (!target.IsValid()) {
			throw InvalidInputException("AFTER MATCH SKIP TO %s found a match with no row matched to %s, so there is "
			                            "nowhere to resume from",
			                            variable, variable);
		}
		resume = target.GetIndex();
		if (resume == match_start) {
			throw InvalidInputException(
			    "AFTER MATCH SKIP TO %s resumes at the row the match started on, so matching cannot advance", variable);
		}
		break;
	}
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_LAST_ROW:
	case MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_DEFAULT:
		break;
	}
	// never resume at or before the row the match started on, that would not terminate
	return MaxValue(resume, match_start + 1);
}

//! Work out where the partitions are, once, for every thread that reaches Finalize
static void PrepareHashGroup(WindowMatchRecognizeGlobalState &gstate) {
	lock_guard<mutex> lock(gstate.state_lock);
	if (gstate.prepared) {
		return;
	}
	gstate.prepared = true;

	idx_t partition_start = 0;
	for (idx_t payload_idx = 1; payload_idx <= gstate.payload_count; payload_idx++) {
		const auto at_end = payload_idx == gstate.payload_count;
		if (!at_end && !gstate.partition_mask.RowIsValid(payload_idx)) {
			continue;
		}
		gstate.partitions.emplace_back(partition_start, payload_idx - 1);
		partition_start = payload_idx;
	}
}

//! Decides whether a row can be a given symbol. Conditions that do not depend on the match were
//! settled in Sink; the rest are evaluated here, against the match being assembled.
class RowConditions {
public:
	RowConditions(ExecutionContext &context, WindowMatchRecognizeGlobalState &gstate,
	              const MatchRecognizeFunctionData &config, const WindowCollection &collection)
	    : context(context), gstate(gstate), config(config), collection(collection),
	      columns_idx(gstate.executor.aux_idx), executors(config.conditions.size()) {
		D_ASSERT(config.row_scoped.size() == config.conditions.size());
		// the conditions settled in Sink are decided by a lookup here, so only the rest are copied
		conditions.resize(config.conditions.size());
		for (idx_t i = 0; i < config.conditions.size(); i++) {
			if (config.row_scoped[i]) {
				conditions[i] = config.conditions[i]->Copy();
			}
		}
		// resolving where each field's value comes from once is what lets a row be assembled by copying
		// only the fields the condition being decided reads
		for (auto &condition : config.conditions) {
			ExpressionIterator::VisitExpression<BoundReferenceExpression>(
			    *condition, [&](const BoundReferenceExpression &bound_ref) {
				    field_plan.resize(MaxValue<idx_t>(field_plan.size(), bound_ref.Index() + 1));
			    });
		}
		for (idx_t i = 0; i < config.navigations.size(); i++) {
			field_plan.resize(MaxValue<idx_t>(field_plan.size(), config.navigations[i].field + 1));
			field_plan[config.navigations[i].field] = FieldPlan {FieldSource::NAVIGATION, i};
		}
		// the collected column holds the constant the matcher rewrites per match, not the number
		idx_t fields = config.match_number_field + 1;
		for (auto &aggregate : config.aggregates) {
			fields = MaxValue<idx_t>(fields, aggregate.field + 1);
		}
		field_plan.resize(MaxValue<idx_t>(field_plan.size(), fields));
		field_plan[config.match_number_field] = FieldPlan {FieldSource::MATCH_NUMBER, DConstants::INVALID_INDEX};
		for (idx_t i = 0; i < config.aggregates.size(); i++) {
			field_plan[config.aggregates[i].field] = FieldPlan {FieldSource::AGGREGATE, i};
		}
		for (auto &condition : config.conditions) {
			unordered_set<idx_t> seen;
			vector<idx_t> fields;
			ExpressionIterator::VisitExpression<BoundReferenceExpression>(
			    *condition, [&](const BoundReferenceExpression &bound_ref) {
				    if (seen.insert(bound_ref.Index()).second) {
					    fields.push_back(bound_ref.Index());
				    }
			    });
			condition_fields.push_back(std::move(fields));
		}
		case_insensitive_map_t<idx_t> symbol_index;
		for (idx_t i = 0; i < config.symbols.size(); i++) {
			symbol_index[config.symbols[i]] = i;
		}
		// a name is a primary variable, which is one symbol, or a union of them, which is its members'
		auto lookup = [&](const string &name) {
			vector<idx_t> found;
			auto entry = symbol_index.find(name);
			if (entry != symbol_index.end()) {
				found.push_back(entry->second);
				return found;
			}
			for (auto &subset : config.subsets) {
				if (!StringUtil::CIEquals(subset.name, name)) {
					continue;
				}
				for (auto &member : subset.members) {
					found.push_back(symbol_index.at(member));
				}
			}
			return found;
		};
		skip_symbols = lookup(config.after_match_variable);
		// a navigation over the match as a whole reads its ends, so only a named variable needs a run
		for (auto &navigation : config.navigations) {
			navigation_runs.push_back(navigation.symbol.empty() ? DConstants::INVALID_INDEX
			                                                    : runs.Track(lookup(navigation.symbol)));
			if (navigation.classifier) {
				runs.TrackClassifiers();
			}
		}
		// an aggregate's rows are recorded from the first call on, so its run is built here rather than
		// with the rest of what a condition needs
		for (auto &aggregate : config.aggregates) {
			auto &bound = aggregate.expression->Cast<BoundAggregateExpression>();
			const AggregateObject object(bound);
			if (object.function.HasStateDestructorCallback()) {
				// its state owns memory the matcher would have to hand back on every rewind
				throw NotImplementedException(
				    "An aggregate whose state holds its own memory is not supported in a DEFINE condition yet");
			}
			// an aggregate over the match as a whole folds every row up to the one being tested, so
			// only one over a named variable needs its rows recorded
			const auto rows =
			    aggregate.symbol.empty() ? DConstants::INVALID_INDEX : runs.Track(lookup(aggregate.symbol));
			auto run = make_uniq<AggregateRun>(context.client, object, rows);
			// the operand is read as the aggregate's own argument type, which binding may have widened
			if (aggregate.operand.IsValid()) {
				auto &column = collection.GetTypes()[columns_idx[aggregate.operand.GetIndex()]];
				auto &argument = bound.GetChildren()[0]->GetReturnType();
				run->row = make_uniq<CursorRow>(column);
				if (column != argument) {
					run->operand = make_uniq<DataChunk>();
					run->operand->Initialize(context.client, {argument}, 1U);
				}
			}
			if (!columns_idx.empty()) {
				run->cursor = make_uniq<WindowCursor>(collection, columns_idx);
			}
			aggregate_runs.push_back(std::move(run));
		}
	}

	void BeginMatch(idx_t start, idx_t number) {
		match_start = start;
		match_number = number;
		runs.BeginMatch(start);
		for (auto &run : aggregate_runs) {
			run->begin = DConstants::INVALID_INDEX;
		}
	}
	const vector<idx_t> &SkipSymbols() const {
		return skip_symbols;
	}

	bool Matches(idx_t index, idx_t row) {
		D_ASSERT(index < config.symbols.size());
		if (!runs.Empty()) {
			runs.Classify(index, row);
		}
		if (!config.row_scoped[index]) {
			return gstate.condition_values[index][row] != 0;
		}

		if (!ready) {
			Initialize();
		}
		// release the variable-size values of the previous evaluation: an aggregate finalizing into a
		// field appends to its storage rather than replacing what is there
		if (row_grows) {
			ResetRow();
		}

		// one row of the fields this condition reads, rather than a copy of everything collected
		for (auto field : condition_fields[index]) {
			auto &plan = field_plan[field];
			auto &target = row_chunk.data[field];
			switch (plan.source) {
			case FieldSource::MATCH_NUMBER:
				target.SetValue(0, Value::UBIGINT(match_number));
				break;
			case FieldSource::NAVIGATION: {
				auto &navigation = config.navigations[plan.index];
				auto navigated = Navigate(navigation, plan.index, row);
				if (navigated.IsValid() && navigation.step != 0) {
					// a step from the row navigated to walks the partition, and a condition only knows
					// the rows of the match so far: before its start, or past the row being tested,
					// there is nothing it can read (5.6.2, 5.9)
					const auto stepped = NumericCast<int64_t>(navigated.GetIndex()) + navigation.step;
					navigated = stepped >= NumericCast<int64_t>(match_start) && stepped <= NumericCast<int64_t>(row)
					                ? optional_idx(NumericCast<idx_t>(stepped))
					                : optional_idx();
				}
				if (!navigated.IsValid()) {
					// the match has no such row, which is what the condition reads as NULL
					target.Reference(*field_nulls[field]);
				} else if (navigation.classifier) {
					// the classifier of a row of the attempt under way, which the matcher knows itself
					const auto symbol = runs.ClassifierOf(navigated.GetIndex());
					target.Reference(symbol == DConstants::INVALID_INDEX ? *field_nulls[field] : *symbol_names[symbol]);
				} else {
					// a cursor of its own, because seeking the row being tested would move this one
					auto &cursor = *navigation_cursors[plan.index];
					target.Reference(field_rows[field]->Read(cursor, field, navigated.GetIndex()));
				}
				break;
			}
			case FieldSource::CURRENT_ROW:
				// a condition only reads fields that are columns of the collection, which are the
				// fields a view was built for
				D_ASSERT(field_rows[field]);
				target.Reference(field_rows[field]->Read(*row_cursor, field, row));
				break;
			case FieldSource::AGGREGATE:
				FoldAggregate(plan.index, row, target);
				break;
			}
		}

		row_result.Reset();
		if (!executors[index]) {
			executors[index] = make_uniq<ExpressionExecutor>(context.client, *conditions[index]);
		}
		executors[index]->Execute(row_chunk, row_result);
		// the entry borrows from the iterator, so the iterator has to outlive it
		const auto results = row_result.data[0].Values<bool>();
		const auto result = results[0];
		return result.IsValid() && result.GetValueUnsafe();
	}

private:
	//! Where a field of the condition input takes its value from
	enum class FieldSource : uint8_t { CURRENT_ROW, MATCH_NUMBER, NAVIGATION, AGGREGATE };
	struct FieldPlan {
		FieldSource source = FieldSource::CURRENT_ROW;
		//! Which navigation or aggregate the value comes from, unused by the other sources
		idx_t index = DConstants::INVALID_INDEX;
	};

	void Initialize() {
		vector<LogicalType> types;
		for (auto column_idx : columns_idx) {
			types.push_back(collection.GetTypes()[column_idx]);
		}
		// the matcher supplies its own field, which sits after the ones the plan does
		// the matcher supplies the match number and one field per aggregate, past the plan's columns
		idx_t supplied = config.match_number_field + 1;
		for (auto &aggregate : config.aggregates) {
			supplied = MaxValue<idx_t>(supplied, aggregate.field + 1);
		}
		for (auto &navigation : config.navigations) {
			if (navigation.classifier) {
				supplied = MaxValue<idx_t>(supplied, navigation.field + 1);
			}
		}
		types.resize(MaxValue<idx_t>(types.size(), supplied), LogicalType::UBIGINT);
		for (auto &aggregate : config.aggregates) {
			types[aggregate.field] = aggregate.expression->GetReturnType();
		}
		for (auto &navigation : config.navigations) {
			if (navigation.classifier) {
				types[navigation.field] = LogicalType::VARCHAR;
			}
		}
		// what a navigation reading a classifier points its field at: one constant per symbol
		for (auto &name : gstate.classifier_names) {
			symbol_names.push_back(make_uniq<Vector>(Value(name), count_t(1)));
		}
		row_chunk.Initialize(context.client, types, 1);
		// one expression is evaluated at a time here, so the result holds a single column
		row_result.Initialize(context.client, vector<LogicalType> {LogicalType::BOOLEAN}, 1);
		// a field read off the collection is pointed at rather than copied, so the only field that can
		// grow is one an aggregate writes into
		for (auto &aggregate : config.aggregates) {
			row_grows = row_grows || !TypeIsConstantSize(types[aggregate.field].InternalType());
		}
		ResetRow();
		field_plan.resize(MaxValue<idx_t>(field_plan.size(), types.size()));

		// a field that reads a collected row does so in place; a navigation that lands outside the
		// match reads a row that is not there, which is the one value it needs of its own
		field_rows.resize(field_plan.size());
		field_nulls.resize(field_plan.size());
		for (idx_t field = 0; field < field_plan.size(); field++) {
			auto &plan = field_plan[field];
			if (plan.source != FieldSource::CURRENT_ROW && plan.source != FieldSource::NAVIGATION) {
				continue;
			}
			// a classifier is supplied by the matcher rather than read off a collected column
			const bool collected = field < columns_idx.size();
			if (collected) {
				field_rows[field] = make_uniq<CursorRow>(types[field]);
			}
			if (plan.source == FieldSource::NAVIGATION) {
				field_nulls[field] = make_uniq<Vector>(types[field], 1U);
				field_nulls[field]->SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(*field_nulls[field], true);
			}
		}

		if (!columns_idx.empty()) {
			row_cursor = make_uniq<WindowCursor>(collection, columns_idx);
			for (idx_t i = 0; i < config.navigations.size(); i++) {
				navigation_cursors.push_back(make_uniq<WindowCursor>(collection, columns_idx));
			}
			D_ASSERT(row_cursor->chunk.ColumnCount() == columns_idx.size());
		}
		ready = true;
	}

	//! Give the assembled row back, so that what it holds lives for one evaluation
	void ResetRow() {
		row_chunk.Reset();
		// the vectors carry their own size, and reading one row out of them means saying so here
		row_chunk.SetChildCardinality(1);
		// a condition only writes the fields it reads, so the rest must read as NULL rather than as
		// whatever the allocation held
		for (auto &field : row_chunk.data) {
			field.SetVectorType(VectorType::FLAT_VECTOR);
			FlatVector::ValidityMutable(field).SetInvalid(0);
		}
	}

	//! Settle one aggregate for the row being tested. The rows it reads are the ones its variable has
	//! matched so far, which only ever grow while a match is assembled - so a row is folded in once
	//! and the state carries it from there. A rewind is the one thing that takes rows back, and the
	//! state is rebuilt from the start when it does.
	void FoldAggregate(idx_t index, idx_t row, Vector &target) {
		auto &aggregate = config.aggregates[index];
		auto &run = *aggregate_runs[index];
		// a variable folds the rows it matched; the match as a whole folds every row up to this one
		optional_ptr<const vector<idx_t>> positions;
		if (run.rows != DConstants::INVALID_INDEX) {
			positions = runs.Rows(run.rows);
		}
		const idx_t needed = positions ? positions->size() : row - match_start + 1;
		// a run that gave rows back holds different rows than the state folded, whatever it now
		// counts; the match as a whole gives them back by covering fewer rows than before
		const idx_t version = positions ? runs.Version(run.rows) : 0;

		if (run.begin != match_start || needed < run.folded || version != run.version) {
			// what the state allocated while folding goes with it, or a pattern that keeps giving rows
			// back keeps every rebuild's allocations for the whole query
			run.running.allocator.Reset();
			auto state = run.running.GetStatePtr(0);
			AggregateStateInput state_input(run.aggr.function, run.aggr.GetFunctionData());
			run.aggr.function.GetStateInitCallback()(state_input, &state, 1);
			run.folded = 0;
			run.begin = match_start;
			run.version = version;
		}
		AggregateInputData input_data(run.aggr, run.running.allocator);
		for (idx_t i = run.folded; i < needed; i++) {
			if (!aggregate.operand.IsValid()) {
				// nothing is read off the row: the row itself is what is counted
				run.aggr.function.GetStateUpdateCallback()(nullptr, input_data, 0, run.statep, 1);
				continue;
			}
			const idx_t source = positions ? (*positions)[i] : match_start + i;
			auto &value = run.row->Read(*run.cursor, aggregate.operand.GetIndex(), source);
			auto input = &value;
			if (run.operand) {
				// the aggregate takes an argument wider than the column, so the row is cast into one
				auto &operands = *run.operand;
				operands.Reset();
				operands.SetChildCardinality(1);
				VectorOperations::Cast(context.client, value, operands.data[0], 1);
				input = operands.data.data();
			}
			run.aggr.function.GetStateUpdateCallback()(input, input_data, 1, run.statep, 1);
		}
		run.folded = needed;

		// A condition reads the aggregate of the rows mapped so far, so this is settled once per
		// candidate row rather than once per run - running semantics are the only ones DEFINE has
		// (ISO/IEC 19075-5 5.4). Finalizing reads the state without consuming it, so the same state
		// carries on growing.
		AggregateFinalizeInputData finalize_input(run.aggr, run.running.allocator);
		// a finalize marks its result null when the state holds nothing and otherwise leaves the mask
		// alone, so the row this writes into has to start out valid rather than carrying the NULL that
		// an unwritten field reads as
		target.SetVectorType(VectorType::FLAT_VECTOR);
		FlatVector::ValidityMutable(target).SetValid(0);
		run.aggr.function.GetStateFinalizeCallback()(*run.running.statef, finalize_input, target, 1, 0);
	}

	//! The rows of the match classified as each symbol that anything reads, in match order.
	//! FIRST()/LAST() and an aggregate over a variable both read these, so they are recorded as the
	//! match assembles rather than rescanned, and two readings of the same variable share one run.
	//! Testing a row again gives back what was recorded from there on, which belonged to an attempt
	//! the matcher has abandoned.
	class SymbolRuns {
	public:
		//! Read the rows of these symbols from here on, and report where they are kept. A union
		//! variable is a set of them; a primary variable a set of one.
		idx_t Track(vector<idx_t> symbols_p) {
			for (idx_t i = 0; i < symbols.size(); i++) {
				if (symbols[i] == symbols_p) {
					return i;
				}
			}
			symbols.push_back(std::move(symbols_p));
			runs.emplace_back();
			versions.push_back(0);
			return symbols.size() - 1;
		}
		bool Empty() const {
			return symbols.empty() && !classifying;
		}
		//! Remember what each row was classified as, for a navigation that reads a row's classifier
		void TrackClassifiers() {
			classifying = true;
		}
		//! The symbol `row` was classified as in the attempt under way, if it was
		idx_t ClassifierOf(idx_t row) const {
			const auto at = row - start;
			return row >= start && at < classified.size() ? classified[at] : DConstants::INVALID_INDEX;
		}
		const vector<idx_t> &Rows(idx_t tracked) const {
			return runs[tracked];
		}
		//! Changes whenever rows are given back, which is the one thing that makes a run's rows
		//! differ from the ones a reading of it already folded in - giving back two rows and taking
		//! two more leaves the count alone, so the count cannot say it
		idx_t Version(idx_t tracked) const {
			return versions[tracked];
		}

		void BeginMatch(idx_t start_p) {
			for (idx_t i = 0; i < runs.size(); i++) {
				if (!runs[i].empty()) {
					runs[i].clear();
					++versions[i];
				}
			}
			classified.clear();
			start = start_p;
			next_row = start_p;
		}

		//! Record that `row` was classified as `symbol`
		void Classify(idx_t symbol, idx_t row) {
			D_ASSERT(row <= next_row);
			if (row < next_row) {
				for (idx_t i = 0; i < runs.size(); i++) {
					auto &run = runs[i];
					const auto had = run.size();
					while (!run.empty() && run.back() >= row) {
						run.pop_back();
					}
					versions[i] += (run.size() != had);
				}
			}
			for (idx_t i = 0; i < symbols.size(); i++) {
				if (Contains(symbols[i], symbol)) {
					runs[i].push_back(row);
				}
			}
			if (classifying) {
				classified.resize(row - start);
				classified.push_back(symbol);
			}
			next_row = row + 1;
		}

	private:
		//! The symbols each run reads, and the rows of the match classified as any of them
		vector<vector<idx_t>> symbols;
		vector<vector<idx_t>> runs;
		//! Bumped for a run whenever it gives rows back
		vector<idx_t> versions;
		//! What each row from the match's start on was classified as, kept only when something reads it
		bool classifying = false;
		idx_t start = 0;
		vector<idx_t> classified;
		//! One past the last row a classification was recorded for
		idx_t next_row = 0;
	};

	//! One row of one column of a cursor, read where it lies rather than copied out. The row is a
	//! dictionary over the chunk the cursor holds, so stepping to another row of that chunk writes a
	//! single index; seeking can replace the chunk, and a view whose chunk has moved is built again.
	struct CursorRow {
		explicit CursorRow(const LogicalType &type) : view(type, 1U), flat(type, 1U) {
			// the view is built over a chunk before it selects a row, so it starts on a row it has
			one.set_index(0, 0);
		}

		Vector &Read(WindowCursor &cursor, idx_t field, idx_t row) {
			const auto index = cursor.Seek(row);
			if (base != cursor.state.current_row_index) {
				auto &source = cursor.chunk.data[field];
				// a dictionary selects rows of a flat vector, so anything else is laid out flat first
				auto &dictionary = source.GetVectorType() == VectorType::FLAT_VECTOR ? source : Flatten(source);
				view.Dictionary(dictionary, dictionary.size(), one, 1);
				base = cursor.state.current_row_index;
				sel = &DictionaryVector::SelVector(view);
			}
			sel->set_index(0, UnsafeNumericCast<sel_t>(index));
			return view;
		}

		Vector &Flatten(Vector &source) {
			flat.Reference(source);
			flat.Flatten();
			return flat;
		}

		//! The row itself, a one-entry dictionary over the cursor's chunk
		Vector view;
		//! Where a chunk the dictionary cannot select from is laid out
		Vector flat;
		//! The entry the view selects, which the view holds its own copy of once it is built
		SelectionVector one {1};
		optional_ptr<SelectionVector> sel;
		//! The chunk the view was built over, so that a cursor moving off it is noticed
		idx_t base = DConstants::INVALID_INDEX;
	};

	//! The row FIRST()/LAST() navigates to, or an invalid index when the match has no such row
	optional_idx Navigate(const MatchRecognizeFunctionData::Navigation &navigation, idx_t navigation_idx,
	                      idx_t row) const {
		if (navigation.symbol.empty()) {
			// the match covers [match_start, row], so the offset is compared against how many rows
			// that is; added to an end first it would wrap and land back inside the match
			const auto matched = row - match_start;
			if (navigation.offset > matched) {
				return optional_idx();
			}
			return navigation.last ? optional_idx(row - navigation.offset)
			                       : optional_idx(match_start + navigation.offset);
		}
		auto &positions = runs.Rows(navigation_runs[navigation_idx]);
		if (positions.size() <= navigation.offset) {
			return optional_idx();
		}
		return navigation.last ? positions[positions.size() - 1 - navigation.offset] : positions[navigation.offset];
	}

	ExecutionContext &context;
	WindowMatchRecognizeGlobalState &gstate;
	const MatchRecognizeFunctionData &config;
	const WindowCollection &collection;
	//! The collected columns the matcher reads, in the order the conditions address them
	const vector<column_t> &columns_idx;
	vector<unique_ptr<ExpressionExecutor>> executors;
	vector<unique_ptr<Expression>> conditions;
	//! The rows of each symbol read by a navigation or an aggregate, in match order
	SymbolRuns runs;
	//! Where each navigation's variable keeps its rows, invalid for one over the match as a whole
	vector<idx_t> navigation_runs;
	//! What AFTER MATCH SKIP TO resumes at, which for a union variable is any of its members
	vector<idx_t> skip_symbols;
	idx_t match_start = 0;
	idx_t match_number = 1;
	DataChunk row_chunk;
	DataChunk row_result;
	//! Whether any field of the row holds its values outside the vector's own data
	bool row_grows = false;
	//! Where each field of row_chunk takes its value from
	vector<FieldPlan> field_plan;
	//! One view per field that reads a collected row, and the NULL a navigation off the match reads
	vector<unique_ptr<CursorRow>> field_rows;
	vector<unique_ptr<Vector>> field_nulls;
	//! The name of each symbol as a constant, for a navigation that reads a row's classifier
	vector<unique_ptr<Vector>> symbol_names;
	//! The fields each condition reads, so that deciding one copies no more than it needs
	vector<vector<idx_t>> condition_fields;
	//! Reads the row being tested. Owned by this thread, like the ones below.
	unique_ptr<WindowCursor> row_cursor;
	//! One per navigation, because two of them can be reading two different rows at once
	vector<unique_ptr<WindowCursor>> navigation_cursors;

	//! One aggregate's running state, and how much of its variable's run is already in it. Folding a
	//! row in costs one update, and the value a condition needs per candidate row is read by finalizing
	//! the running state in place. That relies on finalize leaving the state as it found it, which
	//! holds for every aggregate without a state destructor - the ones with one are refused above.
	struct AggregateRun {
		AggregateRun(ClientContext &client, const AggregateObject &aggr_p, idx_t rows_p)
		    : aggr(aggr_p), rows(rows_p), running(client, aggr_p) {
			running.Initialize(1);
			statep.SetVectorType(VectorType::CONSTANT_VECTOR);
			statep.Flatten();
			auto pointers = FlatVector::GetDataMutable<data_ptr_t>(statep);
			for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
				pointers[i] = running.GetStatePtr(0);
			}
		}
		AggregateObject aggr;
		//! Where its symbol's rows are kept, invalid when it reads the match as a whole
		idx_t rows;
		WindowAggregateStates running;
		//! one pointer per row folded, all of them the running state's
		Vector statep {LogicalType::POINTER};
		//! how many of the variable's rows the running state holds, and which shape of the run they
		//! were taken from
		idx_t folded = 0;
		idx_t version = 0;
		//! the match the state belongs to, so that a new one is noticed
		idx_t begin = DConstants::INVALID_INDEX;
		//! Its own cursor, because two aggregates can be reading two different rows at once
		unique_ptr<WindowCursor> cursor;
		//! The operand where it lies in the collection
		unique_ptr<CursorRow> row;
		//! Where it is cast when the aggregate's argument type is not the column's own
		unique_ptr<DataChunk> operand;
	};
	vector<unique_ptr<AggregateRun>> aggregate_runs;
	bool ready = false;
};

//! How long a walked state stays proof of a dead end, which is as long as the conditions reaching it
//! answer the same way each time (see PatternMemo). The strongest reason any of them gives wins.
static PatternMemo RequiredMemo(const MatchRecognizeFunctionData &config) {
	// the match as a whole starts where the attempt does, but which rows were matched to a variable
	// differs between two ways of reaching the same state - and so does what any row of it was
	// classified as, which a navigation over CLASSIFIER() reads whether or not it names a variable
	for (auto &navigation : config.navigations) {
		if (!navigation.symbol.empty() || navigation.classifier) {
			return PatternMemo::HISTORY;
		}
	}
	// an aggregate over a variable reads the rows mapped to it, so the same reasoning holds
	for (auto &aggregate : config.aggregates) {
		if (!aggregate.symbol.empty()) {
			return PatternMemo::HISTORY;
		}
	}
	// what is left reads MATCH_NUMBER() or navigates the match as a whole, which an attempt fixes
	for (auto scoped : config.row_scoped) {
		if (scoped) {
			return PatternMemo::ATTEMPT;
		}
	}
	return PatternMemo::PARTITION;
}

static void ScanPartitions(ExecutionContext &context, WindowMatchRecognizeGlobalState &gstate,
                           const MatchRecognizeFunctionData &config, const WindowCollection &collection) {
	auto &classifiers = gstate.classifiers;
	MatchRecognizeSpanWriter writer(gstate.SpanArena(context.client));
	RowConditions row_conditions(context, gstate, config, collection);
	SymbolMatcher symbol_matches = [&](idx_t index, idx_t row) {
		return row_conditions.Matches(index, row);
	};

	PatternProgram program;
	program.Compile(*config.pattern, classifiers.size());
	program.Finish();
	PatternMatcher matcher(context.client, program, symbol_matches, classifiers, gstate.excluded_rows,
	                       RequiredMemo(config));

	// partitions are independent, so the threads reaching Finalize share them out
	while (true) {
		const auto partition_idx = gstate.next_partition++;
		if (partition_idx >= gstate.partitions.size()) {
			break;
		}
		const auto partition_start = gstate.partitions[partition_idx].first;
		const auto partition_end = gstate.partitions[partition_idx].second;
		matcher.BeginPartition();

		// scan left to right, applying AFTER MATCH SKIP after every match
		idx_t match_number = 0;
		auto row = partition_start;
		while (row <= partition_end) {
			context.client.InterruptCheck();
			row_conditions.BeginMatch(row, match_number + 1);
			if (!matcher.Match(row, partition_start, partition_end + 1)) {
				row++;
				continue;
			}
			// an empty match is still reported, but covers no rows - the scan steps past it rather than
			// skipping, or it would never move
			if (matcher.match_end <= row) {
				match_number++;
				writer.Append(gstate.row_spans[row], MatchRecognizeSpan::Empty(match_number, row));
				row++;
				continue;
			}
			// a match can never reach beyond its own partition
			const auto match_end = MinValue(matcher.match_end - 1, partition_end);
			match_number++;

			for (idx_t match_row = row; match_row <= match_end; match_row++) {
				writer.Append(gstate.row_spans[match_row],
				              MatchRecognizeSpan::Row(classifiers[match_row], match_number, row, match_end,
				                                      gstate.excluded_rows[match_row] != 0));
			}
			row = SkipTo(config, row_conditions.SkipSymbols(), row, match_end, classifiers);
		}
	}
}

SourceResultType WindowMatchRecognizeExecutor::Finalize(ExecutionContext &context,
                                                        optional_ptr<WindowCollection> collection,
                                                        OperatorSinkInput &sink) {
	auto &gstate = sink.global_state.Cast<WindowMatchRecognizeGlobalState>();
	auto &config = gstate.executor.wexpr.BindInfo()->Cast<MatchRecognizeFunctionData>();

	// we always start with a new partition
	D_ASSERT(gstate.partition_mask.RowIsValid(0));

	PrepareHashGroup(gstate);
	ScanPartitions(context, gstate, config, *collection);
	return SourceResultType::FINISHED;
}

void WindowMatchRecognizeExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                           Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gstate = sink.global_state.Cast<WindowMatchRecognizeGlobalState>();
	// one chunk of memberships at a time rather than the whole input. Matching is over by now - every
	// thread has left Finalize - so the shared state below is only read.
	const auto count = bounds.size();
	auto writer = FlatVector::Writer<VectorListType<SpanStruct>>(result, count);
	for (idx_t i = 0; i < count; i++) {
		const auto row = row_idx + i;
		auto &row_spans = gstate.row_spans[row];
		auto node = row_spans.first;
		for (auto &membership : writer.WriteList(row_spans.count)) {
			auto &span = node->span;
			membership.WriteValue([&](auto &classifier, auto &match_number, auto &is_match_start, auto &is_match_end,
			                          auto &match_start, auto &match_end, auto &is_excluded, auto &is_empty,
			                          auto &row_index) {
				// an empty match covers no rows, so no row of it classified as anything
				if (span.empty) {
					classifier.WriteNull();
				} else {
					classifier.WriteValue(string_t(gstate.classifier_names[span.symbol]));
				}
				match_number.WriteValue(span.match_number);
				is_match_start.WriteValue(row == span.match_start);
				is_match_end.WriteValue(row == span.match_end);
				match_start.WriteValue(span.match_start);
				match_end.WriteValue(span.match_end);
				is_excluded.WriteValue(span.excluded);
				is_empty.WriteValue(span.empty);
				row_index.WriteValue(row);
			});
			node = node->next;
		}
	}
}

WindowFunction MatchRecognizeFun::GetFunction() {
	// called with the columns the conditions read; everything else arrives as bind data
	WindowFunction fun(Name, {LogicalType::ANY}, WindowMatchRecognizeExecutor::ResultType(),
	                   ExpressionType::WINDOW_FUNCTION, WindowMatchRecognizeExecutor::Bind,
	                   WindowMatchRecognizeExecutor::GetBounds, WindowMatchRecognizeExecutor::GetSharing,
	                   WindowMatchRecognizeExecutor::GetGlobal, WindowMatchRecognizeExecutor::GetLocal,
	                   WindowMatchRecognizeExecutor::Sink, WindowMatchRecognizeExecutor::Finalize,
	                   WindowMatchRecognizeExecutor::GetData);
	auto &signature = fun.GetSignature();
	signature = FunctionSignature(vector<FunctionParameter>(), WindowMatchRecognizeExecutor::ResultType());
	signature.AddParameter(Identifier("columns"), LogicalType::ANY);
	signature.AddArgs("args", LogicalType::ANY);

	fun.SetSerializeCallback(WindowMatchRecognizeExecutor::Serialize);
	fun.SetDeserializeCallback(WindowMatchRecognizeExecutor::Deserialize);

	fun.SetFallible(); // AFTER MATCH SKIP validation and matcher budget checks throw at runtime

	return fun;
}

} // namespace duckdb
