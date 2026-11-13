#include "duckdb/parser/parsed_data/copy_info.hpp"

#include "duckdb/common/algorithm.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/query_node.hpp"

namespace duckdb {

CopyInfo::CopyInfo()
    : ParseInfo(TYPE), is_from(false), is_format_auto_detected(true),
      qualified_name({Identifier::DefaultSchema()}, Identifier()) {
}

unique_ptr<CopyInfo> CopyInfo::Copy() const {
	auto result = make_uniq<CopyInfo>();
	result->qualified_name = qualified_name;
	result->select_list = select_list;
	result->file_path_expression = file_path_expression ? file_path_expression->Copy() : nullptr;
	result->file_path = file_path;
	result->is_from = is_from;
	result->format = format;
	result->is_format_auto_detected = is_format_auto_detected;
	for (auto &entry : parsed_options) {
		result->parsed_options[entry.first] = entry.second ? entry.second->Copy() : nullptr;
	}
	result->options = options;
	if (select_statement) {
		result->select_statement = select_statement->Copy();
	}
	return result;
}

bool CopyInfo::Equals(const CopyInfo &other) const {
	if (qualified_name != other.qualified_name || select_list != other.select_list || is_from != other.is_from ||
	    format != other.format || is_format_auto_detected != other.is_format_auto_detected ||
	    file_path != other.file_path || options.size() != other.options.size() ||
	    parsed_options.size() != other.parsed_options.size()) {
		return false;
	}
	if (!ParsedExpression::Equals(file_path_expression, other.file_path_expression)) {
		return false;
	}
	for (auto &entry : options) {
		auto other_entry = other.options.find(entry.first);
		if (other_entry == other.options.end() || entry.second != other_entry->second) {
			return false;
		}
	}
	for (auto &entry : parsed_options) {
		auto other_entry = other.parsed_options.find(entry.first);
		if (other_entry == other.parsed_options.end() || !ParsedExpression::Equals(entry.second, other_entry->second)) {
			return false;
		}
	}
	if (select_statement && other.select_statement) {
		return select_statement->Equals(other.select_statement.get());
	}
	return !select_statement && !other.select_statement;
}

string CopyInfo::CopyOptionsToString() const {
	// We only output the format if there is a format, and it was manually set.
	const bool output_format = !format.empty() && !is_format_auto_detected;
	if (!output_format && options.empty() && parsed_options.empty()) {
		return string();
	}
	string result;

	result += " (";
	vector<string> stringified;
	if (!format.empty() && !is_format_auto_detected) {
		stringified.push_back(StringUtil::Format(" FORMAT %s", SQLString(format)));
	}
	// render the options in sorted order so the generated text is deterministic across platforms
	vector<Identifier> parsed_option_names;
	for (auto &opt : parsed_options) {
		parsed_option_names.push_back(opt.first);
	}
	std::sort(parsed_option_names.begin(), parsed_option_names.end());
	for (auto &name : parsed_option_names) {
		auto &expr = parsed_options.find(name)->second;
		string option_string = StringUtil::Format("%s", name);
		if (expr) {
			option_string += " " + expr->ToString();
		}
		stringified.push_back(option_string);
	}
	vector<Identifier> option_names;
	for (auto &opt : options) {
		option_names.push_back(opt.first);
	}
	std::sort(option_names.begin(), option_names.end());
	for (auto &name : option_names) {
		auto &values = options.find(name)->second;

		auto option = StringUtil::Format("%s ", name);
		if (values.empty()) {
			// Options like HEADER don't need an explicit value
			// just providing the name already sets it to true
			stringified.push_back(option);
		} else if (values.size() == 1) {
			stringified.push_back(option + values[0].ToSQLString());
		} else {
			vector<string> sub_values;
			for (auto &val : values) {
				sub_values.push_back(val.ToSQLString());
			}
			stringified.push_back(option + "( " + StringUtil::Join(sub_values, ", ") + " )");
		}
	}
	result += StringUtil::Join(stringified, ", ");
	result += " )";
	return result;
}

string CopyInfo::TablePartToString() const {
	string result;

	D_ASSERT(!GetQualifiedName().Name().empty());
	result += qualified_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);

	// (c1, c2, ..)
	if (!select_list.empty()) {
		vector<string> options;
		for (auto &option : select_list) {
			options.push_back(SQLIdentifier::ToString(option));
		}
		result += " (";
		result += StringUtil::Join(options, ", ");
		result += " )";
	}
	return result;
}

string CopyInfo::ToString() const {
	string result = "";
	result += "COPY ";
	if (is_from) {
		D_ASSERT(!select_statement);
		result += TablePartToString();
		result += " FROM";
	} else {
		if (select_statement) {
			// COPY (select-node) TO ...
			result += "(" + select_statement->ToString() + ")";
		} else {
			result += TablePartToString();
		}
		result += " TO ";
	}
	if (file_path_expression && file_path.empty()) {
		result += "(";
		result += file_path_expression->ToString();
		result += ")";
	} else {
		result += StringUtil::Format(" %s", SQLString(file_path));
	}
	result += CopyOptionsToString();
	result += ";";
	return result;
}

} // namespace duckdb
