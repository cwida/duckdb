#include "duckdb/common/tree_renderer/text_tree_renderer.hpp"

#include "duckdb/common/pair.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/typedefs.hpp"

#include <algorithm>

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Render-width helpers (UTF8-aware)
//===--------------------------------------------------------------------===//
static idx_t RenderLength(const string &str) {
	return Utf8Proc::RenderWidth(str);
}
static idx_t RenderLength(const char *str, idx_t len) {
	return Utf8Proc::RenderWidth(string(str, len));
}
//! Whether the byte starts a codepoint (i.e. is not a UTF8 continuation byte)
static bool IsCharacter(char c) {
	return (static_cast<uint8_t>(c) & 0xC0) != 0x80;
}

//===--------------------------------------------------------------------===//
// Styled text primitives
//===--------------------------------------------------------------------===//
struct ExplainSpan {
	ExplainSpan(string text_p, TreeRenderType type_p) : text(std::move(text_p)), type(type_p) {
		width = RenderLength(text);
	}
	void SetText(string text_p) {
		text = std::move(text_p);
		width = RenderLength(text);
	}
	string text;
	TreeRenderType type;
	//! The render width of the text, measured once, as lines are measured many times while a plan is laid out
	idx_t width;
};

using ExplainLine = vector<ExplainSpan>;

//! A rectangular block of rendered lines. `spine` is the column at which a parent connects down into this block.
struct ExplainBlock {
	vector<ExplainLine> lines;
	idx_t width = 0;
	idx_t spine = 0;
	//! The number of rows this block's root operator passes up to its parent - drives the connector's line weight.
	optional_idx output_rows;
};

static idx_t LineWidth(const ExplainLine &line) {
	idx_t width = 0;
	for (auto &span : line) {
		width += span.width;
	}
	return width;
}

static void OffsetLine(ExplainLine &line, idx_t offset) {
	if (offset == 0) {
		return;
	}
	line.insert(line.begin(), ExplainSpan(string(offset, ' '), TreeRenderType::LAYOUT));
}

static void PadLine(ExplainLine &line, idx_t width) {
	idx_t current_width = LineWidth(line);
	if (current_width >= width) {
		return;
	}
	line.emplace_back(string(width - current_width, ' '), TreeRenderType::LAYOUT);
}

static idx_t LeadingSpaces(const ExplainLine &line) {
	idx_t count = 0;
	for (auto &span : line) {
		for (char c : span.text) {
			if (c != ' ') {
				return count;
			}
			count++;
		}
	}
	return count;
}

//! Append `src` to `dst` so that the first non-space character of `src` lands at render column `content_col`.
static void AppendAt(ExplainLine &dst, const ExplainLine &src, idx_t content_col) {
	PadLine(dst, content_col);
	idx_t to_skip = LeadingSpaces(src);
	idx_t skipped = 0;
	for (auto &span : src) {
		if (skipped >= to_skip) {
			dst.push_back(span);
			continue;
		}
		idx_t i = 0;
		while (i < span.text.size() && span.text[i] == ' ' && skipped < to_skip) {
			i++;
			skipped++;
		}
		if (i < span.text.size()) {
			ExplainSpan trimmed = span;
			trimmed.SetText(span.text.substr(i));
			dst.push_back(std::move(trimmed));
		}
	}
}

//! Returns true if the codepoint at render column "col" equals the given (single-width) UTF8 character.
static bool RenderCharEquals(const ExplainLine &line, idx_t col, const char *expected) {
	idx_t col_offset = 0;
	for (auto &span : line) {
		idx_t span_width = span.width;
		if (col >= col_offset + span_width) {
			col_offset += span_width;
			continue;
		}
		idx_t char_idx = 0;
		for (idx_t i = 0; i < span.text.size(); i++) {
			if (IsCharacter(span.text[i])) {
				if (char_idx == col - col_offset) {
					idx_t j = i + 1;
					while (j < span.text.size() && !IsCharacter(span.text[j])) {
						j++;
					}
					return span.text.compare(i, j - i, expected) == 0;
				}
				char_idx++;
			}
		}
		return false;
	}
	return false;
}

//! Replace the (single-width) character at render column "col" with the given UTF8 character.
static void SetLayoutChar(ExplainLine &line, idx_t col, const char *replacement) {
	idx_t col_offset = 0;
	for (auto &span : line) {
		idx_t span_width = span.width;
		if (col >= col_offset + span_width) {
			col_offset += span_width;
			continue;
		}
		idx_t char_idx = 0;
		idx_t byte_start = 0;
		idx_t byte_end = span.text.size();
		for (idx_t i = 0; i < span.text.size(); i++) {
			if (IsCharacter(span.text[i])) {
				if (char_idx == col - col_offset) {
					byte_start = i;
				} else if (char_idx == col - col_offset + 1) {
					byte_end = i;
					break;
				}
				char_idx++;
			}
		}
		span.SetText(span.text.substr(0, byte_start) + replacement + span.text.substr(byte_end));
		return;
	}
}

//! Map a light routing glyph to the variant with a heavy DOWNWARD stroke, used where a heavier edge drops off
//! the (light) horizontal bus into a child while still connecting along the bus:
//! e.g. ┬ -> ┰ (heavy drop), ╮ -> ┒ (heavy drop, arm from the left), ├ -> ┟ (heavy drop, arm to the right).
static const char *ThickDownVariant(const char *glyph) {
	string g(glyph);
	if (g == "┬") {
		return "┰";
	}
	if (g == "╮") {
		return "┒";
	}
	if (g == "╭") {
		return "┎";
	}
	if (g == "├") {
		return "┟";
	}
	if (g == "┤") {
		return "┧";
	}
	if (g == "┼") {
		return "╁";
	}
	return glyph;
}

//! The cap glyph where a tier-`tier` connector meets a *free* box border. `top`: the parent side (stem points down),
//! otherwise the child side (stem points up). `right`: the second column of a 2-column wide pipe.
//! (the wide pipe attaching to a join's bus is drawn separately, since the bus needs arm-bearing glyphs)
static const char *TierCap(idx_t tier, bool top, bool right) {
	switch (tier) {
	case 0:
		return top ? "┬" : "┴";
	case 1:
		return top ? "┰" : "┸";
	default: // wide pipe: a 2-column mouth with the arms folding back into the border
		if (top) {
			return right ? "┎" : "┒";
		}
		return right ? "┖" : "┚";
	}
}

//===--------------------------------------------------------------------===//
// Plan tree
//===--------------------------------------------------------------------===//
struct ExplainTreeNode {
	string name;
	vector<std::pair<string, vector<string>>> details;
	optional_idx rows;
	optional_idx estimated_rows;
	double timing = -1;
	vector<unique_ptr<ExplainTreeNode>> children;

