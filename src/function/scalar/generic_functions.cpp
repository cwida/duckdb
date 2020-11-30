#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {
using namespace std;

void BuiltinFunctions::RegisterGenericFunctions() {
	Register<AliasFun>();
	Register<LeastFun>();
	Register<GreatestFun>();
	Register<StatsFun>();
	Register<TypeOfFun>();
}

} // namespace duckdb
