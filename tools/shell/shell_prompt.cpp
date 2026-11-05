#include "shell_prompt.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb_shell {

enum class PromptParseState { STANDARD, PARSE_BRACKET_TYPE, PARSE_BRACKET_CONTENT, ESCAPED };

void Prompt::AddLiteral(const string &str) {
	if (components.empty() || components.back().type != PromptComponentType::LITERAL) {
		PromptComponent component;
		component.type = PromptComponentType::LITERAL;
		components.push_back(std::move(component));
	}
	components.back().literal += str;
}

string Prompt::HandleColor(const PromptComponent &component) {
	if (!ShellHighlight::IsEnabled()) {
		return string();
	}
	switch (component.type) {
	case PromptComponentType::SET_COLOR:
		return ShellHighlight::TerminalCode(component.color, PrintIntensity::STANDARD);
	case PromptComponentType::SET_INTENSITY:
		return ShellHighlight::TerminalCode(PrintColor::STANDARD, component.intensity);
	case PromptComponentType::RESET_COLOR:
		return ShellHighlight::ResetTerminalCode();
	case PromptComponentType::SET_HIGHLIGHT_ELEMENT: {
		auto &element = ShellHighlight::GetHighlightElement(component.highlight_element);
		return ShellHighlight::TerminalCode(element.color, element.intensity);
	}
	default:
		throw InternalException("Invalid prompt color component");
	}
}

bool Prompt::ParseSetting(const string &bracket_type, const string &value) {
	if (bracket_type == "max_length") {
		if (value.empty()) {
			throw InvalidInputException("max_length requires a parameter");
		}
		max_length = StringUtil::ToUnsigned(value);
		return true;
	}
	// unknown setting
	return false;
}

void Prompt::AddComponent(const string &bracket_type, const string &value) {
	PromptComponent component;
	if (bracket_type == "setting") {
		if (value.empty()) {
			throw InvalidInputException("setting requires a parameter");
		}
		auto supported_settings = GetSupportedSettings();
		bool found = false;
		for (auto &entry : supported_settings) {
			if (value == entry) {
				found = true;
				break;
			}
		}
		if (!found) {
			throw InvalidInputException("unsupported setting \"%s\" for setting, supported values: %s", value,
			                            StringUtil::Join(supported_settings, ", "));
		}
		component.type = PromptComponentType::SETTING;
		component.literal = value;
	} else if (bracket_type == "sql") {
		if (value.empty()) {
			throw InvalidInputException("sql requires a parameter");
		}
		component.type = PromptComponentType::SQL;
		component.literal = value;
	} else if (bracket_type == "color") {
		if (value.empty()) {
			throw InvalidInputException("color requires a parameter");
		}
		if (value == "bold") {
			component.type = PromptComponentType::SET_INTENSITY;
			component.intensity = PrintIntensity::BOLD;
		} else if (value == "underline") {
			component.type = PromptComponentType::SET_INTENSITY;
			component.intensity = PrintIntensity::UNDERLINE;
		} else if (value == "reset") {
			component.type = PromptComponentType::RESET_COLOR;
		} else {
			// color value
			string error_msg;
			if (!ShellHighlight::TryGetPrintColor(value.c_str(), component.color, error_msg)) {
				throw InvalidInputException(error_msg);
			}
			component.type = PromptComponentType::SET_COLOR;
		}
	} else if (bracket_type == "highlight_element") {
		if (value.empty()) {
			throw InvalidInputException("highlight_element requires a parameter");
		}
		component.type = PromptComponentType::SET_HIGHLIGHT_ELEMENT;
		string error_msg;
		component.highlight_element = ShellHighlight::TryGetHighlightElement(value.c_str(), error_msg);
		if (!error_msg.empty()) {
			throw InvalidInputException(error_msg);
		}
	} else if (ParseSetting(bracket_type, value)) {
		return;
	} else {
		throw InvalidInputException("Unknown bracket type %s", bracket_type);
	}
	components.push_back(std::move(component));
}