	double subtree_time = 0;
	idx_t subtree_count = 1;
};

static void ComputeSubtreeStats(ExplainTreeNode &node) {
	node.subtree_time = node.timing > 0 ? node.timing : 0;
	node.subtree_count = 1;
	for (auto &child : node.children) {
		ComputeSubtreeStats(*child);
		node.subtree_time += child->subtree_time;
		node.subtree_count += child->subtree_count;
	}
}

//! The rows used to scale a connector's thickness: only the actual count (EXPLAIN ANALYZE) thickens a line.
//! Estimates are deliberately ignored so plain EXPLAIN plans stay light (the weight reflects real data flow).
static optional_idx NodeRows(const ExplainTreeNode &node) {
	return node.rows;
}

//! The largest row count emitted by any operator in the subtree (the plan's peak row flow).
static idx_t MaxNodeRows(const ExplainTreeNode &node) {
	idx_t result = 0;
	auto rows = NodeRows(node);
	if (rows.IsValid()) {
		result = rows.GetIndex();
	}
	for (auto &child : node.children) {
		result = MaxValue<idx_t>(result, MaxNodeRows(*child));
	}
	return result;
}

//! The operator name as shown in a box title - underscores become spaces and the result is title-cased
//! (e.g. HASH_JOIN -> Hash Join)
static string DisplayName(const string &name, bool upper_case) {
	if (upper_case) {
		// keep the raw operator name (e.g. HASH_JOIN)
		return name;
	}
	auto result = StringUtil::Title(StringUtil::Replace(name, "_", " "));
	// keep known acronyms capitalized (title-casing turns "CTE" into "Cte" and "IE" into "Ie")
	result = StringUtil::Replace(result, "Cte", "CTE");
	result = StringUtil::Replace(result, "Ie Join", "IE Join");
	return result;
}

static TreeRenderType GetOperatorType(const ExplainTreeNode &node) {
	auto &name = node.name;
	if (StringUtil::Contains(name, "SCAN") || StringUtil::Contains(name, "GET")) {
		return TreeRenderType::NODE_NAME_SCAN;
	}
	if (StringUtil::Contains(name, "JOIN") || name == "CROSS_PRODUCT") {
		return TreeRenderType::NODE_NAME_JOIN;
	}
	if (StringUtil::Contains(name, "AGGREGATE") || StringUtil::Contains(name, "GROUP_BY") ||
	    StringUtil::Contains(name, "DISTINCT") || StringUtil::Contains(name, "WINDOW")) {
		return TreeRenderType::NODE_NAME_AGGREGATE;
	}
	if (StringUtil::Contains(name, "ORDER_BY") || StringUtil::Contains(name, "TOP_N")) {
		return TreeRenderType::NODE_NAME_ORDER;
	}
	if (node.children.empty()) {
		for (auto &entry : node.details) {
			if (entry.first == "Table" || entry.first == "Function") {
				return TreeRenderType::NODE_NAME_SCAN;
			}
		}
	}
	return TreeRenderType::NODE_NAME;
}

//! Remove the redundant outer pair of parentheses wrapping an expression, e.g. "(a >= 1)" -> "a >= 1".
static string StripOuterParens(string text) {
	while (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
		idx_t depth = 0;
		bool in_quote = false;
		bool wraps = true;
		for (idx_t i = 0; i < text.size(); i++) {
			char c = text[i];
			if (c == '\'') {
				in_quote = !in_quote;
				continue;
			}
			if (in_quote) {
				continue;
			}
			if (c == '(') {
				depth++;
			} else if (c == ')') {
				if (depth == 0) {
					wraps = false;
					break;
				}
				depth--;
				if (depth == 0 && i + 1 != text.size()) {
					wraps = false;
					break;
				}
			}
		}
		if (!wraps || depth != 0 || in_quote) {
			break;
		}
		text = text.substr(1, text.size() - 2);
	}
	return text;
}

//! Build the renderer's plan tree from a RenderTree (mapping the extra_text metadata onto name/details/rows/timing)
static unique_ptr<ExplainTreeNode> BuildExplainTree(RenderTree &tree, idx_t x, idx_t y) {
	auto node_p = tree.GetNode(x, y);
	if (!node_p) {
		return nullptr;
	}
	auto &render_node = *node_p;
	auto node = make_uniq<ExplainTreeNode>();
	node->name = render_node.name;
	for (auto &entry : render_node.extra_text) {
		auto &key = entry.first;
		auto &value = entry.second;
		if (value.empty()) {
			continue;
		}
		if (key == RenderTreeNode::CARDINALITY) {
			// a huge (e.g. overflowed cross-product) estimate can parse to the invalid sentinel - treat it as unknown
			auto parsed = idx_t(std::strtoull(value.c_str(), nullptr, 10));
			if (parsed != DConstants::INVALID_INDEX) {
				node->rows = parsed;
			}
			continue;
		}
		if (key == RenderTreeNode::ESTIMATED_CARDINALITY) {
			auto parsed = idx_t(std::strtoull(value.c_str(), nullptr, 10));
			if (parsed != DConstants::INVALID_INDEX) {
				node->estimated_rows = parsed;
			}
			continue;
		}
		if (key == RenderTreeNode::TIMING) {
			// stored as a formatted "<seconds>s" string - parse the leading number back to seconds
			node->timing = std::strtod(value.c_str(), nullptr);
			continue;
		}
		auto values = StringUtil::Split(value, "\n");
		if (values.empty()) {
			continue;
		}
		for (auto &v : values) {
			v = StripOuterParens(std::move(v));
		}
		// internal keys (e.g. __projections__) are cleaned up for display: __projections__ -> "Projections"
		string display_key = key;
		if (StringUtil::StartsWith(display_key, "__")) {
			display_key = StringUtil::Title(StringUtil::Replace(StringUtil::Replace(display_key, "__", ""), "_", " "));
		}
		node->details.emplace_back(std::move(display_key), std::move(values));
	}
	for (auto &child_pos : render_node.child_positions) {
		auto child = BuildExplainTree(tree, child_pos.x, child_pos.y);
		if (child) {
			node->children.push_back(std::move(child));
		}
	}
	return node;
}

static double SumOperatorTimings(const ExplainTreeNode &node) {
	double total = node.timing > 0 ? node.timing : 0;
	for (auto &child : node.children) {
		total += SumOperatorTimings(*child);
	}
	return total;
}

//===--------------------------------------------------------------------===//
// Formatting helpers
//===--------------------------------------------------------------------===//
static string FormatCount(idx_t count, char thousand_separator) {
	string result = std::to_string(count);
	if (!thousand_separator) {
		return result;
	}
	string formatted;
	idx_t digits_until_separator = result.size() % 3 == 0 ? 3 : result.size() % 3;
	for (idx_t i = 0; i < result.size(); i++) {
		if (digits_until_separator == 0) {
			formatted += thousand_separator;
			digits_until_separator = 3;
		}
		formatted += result[i];
		digits_until_separator--;
	}
	return formatted;
}

static string FormatTiming(double seconds) {
	if (seconds >= 1.0) {
		return StringUtil::Format("%.2fs", seconds);
	}
	if (seconds >= 0.001) {
		return StringUtil::Format("%.1fms", seconds * 1000.0);
	}
	return StringUtil::Format("%.0fµs", seconds * 1000000.0);
}

//! Heat-color the timing based on the share of the total query time spent in this operator
static ExplainSpan TimingSpan(double seconds, double total_time) {
	double fraction = total_time > 0 ? seconds / total_time : 0;
	auto text = FormatTiming(seconds);
	if (fraction >= 0.25) {
		return ExplainSpan(std::move(text), TreeRenderType::TIMING_CRITICAL);
	}
	if (fraction >= 0.10) {
		return ExplainSpan(std::move(text), TreeRenderType::TIMING_HIGH);
	}
	if (fraction >= 0.01) {
		return ExplainSpan(std::move(text), TreeRenderType::TIMING_MODERATE);
	}
	return ExplainSpan(std::move(text), TreeRenderType::TIMING_LOW);
}

static string TruncateText(const string &text, idx_t max_width) {
	if (RenderLength(text) <= max_width) {
		return text;
	}
	if (max_width <= 1) {
		return "…";
	}
	string result;
	idx_t width = 0;
	for (idx_t i = 0; i < text.size();) {
		idx_t next = i + 1;
		while (next < text.size() && !IsCharacter(text[next])) {
			next++;
		}
		auto char_width = RenderLength(text.c_str() + i, next - i);
		if (width + char_width > max_width - 1) {
			break;
		}
		result += text.substr(i, next - i);
		width += char_width;
		i = next;
	}
	return result + "…";
}

//! Truncate keeping the tail visible (e.g. a qualified name's final component - the table - is the most important).
static string TruncateHead(const string &text, idx_t max_width) {
	if (RenderLength(text) <= max_width) {
		return text;
	}
	if (max_width <= 1) {
		return "…";
	}
	// reserve one column for the leading ellipsis, then keep as many trailing characters as fit
	idx_t budget = max_width - 1;
	vector<idx_t> char_starts;
	for (idx_t i = 0; i < text.size(); i++) {
		if (IsCharacter(text[i])) {
			char_starts.push_back(i);
		}
	}
	char_starts.push_back(text.size());
	idx_t n = char_starts.size() - 1;
	idx_t width = 0;
	idx_t start = n;
	for (idx_t c = n; c-- > 0;) {
		auto char_width = RenderLength(text.c_str() + char_starts[c], char_starts[c + 1] - char_starts[c]);
		if (width + char_width > budget) {
			break;
		}
		width += char_width;
		start = c;
	}
	return "…" + text.substr(char_starts[start]);
}

static string CutToWidth(const string &text, idx_t width) {
	if (RenderLength(text) <= width) {
		return text;
	}
	string result;
	idx_t rendered = 0;
	for (idx_t i = 0; i < text.size();) {
		idx_t next = i + 1;
		while (next < text.size() && !IsCharacter(text[next])) {
			next++;
		}
		auto char_width = RenderLength(text.c_str() + i, next - i);
		if (rendered + char_width > width) {
			break;
		}
		result += text.substr(i, next - i);
		rendered += char_width;
		i = next;
	}
	return result;
}

//! Word-wrap a single string across lines of the given budget, breaking at spaces (no line limit).
static vector<string> WordWrap(const string &text, idx_t budget) {
	if (budget < 2) {
		budget = 2;
	}
	vector<string> lines;
	string line;
	idx_t line_width = 0;
	for (idx_t i = 0; i < text.size();) {
		idx_t j = i;
		while (j < text.size() && text[j] != ' ') {
			j++;
		}
		string word = text.substr(i, j - i);
		i = j < text.size() ? j + 1 : j;
		if (word.empty()) {
			continue;
		}
		if (RenderLength(word) > budget) {
			word = TruncateText(word, budget);
		}
		idx_t word_width = RenderLength(word);
		idx_t needed = word_width + (line.empty() ? 0 : 1);
		if (line_width + needed <= budget) {
			if (!line.empty()) {
				line += ' ';
				line_width++;
			}
			line += word;
			line_width += word_width;
		} else {
			if (!line.empty()) {
				lines.push_back(std::move(line));
			}
			line = std::move(word);
			line_width = word_width;
		}
	}
	if (!line.empty() || lines.empty()) {
		lines.push_back(std::move(line));
	}
	return lines;
}

//! Wrap a list of values across up to max_lines lines of the given budget.
static vector<string> WrapValues(const vector<string> &values, idx_t budget, idx_t max_lines) {
	if (budget < 2) {
		budget = 2;
	}
	vector<string> lines;
	string cur;
	idx_t cur_width = 0;
	for (idx_t k = 0; k < values.size(); k++) {
		string piece = k + 1 < values.size() ? values[k] + "," : values[k];
		idx_t piece_width = RenderLength(piece);
		idx_t sep = cur.empty() ? 0 : 1;
		if (cur_width + sep + piece_width <= budget) {
			cur = cur.empty() ? std::move(piece) : cur + " " + piece;
			cur_width += sep + piece_width;
			continue;
		}
		if (!cur.empty()) {
			lines.push_back(std::move(cur));
			cur.clear();
			cur_width = 0;
		}
		if (piece_width <= budget) {
			cur = std::move(piece);
			cur_width = piece_width;
		} else {
			auto sub = WordWrap(piece, budget);
			for (idx_t s = 0; s + 1 < sub.size(); s++) {
				lines.push_back(std::move(sub[s]));
			}
			cur = std::move(sub.back());
			cur_width = RenderLength(cur);
		}
	}
	if (!cur.empty() || lines.empty()) {
		lines.push_back(std::move(cur));
	}
	if (lines.size() > max_lines) {
		lines.resize(max_lines);
		auto &last = lines.back();
		if (RenderLength(last) + 2 > budget) {
			last = CutToWidth(last, budget >= 2 ? budget - 2 : 0);
		}
		last += last.empty() ? "…" : " …";
	}
	return lines;
}

//===--------------------------------------------------------------------===//
// Box layout
//===--------------------------------------------------------------------===//
//! How the query plan tree is laid out horizontally
enum class ExplainAlignment { LEFT, CENTER };

