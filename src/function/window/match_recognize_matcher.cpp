#include "duckdb/function/window/match_recognize_matcher.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

//! The fewest rows a pattern node can match, which bounds how often a repetition of it is reachable
static idx_t MinConsumption(const MatchRecognizePattern &node) {
	const auto saturating_add = [](idx_t left, idx_t right) {
		return left > NumericLimits<idx_t>::Maximum() - right ? NumericLimits<idx_t>::Maximum() : left + right;
	};
	switch (node.type) {
	case MatchRecognizePatternType::SYMBOL:
		return 1;
	case MatchRecognizePatternType::CONCATENATION: {
		idx_t total = 0;
		for (auto &child : node.children) {
			total = saturating_add(total, MinConsumption(*child));
		}
		return total;
	}
	case MatchRecognizePatternType::ALTERNATION:
		return MinValue(MinConsumption(*node.children[0]), MinConsumption(*node.children[1]));
	case MatchRecognizePatternType::QUANTIFIER: {
		if (!node.min_count.IsValid() || node.min_count.GetIndex() == 0) {
			return 0;
		}
		const auto child = MinConsumption(*node.children[0]);
		const auto count = node.min_count.GetIndex();
		return child != 0 && count > NumericLimits<idx_t>::Maximum() / child ? NumericLimits<idx_t>::Maximum()
		                                                                     : child * count;
	}
	default:
		// an anchor takes no row
		return 0;
	}
}

void PatternProgram::Compile(const MatchRecognizePattern &node, idx_t limit, bool excluded) {
	switch (node.type) {
	case MatchRecognizePatternType::SYMBOL: {
		PatternInstruction symbol;
		symbol.op = PatternOp::SYMBOL;
		symbol.symbol = node.symbol;
		symbol.excluded = excluded;
		Push(symbol);
		break;
	}
	case MatchRecognizePatternType::ANCHOR: {
		PatternInstruction anchor;
		anchor.op = PatternOp::ANCHOR;
		anchor.at_end = node.at_end;
		Push(anchor);
		break;
	}
	case MatchRecognizePatternType::CONCATENATION:
		for (auto &child : node.children) {
			Compile(*child, limit, excluded);
		}
		break;
	case MatchRecognizePatternType::ALTERNATION: {
		auto split = Emit(PatternOp::SPLIT);
		code[split].target = code.size();
		Compile(*node.children[0], limit, excluded);
		auto jump = Emit(PatternOp::JUMP);
		code[split].alternative = code.size();
		Compile(*node.children[1], limit, excluded);
		code[jump].target = code.size();
		break;
	}
	case MatchRecognizePatternType::QUANTIFIER: {
		auto &child_node = *node.children[0];
		const auto inner = excluded || node.excluded;
		// one repetition past what the rows allow is already unsatisfiable, as every further one is
		const idx_t declared_min = node.min_count.IsValid() ? node.min_count.GetIndex() : 0;
		const idx_t consumption = MinConsumption(child_node);
		const idx_t reachable = consumption == 0 ? limit + 1 : limit / consumption + 1;
		const idx_t min_count = MinValue(declared_min, reachable);
		for (idx_t i = 0; i < min_count; i++) {
			Compile(child_node, limit, inner);
		}
		// a split's target is taken before its alternative, which is what greedy and reluctant swap
		const auto reluctant = node.reluctant;
		if (!node.max_count.IsValid()) {
			const auto loop = code.size();
			auto split = Emit(PatternOp::SPLIT);
			const auto again = code.size();
			Compile(child_node, limit, inner);
			code[Emit(PatternOp::JUMP)].target = loop;
			const auto leave = code.size();
			code[split].target = reluctant ? leave : again;
			code[split].alternative = reluctant ? again : leave;
			break;
		}
		const auto max_count = MinValue(node.max_count.GetIndex(), min_count + reachable);
		vector<idx_t> exits;
		for (idx_t i = min_count; i < max_count; i++) {
			auto split = Emit(PatternOp::SPLIT);
			const auto again = code.size();
			(reluctant ? code[split].alternative : code[split].target) = again;
			exits.push_back(split);
			Compile(child_node, limit, inner);
		}
		for (auto exit_split : exits) {
			(reluctant ? code[exit_split].target : code[exit_split].alternative) = code.size();
		}
		break;
	}
	default:
		throw InternalException("Unsupported MATCH_RECOGNIZE pattern node");
	}
}

