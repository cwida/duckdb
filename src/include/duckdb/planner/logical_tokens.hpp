//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/logical_tokens.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class LogicalOperator;

class LogicalAggregate;
class LogicalAnyJoin;
class LogicalColumnDataGet;
class LogicalComparisonJoin;
class LogicalCopyDatabase;
class LogicalCopyToFile;
class LogicalCreate;
class LogicalCreateTable;
class LogicalCreateIndex;
class LogicalCreateTable;
class LogicalCreateSecret;
class LogicalCrossProduct;
class LogicalCTERef;
class LogicalDelete;
class LogicalDelimGet;
class LogicalDistinct;
class LogicalDummyScan;
class LogicalEmptyResult;
class LogicalExplain;
class LogicalExport;
class LogicalExpressionGet;
class LogicalFilter;
class LogicalGet;
class LogicalInsert;
class LogicalJoin;
class LogicalLimit;
class LogicalMergeInto;
class LogicalOrder;
class LogicalPivot;
class LogicalPositionalJoin;
class LogicalPragma;
class LogicalPrepare;
class LogicalProjection;
class LogicalRecursiveCTE;
class LogicalMaterializedCTE;
class LogicalSetOperation;
class LogicalSample;
class LogicalSecureView;
class LogicalAlter;
class LogicalAttach;
class LogicalConnect;
class LogicalExternalResource;
class LogicalDetach;
class LogicalDisconnect;
class LogicalDrop;
class LogicalLoad;
class LogicalTransaction;
class LogicalUpdateExtensions;
class LogicalVacuum;
class LogicalSet;
class LogicalReset;
class LogicalTopN;
class LogicalUnnest;
class LogicalUpdate;
class LogicalWindow;

} // namespace duckdb