class ExplainBoxRenderer {
public:
	static constexpr idx_t HORIZONTAL_GAP = 2;
	static constexpr idx_t MAX_BOX_CONTENT_WIDTH = 48;
	static constexpr idx_t MAX_COMPACT_DETAIL = 40;
	static constexpr idx_t NARROW_MIN_WIDTH = 30;
	static constexpr idx_t MAX_DETAIL_LINES = 5;
	static constexpr idx_t MINIMUM_RENDER_WIDTH = 30;
	static constexpr idx_t METRICS_GAP = 3;
	static constexpr idx_t MAX_TREE_WIDTH = 400;
	static constexpr double FLATTEN_TIME_FRACTION = 0.01;
	static constexpr idx_t MIN_FLATTEN_NODES = 2;
	static constexpr idx_t MIN_GROUP_NODES = 2;
	static constexpr double SIGNIFICANT_TIME_FRACTION = 0.05;
	//! Connector weight is purely relative: an edge's tier is its share of the plan's peak row flow.
	//! (so 100k rows is a thick pipe in a small plan, but a thin one next to a 100M-row hot path)
	static constexpr double TIER1_FRACTION = 0.33; // heavy line
	static constexpr double TIER2_FRACTION = 0.66; // wide (2-column) pipe

	ExplainBoxRenderer(idx_t max_width, char thousand_separator, double total_time, ExplainAlignment alignment,
	                   bool flatten, bool merge, bool upper_case, bool expand_all)
	    : max_width(MaxValue<idx_t>(max_width, MINIMUM_RENDER_WIDTH)), thousand_separator(thousand_separator),
	      total_time(total_time), alignment(alignment),
	      layout_width(MaxValue<idx_t>(MaxValue<idx_t>(max_width, MINIMUM_RENDER_WIDTH), MAX_TREE_WIDTH)),
	      flatten(flatten), merge(merge), upper_case(upper_case), expand_all(expand_all),
	      flatten_threshold(total_time * FLATTEN_TIME_FRACTION),
	      significant_threshold(total_time * SIGNIFICANT_TIME_FRACTION) {
	}

	//! Max number of wrapped lines shown per detail value (unbounded when expanding, so nothing is hidden behind "…")
	idx_t MaxDetailLines() const {
		return expand_all ? NumericLimits<idx_t>::Maximum() : MAX_DETAIL_LINES;
	}
	//! Max box content width - uncapped (up to the layout width) when expanding so long values are not truncated
	idx_t MaxContentWidth() const {
		idx_t available = layout_width >= 4 ? layout_width - 4 : layout_width;
		return expand_all ? available : MinValue<idx_t>(available, MAX_BOX_CONTENT_WIDTH);
	}

	//! Whether the render condensed or merged any operators (i.e. a full render would show more)
	bool HidContent() const {
		return hid_content;
	}

	vector<ExplainLine> Render(const ExplainTreeNode &root) {
		max_edge_rows = MaxNodeRows(root);
		tree_content_width = MaxValue<idx_t>(TreeContentWidth(root), 6);
		auto block = RenderNode(root, ChainWidth(root));
		return std::move(block.lines);
	}

private:
	//! Connector thickness tier (0 = thin light line ... 3 = wide pipe) for an edge passing `rows` rows,
	//! taken purely as a fraction of the plan's peak row flow so it scales with the whole plan.
	idx_t EdgeTier(optional_idx rows) const {
		if (!rows.IsValid() || max_edge_rows == 0) {
			return 0;
		}
		double fraction = static_cast<double>(rows.GetIndex()) / static_cast<double>(max_edge_rows);
		if (fraction >= TIER2_FRACTION) {
			return 2;
		}
		if (fraction >= TIER1_FRACTION) {
			return 1;
		}
		return 0;
	}
	//! The number of columns a tier's connector occupies (the heaviest edges widen to a 2-column pipe).
	//! Weight is expressed purely through width/glyph - connectors are never made longer.
	static idx_t TierWidth(idx_t tier) {
		return tier >= 2 ? 2 : 1;
	}
	idx_t EdgeTierAtSpine(const vector<ExplainBlock> &children, const vector<idx_t> &spines, idx_t col) const {
		for (idx_t i = 0; i < children.size() && i < spines.size(); i++) {
			if (spines[i] == col) {
				return EdgeTier(children[i].output_rows);
			}
		}
		return 0;
	}

	//! Stamp a connector cap (and its second column, for a wide pipe) onto a box border line at `col`.
	static void DrawCap(ExplainLine &border, idx_t col, idx_t tier, bool top) {
		if (RenderCharEquals(border, col, "─")) {
			SetLayoutChar(border, col, TierCap(tier, top, false));
		}
		if (TierWidth(tier) == 2 && RenderCharEquals(border, col + 1, "─")) {
			SetLayoutChar(border, col + 1, TierCap(tier, top, true));
		}
	}

	static idx_t LongestValue(const vector<string> &values) {
		idx_t longest = 0;
		for (auto &value : values) {
			longest = MaxValue<idx_t>(longest, RenderLength(value));
		}
		return longest;
	}

	static bool DetailIsNarrow(const std::pair<string, vector<string>> &entry) {
		if (entry.first == "Text") {
			return false;
		}
		// the table name is always rendered inline ("Table: ...") so the qualified name stays identifiable
		if (entry.first == "Table") {
			return false;
		}
		idx_t compact = RenderLength(entry.first) + 2 + LongestValue(entry.second);
		return compact > MAX_COMPACT_DETAIL;
	}

	static idx_t DetailWidth(const std::pair<string, vector<string>> &entry, idx_t max_content) {
		idx_t longest = LongestValue(entry.second) + (entry.second.size() > 1 ? 1 : 0);
		if (!DetailIsNarrow(entry)) {
			idx_t key = entry.first == "Text" ? 0 : RenderLength(entry.first) + 2;
			return key + longest;
		}
		idx_t narrow = MaxValue<idx_t>(RenderLength(entry.first), MinValue<idx_t>(longest, max_content));
		return MaxValue<idx_t>(narrow, NARROW_MIN_WIDTH);
	}

	bool IsCondensed(const ExplainTreeNode &node) const {
		return flatten && node.timing >= 0 && node.timing < significant_threshold;
	}

	static bool IsEssentialDetail(const std::pair<string, vector<string>> &entry) {
		return entry.first == "Table";
	}

	idx_t BoxContentWidth(const ExplainTreeNode &node) {
		idx_t max_content = MaxContentWidth();
		bool condensed = IsCondensed(node);
		// reserve 2 extra columns so the operator name fits in the box border without being truncated
		idx_t width = RenderLength(DisplayName(node.name, upper_case)) + 2;
		for (auto &entry : node.details) {
			if (condensed && !IsEssentialDetail(entry)) {
				continue;
			}
			width = MaxValue<idx_t>(width, DetailWidth(entry, max_content));
		}
		width = MaxValue<idx_t>(width, MetricsWidth(node));
		return MaxValue<idx_t>(MinValue<idx_t>(width, max_content), 6);
	}