void PatternProgram::Finish() {
	Emit(PatternOp::MATCH);
}

idx_t PatternProgram::Emit(PatternOp op) {
	PatternInstruction instruction;
	instruction.op = op;
	Push(instruction);
	return code.size() - 1;
}

void PatternProgram::Push(const PatternInstruction &instruction) {
	if (code.size() >= MAX_INSTRUCTIONS) {
		throw InvalidInputException(
		    "The MATCH_RECOGNIZE pattern compiles to more than %llu instructions, which is more than can be "
		    "matched. Repetition counts multiply, so nesting them is what usually gets here.",
		    MAX_INSTRUCTIONS);
	}
	code.push_back(instruction);
}

PatternMatcher::PatternMatcher(ClientContext &context_p, const PatternProgram &program_p,
                               const SymbolMatcher &symbol_matches_p, vector<idx_t> &classifiers_p,
                               vector<uint8_t> &excluded_rows_p, PatternMemo memo_p)
    : context(context_p), program(program_p), symbol_matches(symbol_matches_p), classifiers(classifiers_p),
      excluded_rows(excluded_rows_p), memo(memo_p), row_count(classifiers_p.size()),
      max_states(Settings::Get<MatchRecognizeMaxStatesSetting>(context_p)) {
	if (memo == PatternMemo::HISTORY) {
		// no row is matched within a scope, so one mark per instruction is enough
		history_marks.assign(program.code.size(), 0);
		return;
	}
	const auto rows = row_count + 1;
	if (program.code.size() > NumericLimits<idx_t>::Maximum() / rows) {
		throw OutOfMemoryException("The MATCH_RECOGNIZE pattern needs a record per instruction per row, which does "
		                           "not fit in memory for %llu instructions over %llu rows",
		                           program.code.size(), row_count);
	}
	explored_size = program.code.size() * rows;
	// through the buffer manager's allocator, so that it counts against the memory limit
	explored = BufferManager::GetBufferManager(context).GetBufferAllocator().Allocate(explored_size);
	ClearExplored();
}

//! A walk that never explores a state twice takes at most one step per instruction per row, and one more
//! for every alternative it resumes only to find exhausted. Refusing a match below that would refuse it
//! for the work matching the pattern takes rather than for the backtracking the limit is there to stop.
idx_t PatternMatcher::StateLimit(idx_t rows) const {
	if (!max_states) {
		return 0;
	}
	const auto states_per_instruction = 2 * (rows + 1);
	if (program.code.size() > NumericLimits<idx_t>::Maximum() / states_per_instruction) {
		return 0;
	}
	return MaxValue(max_states, program.code.size() * states_per_instruction);
}

void PatternMatcher::BeginPartition() {
	if (memo == PatternMemo::HISTORY) {
		// every attempt opens a scope of its own below, and a mark only outlives the walk that took it
		return;
	}
	NextEpoch();
}