void Prompt::ParsePrompt(const string &prompt) {
	components.clear();
	max_length = optional_idx();
	PromptParseState parse_state = PromptParseState::STANDARD;
	PromptParseState prev_state = parse_state;
	string bracket_type;
	string literal;
	for (auto c : prompt) {
		switch (parse_state) {
		case PromptParseState::STANDARD:
			switch (c) {
			case '\\':
				// escape - add next character literally
				prev_state = parse_state;
				parse_state = PromptParseState::ESCAPED;
				break;
			case '{':
				// found a bracket - move to parsing it
				if (!literal.empty()) {
					AddLiteral(literal);
					literal.clear();
				}
				parse_state = PromptParseState::PARSE_BRACKET_TYPE;
				break;
			default:
				literal += c;
				break;
			}
			break;
		case PromptParseState::ESCAPED:
			// escaped character - revert to original state
			literal += c;
			parse_state = prev_state;
			break;
		case PromptParseState::PARSE_BRACKET_TYPE:
			switch (c) {
			case '}': {
				// closing bracket - this is a bracket type WITHOUT value
				string value;
				AddComponent(literal, value);
				literal.clear();
				parse_state = PromptParseState::STANDARD;
				break;
			}
			case ':':
				// colon - this is the name of the bracket type
				bracket_type = std::move(literal);
				literal.clear();
				// now move to parsing the value
				parse_state = PromptParseState::PARSE_BRACKET_CONTENT;
				break;
			case '\\':
				// escape - add next character literally
				prev_state = parse_state;
				parse_state = PromptParseState::ESCAPED;
				break;
			default:
				literal += c;
				break;
			}
			break;
		case PromptParseState::PARSE_BRACKET_CONTENT:
			switch (c) {
			case '}':
				// closing bracket - we have terminated the bracket - add the component
				AddComponent(bracket_type, literal);
				bracket_type.clear();
				literal.clear();
				parse_state = PromptParseState::STANDARD;
				break;
			case '\\':
				// escape - add next character literally
				prev_state = parse_state;
				parse_state = PromptParseState::ESCAPED;
				break;
			default:
				literal += c;
				break;
			}
			break;
		default:
			throw InternalException("Invalid prompt state");
		}
	}
	if (parse_state != PromptParseState::STANDARD) {
		throw InvalidInputException("Failed to parse prompt \"%s\" - unterminated bracket or escape", prompt);
	}
	if (!literal.empty()) {
		AddLiteral(literal);
		literal.clear();
	}
}

duckdb::Connection &Prompt::GetConnection(ShellState &state) {
	return *state.conn;
}

vector<string> Prompt::GetSupportedSettings() {
	return vector<string> {"current_database",
	                       "current_schema",
	                       "current_database_and_schema",
	                       "connect_name_prefix",
	                       "connect_display_prefix",
	                       "memory_limit",
	                       "memory_usage",
	                       "swap_usage",
	                       "swap_max",
	                       "bytes_written",
	                       "bytes_read"};
}