	idx_t TreeContentWidth(const ExplainTreeNode &node) {
		idx_t width = BoxContentWidth(node);
		for (auto &child : node.children) {
			width = MaxValue<idx_t>(width, TreeContentWidth(*child));
		}
		return width;
	}

	idx_t ChainWidth(const ExplainTreeNode &node) {
		idx_t width = 6;
		const ExplainTreeNode *current = &node;
		while (true) {
			if (IsCollapsible(*current)) {
				width = MaxValue<idx_t>(width, SubtreeGroupedWidth(*current));
				break;
			}
			if (IsGroupable(*current)) {
				width = MaxValue<idx_t>(width, GroupedRowWidth(*current));
			} else {
				width = MaxValue<idx_t>(width, BoxContentWidth(*current));
			}
			if (current->children.empty()) {
				break;
			}
			current = current->children[0].get();
		}
		return width;
	}

	idx_t EffectiveWidth(idx_t chain_width) const {
		return alignment == ExplainAlignment::LEFT ? chain_width : tree_content_width;
	}

	bool IsCollapsible(const ExplainTreeNode &node) const {
		return merge && node.subtree_count >= MIN_FLATTEN_NODES && node.subtree_time < flatten_threshold;
	}

	bool IsGroupable(const ExplainTreeNode &node) const {
		return IsCondensed(node) && node.children.size() == 1 && !IsCollapsible(node);
	}

	idx_t GroupRunLength(const ExplainTreeNode &node) const {
		idx_t count = 0;
		const ExplainTreeNode *current = &node;
		while (IsGroupable(*current)) {
			count++;
			current = current->children[0].get();
		}
		return count;
	}

	const ExplainTreeNode &GroupRunAfter(const ExplainTreeNode &node, idx_t count) const {
		const ExplainTreeNode *current = &node;
		for (idx_t i = 0; i < count; i++) {
			current = current->children[0].get();
		}
		return *current;
	}

	ExplainLine GroupedMetrics(const ExplainTreeNode &node) {
		ExplainLine metrics;
		if (node.rows.IsValid()) {
			auto rows = node.rows.GetIndex();
			metrics.emplace_back(FormatCount(rows, thousand_separator) + (rows == 1 ? " row" : " rows"),
			                     TreeRenderType::ROWS);
		} else if (node.estimated_rows.IsValid()) {
			auto estimate = node.estimated_rows.GetIndex();
			metrics.emplace_back("~" + FormatCount(estimate, thousand_separator) + (estimate == 1 ? " row" : " rows"),
			                     TreeRenderType::KEY);
		}
		if (node.timing >= 0) {
			if (!metrics.empty()) {
				metrics.emplace_back(" · ", TreeRenderType::KEY);
			}
			metrics.push_back(TimingSpan(node.timing, total_time));
		}
		return metrics;
	}

	string GroupedRowName(const ExplainTreeNode &node) {
		string name = DisplayName(node.name, upper_case);
		for (auto &entry : node.details) {
			if (entry.first == "Table" && !entry.second.empty()) {
				const string &table = entry.second.front();
				auto pos = table.find_last_of('.');
				return name + " " + (pos == string::npos ? table : table.substr(pos + 1));
			}
		}
		return name;
	}

	idx_t GroupedRowWidth(const ExplainTreeNode &node) {
		return RenderLength(GroupedRowName(node)) + METRICS_GAP + LineWidth(GroupedMetrics(node));
	}

	idx_t SubtreeGroupedWidth(const ExplainTreeNode &node) {
		idx_t width = GroupedRowWidth(node);
		for (auto &child : node.children) {
			width = MaxValue<idx_t>(width, SubtreeGroupedWidth(*child));
		}
		return width;
	}

	struct BoxMetrics {
		ExplainLine left;
		ExplainLine right;
		bool Empty() const {
			return left.empty() && right.empty();
		}
	};

	BoxMetrics BuildMetrics(const ExplainTreeNode &node) {
		BoxMetrics metrics;
		if (node.rows.IsValid()) {
			auto rows = node.rows.GetIndex();
			metrics.left.emplace_back(FormatCount(rows, thousand_separator) + (rows == 1 ? " row" : " rows"),
			                          TreeRenderType::ROWS);
		} else if (node.estimated_rows.IsValid()) {
			auto estimate = node.estimated_rows.GetIndex();
			metrics.left.emplace_back("~" + FormatCount(estimate, thousand_separator) +
			                              (estimate == 1 ? " row" : " rows"),
			                          TreeRenderType::KEY);
		}
		if (node.timing >= 0) {
			metrics.right.push_back(TimingSpan(node.timing, total_time));
		}
		return metrics;
	}

	idx_t MetricsWidth(const ExplainTreeNode &node) {
		auto metrics = BuildMetrics(node);
		idx_t left_width = LineWidth(metrics.left);
		idx_t right_width = LineWidth(metrics.right);
		if (left_width > 0 && right_width > 0) {
			return left_width + METRICS_GAP + right_width;
		}
		return MaxValue<idx_t>(left_width, right_width);
	}

