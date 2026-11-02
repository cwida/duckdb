#include "shell_progress_bar.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/main/client_data.hpp"
#include "shell_state.hpp"

namespace duckdb_shell {
using duckdb::OutputStream;
using duckdb::Printer;

enum class ProgressBarPadding { NO_PADDING };

enum class ProgressBarAlignment { LEFT, MIDDLE, RIGHT };

struct ProgressBarPrompt : public Prompt {
public:
	ProgressBarPrompt(ShellProgressBar &status_bar_p) : status_bar(status_bar_p) {
	}

	ShellProgressBar &status_bar;
	//! Alignment of the status bar relative to the total bar
	ProgressBarAlignment alignment = ProgressBarAlignment::LEFT;
	//! Padding of the content of the status bar
	ProgressBarPadding padding_type = ProgressBarPadding::NO_PADDING;
	//! In case there is padding (i.e. padding_type is not NO_PADDING) - where to position the content
	ProgressBarAlignment content_alignment = ProgressBarAlignment::LEFT;
	//! Display condition, if any
	string does_not_contain;
	//! Minimum render size
	optional_idx min_size;

public:
	void AddPaddingIfRequired(string &text, idx_t &render_length) {
		if (!min_size.IsValid() || render_length >= min_size.GetIndex()) {
			return;
		}
		auto padding_required = min_size.GetIndex() - render_length;
		// need to add at least one space of padding to each side
		idx_t left_padding;
		idx_t right_padding;
		switch (content_alignment) {
		case ProgressBarAlignment::LEFT:
			left_padding = 1;
			right_padding = padding_required - 1;
			break;
		case ProgressBarAlignment::MIDDLE:
			right_padding = padding_required / 2;
			left_padding = padding_required - right_padding;
			break;
		case ProgressBarAlignment::RIGHT:
			left_padding = padding_required - 1;
			right_padding = 1;
			break;
		default:
			throw InternalException("Unsupported alignment type");
		}
		string result;
		result += string(left_padding, ' ');
		result += text;
		result += string(right_padding, ' ');
		text = result;
		render_length += padding_required;
	}

protected:
	vector<string> GetSupportedSettings() override {
		auto supported = Prompt::GetSupportedSettings();
		supported.push_back("progress_bar_percentage");
		supported.push_back("progress_bar");
		supported.push_back("eta");
		return supported;
	}

	bool ParseSetting(const string &bracket_type, const string &value) override {
		if (bracket_type == "align") {
			if (value == "right") {
				alignment = ProgressBarAlignment::RIGHT;
			} else if (value == "left") {
				alignment = ProgressBarAlignment::LEFT;
			} else {
				throw InvalidInputException("Unsupported type %s for align: expected left or right", value);
			}
			return true;
		}
		if (bracket_type == "content_align") {
			if (value == "right") {
				content_alignment = ProgressBarAlignment::RIGHT;
			} else if (value == "left") {
				content_alignment = ProgressBarAlignment::LEFT;
			} else if (value == "middle") {
				content_alignment = ProgressBarAlignment::MIDDLE;
			} else {
				throw InvalidInputException("Unsupported type %s for content_align: expected left or right", value);
			}
			return true;
		}
		if (bracket_type == "hide_if_contains") {
			// parse condition
			does_not_contain = value;
			return true;
		}
		if (bracket_type == "min_size") {
			min_size = StringUtil::ToUnsigned(value);
			return true;
		}
		return false;
	}

	string HandleSetting(ShellState &state, const PromptComponent &component) override {
		if (component.literal == "progress_bar_percentage") {
			string result;
			if (status_bar.percentage < 100) {
				result += " ";
			}
			if (status_bar.percentage < 10) {
				result += " ";
			}
			result += to_string(status_bar.percentage) + "%";
			return result;
		}
		if (component.literal == "progress_bar") {
			return duckdb::TerminalProgressBarDisplay::FormatProgressBar(status_bar.display_info,
			                                                             status_bar.percentage);
		}
		if (component.literal == "eta") {
			return duckdb::TerminalProgressBarDisplay::FormatETA(status_bar.estimated_remaining_seconds);
		}
		// bytes_read/bytes_written must be read from the main connection's profiler,
		// not the progress bar's separate connection
		if (component.literal == "bytes_read") {
			auto &context = *state.conn->context;
			auto &client_data = duckdb::ClientData::Get(context);
			auto profiler = client_data.profiler;
			return duckdb::StringUtil::BytesToHumanReadableString(profiler->GetBytesRead(), 1000);
		} else if (component.literal == "bytes_written") {
			auto &context = *state.conn->context;
			auto &client_data = duckdb::ClientData::Get(context);
			auto profiler = client_data.profiler;
			return duckdb::StringUtil::BytesToHumanReadableString(profiler->GetBytesWritten(), 1000);
		}
		return Prompt::HandleSetting(state, component);
	}

