//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/bind_helpers.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

class Value;
struct BoundOrderByNode;
struct BoundStatement;
struct LogicalType;
struct OrderByNode;
struct TableIndex;
class Binder;

Value ConvertVectorToValue(vector<Value> set);
vector<bool> ParseColumnList(const vector<Value> &set, const vector<Identifier> &names, const Identifier &option_name);
vector<bool> ParseColumnList(const Value &value, const vector<Identifier> &names, const Identifier &option_name);
vector<idx_t> ParseColumnsOrdered(const vector<Value> &set, const vector<Identifier> &names,
                                  const Identifier &option_name);
vector<idx_t> ParseColumnsOrdered(const Value &value, const vector<Identifier> &names, const Identifier &option_name);
vector<BoundOrderByNode> ParseOrderByColumns(Binder &binder, const vector<Value> &set,
                                             const BoundStatement &bound_statement, const Identifier &option_name);
DUCKDB_API vector<BoundOrderByNode> BindOrderByNodes(Binder &binder, TableIndex table_index, const Identifier &alias,
                                                     const vector<Identifier> &names, const vector<LogicalType> &types,
                                                     vector<OrderByNode> &orders);

} // namespace duckdb
