#include "duckdb.hpp"
#include "shell_state.hpp"
#include "shell_highlight.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/common/box_renderer.hpp"
#include "duckdb/common/map.hpp"

namespace duckdb_shell {

bool ShellState::UseDescribeRenderMode(const duckdb::SQLStatement &statement, string &describe_table_name) {
	// identify if this is a DESCRIBE {table} or DESCRIBE {query} statement
	// if it is we use the special DESCRIBE render mode
	if (statement.type != duckdb::StatementType::SELECT_STATEMENT) {
		return false;
	}
	auto &select = statement.Cast<duckdb::SelectStatement>();
	if (select.node->type != duckdb::QueryNodeType::SELECT_NODE) {
		return false;
	}
	auto &select_node = select.node->Cast<duckdb::SelectNode>();
	if (select_node.select_list.size() != 1 ||
	    select_node.select_list[0]->GetExpressionType() != duckdb::ExpressionType::STAR) {
		return false;
	}
	if (select_node.from_table->type != duckdb::TableReferenceType::SHOW_REF) {
		return false;
	}
	auto &showref = select_node.from_table->Cast<duckdb::ShowRef>();
	// DESCRIBE always uses the compact describe rendering. Using SHOW to describe something - bareword "SHOW name" or
	// "SHOW (query)" - is deprecated, but while it is still supported we render it the same way for consistency with
	// DESCRIBE. Note that a bareword "SHOW name" may instead resolve to a setting value at execution time; that is a
	// regular (non-describe) result, which ExecuteStatement detects from the result shape and renders with the default
	// mode.
	bool is_show = showref.show_type == duckdb::ShowType::SHOW && (!showref.GetTableName().empty() || showref.query);
	if (showref.show_type != duckdb::ShowType::DESCRIBE && !is_show) {
		return false;
	}
	describe_table_name = "Describe";
	if (!showref.GetTableName().empty()) {
		describe_table_name = showref.GetTableName().GetIdentifierName();
	} else if (showref.query && showref.query->type == duckdb::QueryNodeType::SELECT_NODE) {
		auto &show_select = showref.query->Cast<duckdb::SelectNode>();
		if (show_select.from_table->type == duckdb::TableReferenceType::BASE_TABLE) {
			auto &base_table = show_select.from_table->Cast<duckdb::BaseTableRef>();
			describe_table_name = base_table.Table().GetIdentifierName();
		}
	}
	return true;
}

struct RenderComponent {
	RenderComponent(string text_p, HighlightElementType type) : text(std::move(text_p)), type(type) {
		render_width = ShellState::RenderLength(text);
	}

	string text;
	idx_t render_width;
	HighlightElementType type;
};

struct ShellColumnRenderInfo {
	vector<RenderComponent> components;
};

struct ColumnRenderRow {
	vector<ShellColumnRenderInfo> columns;
};

struct ShellTableRenderInfo {
	ShellTableRenderInfo(ShellTableInfo table, char decimal_sep);

	ShellTableInfo table;
	idx_t table_name_length;
	idx_t render_width;
	vector<idx_t> max_component_widths;
	string estimated_size_text;
	optional_idx estimated_size_length;
	vector<ColumnRenderRow> column_renders;

	idx_t LineCount() const {
		idx_t constant_count = 4;
		if (estimated_size_length.IsValid()) {
			constant_count += 2;
		}
		return ColumnLines() + constant_count;
	}
	idx_t PerColumnWidth() const {
		idx_t width = 4;
		for (auto &component_width : max_component_widths) {
			width += component_width;
		}
		width += max_component_widths.size() - 1;
		return width;
	}

	idx_t ColumnLines() const {
		return column_renders[0].columns.size();
	}

	void Truncate(idx_t max_render_width);
	static void TruncateValueIfRequired(string &value, idx_t &render_length, idx_t max_render_width);
};

struct TableMetadataLine {
	idx_t render_height = 0;
	idx_t render_width = 0;
	vector<idx_t> max_component_widths;
	vector<idx_t> tables;

