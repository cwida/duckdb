//===----------------------------------------------------------------------===//
//                         DuckDB
//
// shell_progress_bar.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/progress_bar/display/terminal_progress_bar_display.hpp"
#include "shell_prompt.hpp"

namespace duckdb_shell {
struct ProgressBarPrompt;

struct ShellProgressBar {
	friend class ShellProgressBarDisplay;
	friend struct ProgressBarPrompt;

public:
	ShellProgressBar();
	~ShellProgressBar();

public:
	void AddComponent(const string &text);
	void ClearComponents();
	string GenerateProgressBar(ShellState &state, idx_t terminal_width);

private:
	vector<unique_ptr<ProgressBarPrompt>> components;
	duckdb::ProgressBarDisplayInfo display_info;
	int32_t percentage = 0;
	double estimated_remaining_seconds = 0;
	unique_ptr<duckdb::Connection> connection;
};

//! Displays a status bar alongside the progress bar
class ShellProgressBarDisplay : public duckdb::TerminalProgressBarDisplay {
public:
	ShellProgressBarDisplay();

public:
	void Finish() override;

protected:
	void PrintProgressInternal(int32_t percentage, double estimated_remaining_seconds, bool is_finished) override;

private:
	optional_idx previous_terminal_width;
};

//! Prints a progress line to stderr every few seconds, for consumers that read our output through a pipe (agents)
class AgentProgressBarDisplay : public duckdb::TerminalProgressBarDisplay {
public:
	//! Minimum time between two progress lines
	static constexpr double PRINT_INTERVAL_SECONDS = 5.0;

public:
	AgentProgressBarDisplay();

public:
	void Finish() override;

protected:
	void PrintProgressInternal(int32_t percentage, double estimated_remaining_seconds, bool is_finished) override;

private:
	//! When the previous line was printed (seconds since the display was created), if any
	double last_print_time = -1;
};

} // namespace duckdb_shell
