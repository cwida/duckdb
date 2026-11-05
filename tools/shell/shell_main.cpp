#include "shell_state.hpp"

#include <cstdio>
#include <exception>

using namespace duckdb_shell;

int RunShell(int argc, const char **argv);

#if !((defined(_WIN32) || defined(WIN32)) && defined(_MSC_VER))
int main(int argc, const char **argv) {
#else
int wmain(int argc, wchar_t **wargv) {
	vector<string> utf8_args;
	utf8_args.resize(argc);
	vector<const char *> utf8_args_ptrs;
	utf8_args_ptrs.resize(argc);
	const char **argv = utf8_args_ptrs.data();
	for (int i = 0; i < argc; i++) {
		utf8_args[i] = ShellState::Win32UnicodeToUtf8(wargv[i]);
		utf8_args_ptrs[i] = utf8_args[i].c_str();
	}
#endif

	auto &shell_state = ShellState::GetReference();
	int rc = 0;
	try {
		rc = RunShell(argc, argv);
	} catch (const ShellExitException &ex) {
		rc = ex.exit_code;
	} catch (std::exception &ex) {
		rc = 1;
		ErrorData error(ex);
		fprintf(stderr, "Exited due to error: %s", error.Message().c_str());
	}
	try {
		// Stop new interrupt callbacks before destroying any shell owners.
		ShellState::IgnoreInterrupts();
		// Print before output cleanup forgets whether stdout was a console.
		if (shell_state) {
			shell_state->PrintExitHint(rc);
		}
		// destroy shell state prior to program clean-up
		if (shell_state) {
			delete shell_state;
		}
		shell_state = nullptr;
	} catch (std::exception &ex) {
		rc = 1;
		ErrorData error(ex);
		fprintf(stderr, "Error during clean-up due to error: %s", error.Message().c_str());
	}
	return rc;
}