bool PatternMatcher::Match(idx_t start, idx_t partition_start, idx_t input_size) {
	if (memo == PatternMemo::ATTEMPT) {
		NextEpoch();
	}
	attempt_marks.clear();
	pending.clear();
	states = 0;
	const auto state_limit = StateLimit(input_size - partition_start);
	// the marks of the attempt before this one belong to walks that are over
	UnwindHistory(0);
	pending.push_back(PendingState {0, start, NextScope(), 0});
	while (!pending.empty()) {
		auto state = pending.back();
		pending.pop_back();
		auto pc = state.pc;
		auto offset = state.offset;
		// the marks this alternative may read are the ones its path had taken when it was pushed
		UnwindHistory(state.trail_size);
		auto walk = state.scope;
		while (true) {
			// a walk can be long, and a pattern that has to try again for every row longer still
			if (++steps >= INTERRUPT_INTERVAL) {
				steps = 0;
				context.InterruptCheck();
			}
			if (state_limit && ++states > state_limit) {
				throw InvalidInputException(
				    "MATCH_RECOGNIZE gave up on a match after exploring %llu states. A DEFINE condition here reads "
				    "the match itself and not only the row it tests, so the matcher has to keep the different ways "
				    "of matching the same row apart, and their number grows quickly with the size of the partition. "
				    "Narrow the PATTERN or the DEFINE conditions so that fewer ways reach the same row, or raise "
				    "match_recognize_max_states.",
				    state_limit);
			}
			if (!Visit(pc, offset, walk)) {
				break;
			}
			auto &instruction = program.code[pc];
			if (instruction.op == PatternOp::MATCH) {
				// a record only proves a dead end once its subtree was searched to exhaustion,
				// and this search stopped early
				for (auto mark : attempt_marks) {
					explored.get()[mark] = 0;
				}
				match_end = offset;
				return true;
			}
			if (instruction.op == PatternOp::JUMP) {
				pc = instruction.target;
				continue;
			}
			if (instruction.op == PatternOp::SPLIT) {
				pending.push_back(PendingState {instruction.alternative, offset, walk, history_trail.size()});
				pc = instruction.target;
				continue;
			}
			if (instruction.op == PatternOp::ANCHOR) {
				// an anchor takes no row, so it either holds where the walk stands or it does not
				if (offset != (instruction.at_end ? input_size : partition_start)) {
					break;
				}
				pc++;
				continue;
			}
			if (offset >= input_size) {
				break;
			}
			// the row is tentatively this symbol while its condition runs, so LAST(X.c) sees it
			classifiers[offset] = instruction.symbol;
			if (!symbol_matches(instruction.symbol, offset)) {
				break;
			}
			excluded_rows[offset] = instruction.excluded ? 1 : 0;
			pc++;
			offset++;
			// the walk now carries one more matched row, which is a history of its own
			walk = NextScope();
		}
	}
	return false;
}

bool PatternMatcher::Visit(idx_t pc, idx_t offset, idx_t walk) {
	if (memo == PatternMemo::HISTORY) {
		auto &mark = history_marks[pc];
		if (mark == walk) {
			return false;
		}
		// what the mark said before is what an alternative pushed before now has to see again
		history_trail.push_back(HistoryMark {pc, mark});
		mark = walk;
		return true;
	}
	const auto slot_index = pc * (row_count + 1) + offset;
	auto &slot = explored.get()[slot_index];
	if (slot == epoch) {
		return false;
	}
	slot = epoch;
	if (memo == PatternMemo::PARTITION) {
		attempt_marks.push_back(slot_index);
	}
	return true;
}

void PatternMatcher::NextEpoch() {
	if (++epoch == 0) {
		ClearExplored();
		epoch = 1;
	}
}

//! Open the stretch of the walk that starts where the last row was matched. Scope 0 is the one no mark
//! was ever taken in, and a 64 bit counter never comes back round to a scope still in use.
idx_t PatternMatcher::NextScope() {
	return memo == PatternMemo::HISTORY ? ++history_scope : 0;
}

//! Put the marks back the way the path being resumed left them
void PatternMatcher::UnwindHistory(idx_t trail_size) {
	while (history_trail.size() > trail_size) {
		auto &entry = history_trail.back();
		history_marks[entry.pc] = entry.scope;
		history_trail.pop_back();
	}
}

void PatternMatcher::ClearExplored() {
	memset(explored.get(), 0, explored_size);
}

} // namespace duckdb
