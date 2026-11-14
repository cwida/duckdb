#pragma once

#include "shell_state.hpp"

namespace duckdb_shell {

MetadataResult SetThreadStackSize(ShellState &state, const duckdb::vector<duckdb::string> &args);

} // namespace duckdb_shell