string Prompt::HandleSetting(ShellState &state, const PromptComponent &component) {
	auto &con = GetConnection(state);
	auto &context = *con.context;
	if (component.literal == "memory_limit") {
		auto &config = duckdb::DBConfig::GetConfig(context);
		return StringUtil::BytesToHumanReadableString(config.options.maximum_memory, 1000);
	}
	if (component.literal == "memory_usage") {
		auto &buffer_manager = duckdb::BufferManager::GetBufferManager(context);
		return StringUtil::BytesToHumanReadableString(buffer_manager.GetUsedMemory(), 1000);
	}
	if (component.literal == "swap_usage") {
		auto &buffer_manager = duckdb::BufferManager::GetBufferManager(context);
		return StringUtil::BytesToHumanReadableString(buffer_manager.GetUsedSwap(), 1000);
	}
	if (component.literal == "swap_max") {
		auto &buffer_manager = duckdb::BufferManager::GetBufferManager(context);
		auto max_swap = buffer_manager.GetMaxSwap();
		if (!max_swap.IsValid()) {
			return "INF";
		}
		return StringUtil::BytesToHumanReadableString(max_swap.GetIndex(), 1000);
	}
	if (component.literal == "bytes_read") {
		auto profiler = context.services->GetQueryProfilerPtr();
		return StringUtil::BytesToHumanReadableString(profiler->GetBytesRead(), 1000);
	}
	if (component.literal == "bytes_written") {
		auto profiler = context.services->GetQueryProfilerPtr();
		return StringUtil::BytesToHumanReadableString(profiler->GetBytesWritten(), 1000);
	}
	// When CONNECT-ed the local catalog_search_path doesn't reflect where queries route, so route
	// the setting query through the chokepoint to match what `SELECT current_database()` returns.
	if (context.IsConnected()) {
		if (component.literal == "current_database") {
			return ExecuteSQL(state, "SELECT current_database()");
		}
		if (component.literal == "current_schema") {
			return ExecuteSQL(state, "SELECT current_schema()");
		}
		if (component.literal == "current_database_and_schema") {
			return ExecuteSQL(state, "SELECT CASE WHEN current_schema() = 'main' THEN current_database() "
			                         "ELSE current_database() || '.' || current_schema() END");
		}
		if (component.literal == "connect_display_prefix") {
			auto connected = context.TryGetConnectedCatalog();
			if (connected) {
				return connected->GetCatalog().GetConnectDisplay() + " ";
			}
			return string();
		}
		if (component.literal == "connect_name_prefix") {
			auto connected = context.TryGetConnectedCatalog();
			if (connected) {
				auto &catalog = connected->GetCatalog();
				// Ephemeral connections (CONNECT '<uri>') have no user-facing name; show the display (URI).
				if (catalog.GetAttached().IsEphemeral()) {
					return catalog.GetConnectDisplay() + " ";
				}
				return catalog.GetAttached().GetName() + " ";
			}
			return string();
		}
	}
	if (component.literal == "connect_name_prefix" || component.literal == "connect_display_prefix") {
		return string();
	}
	auto current_db = duckdb::DatabaseManager::GetDefaultDatabase(context);
	auto &current_schema = duckdb::ClientData::Get(*con.context).catalog_search_path->GetDefault().GetSchema();
	if (component.literal == "current_database") {
		return current_db.GetIdentifierName();
	}
	if (component.literal == "current_schema") {
		return current_schema.GetIdentifierName();
	}
	if (component.literal == "current_database_and_schema") {
		if (current_schema == "main") {
			return current_db.GetIdentifierName();
		} else {
			return current_db + "." + current_schema;
		}
	}
	throw InternalException("Unsupported setting %s", component.literal);
}

string Prompt::HandleText(ShellState &state, const string &text, idx_t &length) {
	if (!max_length.IsValid()) {
		// no max length specified - just use the code
		return text;
	}
	if (length > max_length.GetIndex()) {
		// max length was already exceeded - skip rendering
		return string();
	}
	auto render_length = state.RenderLength(text);
	if (length + render_length <= max_length.GetIndex()) {
		// not exceeded - render entire string
		length += render_length;
		return text;
	}
	// length gets exceeded by this string - render whatever we can, reserving room for "... D ".
	constexpr idx_t TRUNCATION_SUFFIX_LENGTH = 6; // "... D "
	const idx_t max = max_length.GetIndex();
	const idx_t truncate_budget = max > TRUNCATION_SUFFIX_LENGTH ? max - TRUNCATION_SUFFIX_LENGTH : 0;
	idx_t start_pos = 0;
	string truncated_text;
	for (idx_t i = 1; i <= text.size(); i++) {
		if (i < text.size() && !state.IsCharacter(text[i])) {
			// not a character - we cannot partially render at this position
			continue;
		}
		// this is a character - can we render the PREVIOUS character?
		auto prev_character = text.substr(start_pos, i - start_pos);
		auto char_length = state.RenderLength(prev_character);
		if (length + char_length > truncate_budget) {
			// we cannot - we are done!
			break;
		}
		// we can - render it and move to the next character
		truncated_text += prev_character;
		length += char_length;

		// set the start pos
		start_pos = i;
	}
	// add the final components
	truncated_text += "... D ";
	length += 6;
	return truncated_text;
}

