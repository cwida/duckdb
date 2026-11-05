// this file is used to instantiate symbols for LLDB so e.g.
// vector and unique_ptr can be accessed from the debugger

#ifdef DEBUG

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/common/types/column/column_data_allocator.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/rule.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/tableref/list.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/transaction.hpp"

using namespace duckdb;

namespace duckdb {

template class unique_ptr<SQLStatement>;
template class unique_ptr<AlterStatement>;
template class unique_ptr<CopyStatement>;
template class unique_ptr<CreateStatement>;
template class unique_ptr<DeleteStatement>;
template class unique_ptr<DropStatement>;
template class unique_ptr<InsertStatement>;
template class unique_ptr<SelectStatement>;
template class unique_ptr<TransactionStatement>;
template class unique_ptr<UpdateStatement>;
template class unique_ptr<PrepareStatement>;
template class unique_ptr<ExecuteStatement>;
template class unique_ptr<VacuumStatement>;
template class unique_ptr<QueryNode>;
template class unique_ptr<SelectNode>;
template class unique_ptr<SetOperationNode>;
template class unique_ptr<UpdateQueryNode>;
template class unique_ptr<DeleteQueryNode>;
template class unique_ptr<InsertQueryNode>;
template class unique_ptr<MergeQueryNode>;
template class unique_ptr<ParsedExpression>;
template class unique_ptr<CaseExpression>;
template class unique_ptr<CastExpression>;
template class unique_ptr<ColumnRefExpression>;
template class unique_ptr<ComparisonExpression>;
template class unique_ptr<ConjunctionExpression>;
template class unique_ptr<ConstantExpression>;
template class unique_ptr<DefaultExpression>;
template class unique_ptr<FunctionExpression>;
template class unique_ptr<OperatorExpression>;
template class unique_ptr<ParameterExpression>;
template class unique_ptr<StarExpression>;
template class unique_ptr<SubqueryExpression>;
template class unique_ptr<WindowExpression>;
template class unique_ptr<Constraint>;
template class unique_ptr<NotNullConstraint>;
template class unique_ptr<CheckConstraint>;
template class unique_ptr<UniqueConstraint>;
template class unique_ptr<ForeignKeyConstraint>;
// template class unique_ptr<TableRef>;
template class unique_ptr<BaseTableRef>;
template class unique_ptr<JoinRef>;
template class unique_ptr<SubqueryRef>;
template class unique_ptr<TableFunctionRef>;
template class unique_ptr<Pipeline>;
template class unique_ptr<RowGroup>;
template class unique_ptr<ColumnDataCollection>;
template class unique_ptr<PartitionedColumnData>;
template class unique_ptr<VacuumInfo>;

template class unique_ptr<Expression>;
template class unique_ptr<BoundQueryNode>;
template class unique_ptr<BoundSelectNode>;
template class unique_ptr<BoundSetOperationNode>;
template class unique_ptr<BoundAggregateExpression>;
template class unique_ptr<BoundCaseExpression>;
template class unique_ptr<BoundColumnRefExpression>;
template class unique_ptr<BoundConjunctionExpression>;
template class unique_ptr<BoundConstantExpression>;
template class unique_ptr<BoundDefaultExpression>;
template class unique_ptr<BoundFunctionExpression>;
template class unique_ptr<BoundOperatorExpression>;
template class unique_ptr<BoundParameterExpression>;
template class unique_ptr<BoundReferenceExpression>;
template class unique_ptr<BoundSubqueryExpression>;
template class unique_ptr<BoundWindowExpression>;

template class unique_ptr<CatalogEntry>;
template class unique_ptr<BindContext>;
template class unique_ptr<char[]>;
template class unique_ptr<QueryResult>;
template class unique_ptr<LogicalOperator>;
template class unique_ptr<PhysicalOperator>;
template class unique_ptr<OperatorState>;
template class unique_ptr<sel_t[]>;
template class unique_ptr<StringHeap>;
template class unique_ptr<GroupedAggregateHashTable>;
template class unique_ptr<TableRef>;
template class unique_ptr<Transaction>;
template class unique_ptr<uint64_t[]>;
template class unique_ptr<data_t[]>;
template class unique_ptr<Vector[]>;
template class unique_ptr<DataChunk>;
template class unique_ptr<JoinHashTable>;
template class unique_ptr<JoinHashTable::ScanStructure>;
template class unique_ptr<JoinHashTable::ProbeSpill>;
template class unique_ptr<data_ptr_t[]>;
template class unique_ptr<Rule>;
template class unique_ptr<LogicalFilter>;
template class unique_ptr<LogicalJoin>;
template class unique_ptr<LogicalComparisonJoin>;
template class unique_ptr<FilterInfo>;
template class unique_ptr<SingleJoinRelation>;
template class unique_ptr<CatalogSet>;
template class unique_ptr<Binder>;
template class unique_ptr<PrivateAllocatorData>;
template class unique_ptr<BaseStatistics>;

template class shared_ptr<Relation>;
template class shared_ptr<Event>;
template class shared_ptr<Pipeline>;
template class shared_ptr<MetaPipeline>;
template class shared_ptr<RowGroupCollection>;
template class shared_ptr<ColumnDataAllocator>;
template class shared_ptr<PreparedStatementData>;
template class weak_ptr<Pipeline>;

} // namespace duckdb