	ExplainBlock BuildBox(const ExplainTreeNode &node, idx_t chain_width) {
		idx_t content_width = EffectiveWidth(chain_width);

		vector<ExplainLine> content;
		bool condensed = IsCondensed(node);
		for (auto &entry : node.details) {
			if (condensed && !IsEssentialDetail(entry)) {
				hid_content = true;
				continue;
			}
			bool has_key = entry.first != "Text";
			if (entry.first == "Table" && !entry.second.empty()) {
				// render the table name inline on a single line, truncating the (less important) catalog/schema
				// prefix rather than the table name itself when the qualified name does not fit
				string key = "Table: ";
				idx_t key_width = RenderLength(key);
				ExplainLine line;
				line.emplace_back(key, TreeRenderType::KEY);
				line.emplace_back(TruncateHead(entry.second.front(), content_width - key_width), TreeRenderType::VALUE);
				content.push_back(std::move(line));
				continue;
			}
			if (has_key && DetailIsNarrow(entry)) {
				content.emplace_back();
				// keep the trailing colon so a narrow detail reads the same as an inline "Key: value" detail
				content.back().emplace_back(TruncateText(entry.first + ":", content_width), TreeRenderType::KEY);
				for (auto &value_line : WrapValues(entry.second, content_width, MaxDetailLines())) {
					content.emplace_back();
					content.back().emplace_back(std::move(value_line), TreeRenderType::VALUE);
				}
				continue;
			}
			string key = has_key ? entry.first + ": " : string();
			idx_t key_width = RenderLength(key);
			if (has_key && key_width >= content_width) {
				key = TruncateText(key, content_width >= 1 ? content_width - 1 : 1);
				key_width = RenderLength(key);
			}
			auto value_lines = WrapValues(entry.second, content_width - key_width, MaxDetailLines());
			for (idx_t li = 0; li < value_lines.size(); li++) {
				ExplainLine line;
				if (li == 0 && has_key) {
					line.emplace_back(key, TreeRenderType::KEY);
				} else if (has_key) {
					line.emplace_back(string(key_width, ' '), TreeRenderType::LAYOUT);
				}
				line.emplace_back(std::move(value_lines[li]), TreeRenderType::VALUE);
				content.push_back(std::move(line));
			}
		}
		auto metrics = BuildMetrics(node);
		if (!metrics.Empty()) {
			idx_t left_width = LineWidth(metrics.left);
			idx_t right_width = LineWidth(metrics.right);
			ExplainLine line;
			for (auto &span : metrics.left) {
				line.push_back(std::move(span));
			}
			if (right_width > 0) {
				idx_t gap = content_width > left_width + right_width ? content_width - left_width - right_width : 1;
				line.emplace_back(string(gap, ' '), TreeRenderType::LAYOUT);
				for (auto &span : metrics.right) {
					line.push_back(std::move(span));
				}
			}
			content.push_back(std::move(line));
		}

		string title = TruncateText(DisplayName(node.name, upper_case), content_width >= 2 ? content_width - 2 : 1);
		idx_t title_width = RenderLength(title);
		idx_t box_width = content_width + 4;

		ExplainBlock block;
		ExplainLine top;
		top.emplace_back("╭─ ", TreeRenderType::LAYOUT);
		top.emplace_back(std::move(title), GetOperatorType(node));
		top.emplace_back(" " + StringUtil::Repeat("─", box_width - 5 - title_width) + "╮", TreeRenderType::LAYOUT);
		block.lines.push_back(std::move(top));
		for (auto &line : content) {
			ExplainLine content_line;
			content_line.emplace_back("│ ", TreeRenderType::LAYOUT);
			idx_t line_width = LineWidth(line);
			for (auto &span : line) {
				content_line.push_back(std::move(span));
			}
			idx_t padding = content_width >= line_width ? content_width - line_width : 0;
			content_line.emplace_back(string(padding + 1, ' ') + "│", TreeRenderType::LAYOUT);
			block.lines.push_back(std::move(content_line));
		}
		ExplainLine bottom;
		bottom.emplace_back("╰" + StringUtil::Repeat("─", box_width - 2) + "╯", TreeRenderType::LAYOUT);
		block.lines.push_back(std::move(bottom));
		block.width = box_width;
		block.spine = box_width / 2;
		return block;
	}

	void AddGroupedRow(ExplainBlock &block, const string &label, TreeRenderType label_type, ExplainLine metrics,
	                   idx_t content_width) {
		string text = TruncateText(label, content_width);
		idx_t name_width = RenderLength(text);
		idx_t metrics_width = LineWidth(metrics);
		ExplainLine line;
		line.emplace_back("│ ", TreeRenderType::LAYOUT);
		line.emplace_back(std::move(text), label_type);
		idx_t used = name_width + metrics_width;
		idx_t gap = content_width > used ? content_width - used : 1;
		line.emplace_back(string(gap, ' '), TreeRenderType::LAYOUT);
		for (auto &span : metrics) {
			line.push_back(std::move(span));
		}
		idx_t line_content = name_width + gap + metrics_width;
		idx_t padding = content_width >= line_content ? content_width - line_content : 0;
		line.emplace_back(string(padding + 1, ' ') + "│", TreeRenderType::LAYOUT);
		block.lines.push_back(std::move(line));
	}

	ExplainBlock BuildGroupedBox(const vector<const ExplainTreeNode *> &nodes, idx_t chain_width) {
		idx_t content_width = EffectiveWidth(chain_width);
		idx_t box_width = content_width + 4;

		ExplainBlock block;
		block.lines.emplace_back();
		block.lines.back().emplace_back("╭" + StringUtil::Repeat("─", box_width - 2) + "╮", TreeRenderType::LAYOUT);

		for (idx_t i = 0; i < nodes.size(); i++) {
			AddGroupedRow(block, GroupedRowName(*nodes[i]), GetOperatorType(*nodes[i]), GroupedMetrics(*nodes[i]),
			              content_width);
		}

		block.lines.emplace_back();
		block.lines.back().emplace_back("╰" + StringUtil::Repeat("─", box_width - 2) + "╯", TreeRenderType::LAYOUT);
		block.width = box_width;
		block.spine = box_width / 2;
		return block;
	}

	vector<const ExplainTreeNode *> GroupRunNodes(const ExplainTreeNode &node, idx_t count) {
		vector<const ExplainTreeNode *> nodes;
		const ExplainTreeNode *current = &node;
		for (idx_t i = 0; i < count; i++) {
			nodes.push_back(current);
			current = current->children[0].get();
		}
		return nodes;
	}

	void CollectSubtreeNodes(const ExplainTreeNode &node, vector<const ExplainTreeNode *> &nodes) {
		nodes.push_back(&node);
		for (auto &child : node.children) {
			CollectSubtreeNodes(*child, nodes);
		}
	}

	ExplainBlock StackChain(ExplainBlock box, ExplainBlock child) {
		idx_t target = MaxValue<idx_t>(box.spine, child.spine);
		idx_t box_offset = target - box.spine;
		idx_t child_offset = target - child.spine;
		idx_t tier = EdgeTier(child.output_rows);
		// both borders are free here (no horizontal bus), so the cap glyphs can fully reflect the tier
		DrawCap(box.lines.back(), box.spine, tier, true);
		DrawCap(child.lines.front(), child.spine, tier, false);

		ExplainBlock result;
		for (auto &line : box.lines) {
			OffsetLine(line, box_offset);
			result.lines.push_back(std::move(line));
		}
		for (auto &line : child.lines) {
			OffsetLine(line, child_offset);
			result.lines.push_back(std::move(line));
		}
		result.width = MaxValue<idx_t>(box_offset + box.width, child_offset + child.width);
		result.spine = target;
		return result;
	}

	ExplainBlock AttachHorizontal(ExplainBlock box, vector<ExplainBlock> &children) {
		if (alignment == ExplainAlignment::LEFT) {
			return AttachHorizontalLeft(std::move(box), children);
		}
		return AttachHorizontalCentered(std::move(box), children);
	}