	duckdb::Connection &GetConnection(ShellState &state) override {
		if (!status_bar.connection) {
			status_bar.connection = make_uniq<duckdb::Connection>(*state.db);
		}
		return *status_bar.connection;
	}
};

ShellProgressBar::ShellProgressBar() {
}
ShellProgressBar::~ShellProgressBar() {
}

void ShellProgressBar::AddComponent(const string &component_text) {
	auto component = make_uniq<ProgressBarPrompt>(*this);
	component->ParsePrompt(component_text);
	components.push_back(std::move(component));
}

void ShellProgressBar::ClearComponents() {
	components.clear();
}

string ShellProgressBar::GenerateProgressBar(ShellState &state, idx_t terminal_width) {
	string lhs;
	string rhs;
	idx_t total_render_length = 0;
	for (auto &component : components) {
		auto text = component->GeneratePrompt(state);
		if (!component->does_not_contain.empty()) {
			if (StringUtil::Contains(text, component->does_not_contain)) {
				continue;
			}
		}
		auto render_length = state.RenderLength(text);
		component->AddPaddingIfRequired(text, render_length);

		if (total_render_length + render_length > terminal_width) {
			// exceeds terminal width - don't stop rendering
			break;
		}
		total_render_length += render_length;
		if (component->alignment == ProgressBarAlignment::LEFT) {
			lhs += text;
		} else {
			rhs += text;
		}
	}
	string result;
	result = lhs;
	if (total_render_length < terminal_width) {
		result += string(terminal_width - total_render_length, ' ');
		result += rhs;
	}
	return result;
}

ShellProgressBarDisplay::ShellProgressBarDisplay() {
}

void ShellProgressBarDisplay::Finish() {
	PrintProgressInternal(100, GetElapsedDuration(), true);
	Printer::Flush(OutputStream::STREAM_STDOUT);
}

void ShellProgressBarDisplay::PrintProgressInternal(int32_t percentage, double estimated_remaining_seconds,
                                                    bool is_finished) {
	auto terminal_width = Printer::TerminalWidth();
	string result;
	if (previous_terminal_width.IsValid() && terminal_width < previous_terminal_width.GetIndex()) {
		// terminal size got smaller - need to erase more lines
		idx_t line_count = (previous_terminal_width.GetIndex() + terminal_width - 1) / terminal_width;
		D_ASSERT(line_count > 1);

		for (idx_t i = 1; i < line_count; i++) {
			// go up + clear line until we reach the first line again
			result += "\r\x1b[0K\x1b[1A";
		}
	}
	// clear the current line
	result += "\r\x1b[0K";
	previous_terminal_width = terminal_width;
	if (!is_finished) {
		auto &state = ShellState::Get();
		try {
			state.progress_bar->percentage = percentage;
			state.progress_bar->estimated_remaining_seconds = estimated_remaining_seconds;
			result += state.progress_bar->GenerateProgressBar(state, terminal_width);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			result += error.Message();
		}
		state.progress_bar->connection.reset();
	}
	Printer::RawPrint(OutputStream::STREAM_STDOUT, result);
}

AgentProgressBarDisplay::AgentProgressBarDisplay() {
}

void AgentProgressBarDisplay::Finish() {
	PrintProgressInternal(100, GetElapsedDuration(), true);
}

void AgentProgressBarDisplay::PrintProgressInternal(int32_t percentage, double seconds, bool is_finished) {
	auto elapsed = GetElapsedDuration();
	if (is_finished) {
		if (last_print_time < 0) {
			// the query finished before we printed anything - stay silent
			return;
		}
		Printer::RawPrint(OutputStream::STREAM_STDERR, StringUtil::Format("progress: done (elapsed %.1fs)\n", seconds));
		Printer::Flush(OutputStream::STREAM_STDERR);
		return;
	}
	if (last_print_time >= 0 && elapsed - last_print_time < PRINT_INTERVAL_SECONDS) {
		return;
	}
	last_print_time = elapsed;
	string line = StringUtil::Format("progress: %d%% (elapsed %.1fs", percentage, elapsed);
	if (seconds >= 0 && seconds < 2147483647.0) {
		line += StringUtil::Format(", remaining ~%.1fs", seconds);
	}
	line += ")\n";
	Printer::RawPrint(OutputStream::STREAM_STDERR, line);
	Printer::Flush(OutputStream::STREAM_STDERR);
}

} // namespace duckdb_shell
