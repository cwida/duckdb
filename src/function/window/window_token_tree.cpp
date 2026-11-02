#include "duckdb/function/window/window_token_tree.hpp"
#include "duckdb/function/window/window_collection.hpp"

namespace duckdb {

class WindowTokenTreeLocalState : public WindowMergeSortTreeLocalState {
public:
	WindowTokenTreeLocalState(ExecutionContext &context, WindowTokenTree &token_tree)
	    : WindowMergeSortTreeLocalState(context, token_tree), token_tree(token_tree) {
	}
	//! Process sorted leaf data
	void BuildLeaves() override;

	WindowTokenTree &token_tree;
};

void WindowTokenTreeLocalState::BuildLeaves() {
	// Find our chunk range
	auto &collection = *token_tree.sorted;
	const auto block_begin = (build_task * collection.ChunkCount()) / token_tree.total_tasks;
	const auto block_end = ((build_task + 1) * collection.ChunkCount()) / token_tree.total_tasks;

	auto &deltas = token_tree.deltas;
	if (!block_begin) {
		// First block, so set up initial delta
		deltas[0] = 0;
	}

	const auto &scan_cols = token_tree.key_cols;
	const auto key_count = scan_cols.size();
	WindowDeltaScanner(collection, block_begin, block_end, scan_cols, key_count,
	                   [&](const idx_t row_idx, DataChunk &prev, DataChunk &curr, const idx_t ndistinct,
	                       SelectionVector &distinct, const SelectionVector &matching) {
		                   //	Same as previous - token delta is 0
		                   const auto count = MinValue<idx_t>(prev.size(), curr.size());
		                   const auto nmatch = count - ndistinct;
		                   for (idx_t j = 0; j < nmatch; ++j) {
			                   auto scan_idx = matching.get_index(j);
			                   deltas[scan_idx + row_idx] = 0;
		                   }

		                   //	Different value - token delta is 1
		                   for (idx_t j = 0; j < ndistinct; ++j) {
			                   auto scan_idx = distinct.get_index(j);
			                   deltas[scan_idx + row_idx] = 1;
		                   }
	                   });
}

idx_t WindowTokenTree::MeasurePayloadBlocks() {
	const auto count = WindowMergeSortTree::MeasurePayloadBlocks();

	deltas.resize(count);

	return count;
}

template <typename TOKENS>
static void BuildTokens(WindowTokenTree &token_tree, TOKENS &tokens) {
	using T = typename TOKENS::value_type;
	auto &collection = *token_tree.sorted;
	if (!collection.Count()) {
		return;
	}
	//	Scan the index column
	vector<column_t> scan_ids(1, token_tree.scan_cols.size() - 1);
	WindowCollectionChunkScanner scanner(collection, scan_ids, 0);
	auto &payload_chunk = scanner.chunk;

	const T *row_idx = nullptr;
	idx_t i = 0;

	T token = 0;
	for (auto &d : token_tree.deltas) {
		if (i >= payload_chunk.size()) {
			if (!scanner.Scan()) {
				break;
			}
			row_idx = FlatVector::GetDataUnsafe<T>(payload_chunk.data[0]);
			i = 0;
		}
		D_ASSERT(row_idx != nullptr);
		token += d;
		tokens[row_idx[i++]] = token;
	}
}

unique_ptr<LocalSinkState> WindowTokenTree::GetLocalState(ExecutionContext &context) {
	return make_uniq<WindowTokenTreeLocalState>(context, *this);
}

void WindowTokenTree::Finished() {
	//	Convert the deltas to tokens
	if (mst64) {
		BuildTokens(*this, mst64->LowestLevel());
	} else {
		BuildTokens(*this, mst32->LowestLevel());
	}
	/*
	    for (const auto &d : deltas) {
	        Printer::Print(StringUtil::Format("%lld", d));
	    }
	*/
	// Deallocate memory
	vector<uint8_t> empty;
	deltas.swap(empty);

	WindowMergeSortTree::Finished();
}

template <typename TREE>
static idx_t TokenRank(const TREE &tree, const idx_t lower, const idx_t upper, const idx_t row_idx) {
	idx_t rank = 1;
	const auto needle = tree.LowestLevel()[row_idx];
	tree.AggregateLowerBound(lower, upper, needle, [&](idx_t level, const idx_t run_begin, const idx_t run_pos) {
		rank += run_pos - run_begin;
	});
	return rank;
}

idx_t WindowTokenTree::Rank(const idx_t lower, const idx_t upper, const idx_t row_idx) const {
	if (mst64) {
		return TokenRank(*mst64, lower, upper, row_idx);
	} else {
		return TokenRank(*mst32, lower, upper, row_idx);
	}
}

template <typename TREE>
static idx_t NextPeer(const TREE &tree, const idx_t lower, const idx_t upper, const idx_t row_idx) {
	// We return an index, not a relative position
	idx_t idx = lower;
	// Because tokens are dense, we can find the next peer by adding 1 to the probed token value
	const auto needle = tree.LowestLevel()[row_idx] + 1;
	tree.AggregateLowerBound(lower, upper, needle, [&](idx_t level, const idx_t run_begin, const idx_t run_pos) {
		idx += run_pos - run_begin;
	});
	return idx;
}

idx_t WindowTokenTree::PeerEnd(const idx_t lower, const idx_t upper, const idx_t row_idx) const {
	if (mst64) {
		return NextPeer(*mst64, lower, upper, row_idx);
	} else {
		return NextPeer(*mst32, lower, upper, row_idx);
	}
}

} // namespace duckdb