#define INSTANTIATE_VECTOR(VECTOR_DEFINITION)                                                                          \
	template std::VECTOR_DEFINITION::size_type std::VECTOR_DEFINITION::size() const;                                   \
	template std::VECTOR_DEFINITION::const_reference std::VECTOR_DEFINITION::operator[](                               \
	    std::VECTOR_DEFINITION::size_type n) const;                                                                    \
	template std::VECTOR_DEFINITION::reference std::VECTOR_DEFINITION::operator[](                                     \
	    std::VECTOR_DEFINITION::size_type n);                                                                          \
	template std::VECTOR_DEFINITION::const_reference std::VECTOR_DEFINITION::back() const;                             \
	template std::VECTOR_DEFINITION::reference std::VECTOR_DEFINITION::back();                                         \
	template std::VECTOR_DEFINITION::const_reference std::VECTOR_DEFINITION::front() const;                            \
	template std::VECTOR_DEFINITION::reference std::VECTOR_DEFINITION::front();

INSTANTIATE_VECTOR(vector<ColumnDefinition>)
INSTANTIATE_VECTOR(vector<JoinCondition>)
INSTANTIATE_VECTOR(vector<OrderByNode>)
INSTANTIATE_VECTOR(vector<Expression *>)
INSTANTIATE_VECTOR(vector<BoundParameterExpression *>)
INSTANTIATE_VECTOR(vector<unique_ptr<Expression>>)
INSTANTIATE_VECTOR(vector<unique_ptr<DataChunk>>)
INSTANTIATE_VECTOR(vector<unique_ptr<SQLStatement>>)
INSTANTIATE_VECTOR(vector<unique_ptr<PhysicalOperator>>)
INSTANTIATE_VECTOR(vector<unique_ptr<LogicalOperator>>)
INSTANTIATE_VECTOR(vector<unique_ptr<Transaction>>)
INSTANTIATE_VECTOR(vector<unique_ptr<Rule>>)
INSTANTIATE_VECTOR(vector<shared_ptr<Event>>)
INSTANTIATE_VECTOR(vector<unique_ptr<Pipeline>>)
INSTANTIATE_VECTOR(vector<shared_ptr<Pipeline>>)
INSTANTIATE_VECTOR(vector<weak_ptr<Pipeline>>)
INSTANTIATE_VECTOR(vector<shared_ptr<MetaPipeline>>)
INSTANTIATE_VECTOR(vector<unique_ptr<JoinHashTable>>)
INSTANTIATE_VECTOR(vector<unique_ptr<ColumnDataCollection>>)
INSTANTIATE_VECTOR(vector<shared_ptr<ColumnDataAllocator>>)

template class duckdb::vector<ExpressionType>;
template class duckdb::vector<uint64_t>;
template class duckdb::vector<string>;
template class duckdb::vector<PhysicalType>;
template class duckdb::vector<Value>;
template class duckdb::vector<int>;
template class duckdb::vector<duckdb::vector<Expression *>>;
template class duckdb::vector<LogicalType>;

#if !defined(__clang__)
template struct std::atomic<uint64_t>;
#endif

template class std::bitset<STANDARD_VECTOR_SIZE>;
template class std::unordered_map<PhysicalOperator *, ProfilingNode *>;
template class std::stack<PhysicalOperator *>;

/* -pedantic does not like this
#define INSTANTIATE_UNORDERED_MAP(MAP_DEFINITION)                                                                      \
    template MAP_DEFINITION::mapped_type &MAP_DEFINITION::operator[](MAP_DEFINITION::key_type &&k);                    \
    template MAP_DEFINITION::mapped_type &MAP_DEFINITION::operator[](const MAP_DEFINITION::key_type &k);

using catalog_map = std::unordered_map<string, unique_ptr<CatalogEntry>>;
INSTANTIATE_UNORDERED_MAP(catalog_map)
*/

template class std::unordered_map<string, uint64_t>;
template class std::unordered_map<string, vector<string>>;
template class std::unordered_map<string, std::pair<uint64_t, Expression *>>;
// template class std::unordered_map<string, TableBinding>;
template class std::unordered_map<string, SelectStatement *>;
template class std::unordered_map<uint64_t, uint64_t>;

#endif