	void LayoutRow(vector<ExplainBlock> &children, vector<ExplainLine> &row_lines, vector<idx_t> &spines,
	               idx_t &row_width) {
		bool compact = alignment == ExplainAlignment::LEFT;
		vector<idx_t> offsets(children.size(), 0);
		vector<idx_t> profile;
		idx_t cumulative = 0;
		for (idx_t i = 0; i < children.size(); i++) {
			auto &lines = children[i].lines;
			idx_t offset = cumulative;
			if (compact) {
				offset = 0;
				for (idx_t r = 0; r < lines.size() && r < profile.size(); r++) {
					if (profile[r] == 0) {
						continue;
					}
					idx_t need = profile[r] + HORIZONTAL_GAP;
					idx_t left = LeadingSpaces(lines[r]);
					if (need > left) {
						offset = MaxValue<idx_t>(offset, need - left);
					}
				}
			}
			offsets[i] = offset;
			spines.push_back(offset + children[i].spine);
			for (idx_t r = 0; r < lines.size(); r++) {
				if (r >= profile.size()) {
					profile.resize(r + 1, 0);
				}
				profile[r] = MaxValue<idx_t>(profile[r], offset + LineWidth(lines[r]));
			}
			cumulative = offset + children[i].width + HORIZONTAL_GAP;
		}
		row_width = 0;
		for (auto width : profile) {
			row_width = MaxValue<idx_t>(row_width, width);
		}
		for (idx_t line_idx = 0; line_idx < profile.size(); line_idx++) {
			ExplainLine line;
			for (idx_t i = 0; i < children.size(); i++) {
				if (line_idx < children[i].lines.size()) {
					auto &child_line = children[i].lines[line_idx];
					AppendAt(line, child_line, offsets[i] + LeadingSpaces(child_line));
				}
			}
			row_lines.push_back(std::move(line));
		}
	}

	ExplainBlock AttachHorizontalLeft(ExplainBlock box, vector<ExplainBlock> &children) {
		vector<ExplainLine> row_lines;
		vector<idx_t> spines;
		idx_t row_width = 0;
		LayoutRow(children, row_lines, spines, row_width);

		// the parent connects to the bus over its first child (left-aligned); widen that junction to ┰┰ when the
		// first child is a wide pipe so both of its walls rise into the parent
		idx_t front = spines.front();
		bool wide_first = TierWidth(EdgeTier(children.front().output_rows)) == 2 && box.spine == front;
		if (wide_first) {
			SetLayoutChar(box.lines.back(), box.spine, "┰");
			if (RenderCharEquals(box.lines.back(), box.spine + 1, "─")) {
				SetLayoutChar(box.lines.back(), box.spine + 1, "┰");
			}
		} else {
			SetLayoutChar(box.lines.back(), box.spine, "┬");
		}

		// each child's top cap (┴ / ┸ / ┚┖) on the merged children-top border
		idx_t bus_end = spines.back();
		for (idx_t i = 0; i < children.size(); i++) {
			idx_t tier = EdgeTier(children[i].output_rows);
			DrawCap(row_lines.front(), spines[i], tier, false);
			bus_end = MaxValue<idx_t>(bus_end, spines[i] + TierWidth(tier) - 1);
		}

		// build the bus: light children branch (├/┬/╮), heavy ones drop a heavy stem (┟/┰/┒), and the widest drop a
		// 2-column pipe whose walls carry the arms so the bus and parent still connect (┃┠ under the parent, else ┒┎)
		vector<string> bus(bus_end - front + 1, "─");
		for (idx_t i = 0; i < children.size(); i++) {
			idx_t s = spines[i];
			idx_t tier = EdgeTier(children[i].output_rows);
			bool first = i == 0;
			bool last = i + 1 == children.size();
			if (TierWidth(tier) == 2) {
				if (first && wide_first) {
					bus[s - front] = "┃";     // left wall rises to the parent (┰ above)
					bus[s + 1 - front] = "┠"; // right wall rises to the parent and carries the bus onward
				} else if (last) {
					// the bus arrives from the left and ends here: cap the pipe with a light lid (┰─┒) so the right
					// wall does not dangle past the end of the bus
					bus[s - front] = "┰";
					bus[s + 1 - front] = "┒";
				} else {
					// a middle child: the bus feeds each wall from its own side
					bus[s - front] = "┒";
					bus[s + 1 - front] = "┎";
				}
				continue;
			}
			const char *base = first ? "├" : (last ? "╮" : "┬");
			bus[s - front] = tier >= 1 ? ThickDownVariant(base) : base;
		}
		string routing;
		for (auto &cell : bus) {
			routing += cell;
		}
		ExplainLine routing_line;
		routing_line.emplace_back(std::move(routing), TreeRenderType::LAYOUT);
		OffsetLine(routing_line, front);

		ExplainBlock result;
		for (auto &line : box.lines) {
			result.lines.push_back(std::move(line));
		}
		result.lines.push_back(std::move(routing_line));
		for (auto &line : row_lines) {
			result.lines.push_back(std::move(line));
		}
		result.width = MaxValue<idx_t>(MaxValue<idx_t>(box.width, row_width), bus_end + 1);
		result.spine = box.spine;
		return result;
	}

	ExplainBlock AttachHorizontalCentered(ExplainBlock box, vector<ExplainBlock> &children) {
		vector<ExplainLine> row_lines;
		vector<idx_t> spines;
		idx_t row_width = 0;
		LayoutRow(children, row_lines, spines, row_width);

		idx_t parent_spine = (spines.front() + spines.back()) / 2;
		idx_t box_offset = 0;
		idx_t row_offset = 0;
		if (parent_spine > box.spine) {
			box_offset = parent_spine - box.spine;
		} else {
			row_offset = box.spine - parent_spine;
			for (auto &spine : spines) {
				spine += row_offset;
			}
			parent_spine = box.spine;
		}

		bool direct = true;
		for (auto spine : spines) {
			if (spine <= box_offset || spine >= box_offset + box.width - 1) {
				direct = false;
				break;
			}
		}
		ExplainBlock result;
		if (direct) {
			for (idx_t i = 0; i < spines.size(); i++) {
				DrawCap(box.lines.back(), spines[i] - box_offset, EdgeTier(children[i].output_rows), true);
			}
		} else {
			SetLayoutChar(box.lines.back(), box.spine, "┬");
		}
		for (auto &line : box.lines) {
			OffsetLine(line, box_offset);
			result.lines.push_back(std::move(line));
		}
		if (!direct) {
			string routing;
			for (idx_t col = spines.front(); col <= spines.back(); col++) {
				const char *c = "─";
				bool is_child_spine = std::find(spines.begin(), spines.end(), col) != spines.end();
				if (col == spines.front()) {
					c = is_child_spine && col == parent_spine ? "├" : "╭";
				} else if (col == spines.back()) {
					c = is_child_spine && col == parent_spine ? "┤" : "╮";
				} else if (is_child_spine) {
					c = col == parent_spine ? "┼" : "┬";
				} else if (col == parent_spine) {
					c = "┴";
				}
				// thicken the drop into a child that passes a large share of the plan's rows
				if (EdgeTierAtSpine(children, spines, col) >= 1) {
					c = ThickDownVariant(c);
				}
				routing += c;
			}
			ExplainLine routing_line;
			routing_line.emplace_back(std::move(routing), TreeRenderType::LAYOUT);
			OffsetLine(routing_line, spines.front());
			result.lines.push_back(std::move(routing_line));
		}
		for (auto &line : row_lines) {
			OffsetLine(line, row_offset);
			result.lines.push_back(std::move(line));
		}
		result.width = MaxValue<idx_t>(box_offset + box.width, row_offset + row_width);
		result.spine = box_offset + box.spine;
		return result;
	}