string Prompt::ExecuteSQL(ShellState &state, const string &query) {
	string query_result;
	auto &con = GetConnection(state);
	auto exec_result = state.ExecuteSQLSingleValue(con, query, query_result);
	switch (exec_result) {
	case ExecuteSQLSingleValueResult::SUCCESS:
		return query_result;
	case ExecuteSQLSingleValueResult::EMPTY_RESULT:
		return "#EMPTY#";
	case ExecuteSQLSingleValueResult::MULTIPLE_ROWS:
		return "#MULTIPLE_ROWS#";
	case ExecuteSQLSingleValueResult::MULTIPLE_COLUMNS:
		return "#MULTIPLE_COLUMNS#";
	case ExecuteSQLSingleValueResult::NULL_RESULT:
		return "#NULL#";
	case ExecuteSQLSingleValueResult::EXECUTION_ERROR:
	default:
		return "#ERROR";
	}
}

string Prompt::GeneratePrompt(ShellState &state) {
	string prompt;
	idx_t length = 0;
	for (auto &component : components) {
		switch (component.type) {
		case PromptComponentType::LITERAL:
			prompt += HandleText(state, component.literal, length);
			break;
		case PromptComponentType::SQL: {
			auto result = ExecuteSQL(state, component.literal);
			prompt += HandleText(state, result, length);
			break;
		}
		case PromptComponentType::SET_COLOR:
		case PromptComponentType::SET_HIGHLIGHT_ELEMENT:
		case PromptComponentType::SET_INTENSITY:
		case PromptComponentType::RESET_COLOR:
			prompt += HandleColor(component);
			break;
		case PromptComponentType::SETTING: {
			auto setting_val = HandleSetting(state, component);
			prompt += HandleText(state, setting_val, length);
			break;
		}
		default:
			throw InternalException("Invalid prompt component");
		}
	}
	return prompt;
}

void Prompt::PrintPrompt(ShellState &state, PrintOutput output) {
	ShellHighlight highlight(state);
	auto color = PrintColor::STANDARD;
	auto intensity = PrintIntensity::STANDARD;

	idx_t length = 0;
	for (auto &component : components) {
		switch (component.type) {
		case PromptComponentType::LITERAL:
			highlight.PrintText(HandleText(state, component.literal, length), output, color, intensity);
			break;
		case PromptComponentType::SQL: {
			auto result = ExecuteSQL(state, component.literal);
			highlight.PrintText(HandleText(state, result, length), output, color, intensity);
			break;
		}
		case PromptComponentType::SETTING: {
			auto result = HandleSetting(state, component);
			highlight.PrintText(HandleText(state, result, length), output, color, intensity);
			break;
		}
		case PromptComponentType::SET_COLOR: {
			color = component.color;
			break;
		}
		case PromptComponentType::SET_HIGHLIGHT_ELEMENT: {
			auto &element = ShellHighlight::GetHighlightElement(component.highlight_element);
			color = element.color;
			intensity = element.intensity;
			break;
		}
		case PromptComponentType::SET_INTENSITY: {
			intensity = component.intensity;
			break;
		}
		case PromptComponentType::RESET_COLOR: {
			color = PrintColor::STANDARD;
			intensity = PrintIntensity::STANDARD;
			break;
		}
		default:
			throw InternalException("Invalid prompt component");
		}
	}
}

} // namespace duckdb_shell
