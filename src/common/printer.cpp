#include "duckdb/common/printer.hpp"
#include "duckdb/common/progress_bar.hpp"
#include <stdio.h>

namespace duckdb {

// LCOV_EXCL_START
void Printer::Print(const string &str) {
#ifndef DUCKDB_DISABLE_PRINT
	fprintf(stderr, "%s\n", str.c_str());
#endif
}
void Printer::PrintProgress(int percentage, const char *pbstr, int pbwidth) {
#ifndef DUCKDB_DISABLE_PRINT
	int lpad = (int)(percentage / 100.0 * pbwidth);
	int rpad = pbwidth - lpad;
	printf("\r%3d%% [%.*s%*s]", percentage, lpad, pbstr, rpad, "");
	fflush(stdout);
#endif
}

void Printer::FinishProgressBarPrint(const char *pbstr, int pbwidth) {
#ifndef DUCKDB_DISABLE_PRINT
	PrintProgress(100, pbstr, pbwidth);
	printf(" \n");
	fflush(stdout);
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb
