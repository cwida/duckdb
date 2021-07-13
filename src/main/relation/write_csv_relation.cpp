#include "duckdb/main/relation/write_csv_relation.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

WriteCSVRelation::WriteCSVRelation(shared_ptr<Relation> child_p, string csv_file_p)
    : Relation(child_p->context, RelationType::WRITE_CSV_RELATION), child(move(child_p)), csv_file(move(csv_file_p)) {
	context.TryBindRelation(*this, this->columns);
}

BoundStatement WriteCSVRelation::Bind(Binder &binder) {
	CopyStatement copy;
	copy.select_statement = child->GetQueryNode();
	auto info = make_unique<CopyInfo>();
	info->is_from = false;
	info->file_path = csv_file;
	info->format = "csv";
	copy.info = move(info);
	return binder.Bind((SQLStatement &)copy);
}

const vector<ColumnDefinition> &WriteCSVRelation::Columns() {
	return columns;
}

string WriteCSVRelation::ToString(idx_t depth) {
	string str = RenderWhitespace(depth) + "Write To CSV [" + csv_file + "]\n";
	return str + child->ToString(depth + 1);
}

} // namespace duckdb