	idx_t PerColumnWidth() const {
		idx_t width = 4;
		for (auto &component_width : max_component_widths) {
			width += component_width;
		}
		width += max_component_widths.size() - 1;
		return width;
	}
	void RenderLine(ShellHighlight &highlight, const vector<ShellTableRenderInfo> &tables, idx_t line_idx,
	                bool last_line);
};

struct TableMetadataDisplayInfo {
	string database_name;
	string schema_name;
	idx_t render_height = 0;
	idx_t render_width = 0;
	vector<TableMetadataLine> display_lines;
};

void TableMetadataLine::RenderLine(ShellHighlight &highlight, const vector<ShellTableRenderInfo> &table_list,
                                   idx_t line_idx, bool last_line) {
	// figure out the table to render
	idx_t table_idx = 0;
	for (; table_idx < tables.size(); table_idx++) {
		idx_t line_count = table_list[tables[table_idx]].LineCount();
		if (line_idx < line_count) {
			// line belongs to this table - render it
			break;
		}
		// line belongs to a subsequent table - subtract line count and move to next table
		line_idx -= line_count;
	}
	if (table_idx == tables.size()) {
		// we didn't render any table - break
		if (!last_line) {
			// we need to add spaces if this is not the last line
			highlight.PrintText(string(render_width, ' '), PrintOutput::STDOUT, HighlightElementType::LAYOUT);
		}
		return;
	}
	auto &render_table = table_list[tables[table_idx]];
	auto &table = render_table.table;
	auto layout_type = table.is_view ? HighlightElementType::VIEW_LAYOUT : HighlightElementType::TABLE_LAYOUT;
	duckdb::BoxRendererConfig config;
	if (line_idx == 0) {
		// first line - render layout
		string top_line;
		top_line = config.LTCORNER;
		for (idx_t i = 0; i < render_width - 2; i++) {
			top_line += config.HORIZONTAL;
		}
		top_line += config.RTCORNER;
		highlight.PrintText(top_line, PrintOutput::STDOUT, layout_type);
		return;
	}
	if (line_idx == 1) {
		// table name
		string table_name;
		idx_t space_count = render_width - render_table.table_name_length - 2;
		idx_t lspace = space_count / 2;
		idx_t rspace = space_count - lspace;
		string table_line = config.VERTICAL;
		table_line += string(lspace, ' ');
		highlight.PrintText(table_line, PrintOutput::STDOUT, layout_type);
		highlight.PrintText(table.table_name, PrintOutput::STDOUT, HighlightElementType::TABLE_NAME);
		table_line = string(rspace, ' ');
		table_line += config.VERTICAL;
		highlight.PrintText(table_line, PrintOutput::STDOUT, layout_type);
		return;
	}
	if (line_idx > 2 && line_idx < render_table.ColumnLines() + 3) {
		idx_t column_idx = line_idx - 3;
		// column line

		// if we have a long table name the table name might have stretched out the box
		// in that case we need to add padding here
		// compute the required padding
		idx_t total_render_width = render_width;
		idx_t per_column_render_width = PerColumnWidth();
		idx_t column_render_width = render_table.column_renders.size() * per_column_render_width;
		idx_t extra_render_width = total_render_width - column_render_width;
		idx_t render_width_per_column = extra_render_width / render_table.column_renders.size();

		for (idx_t render_idx = 0; render_idx < render_table.column_renders.size(); render_idx++) {
			auto &column_render = render_table.column_renders[render_idx];
			string column_line = render_idx == 0 ? config.VERTICAL : " ";
			bool is_last = render_idx + 1 == render_table.column_renders.size();
			if (column_idx < column_render.columns.size()) {
				auto &col = column_render.columns[column_idx];
				column_line += " ";
				highlight.PrintText(column_line, PrintOutput::STDOUT, layout_type);
				// render each of the components
				for (idx_t component_idx = 0; component_idx < col.components.size(); component_idx++) {
					auto &component = col.components[component_idx];
					highlight.PrintText(component.text, PrintOutput::STDOUT, component.type);

					// render space padding between components
					column_line = string(max_component_widths[component_idx] - component.render_width + 1, ' ');
					if (extra_render_width > 0) {
						// if we need extra spacing add it
						idx_t render_count = is_last ? extra_render_width : render_width_per_column;
						column_line += string(render_count, ' ');
						extra_render_width -= render_count;
					}
					highlight.PrintText(column_line, PrintOutput::STDOUT, layout_type);
				}
			} else {
				// we have already rendered all columns for this line - pad with spaces
				column_line = string(per_column_render_width - 1, ' ');
				highlight.PrintText(column_line, PrintOutput::STDOUT, layout_type);
			}
			column_line = is_last ? config.VERTICAL : " ";
			highlight.PrintText(column_line, PrintOutput::STDOUT, layout_type);
		}
		return;
	}
	if (line_idx == 2 || (table.estimated_size.IsValid() && line_idx == render_table.ColumnLines() + 3)) {
		// blank line after table name
		string blank_line = config.VERTICAL;
		blank_line += string(render_width - 2, ' ');
		blank_line += config.VERTICAL;
		highlight.PrintText(blank_line, PrintOutput::STDOUT, layout_type);
		return;
	}
	if (table.estimated_size.IsValid() && line_idx == render_table.ColumnLines() + 4) {
		idx_t space_count = render_width - render_table.estimated_size_length.GetIndex() - 2;
		idx_t lspace = space_count / 2;
		idx_t rspace = space_count - lspace;
		string estimated_size_line = config.VERTICAL;
		estimated_size_line += string(lspace, ' ');
		highlight.PrintText(estimated_size_line, PrintOutput::STDOUT, layout_type);
		highlight.PrintText(render_table.estimated_size_text, PrintOutput::STDOUT, HighlightElementType::COMMENT);
		estimated_size_line = string(rspace, ' ');
		estimated_size_line += config.VERTICAL;
		highlight.PrintText(estimated_size_line, PrintOutput::STDOUT, layout_type);
		return;
	}
	// bottom line
	string bottom_line;
	bottom_line = config.LDCORNER;
	for (idx_t i = 0; i < render_width - 2; i++) {
		bottom_line += config.HORIZONTAL;
	}
	bottom_line += config.RDCORNER;
	highlight.PrintText(bottom_line, PrintOutput::STDOUT, layout_type);
}

string FormatTableMetadataType(const string &type) {
	auto result = StringUtil::Lower(type);
	if (StringUtil::StartsWith(result, "decimal")) {
		return "decimal";
	}
	return result;
}

ShellTableRenderInfo::ShellTableRenderInfo(ShellTableInfo table_p, char decimal_sep) : table(std::move(table_p)) {
	table_name_length = ShellState::RenderLength(table.table_name);
	ColumnRenderRow render_row;
	// figure out if we need an extra line to render constraint info
	bool has_constraint_component = false;
	for (auto &col : table.columns) {
		if (col.is_not_null || col.is_unique || !col.default_value.empty()) {
			has_constraint_component = true;
			break;
		}
	}

	idx_t component_count = 2 + has_constraint_component;
	for (auto &col_p : table.columns) {
		ShellColumnRenderInfo col_display;
		HighlightElementType column_name_type =
		    col_p.is_primary_key ? HighlightElementType::PRIMARY_KEY_COLUMN : HighlightElementType::COLUMN_NAME;
		col_display.components.emplace_back(std::move(col_p.column_name), column_name_type);
		col_display.components.emplace_back(FormatTableMetadataType(col_p.column_type),
		                                    HighlightElementType::COLUMN_TYPE);
		if (has_constraint_component) {
			string constraint_text;
			if (col_p.is_not_null) {
				constraint_text = "not null";
			}
			if (col_p.is_unique) {
				if (!constraint_text.empty()) {
					constraint_text += " ";
				}
				constraint_text += "unique";
			}
			if (!col_p.default_value.empty()) {
				if (!constraint_text.empty()) {
					constraint_text += " ";
				}
				constraint_text += "default " + col_p.default_value;
			}

			col_display.components.emplace_back(constraint_text, HighlightElementType::COLUMN_TYPE);
		}
		render_row.columns.push_back(std::move(col_display));
	}
	table.columns.clear();

	// iterate over the components to find the max length
	max_component_widths.resize(component_count);
	for (auto &row : render_row.columns) {
		if (row.components.size() != component_count) {
			throw InternalException("Component count is misaligned");
		}
		for (idx_t component_idx = 0; component_idx < row.components.size(); component_idx++) {
			max_component_widths[component_idx] = duckdb::MaxValue<idx_t>(max_component_widths[component_idx],
			                                                              row.components[component_idx].render_width);
		}
	}
	column_renders.push_back(std::move(render_row));

	render_width = table_name_length + 4;
	if (PerColumnWidth() > render_width) {
		render_width = PerColumnWidth();
	}
	if (table.estimated_size.IsValid()) {
		estimated_size_text = to_string(table.estimated_size.GetIndex());
		auto formatted = duckdb::BoxRenderer::TryFormatLargeNumber(estimated_size_text, decimal_sep);
		if (!formatted.empty()) {
			estimated_size_text = std::move(formatted);
		}
		estimated_size_text += " rows";
		estimated_size_length = ShellState::RenderLength(estimated_size_text);
		if (estimated_size_length.GetIndex() + 4 > render_width) {
			render_width = estimated_size_length.GetIndex() + 4;
		}
	}
}

void ShellTableRenderInfo::TruncateValueIfRequired(string &value, idx_t &render_length, idx_t max_render_width) {
	if (render_length <= max_render_width) {
		return;
	}
	duckdb::BoxRendererConfig config;
	idx_t pos = 0;
	idx_t value_render_width = 0;
	value =
	    duckdb::BoxRenderer::TruncateValue(value, max_render_width - config.DOTDOTDOT_LENGTH, pos, value_render_width) +
	    config.DOTDOTDOT;
	render_length = value_render_width + config.DOTDOTDOT_LENGTH;
}

void ShellTableRenderInfo::Truncate(idx_t max_render_width) {
	if (render_width <= max_render_width) {
		return;
	}

	// we exceeded the render width - we need to truncate
	TruncateValueIfRequired(table.table_name, table_name_length, max_render_width - 4);
	// figure out what we need to truncate
	idx_t total_column_length = PerColumnWidth();
	if (total_column_length > max_render_width) {
		// we need to truncate either column names or column types
		// prefer to keep the name as long as possible - only truncate it if it by itself almost exceeds the
		// width
		static constexpr const idx_t MIN_COMPONENT_SIZE = 5;
		idx_t component_count = max_component_widths.size();
		idx_t min_leftover_size = component_count * MIN_COMPONENT_SIZE;
		if (max_component_widths[0] + min_leftover_size > max_render_width) {
			max_component_widths[0] = max_render_width - min_leftover_size;
		}
		total_column_length = PerColumnWidth();
		if (total_column_length > max_render_width) {
			// truncate other components if required
			for (idx_t i = 1; i < max_component_widths.size(); i++) {
				if (max_component_widths[i] <= MIN_COMPONENT_SIZE) {
					// cannot truncate below the min component size
					continue;
				}
				idx_t truncate_amount = duckdb::MinValue<idx_t>(total_column_length - max_render_width,
				                                                max_component_widths[i] - MIN_COMPONENT_SIZE);
				max_component_widths[i] -= truncate_amount;
				total_column_length -= truncate_amount;
				if (total_column_length <= render_width) {
					break;
				}
			}
		}
		// truncate all column components that we need to truncate
		for (auto &column_render : column_renders) {
			for (auto &col : column_render.columns) {
				for (idx_t component_idx = 0; component_idx < col.components.size(); component_idx++) {
					auto &component = col.components[component_idx];
					TruncateValueIfRequired(component.text, component.render_width,
					                        max_component_widths[component_idx]);
				}
			}
		}
	}
	render_width = max_render_width;
}

void RenderLineDisplay(ShellHighlight &highlight, string text, idx_t total_render_width,
                       HighlightElementType element_type) {
	auto render_size = ShellState::RenderLength(text);
	ShellTableRenderInfo::TruncateValueIfRequired(text, render_size, total_render_width - 4);

	duckdb::BoxRendererConfig config;
	idx_t total_lines = total_render_width - render_size - 4;
	idx_t lline = total_lines / 2;
	idx_t rline = total_lines - lline;

	string middle_line;
	middle_line += " ";
	for (idx_t i = 0; i < lline; i++) {
		middle_line += config.HORIZONTAL;
	}
	middle_line += " ";
	middle_line += text;
	middle_line += " ";
	for (idx_t i = 0; i < rline; i++) {
		middle_line += config.HORIZONTAL;
	}
	middle_line += " ";
	middle_line += "\n";
	highlight.PrintText(middle_line, PrintOutput::STDOUT, element_type);
}

idx_t ShellState::GetMaxRenderWidth() const {
	idx_t max_render_width = max_width == 0 ? duckdb::Printer::TerminalWidth() : max_width;
	if (max_render_width < 80) {
		max_render_width = 80;
	}
	return max_render_width;
}

//! One line per table, for a reader that cannot use the box layout: "db.schema.name (table, ~N rows): col TYPE, ..."
static void RenderTableMetadataCompact(ShellState &state, vector<ShellTableInfo> &tables) {
	for (auto &table : tables) {
		string line = table.database_name + "." + table.schema_name + "." + table.table_name;
		line += table.is_view ? " (view" : " (table";
		if (table.estimated_size.IsValid()) {
			line += StringUtil::Format(", ~%llu rows", table.estimated_size.GetIndex());
		}
		line += "):";
		for (idx_t c = 0; c < table.columns.size(); c++) {
			auto &column = table.columns[c];
			line += c == 0 ? " " : ", ";
			line += column.column_name + " " + column.column_type;
			if (column.is_primary_key) {
				line += " PK";
			}
		}
		state.Print(line + "\n");
	}
}

void ShellState::RenderTableMetadata(vector<ShellTableInfo> &tables) {
	if (agent_mode_active) {
		RenderTableMetadataCompact(*this, tables);
		return;
	}
	idx_t max_render_width = GetMaxRenderWidth();
	duckdb::BoxRendererConfig config;
	// figure out the render width of each table
	vector<ShellTableRenderInfo> table_list;
	for (auto &table : tables) {
		table_list.emplace_back(table, config.decimal_separator);
	}
	for (auto &table : table_list) {
		// optionally truncate each table if we exceed the max render width
		table.Truncate(max_render_width);
	}
	// try to split up large tables
	for (auto &table : table_list) {
		static constexpr const idx_t SPLIT_THRESHOLD = 20;
		D_ASSERT(table.column_renders.size() == 1);
		if (table.column_renders[0].columns.size() <= SPLIT_THRESHOLD) {
			// not that many columns - keep it as-is
			continue;
		}
		// try to split up columns if possible
		idx_t max_split_count = (table.column_renders[0].columns.size() + SPLIT_THRESHOLD - 1) / SPLIT_THRESHOLD;
		idx_t width_per_split = table.PerColumnWidth();
		idx_t max_splits = max_render_width / width_per_split;
		D_ASSERT(max_split_count > 1);
		if (max_splits <= 1) {
			// too wide - cannot split
			continue;
		}
		idx_t split_count = duckdb::MinValue<idx_t>(max_split_count, max_splits);

		vector<ColumnRenderRow> new_renders;
		new_renders.resize(split_count);
		idx_t split_idx = 0;
		for (auto &col : table.column_renders[0].columns) {
			new_renders[split_idx % split_count].columns.push_back(std::move(col));
			split_idx++;
		}
		table.column_renders = std::move(new_renders);
		table.render_width = duckdb::MaxValue<idx_t>(table.render_width, split_count * width_per_split);
	}

	// group tables by db + schema
	duckdb::map<string, duckdb::map<string, vector<ShellTableRenderInfo>>> grouped_tables;
	for (auto &entry : table_list) {
		grouped_tables[entry.table.database_name][entry.table.schema_name].push_back(std::move(entry));
	}

	vector<TableMetadataDisplayInfo> metadata_displays;
	// for each set of table groups - make a list of displays
	for (auto &db_entry : grouped_tables) {
		for (auto &schema_entry : db_entry.second) {
			auto &result = schema_entry.second;
			// sort from biggest to smallest table
			std::sort(result.begin(), result.end(), [](const ShellTableRenderInfo &a, const ShellTableRenderInfo &b) {
				return a.LineCount() > b.LineCount();
			});

			// try to colocate different tables on the same lines in a greedy manner
			duckdb::unordered_set<idx_t> displayed_tables;
			for (idx_t table_idx = 0; table_idx < result.size(); table_idx++) {
				if (displayed_tables.find(table_idx) != displayed_tables.end()) {
					// already displayed
					continue;
				}
				displayed_tables.insert(table_idx);
				auto &initial_table = result[table_idx];
				TableMetadataDisplayInfo metadata_display;
				// the first line always has only one table (this table)
				metadata_display.database_name = initial_table.table.database_name;
				metadata_display.schema_name = initial_table.table.schema_name;
				TableMetadataLine initial_line;
				initial_line.tables.push_back(table_idx);
				metadata_display.render_width = initial_line.render_width = initial_table.render_width;
				metadata_display.render_height = initial_line.render_height = initial_table.LineCount();
				initial_line.max_component_widths = initial_table.max_component_widths;
				metadata_display.display_lines.push_back(std::move(initial_line));

				// now for each table, check if we can co-locate it
				for (idx_t next_idx = table_idx + 1; next_idx < result.size(); next_idx++) {
					auto &current_table = result[next_idx];
					auto render_width = current_table.render_width;
					auto render_height = current_table.LineCount();
					// we have two choices with co-locating
					// we can EITHER add a new line
					// OR add it to an existing line
					// we prefer to add it to an existing line if possible
					if (render_height > metadata_display.render_height) {
						// if this table is bigger than the current render height we can never add it - so just skip it
						continue;
					}
					bool added = false;
					for (auto &existing_line : metadata_display.display_lines) {
						if (existing_line.render_height + render_height > metadata_display.render_height) {
							// does not fit!
							continue;
						}
						// adding the table here works out height wise - we could potentially add it here
						// however, we do need to "stretch" the line to fit the current unit
						// get the maximum render width
						vector<idx_t> new_max_component_widths;
						for (idx_t component_idx = 0; component_idx < existing_line.max_component_widths.size();
						     component_idx++) {
							new_max_component_widths.push_back(
							    duckdb::MaxValue<idx_t>(existing_line.max_component_widths[component_idx],
							                            current_table.max_component_widths[component_idx]));
						}
						idx_t new_column_render_width = 3;
						for (auto &component_width : new_max_component_widths) {
							new_column_render_width += component_width + 1;
						}
						idx_t new_rendering_width = duckdb::MaxValue<idx_t>(render_width, existing_line.render_width);
						new_rendering_width = duckdb::MaxValue<idx_t>(new_rendering_width, new_column_render_width);

						D_ASSERT(new_rendering_width >= existing_line.render_width);
						idx_t extra_width = new_rendering_width - existing_line.render_width;

						if (metadata_display.render_width + extra_width > max_render_width) {
							// the extra width makes us exceed the rendering width limit - we cannot add it here
							continue;
						}
						// we can add it here! extend the line and add the table
						existing_line.max_component_widths = std::move(new_max_component_widths);
						existing_line.render_width += extra_width;
						existing_line.render_height += render_height;
						existing_line.tables.push_back(next_idx);
						added = true;
						break;
					}
					if (!added) {
						// if we couldn't add it to an existing line we might still be able to add a new line
						// but only if that fits width wise
						if (metadata_display.render_width + render_width <= max_render_width) {
							// it does! add an extra line
							TableMetadataLine new_line;
							new_line.tables.push_back(next_idx);
							new_line.render_width = render_width;
							new_line.render_height = render_height;
							new_line.max_component_widths = current_table.max_component_widths;
							metadata_display.render_width += render_width;
							metadata_display.display_lines.push_back(std::move(new_line));
							added = true;
						}
					}
					if (added) {
						// we added this table for rendering - add to the displayed tables list
						displayed_tables.insert(next_idx);
					}
				}
				std::sort(metadata_display.display_lines.begin(), metadata_display.display_lines.end(),
				          [](TableMetadataLine &a, TableMetadataLine &b) { return a.render_height > b.render_height; });
				metadata_displays.push_back(std::move(metadata_display));
			}
		}
	}

	idx_t line_count = 0;
	for (auto &display_line : metadata_displays) {
		line_count += display_line.render_height;
	}
	unique_ptr<PagerState> pager_setup;
	if (ShouldUsePager(line_count)) {
		// we should use a pager
		pager_setup = SetupPager();
	}
	// compute the metadata render width
	idx_t metadata_render_width = 0;
	for (auto &metadata_display : metadata_displays) {
		auto metadata_render_size = ShellState::RenderLength(metadata_display.database_name);
		metadata_render_size =
		    duckdb::MaxValue<idx_t>(metadata_render_size, ShellState::RenderLength(metadata_display.schema_name));
		metadata_render_size = duckdb::MinValue<idx_t>(max_render_width, metadata_render_size + 6);
		metadata_render_width = duckdb::MaxValue<idx_t>(metadata_render_width, metadata_render_size);
		metadata_render_width = duckdb::MaxValue<idx_t>(metadata_render_width, metadata_display.render_width);
	}
	// render the metadata
	ShellHighlight highlight(*this);
	string last_displayed_database;
	string last_displayed_schema;
	for (auto &metadata_display : metadata_displays) {
		// check if we should render the database and/or schema name for this batch of tables
		if (!metadata_display.database_name.empty() && last_displayed_database != metadata_display.database_name) {
			RenderLineDisplay(highlight, metadata_display.database_name, metadata_render_width,
			                  HighlightElementType::DATABASE_NAME);
			last_displayed_database = metadata_display.database_name;
			last_displayed_schema = string();
		}
		if (!metadata_display.schema_name.empty() && last_displayed_schema != metadata_display.schema_name) {
			RenderLineDisplay(highlight, metadata_display.schema_name, metadata_render_width,
			                  HighlightElementType::SCHEMA_NAME);
			last_displayed_schema = metadata_display.schema_name;
		}
		for (idx_t line_idx = 0; line_idx < metadata_display.render_height; line_idx++) {
			// construct the line
			for (idx_t table_line_idx = 0; table_line_idx < metadata_display.display_lines.size(); table_line_idx++) {
				bool is_last = table_line_idx + 1 == metadata_display.display_lines.size();
				auto &group_table_list = grouped_tables[metadata_display.database_name][metadata_display.schema_name];
				metadata_display.display_lines[table_line_idx].RenderLine(highlight, group_table_list, line_idx,
				                                                          is_last);
			}
			highlight.PrintText("\n", PrintOutput::STDOUT, HighlightElementType::LAYOUT);
		}
	}
}

} // namespace duckdb_shell