	ExplainBlock RenderNode(const ExplainTreeNode &node, idx_t chain_width) {
		auto block = RenderNodeInternal(node, chain_width);
		// record what this block passes up so its parent can scale the connecting line to the row count
		block.output_rows = NodeRows(node);
		return block;
	}

	ExplainBlock RenderNodeInternal(const ExplainTreeNode &node, idx_t chain_width) {
		if (IsCollapsible(node)) {
			hid_content = true;
			vector<const ExplainTreeNode *> nodes;
			CollectSubtreeNodes(node, nodes);
			return BuildGroupedBox(nodes, chain_width);
		}
		if (node.children.size() == 1) {
			idx_t run = GroupRunLength(node);
			if (run >= MIN_GROUP_NODES) {
				hid_content = true;
				auto &after = GroupRunAfter(node, run);
				return StackChain(BuildGroupedBox(GroupRunNodes(node, run), chain_width),
				                  RenderNode(after, chain_width));
			}
			return StackChain(BuildBox(node, chain_width), RenderNode(*node.children[0], chain_width));
		}
		auto box = BuildBox(node, chain_width);
		if (node.children.empty()) {
			return box;
		}
		vector<ExplainBlock> children;
		for (idx_t i = 0; i < node.children.size(); i++) {
			idx_t child_chain = i == 0 ? chain_width : ChainWidth(*node.children[i]);
			children.push_back(RenderNode(*node.children[i], child_chain));
		}
		return AttachHorizontal(std::move(box), children);
	}

private:
	idx_t max_width;
	char thousand_separator;
	double total_time;
	ExplainAlignment alignment;
	idx_t layout_width;
	bool flatten;
	bool merge;
	bool upper_case;
	bool expand_all;
	double flatten_threshold;
	double significant_threshold;
	idx_t tree_content_width = 0;
	bool hid_content = false;
	//! The plan's peak operator row count, used to scale connector line weights
	idx_t max_edge_rows = 0;
};

//! Several operators are only merged into one node for plans with at least this many operators.
static constexpr idx_t MERGE_MIN_NODES = 30;

} // namespace

string TextTreeRenderer::ToString(const LogicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string TextTreeRenderer::ToString(const PhysicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string TextTreeRenderer::ToString(const ProfilingNode &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string TextTreeRenderer::ToString(const Pipeline &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

void TextTreeRenderer::Render(const LogicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void TextTreeRenderer::Render(const PhysicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void TextTreeRenderer::Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void TextTreeRenderer::Render(const Pipeline &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void TextTreeRenderer::ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) {
	auto plan = BuildExplainTree(root, 0, 0);
	// skip the EXPLAIN_ANALYZE / RESULT_COLLECTOR wrapper operators that wrap an EXPLAIN ANALYZE plan
	while (plan && (plan->name == "EXPLAIN_ANALYZE" || plan->name == "RESULT_COLLECTOR") &&
	       plan->children.size() == 1) {
		plan = std::move(plan->children[0]);
	}
	if (!plan) {
		return;
	}
	ComputeSubtreeStats(*plan);
	double total_time = SumOperatorTimings(*plan);
	// condense low-impact operators when we have timing (EXPLAIN ANALYZE); merge sub-trees only for large plans
	// (unless expand_all is set, in which case every operator is rendered as a full box)
	bool flatten = !config.expand_all && total_time > 0;
	bool merge = flatten && plan->subtree_count >= MERGE_MIN_NODES;
	ExplainBoxRenderer renderer(config.maximum_render_width, config.thousand_separator, total_time,
	                            ExplainAlignment::LEFT, flatten, merge, config.upper_case_operators, config.expand_all);
	auto lines = renderer.Render(*plan);
	if (renderer.HidContent()) {
		ss.hidden_content = true;
	}
	idx_t max_width = 0;
	for (auto &line : lines) {
		max_width = MaxValue<idx_t>(max_width, LineWidth(line));
		for (auto &span : line) {
			ss.Render(span.text, span.type);
		}
		ss << "\n";
	}
	ss.max_render_width = MaxValue<idx_t>(ss.max_render_width, max_width);
}

void TextTreeRenderer::Configure(const unordered_map<string, Value> &settings) {
	for (auto &entry : settings) {
		auto name = StringUtil::Lower(entry.first);
		auto &value = entry.second;
		if (name == "max_extra_lines") {
			config.max_extra_lines = value.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
		} else if (name == "thousand_separator") {
			auto separator = value.ToString();
			if (separator.size() > 1) {
				throw InvalidInputException("Renderer setting \"%s\" must be a single character, got \"%s\"", name,
				                            separator);
			}
			config.thousand_separator = separator.empty() ? '\0' : separator[0];
		} else if (name == "decimal_separator") {
			auto separator = value.ToString();
			if (separator.size() != 1) {
				throw InvalidInputException("Renderer setting \"%s\" must be a single character, got \"%s\"", name,
				                            separator);
			}
			config.decimal_separator = separator[0];
		} else if (name == "expand_all") {
			config.expand_all = value.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
		} else if (name == "operator_casing") {
			auto casing = StringUtil::Lower(value.ToString());
			if (casing == "upper") {
				config.upper_case_operators = true;
			} else if (casing == "title") {
				config.upper_case_operators = false;
			} else {
				throw InvalidInputException("Renderer setting \"%s\" must be either 'title' or 'upper', got \"%s\"",
				                            name, casing);
			}
		}
		// unrecognized settings are ignored - they may be intended for a different renderer
	}
}

void TextTreeRenderer::RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) {
	// the text profiler output is the framed query tree (header, total time, phase timings, operator tree)
	profiler.RenderQueryTree(ss);
}

} // namespace duckdb
