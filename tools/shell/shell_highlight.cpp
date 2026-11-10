#include "shell_highlight.hpp"
#include "shell_state.hpp"
#include "duckdb/parser/parser.hpp"

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#endif

namespace duckdb_shell {

static HighlightElement highlight_elements[] = {
    {"error", PrintColor::RED, PrintIntensity::STANDARD},
    {"keyword", PrintColor::GREEN, PrintIntensity::STANDARD},
    {"numeric_constant", PrintColor::YELLOW, PrintIntensity::STANDARD},
    {"string_constant", PrintColor::YELLOW, PrintIntensity::STANDARD},
    {"line_indicator", PrintColor::STANDARD, PrintIntensity::BOLD},
    {"database_name", PrintColor::ORANGE3, PrintIntensity::STANDARD},
    {"schema_name", PrintColor::DEEPSKYBLUE1, PrintIntensity::STANDARD},
    {"table_name", PrintColor::STANDARD, PrintIntensity::BOLD},
    {"column_name", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"column_type", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"numeric_value", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"string_value", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"temporal_value", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"null_value", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"footer", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"layout", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"startup_text", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"startup_version", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"continuation", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"continuation_selected", PrintColor::GREEN, PrintIntensity::STANDARD},
    {"bracket", PrintColor::STANDARD, PrintIntensity::UNDERLINE},
    {"comment", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"suggestion_catalog_name", PrintColor::ORANGE3, PrintIntensity::STANDARD},
    {"suggestion_schema_name", PrintColor::DEEPSKYBLUE1, PrintIntensity::STANDARD},
    {"suggestion_table_name", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"suggestion_column_name", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"suggestion_file_name", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"suggestion_directory_name", PrintColor::STANDARD, PrintIntensity::BOLD},
    {"suggestion_function_name", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"suggestion_setting_name", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"table_layout", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"view_layout", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"primary_key_column", PrintColor::STANDARD, PrintIntensity::UNDERLINE},
    {"prompt", PrintColor::DARKORANGE, PrintIntensity::BOLD},
    {"prompt_connect", PrintColor::DARKORANGE, PrintIntensity::STANDARD},
    {"error_emphasis", PrintColor::RED, PrintIntensity::BOLD},
    {"error_suggestion", PrintColor::RED, PrintIntensity::BOLD},
    {"log_trace", PrintColor::BLUE, PrintIntensity::BOLD},
    {"log_debug", PrintColor::YELLOW, PrintIntensity::BOLD},
    {"log_info", PrintColor::GREEN, PrintIntensity::BOLD},
    {"log_warning", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"explain_layout", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"explain_operator", PrintColor::STANDARD, PrintIntensity::BOLD},
    {"explain_operator_scan", PrintColor::GREEN, PrintIntensity::BOLD},
    {"explain_operator_join", PrintColor::CYAN, PrintIntensity::BOLD},
    {"explain_operator_aggregate", PrintColor::MAGENTA, PrintIntensity::BOLD},
    {"explain_operator_order", PrintColor::YELLOW, PrintIntensity::BOLD},
    {"explain_rows", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"explain_detail_key", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"explain_detail_value", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"explain_header", PrintColor::STANDARD, PrintIntensity::BOLD},
    {"explain_timing_critical", PrintColor::RED, PrintIntensity::BOLD},
    {"explain_timing_high", PrintColor::YELLOW, PrintIntensity::STANDARD},
    {"explain_timing_moderate", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {"explain_timing_low", PrintColor::GRAY, PrintIntensity::STANDARD},
    {"none", PrintColor::STANDARD, PrintIntensity::STANDARD},
    {nullptr, PrintColor::STANDARD, PrintIntensity::STANDARD},
};

const HighlightElement &ShellHighlight::GetHighlightElement(HighlightElementType type) {
	// colors are about to be used - detect the terminal colors if we have not yet
	ShellState::Get().DetectDarkLightMode();
	auto index = static_cast<uint32_t>(type);
	auto max_index = static_cast<uint32_t>(HighlightElementType::NONE);
	if (index > max_index) {
		index = max_index;
	}
	return highlight_elements[index];
}

ShellHighlight::ShellHighlight(ShellState &state) : state(state) {
}

bool ShellHighlight::IsEnabled() {
	return ShellState::Get().highlighting_enabled;
}

void ShellHighlight::SetHighlighting(bool enabled) {
	ShellState::Get().highlighting_enabled = enabled;
}

void ShellHighlight::PrintText(const string &text, PrintOutput output, PrintColor color, PrintIntensity intensity) {
	if ((color == PrintColor::STANDARD && intensity == PrintIntensity::STANDARD) || !IsEnabled()) {
		// no highlighting - print directly
		state.Print(output, text);
		return;
	}
#ifdef _WIN32
	HANDLE out = GetStdHandle(output == PrintOutput::STDOUT ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
	bool use_terminal_codes = false;
	DWORD mode = 0;
	if (GetConsoleMode(out, &mode)) {
		if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
			// terminal codes are supported - use them!
			use_terminal_codes = true;
		}
	}
	if (!use_terminal_codes) {
		// terminal codes are not supported - use standard 4-bit colors
		CONSOLE_SCREEN_BUFFER_INFO defaultScreenInfo;
		GetConsoleScreenBufferInfo(out, &defaultScreenInfo);
		WORD wAttributes = 0;

		switch (intensity) {
		case PrintIntensity::BOLD:
		case PrintIntensity::BOLD_UNDERLINE:
			wAttributes |= FOREGROUND_INTENSITY;
			break;
		default:
			break;
		}
		switch (color) {
		case PrintColor::RED:
			wAttributes |= FOREGROUND_RED;
			break;
		case PrintColor::GREEN:
			wAttributes |= FOREGROUND_GREEN;
			break;
		case PrintColor::BLUE:
			wAttributes |= FOREGROUND_BLUE;
			break;
		case PrintColor::YELLOW:
			wAttributes |= FOREGROUND_RED | FOREGROUND_GREEN;
			break;
		case PrintColor::GRAY:
			wAttributes |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
			break;
		case PrintColor::MAGENTA:
			wAttributes |= FOREGROUND_BLUE | FOREGROUND_RED;
			break;
		case PrintColor::CYAN:
			wAttributes |= FOREGROUND_BLUE | FOREGROUND_GREEN;
			break;
		case PrintColor::WHITE:
			wAttributes |= FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
			break;
		default:
			break;
		}
		if (wAttributes != 0) {
			SetConsoleTextAttribute(out, wAttributes);
		}

		state.Print(output, text);

		SetConsoleTextAttribute(out, defaultScreenInfo.wAttributes);
		return;
	}
#endif
	string terminal_code = TerminalCode(color, intensity);
	if (terminal_code.empty()) {
		// no highlighting - print directly
		state.Print(output, text);
		return;
	}
	terminal_code += text;
	terminal_code += ResetTerminalCode();
	state.Print(output, terminal_code);
}

void ShellHighlight::PrintText(const string &text, PrintOutput output, HighlightElementType type) {
	auto &highlight_info = GetHighlightElement(type);
	PrintText(text, output, highlight_info.color, highlight_info.intensity);
}

void ShellHighlight::PrintError(string error_msg) {
	if (error_msg.empty()) {
		return;
	}
	vector<duckdb::SimplifiedToken> tokens;
	string error_type;
	auto error_location = duckdb::StringUtil::Find(error_msg, "Error: ");
	if (error_location.IsValid()) {
		error_type = error_msg.substr(0, error_location.GetIndex() + 6);
		error_msg = error_msg.substr(error_location.GetIndex() + 7);
	}
	try {
		tokens = duckdb::Parser::TokenizeError(error_msg);
	} catch (...) {
		// fallback
		state.Print(PrintOutput::STDERR, error_msg.c_str());
		state.Print(PrintOutput::STDERR, "\n");
		return;
	}
	if (!tokens.empty() && tokens[0].start > 0) {
		duckdb::SimplifiedToken new_token;
		new_token.type = duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
		new_token.start = 0;
		tokens.insert(tokens.begin(), new_token);
	}
	if (tokens.empty() && !error_msg.empty()) {
		duckdb::SimplifiedToken new_token;
		new_token.type = duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
		new_token.start = 0;
		tokens.push_back(new_token);
	}
	if (!error_type.empty()) {
		PrintText(error_type + "\n", PrintOutput::STDERR, HighlightElementType::ERROR_EMPHASIS);
	}
	for (idx_t i = 0; i < tokens.size(); i++) {
		HighlightElementType element_type = HighlightElementType::NONE;
		switch (tokens[i].type) {
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER:
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR:
			element_type = HighlightElementType::ERROR_TOKEN;
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS:
			element_type = HighlightElementType::ERROR_EMPHASIS;
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_SUGGESTION:
			element_type = HighlightElementType::ERROR_SUGGESTION;
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
			element_type = HighlightElementType::NUMERIC_CONSTANT;
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT:
			element_type = HighlightElementType::STRING_CONSTANT;
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR:
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
			element_type = HighlightElementType::KEYWORD;
			break;
		case duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
			element_type = HighlightElementType::LINE_INDICATOR;
			break;
		}
		idx_t start = tokens[i].start;
		idx_t end = i + 1 == tokens.size() ? error_msg.size() : tokens[i + 1].start;
		if (end > start) {
			string error_print = error_msg.substr(tokens[i].start, end - start);
			PrintText(error_print, PrintOutput::STDERR, element_type);
		}
	}
	PrintText("\n", PrintOutput::STDERR, PrintColor::STANDARD, PrintIntensity::STANDARD);
}

HighlightElementType ShellHighlight::TryGetHighlightElement(const char *element_type, string &error_msg) {
	for (idx_t i = 0; highlight_elements[i].name; i++) {
		if (StringUtil::CIEquals(element_type, highlight_elements[i].name)) {
			return static_cast<HighlightElementType>(i);
		}
	}
	// element not found
	string supported_options;
	for (idx_t i = 0; highlight_elements[i].name; i++) {
		if (!supported_options.empty()) {
			supported_options += ", ";
		}
		supported_options += highlight_elements[i].name;
	}
	error_msg =
	    StringUtil::Format("Unknown element '%s', supported options: %s\n", element_type, supported_options.c_str());
	return HighlightElementType::NONE;
}

void ShellHighlight::ToggleMode(HighlightMode mode) {
	bool user_configured = false;
	if (mode == HighlightMode::MIXED_MODE) {
		SetColor(HighlightElementType::KEYWORD, PrintColor::GREEN, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::STRING_CONSTANT, PrintColor::YELLOW, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::NUMERIC_CONSTANT, PrintColor::YELLOW, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::CONTINUATION_SELECTED, PrintColor::GREEN, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::PROMPT, PrintColor::DARKORANGE, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::PROMPT_CONNECT, PrintColor::DARKORANGE, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::DATABASE_NAME, PrintColor::ORANGE3, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::SCHEMA_NAME, PrintColor::DEEPSKYBLUE1, PrintIntensity::STANDARD,
		         user_configured);
	}

	if (mode == HighlightMode::DARK_MODE) {
		SetColor(HighlightElementType::KEYWORD, PrintColor::DODGERBLUE1, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::STRING_CONSTANT, PrintColor::GOLD1, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::NUMERIC_CONSTANT, PrintColor::ORCHID2, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::CONTINUATION_SELECTED, PrintColor::DODGERBLUE1, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::ERROR_TOKEN, PrintColor::INDIANRED3, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::ERROR_EMPHASIS, PrintColor::INDIANRED3, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::ERROR_SUGGESTION, PrintColor::INDIANRED3, PrintIntensity::BOLD, user_configured);
	}

	if (mode == HighlightMode::LIGHT_MODE) {
		SetColor(HighlightElementType::KEYWORD, PrintColor::DODGERBLUE2, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::STRING_CONSTANT, PrintColor::ORANGE4, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::NUMERIC_CONSTANT, PrintColor::DARKMAGENTA, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::CONTINUATION_SELECTED, PrintColor::DODGERBLUE2, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::PROMPT, PrintColor::DARKORANGE4, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::PROMPT_CONNECT, PrintColor::DARKORANGE4, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::ERROR_TOKEN, PrintColor::RED3, PrintIntensity::STANDARD, user_configured);
		SetColor(HighlightElementType::ERROR_EMPHASIS, PrintColor::RED3, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::ERROR_SUGGESTION, PrintColor::RED3, PrintIntensity::BOLD, user_configured);
		SetColor(HighlightElementType::DATABASE_NAME, PrintColor::DARKORANGE4, PrintIntensity::STANDARD,
		         user_configured);
		SetColor(HighlightElementType::SCHEMA_NAME, PrintColor::DEEPSKYBLUE6, PrintIntensity::STANDARD,
		         user_configured);
	}
}

void ShellHighlight::SetColor(HighlightElementType element_type, PrintColor print_color, PrintIntensity intensity,
                              bool user_configured) {
	if (user_configured) {
		state.user_configured_elements.insert(element_type);
	} else {
		// don't override user configured highlight colors
		if (state.user_configured_elements.find(element_type) != state.user_configured_elements.end()) {
			return;
		}
	}
	idx_t i = static_cast<idx_t>(element_type);
	highlight_elements[i].color = print_color;
	highlight_elements[i].intensity = intensity;
}

bool ShellHighlight::SetColor(const char *element_type, const char *color, const char *intensity) {
	string error_msg;
	auto element = TryGetHighlightElement(element_type, error_msg);
	if (element == HighlightElementType::NONE && !error_msg.empty()) {
		state.Print(PrintOutput::STDERR, error_msg);
		return false;
	}

	// found the element - parse the color
	PrintColor print_color;
	if (!TryGetPrintColor(color, print_color, error_msg)) {
		state.PrintDatabaseError(error_msg);
		return false;
	}
	PrintIntensity print_intensity = PrintIntensity::STANDARD;
	if (intensity) {
		if (duckdb::StringUtil::CIEquals(intensity, "standard")) {
			print_intensity = PrintIntensity::STANDARD;
		} else if (duckdb::StringUtil::CIEquals(intensity, "bold")) {
			print_intensity = PrintIntensity::BOLD;
		} else if (duckdb::StringUtil::CIEquals(intensity, "underline")) {
			print_intensity = PrintIntensity::UNDERLINE;
		} else if (duckdb::StringUtil::CIEquals(intensity, "bold_underline")) {
			print_intensity = PrintIntensity::BOLD_UNDERLINE;
		} else {
			state.Print(PrintOutput::STDERR,
			            duckdb::StringUtil::Format(
			                "Unknown intensity '%s', supported options: standard, bold, underline\n", intensity));
			return false;
		}
	}
	SetColor(element, print_color, print_intensity);
	return true;
}

static const HighlightColorInfo highlight_color_info[] = {{"black", 0, 0x00, 0x00, 0x00},
                                                          {"red", 1, 0x80, 0x00, 0x00},
                                                          {"green", 2, 0x00, 0x80, 0x00},
                                                          {"yellow", 3, 0x80, 0x80, 0x00},
                                                          {"blue", 4, 0x00, 0x00, 0x80},
                                                          {"magenta", 5, 0x80, 0x00, 0x80},
                                                          {"cyan", 6, 0x00, 0x80, 0x80},
                                                          {"brightgray", 7, 0xC0, 0xC0, 0xC0},
                                                          {"gray", 8, 0x80, 0x80, 0x80},
                                                          {"brightred", 9, 0xFF, 0x00, 0x00},
                                                          {"brightgreen", 10, 0x00, 0xFF, 0x00},
                                                          {"brightyellow", 11, 0xFF, 0xFF, 0x00},
                                                          {"brightblue", 12, 0x00, 0x00, 0xFF},
                                                          {"brightmagenta", 13, 0xFF, 0x00, 0xFF},
                                                          {"brightcyan", 14, 0x00, 0xFF, 0xFF},
                                                          {"white", 15, 0xFF, 0xFF, 0xFF},
                                                          {"grey0", 16, 0x00, 0x00, 0x00},
                                                          {"navyblue", 17, 0x00, 0x00, 0x5F},
                                                          {"darkblue", 18, 0x00, 0x00, 0x87},
                                                          {"blue3", 19, 0x00, 0x00, 0xAF},
                                                          {"blue4", 20, 0x00, 0x00, 0xD7},
                                                          {"blue1", 21, 0x00, 0x00, 0xFF},
                                                          {"darkgreen", 22, 0x00, 0x5F, 0x00},
                                                          {"deepskyblue4", 23, 0x00, 0x5F, 0x5F},
                                                          {"deepskyblue5", 24, 0x00, 0x5F, 0x87},
                                                          {"deepskyblue6", 25, 0x00, 0x5F, 0xAF},
                                                          {"dodgerblue3", 26, 0x00, 0x5F, 0xD7},
                                                          {"dodgerblue2", 27, 0x00, 0x5F, 0xFF},
                                                          {"green4", 28, 0x00, 0x87, 0x00},
                                                          {"springgreen4", 29, 0x00, 0x87, 0x5F},
                                                          {"turquoise4", 30, 0x00, 0x87, 0x87},
                                                          {"deepskyblue3", 31, 0x00, 0x87, 0xAF},
                                                          {"deepskyblue7", 32, 0x00, 0x87, 0xD7},
                                                          {"dodgerblue1", 33, 0x00, 0x87, 0xFF},
                                                          {"green3", 34, 0x00, 0xAF, 0x00},
                                                          {"springgreen3", 35, 0x00, 0xAF, 0x5F},
                                                          {"darkcyan", 36, 0x00, 0xAF, 0x87},
                                                          {"lightseagreen", 37, 0x00, 0xAF, 0xAF},
                                                          {"deepskyblue2", 38, 0x00, 0xAF, 0xD7},
                                                          {"deepskyblue1", 39, 0x00, 0xAF, 0xFF},
                                                          {"green5", 40, 0x00, 0xD7, 0x00},
                                                          {"springgreen5", 41, 0x00, 0xD7, 0x5F},
                                                          {"springgreen2", 42, 0x00, 0xD7, 0x87},
                                                          {"cyan3", 43, 0x00, 0xD7, 0xAF},
                                                          {"darkturquoise", 44, 0x00, 0xD7, 0xD7},
                                                          {"turquoise2", 45, 0x00, 0xD7, 0xFF},
                                                          {"green1", 46, 0x00, 0xFF, 0x00},
                                                          {"springgreen6", 47, 0x00, 0xFF, 0x5F},
                                                          {"springgreen1", 48, 0x00, 0xFF, 0x87},
                                                          {"mediumspringgreen", 49, 0x00, 0xFF, 0xAF},
                                                          {"cyan2", 50, 0x00, 0xFF, 0xD7},
                                                          {"cyan1", 51, 0x00, 0xFF, 0xFF},
                                                          {"darkred1", 52, 0x5F, 0x00, 0x00},
                                                          {"deeppink4", 53, 0x5F, 0x00, 0x5F},
                                                          {"purple4", 54, 0x5F, 0x00, 0x87},
                                                          {"purple5", 55, 0x5F, 0x00, 0xAF},
                                                          {"purple3", 56, 0x5F, 0x00, 0xD7},
                                                          {"blueviolet", 57, 0x5F, 0x00, 0xFF},
                                                          {"orange4", 58, 0x5F, 0x5F, 0x00},
                                                          {"grey37", 59, 0x5F, 0x5F, 0x5F},
                                                          {"mediumpurple4", 60, 0x5F, 0x5F, 0x87},
                                                          {"slateblue3", 61, 0x5F, 0x5F, 0xAF},
                                                          {"slateblue4", 62, 0x5F, 0x5F, 0xD7},
                                                          {"royalblue1", 63, 0x5F, 0x5F, 0xFF},
                                                          {"chartreuse4", 64, 0x5F, 0x87, 0x00},
                                                          {"darkseagreen4", 65, 0x5F, 0x87, 0x5F},
                                                          {"paleturquoise4", 66, 0x5F, 0x87, 0x87},
                                                          {"steelblue", 67, 0x5F, 0x87, 0xAF},
                                                          {"steelblue3", 68, 0x5F, 0x87, 0xD7},
                                                          {"cornflowerblue", 69, 0x5F, 0x87, 0xFF},
                                                          {"chartreuse3", 70, 0x5F, 0xAF, 0x00},
                                                          {"darkseagreen5", 71, 0x5F, 0xAF, 0x5F},
                                                          {"cadetblue", 72, 0x5F, 0xAF, 0x87},
                                                          {"cadetblue2", 73, 0x5F, 0xAF, 0xAF},
                                                          {"skyblue3", 74, 0x5F, 0xAF, 0xD7},
                                                          {"steelblue1", 75, 0x5F, 0xAF, 0xFF},
                                                          {"chartreuse5", 76, 0x5F, 0xD7, 0x00},
                                                          {"palegreen3", 77, 0x5F, 0xD7, 0x5F},
                                                          {"seagreen3", 78, 0x5F, 0xD7, 0x87},
                                                          {"aquamarine3", 79, 0x5F, 0xD7, 0xAF},
                                                          {"mediumturquoise", 80, 0x5F, 0xD7, 0xD7},
                                                          {"steelblue2", 81, 0x5F, 0xD7, 0xFF},
                                                          {"chartreuse2", 82, 0x5F, 0xFF, 0x00},
                                                          {"seagreen2", 83, 0x5F, 0xFF, 0x5F},
                                                          {"seagreen1", 84, 0x5F, 0xFF, 0x87},
                                                          {"seagreen4", 85, 0x5F, 0xFF, 0xAF},
                                                          {"aquamarine1", 86, 0x5F, 0xFF, 0xD7},
                                                          {"darkslategray2", 87, 0x5F, 0xFF, 0xFF},
                                                          {"darkred2", 88, 0x87, 0x00, 0x00},
                                                          {"deeppink5", 89, 0x87, 0x00, 0x5F},
                                                          {"darkmagenta", 90, 0x87, 0x00, 0x87},
                                                          {"darkmagenta2", 91, 0x87, 0x00, 0xAF},
                                                          {"darkviolet1", 92, 0x87, 0x00, 0xD7},
                                                          {"purple2", 93, 0x87, 0x00, 0xFF},
                                                          {"orange5", 94, 0x87, 0x5F, 0x00},
                                                          {"lightpink4", 95, 0x87, 0x5F, 0x5F},
                                                          {"plum4", 96, 0x87, 0x5F, 0x87},
                                                          {"mediumpurple3", 97, 0x87, 0x5F, 0xAF},
                                                          {"mediumpurple5", 98, 0x87, 0x5F, 0xD7},
                                                          {"slateblue1", 99, 0x87, 0x5F, 0xFF},
                                                          {"yellow4", 100, 0x87, 0x87, 0x00},
                                                          {"wheat4", 101, 0x87, 0x87, 0x5F},
                                                          {"grey53", 102, 0x87, 0x87, 0x87},
                                                          {"lightslategrey", 103, 0x87, 0x87, 0xAF},
                                                          {"mediumpurple", 104, 0x87, 0x87, 0xD7},
                                                          {"lightslateblue", 105, 0x87, 0x87, 0xFF},
                                                          {"yellow5", 106, 0x87, 0xAF, 0x00},
                                                          {"darkolivegreen3", 107, 0x87, 0xAF, 0x5F},
                                                          {"darkseagreen", 108, 0x87, 0xAF, 0x87},
                                                          {"lightskyblue3", 109, 0x87, 0xAF, 0xAF},
                                                          {"lightskyblue4", 110, 0x87, 0xAF, 0xD7},
                                                          {"skyblue2", 111, 0x87, 0xAF, 0xFF},
                                                          {"chartreuse6", 112, 0x87, 0xD7, 0x00},
                                                          {"darkolivegreen4", 113, 0x87, 0xD7, 0x5F},
                                                          {"palegreen4", 114, 0x87, 0xD7, 0x87},
                                                          {"darkseagreen3", 115, 0x87, 0xD7, 0xAF},
                                                          {"darkslategray3", 116, 0x87, 0xD7, 0xD7},
                                                          {"skyblue1", 117, 0x87, 0xD7, 0xFF},
                                                          {"chartreuse1", 118, 0x87, 0xFF, 0x00},
                                                          {"lightgreen1", 119, 0x87, 0xFF, 0x5F},
                                                          {"lightgreen2", 120, 0x87, 0xFF, 0x87},
                                                          {"palegreen1", 121, 0x87, 0xFF, 0xAF},
                                                          {"aquamarine2", 122, 0x87, 0xFF, 0xD7},
                                                          {"darkslategray1", 123, 0x87, 0xFF, 0xFF},
                                                          {"red3", 124, 0xAF, 0x00, 0x00},
                                                          {"deeppink6", 125, 0xAF, 0x00, 0x5F},
                                                          {"mediumvioletred", 126, 0xAF, 0x00, 0x87},
                                                          {"magenta3", 127, 0xAF, 0x00, 0xAF},
                                                          {"darkviolet2", 128, 0xAF, 0x00, 0xD7},
                                                          {"purple6", 129, 0xAF, 0x00, 0xFF},
                                                          {"darkorange3", 130, 0xAF, 0x5F, 0x00},
                                                          {"indianred1", 131, 0xAF, 0x5F, 0x5F},
                                                          {"hotpink3", 132, 0xAF, 0x5F, 0x87},
                                                          {"mediumorchid3", 133, 0xAF, 0x5F, 0xAF},
                                                          {"mediumorchid", 134, 0xAF, 0x5F, 0xD7},
                                                          {"mediumpurple2", 135, 0xAF, 0x5F, 0xFF},
                                                          {"darkgoldenrod", 136, 0xAF, 0x87, 0x00},
                                                          {"lightsalmon3", 137, 0xAF, 0x87, 0x5F},
                                                          {"rosybrown", 138, 0xAF, 0x87, 0x87},
                                                          {"grey63", 139, 0xAF, 0x87, 0xAF},
                                                          {"mediumpurple6", 140, 0xAF, 0x87, 0xD7},
                                                          {"mediumpurple1", 141, 0xAF, 0x87, 0xFF},
                                                          {"gold3", 142, 0xAF, 0xAF, 0x00},
                                                          {"darkkhaki", 143, 0xAF, 0xAF, 0x5F},
                                                          {"navajowhite3", 144, 0xAF, 0xAF, 0x87},
                                                          {"grey69", 145, 0xAF, 0xAF, 0xAF},
                                                          {"lightsteelblue3", 146, 0xAF, 0xAF, 0xD7},
                                                          {"lightsteelblue", 147, 0xAF, 0xAF, 0xFF},
                                                          {"yellow3", 148, 0xAF, 0xD7, 0x00},
                                                          {"darkolivegreen5", 149, 0xAF, 0xD7, 0x5F},
                                                          {"darkseagreen6", 150, 0xAF, 0xD7, 0x87},
                                                          {"darkseagreen2", 151, 0xAF, 0xD7, 0xAF},
                                                          {"lightcyan3", 152, 0xAF, 0xD7, 0xD7},
                                                          {"lightskyblue1", 153, 0xAF, 0xD7, 0xFF},
                                                          {"greenyellow", 154, 0xAF, 0xFF, 0x00},
                                                          {"darkolivegreen2", 155, 0xAF, 0xFF, 0x5F},
                                                          {"palegreen2", 156, 0xAF, 0xFF, 0x87},
                                                          {"darkseagreen7", 157, 0xAF, 0xFF, 0xAF},
                                                          {"darkseagreen1", 158, 0xAF, 0xFF, 0xD7},
                                                          {"paleturquoise1", 159, 0xAF, 0xFF, 0xFF},
                                                          {"red4", 160, 0xD7, 0x00, 0x00},
                                                          {"deeppink3", 161, 0xD7, 0x00, 0x5F},
                                                          {"deeppink7", 162, 0xD7, 0x00, 0x87},
                                                          {"magenta4", 163, 0xD7, 0x00, 0xAF},
                                                          {"magenta5", 164, 0xD7, 0x00, 0xD7},
                                                          {"magenta2", 165, 0xD7, 0x00, 0xFF},
                                                          {"darkorange4", 166, 0xD7, 0x5F, 0x00},
                                                          {"indianred2", 167, 0xD7, 0x5F, 0x5F},
                                                          {"hotpink4", 168, 0xD7, 0x5F, 0x87},
                                                          {"hotpink2", 169, 0xD7, 0x5F, 0xAF},
                                                          {"orchid", 170, 0xD7, 0x5F, 0xD7},
                                                          {"mediumorchid1", 171, 0xD7, 0x5F, 0xFF},
                                                          {"orange3", 172, 0xD7, 0x87, 0x00},
                                                          {"lightsalmon4", 173, 0xD7, 0x87, 0x5F},
                                                          {"lightpink3", 174, 0xD7, 0x87, 0x87},
                                                          {"pink3", 175, 0xD7, 0x87, 0xAF},
                                                          {"plum3", 176, 0xD7, 0x87, 0xD7},
                                                          {"violet", 177, 0xD7, 0x87, 0xFF},
                                                          {"gold4", 178, 0xD7, 0xAF, 0x00},
                                                          {"lightgoldenrod3", 179, 0xD7, 0xAF, 0x5F},
                                                          {"tan", 180, 0xD7, 0xAF, 0x87},
                                                          {"mistyrose3", 181, 0xD7, 0xAF, 0xAF},
                                                          {"thistle3", 182, 0xD7, 0xAF, 0xD7},
                                                          {"plum2", 183, 0xD7, 0xAF, 0xFF},
                                                          {"yellow6", 184, 0xD7, 0xD7, 0x00},
                                                          {"khaki3", 185, 0xD7, 0xD7, 0x5F},
                                                          {"lightgoldenrod2", 186, 0xD7, 0xD7, 0x87},
                                                          {"lightyellow3", 187, 0xD7, 0xD7, 0xAF},
                                                          {"grey84", 188, 0xD7, 0xD7, 0xD7},
                                                          {"lightsteelblue1", 189, 0xD7, 0xD7, 0xFF},
                                                          {"yellow2", 190, 0xD7, 0xFF, 0x00},
                                                          {"darkolivegreen1", 191, 0xD7, 0xFF, 0x5F},
                                                          {"darkolivegreen6", 192, 0xD7, 0xFF, 0x87},
                                                          {"darkseagreen8", 193, 0xD7, 0xFF, 0xAF},
                                                          {"honeydew2", 194, 0xD7, 0xFF, 0xD7},
                                                          {"lightcyan1", 195, 0xD7, 0xFF, 0xFF},
                                                          {"red1", 196, 0xFF, 0x00, 0x00},
                                                          {"deeppink2", 197, 0xFF, 0x00, 0x5F},
                                                          {"deeppink1", 198, 0xFF, 0x00, 0x87},
                                                          {"deeppink8", 199, 0xFF, 0x00, 0xAF},
                                                          {"magenta6", 200, 0xFF, 0x00, 0xD7},
                                                          {"magenta1", 201, 0xFF, 0x00, 0xFF},
                                                          {"orangered1", 202, 0xFF, 0x5F, 0x00},
                                                          {"indianred3", 203, 0xFF, 0x5F, 0x5F},
                                                          {"indianred4", 204, 0xFF, 0x5F, 0x87},
                                                          {"hotpink", 205, 0xFF, 0x5F, 0xAF},
                                                          {"hotpink5", 206, 0xFF, 0x5F, 0xD7},
                                                          {"mediumorchid2", 207, 0xFF, 0x5F, 0xFF},
                                                          {"darkorange", 208, 0xFF, 0x87, 0x00},
                                                          {"salmon1", 209, 0xFF, 0x87, 0x5F},
                                                          {"lightcoral", 210, 0xFF, 0x87, 0x87},
                                                          {"palevioletred1", 211, 0xFF, 0x87, 0xAF},
                                                          {"orchid2", 212, 0xFF, 0x87, 0xD7},
                                                          {"orchid1", 213, 0xFF, 0x87, 0xFF},
                                                          {"orange1", 214, 0xFF, 0xAF, 0x00},
                                                          {"sandybrown", 215, 0xFF, 0xAF, 0x5F},
                                                          {"lightsalmon1", 216, 0xFF, 0xAF, 0x87},
                                                          {"lightpink1", 217, 0xFF, 0xAF, 0xAF},
                                                          {"pink1", 218, 0xFF, 0xAF, 0xD7},
                                                          {"plum1", 219, 0xFF, 0xAF, 0xFF},
                                                          {"gold1", 220, 0xFF, 0xD7, 0x00},
                                                          {"lightgoldenrod4", 221, 0xFF, 0xD7, 0x5F},
                                                          {"lightgoldenrod5", 222, 0xFF, 0xD7, 0x87},
                                                          {"navajowhite1", 223, 0xFF, 0xD7, 0xAF},
                                                          {"mistyrose1", 224, 0xFF, 0xD7, 0xD7},
                                                          {"thistle1", 225, 0xFF, 0xD7, 0xFF},
                                                          {"yellow1", 226, 0xFF, 0xFF, 0x00},
                                                          {"lightgoldenrod1", 227, 0xFF, 0xFF, 0x5F},
                                                          {"khaki1", 228, 0xFF, 0xFF, 0x87},
                                                          {"wheat1", 229, 0xFF, 0xFF, 0xAF},
                                                          {"cornsilk1", 230, 0xFF, 0xFF, 0xD7},
                                                          {"grey100", 231, 0xFF, 0xFF, 0xFF},
                                                          {"grey3", 232, 0x08, 0x08, 0x08},
                                                          {"grey7", 233, 0x12, 0x12, 0x12},
                                                          {"grey11", 234, 0x1C, 0x1C, 0x1C},
                                                          {"grey15", 235, 0x26, 0x26, 0x26},
                                                          {"grey19", 236, 0x30, 0x30, 0x30},
                                                          {"grey23", 237, 0x3A, 0x3A, 0x3A},
                                                          {"grey27", 238, 0x44, 0x44, 0x44},
                                                          {"grey30", 239, 0x4E, 0x4E, 0x4E},
                                                          {"grey35", 240, 0x58, 0x58, 0x58},
                                                          {"grey39", 241, 0x62, 0x62, 0x62},
                                                          {"grey42", 242, 0x6C, 0x6C, 0x6C},
                                                          {"grey46", 243, 0x76, 0x76, 0x76},
                                                          {"grey50", 244, 0x80, 0x80, 0x80},
                                                          {"grey54", 245, 0x8A, 0x8A, 0x8A},
                                                          {"grey58", 246, 0x94, 0x94, 0x94},
                                                          {"grey62", 247, 0x9E, 0x9E, 0x9E},
                                                          {"grey66", 248, 0xA8, 0xA8, 0xA8},
                                                          {"grey70", 249, 0xB2, 0xB2, 0xB2},
                                                          {"grey74", 250, 0xBC, 0xBC, 0xBC},
                                                          {"grey78", 251, 0xC6, 0xC6, 0xC6},
                                                          {"grey82", 252, 0xD0, 0xD0, 0xD0},
                                                          {"grey85", 253, 0xDA, 0xDA, 0xDA},
                                                          {"grey89", 254, 0xE4, 0xE4, 0xE4},
                                                          {"grey93", 255, 0xEE, 0xEE, 0xEE},
                                                          {nullptr, 0, 0, 0, 0}};

bool ShellHighlight::IsExtendedColor(PrintColor color) {
	if (color == PrintColor::STANDARD) {
		return false;
	}
	return color >= PrintColor::STANDARD_COLOR_COUNT;
}

optional_ptr<const HighlightColorInfo> ShellHighlight::GetColorInfo(PrintColor color) {
	if (color == PrintColor::STANDARD) {
		return nullptr;
	}
	return highlight_color_info[static_cast<uint16_t>(color)];
}

bool ShellHighlight::TryGetPrintColor(const char *name, PrintColor &result, string &error_msg) {
	for (idx_t i = 0; highlight_color_info[i].color_name; i++) {
		if (StringUtil::CIEquals(name, highlight_color_info[i].color_name)) {
			result = static_cast<PrintColor>(i);
			return true;
		}
	}
	// not found
	error_msg = StringUtil::Format("Unknown highlighting color '%s'\n", name);
	vector<string> color_names;
	for (idx_t i = 0; highlight_color_info[i].color_name; i++) {
		color_names.push_back(highlight_color_info[i].color_name);
	}
	auto candidates_msg = StringUtil::CandidatesErrorMessage(color_names, name, "Did you mean");
	error_msg += candidates_msg + "\n";
	error_msg += StringUtil::Format("Run '.display_colors' for a list of available colors.\n");
	return false;
}

string ShellHighlight::TerminalCode(PrintColor color, PrintIntensity intensity) {
	string terminal_code;
	switch (intensity) {
	case PrintIntensity::BOLD:
		terminal_code = "\033[1m";
		break;
	case PrintIntensity::ITALIC:
		terminal_code = "\033[3m";
		break;
	case PrintIntensity::BOLD_ITALIC:
		terminal_code = "\033[1m\033[3m";
		break;
	case PrintIntensity::UNDERLINE:
		terminal_code = "\033[4m";
		break;
	case PrintIntensity::BOLD_UNDERLINE:
		terminal_code = "\033[1m\033[4m";
		break;
	default:
		break;
	}
	if (color != PrintColor::STANDARD) {
		terminal_code += "\033[";
		if (color >= PrintColor::RED && color <= PrintColor::BRIGHTGRAY) {
			// standard colors have as codes \033[31m through \033[37m
			terminal_code += to_string(31 + static_cast<uint16_t>(color) - static_cast<uint16_t>(PrintColor::RED));
		} else if (color >= PrintColor::GRAY && color <= PrintColor::WHITE) {
			// bright colors have as codes \033[90m through \033[97m
			terminal_code += to_string(90 + static_cast<uint16_t>(color) - static_cast<uint16_t>(PrintColor::GRAY));
		} else {
			// extended color codes have as code \033[38;5;{code}m
			terminal_code += "38;5;" + to_string(static_cast<uint16_t>(color));
		}
		terminal_code += "m";
	}
	return terminal_code;
}

string ShellHighlight::ResetTerminalCode() {
	return "\033[00m";
}

} // namespace duckdb_shell
