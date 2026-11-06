#include "duckdb/parser/parsed_data/create_trigger_info.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateTriggerInfo::CreateTriggerInfo()
    : CreateInfo(CatalogType::TRIGGER_ENTRY, Identifier::InvalidSchema()), timing(TriggerTiming::AFTER),
      event_type(TriggerEventType::INSERT_EVENT), for_each(TriggerForEach::STATEMENT) {
}

unique_ptr<CreateInfo> CreateTriggerInfo::Copy() const {
	auto result = make_uniq<CreateTriggerInfo>();
	CopyProperties(*result);
	result->SetTriggerName(GetTriggerName());
	result->base_table = unique_ptr_cast<TableRef, BaseTableRef>(base_table->Copy());
	result->timing = timing;
	result->event_type = event_type;
	result->columns = columns;
	result->for_each = for_each;
	result->referencing_new_table = referencing_new_table;
	result->referencing_old_table = referencing_old_table;
	result->trigger_action = trigger_action->Copy();
	return std::move(result);
}

string CreateTriggerInfo::ToString() const {
	duckdb::stringstream ss;
	ss << "CREATE";
	if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		ss << " OR REPLACE";
	}
	ss << " TRIGGER ";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		ss << "IF NOT EXISTS ";
	}
	// the trigger name is never schema qualified: a trigger inherits its schema from its base table, and
	// CREATE TRIGGER <schema>.<name> does not parse
	ss << SQLIdentifier(GetTriggerName());
	ss << " ";
	ss << EnumUtil::ToString(timing);
	ss << " ";
	ss << EnumUtil::ToString(event_type);
	if (event_type == TriggerEventType::UPDATE_EVENT && !columns.empty()) {
		ss << " OF ";
		for (idx_t i = 0; i < columns.size(); i++) {
			if (i > 0) {
				ss << ", ";
			}
			ss << SQLIdentifier(columns[i]);
		}
	}
	ss << " ON ";
	// the trigger is qualified with the (possibly nested) schema path of its base table, and the export has already
	// dropped the catalog from it - the database the statement is run against implies the catalog
	QualifiedName base_name(GetQualifiedName().Parent().Path(), base_table->Table());
	ss << base_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (!referencing_new_table.empty() || !referencing_old_table.empty()) {
		ss << " REFERENCING";
		if (!referencing_new_table.empty()) {
			ss << " NEW TABLE AS " << SQLIdentifier(referencing_new_table);
		}
		if (!referencing_old_table.empty()) {
			ss << " OLD TABLE AS " << SQLIdentifier(referencing_old_table);
		}
	}
	ss << " FOR EACH " << EnumUtil::ToString(for_each);
	ss << " " << trigger_action->ToString();
	ss << ";";
	return ss.str();
}

} // namespace duckdb
