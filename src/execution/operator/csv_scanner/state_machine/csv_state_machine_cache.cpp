#include "duckdb/execution/operator/csv_scanner/csv_state_machine.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine_cache.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"

namespace duckdb {

void InitializeTransitionArray(StateMachine &transition_array, const CSVState cur_state, const CSVState state) {
	for (uint32_t i = 0; i < StateMachine::NUM_TRANSITIONS; i++) {
		transition_array[i][static_cast<uint8_t>(cur_state)] = state;
	}
}

//! The block scanner assumes these transitions, a state machine whose table differs anywhere stays on the byte loop
static bool BlockScanEligible(const StateMachine &transition_array, const CSVStateMachineOptions &options) {
	const auto &delimiter = options.delimiter.GetValue();
	const auto new_line = options.new_line.GetValue();
	const auto quote = static_cast<uint8_t>(options.quote.GetValue());
	const auto escape = static_cast<uint8_t>(options.escape.GetValue());
	const auto comment = static_cast<uint8_t>(options.comment.GetValue());
	if (delimiter.empty() || delimiter.size() > 4 || (escape != '\0' && escape != quote)) {
		return false;
	}
	if (comment != '\0' &&
	    (comment == static_cast<uint8_t>(delimiter[0]) || comment == quote || comment == '\n' || comment == '\r')) {
		return false;
	}
	if (new_line != NewLineIdentifier::SINGLE_N && new_line != NewLineIdentifier::CARRY_ON &&
	    new_line != NewLineIdentifier::SINGLE_R) {
		return false;
	}
	const auto delimiter_byte = static_cast<uint8_t>(delimiter[0]);
	// a multi byte delimiter walks through the delimiter byte states, the block scan checks the whole delimiter at once
	const CSVState delimiter_byte_states[] = {CSVState::DELIMITER_FIRST_BYTE, CSVState::DELIMITER_SECOND_BYTE,
	                                          CSVState::DELIMITER_THIRD_BYTE, CSVState::DELIMITER};
	const auto first_byte_state = delimiter.size() == 1 ? CSVState::DELIMITER : CSVState::DELIMITER_FIRST_BYTE;
	for (idx_t i = 1; i < delimiter.size(); i++) {
		const auto from = static_cast<uint8_t>(delimiter_byte_states[i - 1]);
		const auto to = i + 1 == delimiter.size() ? CSVState::DELIMITER : delimiter_byte_states[i];
		if (transition_array[static_cast<uint8_t>(delimiter[i])][from] != to) {
			return false;
		}
	}
	if (transition_array[delimiter_byte][static_cast<uint8_t>(CSVState::UNQUOTED)] != first_byte_state) {
		return false;
	}
	const CSVState from_states[] = {CSVState::STANDARD, CSVState::DELIMITER, CSVState::RECORD_SEPARATOR,
	                                CSVState::EMPTY_SPACE, CSVState::NOT_SET};
	for (const auto from : from_states) {
		const auto from_idx = static_cast<uint8_t>(from);
		if (transition_array[delimiter_byte][from_idx] != first_byte_state) {
			return false;
		}
		if (new_line == NewLineIdentifier::SINGLE_N && transition_array['\n'][from_idx] != CSVState::RECORD_SEPARATOR) {
			return false;
		}
		if (new_line == NewLineIdentifier::CARRY_ON && transition_array['\r'][from_idx] != CSVState::CARRIAGE_RETURN) {
			return false;
		}
		if (new_line == NewLineIdentifier::SINGLE_R && transition_array['\r'][from_idx] != CSVState::RECORD_SEPARATOR) {
			return false;
		}
		if (comment != '\0' && transition_array[comment][from_idx] != CSVState::COMMENT) {
			return false;
		}
		const bool after_separator = from != CSVState::STANDARD && from != CSVState::EMPTY_SPACE;
		for (uint32_t byte = 0; byte < StateMachine::NUM_TRANSITIONS; byte++) {
			if (byte == delimiter_byte || byte == '\n' || byte == '\r' || (quote != '\0' && byte == quote) ||
			    (comment != '\0' && byte == comment)) {
				continue;
			}
			const auto expected = byte == ' ' && after_separator ? CSVState::EMPTY_SPACE : CSVState::STANDARD;
			if (transition_array[byte][from_idx] != expected) {
				return false;
			}
		}
	}
	if (new_line == NewLineIdentifier::CARRY_ON &&
	    transition_array['\n'][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] != CSVState::RECORD_SEPARATOR) {
		return false;
	}
	if (comment != '\0') {
		const auto in_comment = static_cast<uint8_t>(CSVState::COMMENT);
		if (transition_array['\n'][in_comment] != CSVState::RECORD_SEPARATOR) {
			return false;
		}
		if (transition_array['\r'][in_comment] !=
		    (new_line == NewLineIdentifier::CARRY_ON ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR)) {
			return false;
		}
		for (uint32_t byte = 0; byte < StateMachine::NUM_TRANSITIONS; byte++) {
			if (byte != '\n' && byte != '\r' && transition_array[byte][in_comment] != CSVState::COMMENT) {
				return false;
			}
		}
	}
	if (quote == '\0') {
		return true;
	}
	// the quoted value transitions the block scanner assumes
	const auto standard = static_cast<uint8_t>(CSVState::STANDARD);
	const auto quoted = static_cast<uint8_t>(CSVState::QUOTED);
	const auto quoted_new_line = static_cast<uint8_t>(CSVState::QUOTED_NEW_LINE);
	const auto unquoted = static_cast<uint8_t>(CSVState::UNQUOTED);
	if (transition_array[quote][standard] != CSVState::STANDARD) {
		return false;
	}
	const CSVState opening_states[] = {CSVState::DELIMITER, CSVState::RECORD_SEPARATOR, CSVState::EMPTY_SPACE,
	                                   CSVState::NOT_SET};
	for (const auto from : opening_states) {
		if (transition_array[quote][static_cast<uint8_t>(from)] != CSVState::QUOTED) {
			return false;
		}
	}
	if (transition_array[quote][quoted] != CSVState::UNQUOTED ||
	    transition_array[quote][quoted_new_line] != CSVState::UNQUOTED) {
		return false;
	}
	if (transition_array['\n'][quoted] != CSVState::QUOTED_NEW_LINE ||
	    transition_array['\r'][quoted] != CSVState::QUOTED_NEW_LINE) {
		return false;
	}
	if (transition_array['\n'][quoted_new_line] != CSVState::QUOTED ||
	    transition_array['\r'][quoted_new_line] != CSVState::QUOTED) {
		return false;
	}
	for (uint32_t byte = 0; byte < StateMachine::NUM_TRANSITIONS; byte++) {
		if (byte == quote || byte == '\n' || byte == '\r') {
			continue;
		}
		if (transition_array[byte][quoted] != CSVState::QUOTED ||
		    transition_array[byte][quoted_new_line] != CSVState::QUOTED) {
			return false;
		}
	}
	if (new_line == NewLineIdentifier::SINGLE_N && transition_array['\n'][unquoted] != CSVState::RECORD_SEPARATOR) {
		return false;
	}
	if (new_line == NewLineIdentifier::CARRY_ON && transition_array['\r'][unquoted] != CSVState::CARRIAGE_RETURN) {
		return false;
	}
	if (new_line == NewLineIdentifier::SINGLE_R && transition_array['\r'][unquoted] != CSVState::RECORD_SEPARATOR) {
		return false;
	}
	return escape != quote || transition_array[quote][unquoted] == CSVState::QUOTED;
}

void CSVStateMachineCache::Insert(const CSVStateMachineOptions &state_machine_options) {
	D_ASSERT(state_machine_cache.find(state_machine_options) == state_machine_cache.end());
	// Initialize transition array with default values to the Standard option
	auto &transition_array = state_machine_cache[state_machine_options];

	for (uint32_t i = 0; i < StateMachine::NUM_STATES; i++) {
		const auto cur_state = static_cast<CSVState>(i);
		switch (cur_state) {
		case CSVState::MAYBE_QUOTED:
		case CSVState::QUOTED:
		case CSVState::QUOTED_NEW_LINE:
		case CSVState::ESCAPE:
			InitializeTransitionArray(transition_array, cur_state, CSVState::QUOTED);
			break;
		case CSVState::UNQUOTED:
			if (state_machine_options.strict_mode.GetValue()) {
				// If we have an unquoted state, following rfc 4180, our base state is invalid
				InitializeTransitionArray(transition_array, cur_state, CSVState::INVALID);
			} else {
				// This will allow us to accept unescaped quotes
				InitializeTransitionArray(transition_array, cur_state, CSVState::UNQUOTED);
			}
			break;
		case CSVState::COMMENT:
			InitializeTransitionArray(transition_array, cur_state, CSVState::COMMENT);
			break;
		case CSVState::CARRIAGE_RETURN:
			if (state_machine_options.strict_mode.GetValue()) {
				// If we have an unquoted state, following rfc 4180, our base state is invalid
				InitializeTransitionArray(transition_array, cur_state, CSVState::INVALID);
			} else {
				// This will allow us to accept unescaped quotes
				InitializeTransitionArray(transition_array, cur_state, CSVState::STANDARD);
			}
			break;
		default:
			InitializeTransitionArray(transition_array, cur_state, CSVState::STANDARD);
			break;
		}
	}

	const auto delimiter_value = state_machine_options.delimiter.GetValue();
	uint8_t delimiter_first_byte;
	if (!delimiter_value.empty()) {
		delimiter_first_byte = static_cast<uint8_t>(delimiter_value[0]);
	} else {
		delimiter_first_byte = static_cast<uint8_t>('\0');
	}
	const auto quote = static_cast<uint8_t>(state_machine_options.quote.GetValue());
	const auto escape = static_cast<uint8_t>(state_machine_options.escape.GetValue());
	const auto comment = static_cast<uint8_t>(state_machine_options.comment.GetValue());

	const auto new_line_id = state_machine_options.new_line.GetValue();

	const bool multi_byte_delimiter = delimiter_value.size() > 1;

	const bool enable_unquoted_escape = state_machine_options.strict_mode.GetValue() == false &&
	                                    state_machine_options.quote != state_machine_options.escape &&
	                                    state_machine_options.escape != '\0';
	// Now set values depending on configuration
	// 1) Standard/Invalid State
	const vector<uint8_t> std_inv {static_cast<uint8_t>(CSVState::STANDARD), static_cast<uint8_t>(CSVState::INVALID),
	                               static_cast<uint8_t>(CSVState::STANDARD_NEWLINE)};
	for (const auto &state : std_inv) {
		if (multi_byte_delimiter) {
			transition_array[delimiter_first_byte][state] = CSVState::DELIMITER_FIRST_BYTE;
		} else {
			transition_array[delimiter_first_byte][state] = CSVState::DELIMITER;
		}
		if (new_line_id == NewLineIdentifier::CARRY_ON) {
			transition_array[static_cast<uint8_t>('\r')][state] = CSVState::CARRIAGE_RETURN;
			if (state == static_cast<uint8_t>(CSVState::STANDARD_NEWLINE)) {
				transition_array[static_cast<uint8_t>('\n')][state] = CSVState::STANDARD;
			} else if (!state_machine_options.strict_mode.GetValue()) {
				transition_array[static_cast<uint8_t>('\n')][state] = CSVState::RECORD_SEPARATOR;
			} else {
				transition_array[static_cast<uint8_t>('\n')][state] = CSVState::INVALID;
			}
		} else if (new_line_id == NewLineIdentifier::SINGLE_N) {
			transition_array[static_cast<uint8_t>('\n')][state] = CSVState::RECORD_SEPARATOR;
			if (!state_machine_options.strict_mode.GetValue()) {
				transition_array[static_cast<uint8_t>('\r')][state] = CSVState::RECORD_SEPARATOR;
			} else {
				transition_array[static_cast<uint8_t>('\r')][state] = CSVState::INVALID;
			}
		} else if (new_line_id == NewLineIdentifier::SINGLE_R) {
			transition_array[static_cast<uint8_t>('\r')][state] = CSVState::RECORD_SEPARATOR;
			if (!state_machine_options.strict_mode.GetValue()) {
				transition_array[static_cast<uint8_t>('\n')][state] = CSVState::RECORD_SEPARATOR;
			} else {
				transition_array[static_cast<uint8_t>('\n')][state] = CSVState::INVALID;
			}
		} else {
			transition_array[static_cast<uint8_t>('\r')][state] = CSVState::RECORD_SEPARATOR;
			transition_array[static_cast<uint8_t>('\n')][state] = CSVState::RECORD_SEPARATOR;
		}

		if (comment != '\0') {
			transition_array[comment][state] = CSVState::COMMENT;
		}
		if (enable_unquoted_escape) {
			transition_array[escape][state] = CSVState::UNQUOTED_ESCAPE;
		}
	}
	// 2) Field Separator State
	if (quote != '\0') {
		transition_array[quote][static_cast<uint8_t>(CSVState::DELIMITER)] = CSVState::QUOTED;
	}
	if (delimiter_first_byte != ' ') {
		transition_array[' '][static_cast<uint8_t>(CSVState::DELIMITER)] = CSVState::EMPTY_SPACE;
	}

	const vector<uint8_t> delimiter_states {
	    static_cast<uint8_t>(CSVState::DELIMITER), static_cast<uint8_t>(CSVState::DELIMITER_FIRST_BYTE),
	    static_cast<uint8_t>(CSVState::DELIMITER_SECOND_BYTE), static_cast<uint8_t>(CSVState::DELIMITER_THIRD_BYTE)};

	// These are the same transitions for all delimiter states
	for (auto &state : delimiter_states) {
		if (multi_byte_delimiter) {
			transition_array[delimiter_first_byte][state] = CSVState::DELIMITER_FIRST_BYTE;
		} else {
			transition_array[delimiter_first_byte][state] = CSVState::DELIMITER;
		}
		transition_array[static_cast<uint8_t>('\n')][state] = CSVState::RECORD_SEPARATOR;
		if (new_line_id == NewLineIdentifier::CARRY_ON) {
			transition_array[static_cast<uint8_t>('\r')][state] = CSVState::CARRIAGE_RETURN;
		} else {
			transition_array[static_cast<uint8_t>('\r')][state] = CSVState::RECORD_SEPARATOR;
		}
		if (comment != '\0') {
			transition_array[comment][static_cast<uint8_t>(CSVState::DELIMITER)] = CSVState::COMMENT;
		}
	}
	// Deal other multi-byte delimiters
	if (delimiter_value.size() == 2) {
		transition_array[static_cast<uint8_t>(delimiter_value[1])]
		                [static_cast<uint8_t>(CSVState::DELIMITER_FIRST_BYTE)] = CSVState::DELIMITER;
	} else if (delimiter_value.size() == 3) {
		if (delimiter_first_byte == delimiter_value[1]) {
			transition_array[static_cast<uint8_t>(delimiter_value[1])]
			                [static_cast<uint8_t>(CSVState::DELIMITER_SECOND_BYTE)] = CSVState::DELIMITER_SECOND_BYTE;
		}
		transition_array[static_cast<uint8_t>(delimiter_value[1])]
		                [static_cast<uint8_t>(CSVState::DELIMITER_FIRST_BYTE)] = CSVState::DELIMITER_SECOND_BYTE;
		transition_array[static_cast<uint8_t>(delimiter_value[2])]
		                [static_cast<uint8_t>(CSVState::DELIMITER_SECOND_BYTE)] = CSVState::DELIMITER;
	} else if (delimiter_value.size() == 4) {
		if (delimiter_first_byte == delimiter_value[2]) {
			transition_array[static_cast<uint8_t>(delimiter_value[1])]
			                [static_cast<uint8_t>(CSVState::DELIMITER_THIRD_BYTE)] = CSVState::DELIMITER_SECOND_BYTE;
		}
		if (delimiter_first_byte == delimiter_value[1] && delimiter_value[1] == delimiter_value[2]) {
			transition_array[static_cast<uint8_t>(delimiter_value[1])]
			                [static_cast<uint8_t>(CSVState::DELIMITER_THIRD_BYTE)] = CSVState::DELIMITER_THIRD_BYTE;
		}
		transition_array[static_cast<uint8_t>(delimiter_value[1])]
		                [static_cast<uint8_t>(CSVState::DELIMITER_FIRST_BYTE)] = CSVState::DELIMITER_SECOND_BYTE;
		transition_array[static_cast<uint8_t>(delimiter_value[2])]
		                [static_cast<uint8_t>(CSVState::DELIMITER_SECOND_BYTE)] = CSVState::DELIMITER_THIRD_BYTE;
		transition_array[static_cast<uint8_t>(delimiter_value[3])]
		                [static_cast<uint8_t>(CSVState::DELIMITER_THIRD_BYTE)] = CSVState::DELIMITER;
	}
	if (enable_unquoted_escape) {
		transition_array[escape][static_cast<uint8_t>(CSVState::DELIMITER)] = CSVState::UNQUOTED_ESCAPE;
	}

	// 3) Record Separator State
	if (multi_byte_delimiter) {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] =
		    CSVState::DELIMITER_FIRST_BYTE;
	} else {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] = CSVState::DELIMITER;
	}
	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] =
	    CSVState::RECORD_SEPARATOR;
	if (new_line_id == NewLineIdentifier::CARRY_ON) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] =
		    CSVState::CARRIAGE_RETURN;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] =
		    CSVState::RECORD_SEPARATOR;
	}
	if (quote != '\0') {
		transition_array[quote][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] = CSVState::QUOTED;
	}
	if (delimiter_first_byte != ' ') {
		transition_array[' '][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] = CSVState::EMPTY_SPACE;
	}
	if (comment != '\0') {
		transition_array[comment][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		transition_array[escape][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] = CSVState::UNQUOTED_ESCAPE;
	}
	if (state_machine_options.strict_mode.GetValue()) {
		// strict rules to error on the new line delimiter
		switch (new_line_id) {
		case NewLineIdentifier::CARRY_ON:
		case NewLineIdentifier::SINGLE_R:
			transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] =
			    CSVState::INVALID;
			break;
		case NewLineIdentifier::SINGLE_N:
			transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::RECORD_SEPARATOR)] =
			    CSVState::INVALID;
			break;
		default:
			break;
		}
	}

	// 4) Carriage Return State
	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] =
	    CSVState::RECORD_SEPARATOR;
	if (state_machine_options.strict_mode.GetValue()) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] =
		    CSVState::INVALID;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] =
		    CSVState::CARRIAGE_RETURN;
	}

	if (quote != '\0') {
		transition_array[quote][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] = CSVState::QUOTED;
	}
	if (delimiter_first_byte != ' ') {
		transition_array[' '][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] = CSVState::EMPTY_SPACE;
	}
	if (comment != '\0') {
		transition_array[comment][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		transition_array[escape][static_cast<uint8_t>(CSVState::CARRIAGE_RETURN)] = CSVState::UNQUOTED_ESCAPE;
	}

	// 5) Quoted State
	transition_array[quote][static_cast<uint8_t>(CSVState::QUOTED)] = CSVState::UNQUOTED;
	transition_array['\n'][static_cast<uint8_t>(CSVState::QUOTED)] = CSVState::QUOTED_NEW_LINE;
	transition_array['\r'][static_cast<uint8_t>(CSVState::QUOTED)] = CSVState::QUOTED_NEW_LINE;

	if (state_machine_options.quote != state_machine_options.escape &&
	    state_machine_options.escape.GetValue() != '\0') {
		transition_array[escape][static_cast<uint8_t>(CSVState::QUOTED)] = CSVState::ESCAPE;
	}
	// 6) Unquoted State
	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::RECORD_SEPARATOR;
	if (new_line_id == NewLineIdentifier::CARRY_ON) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::UNQUOTED)] =
		    CSVState::CARRIAGE_RETURN;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::UNQUOTED)] =
		    CSVState::RECORD_SEPARATOR;
	}
	if (multi_byte_delimiter) {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::UNQUOTED)] =
		    CSVState::DELIMITER_FIRST_BYTE;
	} else {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::DELIMITER;
	}
	if (state_machine_options.quote == state_machine_options.escape) {
		transition_array[quote][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::QUOTED;
	}
	if (state_machine_options.strict_mode.GetValue()) {
		// strict rules to error on the new line delimiter
		switch (new_line_id) {
		case NewLineIdentifier::CARRY_ON:
		case NewLineIdentifier::SINGLE_R:
			transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::INVALID;
			break;
		case NewLineIdentifier::SINGLE_N:
			transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::INVALID;
			break;
		default:
			break;
		}
	} else {
		if (escape == '\0') {
			// If escape is defined, it limits a bit how relaxed quotes can be in a reliable way.
			transition_array[quote][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::MAYBE_QUOTED;
		} else {
			transition_array[quote][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::QUOTED;
		}
	}
	if (comment != '\0') {
		transition_array[comment][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::COMMENT;
	}
	if (delimiter_first_byte != ' ' && quote != ' ' && escape != ' ' && comment != ' ') {
		// If space is not a special character, we can safely ignore it in an unquoted state
		transition_array[' '][static_cast<uint8_t>(CSVState::UNQUOTED)] = CSVState::UNQUOTED;
	}

	// 8) Not Set
	if (multi_byte_delimiter) {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::NOT_SET)] =
		    CSVState::DELIMITER_FIRST_BYTE;
	} else {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::NOT_SET)] = CSVState::DELIMITER;
	}
	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::NOT_SET)] = CSVState::RECORD_SEPARATOR;
	if (new_line_id == NewLineIdentifier::CARRY_ON) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::NOT_SET)] =
		    CSVState::CARRIAGE_RETURN;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::NOT_SET)] =
		    CSVState::RECORD_SEPARATOR;
	}
	if (quote != '\0') {
		transition_array[quote][static_cast<uint8_t>(CSVState::NOT_SET)] = CSVState::QUOTED;
	}
	if (delimiter_first_byte != ' ') {
		transition_array[' '][static_cast<uint8_t>(CSVState::NOT_SET)] = CSVState::EMPTY_SPACE;
	}
	if (comment != '\0') {
		transition_array[comment][static_cast<uint8_t>(CSVState::NOT_SET)] = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		transition_array[escape][static_cast<uint8_t>(CSVState::NOT_SET)] = CSVState::UNQUOTED_ESCAPE;
	}

	// 9) Quoted NewLine
	transition_array[quote][static_cast<uint8_t>(CSVState::QUOTED_NEW_LINE)] = CSVState::UNQUOTED;
	if (state_machine_options.quote != state_machine_options.escape &&
	    state_machine_options.escape.GetValue() != '\0') {
		transition_array[escape][static_cast<uint8_t>(CSVState::QUOTED_NEW_LINE)] = CSVState::ESCAPE;
	}

	// 10) Empty Value State (Not first value)
	if (multi_byte_delimiter) {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] =
		    CSVState::DELIMITER_FIRST_BYTE;
	} else {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] = CSVState::DELIMITER;
	}
	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] =
	    CSVState::RECORD_SEPARATOR;
	if (new_line_id == NewLineIdentifier::CARRY_ON) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] =
		    CSVState::CARRIAGE_RETURN;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] =
		    CSVState::RECORD_SEPARATOR;
	}
	if (quote != '\0') {
		transition_array[quote][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] = CSVState::QUOTED;
	}
	if (comment != '\0') {
		transition_array[comment][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] = CSVState::COMMENT;
	}
	if (enable_unquoted_escape) {
		transition_array[escape][static_cast<uint8_t>(CSVState::EMPTY_SPACE)] = CSVState::UNQUOTED_ESCAPE;
	}

	// 11) Comment State
	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::COMMENT)] = CSVState::RECORD_SEPARATOR;
	if (new_line_id == NewLineIdentifier::CARRY_ON) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::COMMENT)] =
		    CSVState::CARRIAGE_RETURN;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::COMMENT)] =
		    CSVState::RECORD_SEPARATOR;
	}

	// 12) Unquoted Escape State
	if (enable_unquoted_escape) {
		// Any character can be escaped, so default to STANDARD
		if (new_line_id == NewLineIdentifier::CARRY_ON) {
			transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::UNQUOTED_ESCAPE)] =
			    CSVState::ESCAPED_RETURN;
		}
	}

	// 13) Escaped Return State
	if (enable_unquoted_escape) {
		// The new state is STANDARD for \r + \n and \r + ordinary character.
		// Other special characters need to be handled.
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::ESCAPED_RETURN)] = CSVState::DELIMITER;
		if (new_line_id == NewLineIdentifier::CARRY_ON) {
			transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::ESCAPED_RETURN)] =
			    CSVState::CARRIAGE_RETURN;
		} else {
			transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::ESCAPED_RETURN)] =
			    CSVState::RECORD_SEPARATOR;
		}
		if (comment != '\0') {
			transition_array[comment][static_cast<uint8_t>(CSVState::ESCAPED_RETURN)] = CSVState::COMMENT;
		}
		transition_array[escape][static_cast<uint8_t>(CSVState::ESCAPED_RETURN)] = CSVState::UNQUOTED_ESCAPE;
	}

	// 14) Maybe quoted
	transition_array[quote][static_cast<uint8_t>(CSVState::MAYBE_QUOTED)] = CSVState::MAYBE_QUOTED;

	transition_array[static_cast<uint8_t>('\n')][static_cast<uint8_t>(CSVState::MAYBE_QUOTED)] =
	    CSVState::RECORD_SEPARATOR;
	if (new_line_id == NewLineIdentifier::CARRY_ON) {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::MAYBE_QUOTED)] =
		    CSVState::CARRIAGE_RETURN;
	} else {
		transition_array[static_cast<uint8_t>('\r')][static_cast<uint8_t>(CSVState::MAYBE_QUOTED)] =
		    CSVState::RECORD_SEPARATOR;
	}
	if (multi_byte_delimiter) {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::MAYBE_QUOTED)] =
		    CSVState::DELIMITER_FIRST_BYTE;
	} else {
		transition_array[delimiter_first_byte][static_cast<uint8_t>(CSVState::MAYBE_QUOTED)] = CSVState::DELIMITER;
	}

	// Initialize characters we can skip during processing, for Standard and Quoted states
	for (idx_t i = 0; i < StateMachine::NUM_TRANSITIONS; i++) {
		transition_array.skip_standard[i] = true;
		transition_array.skip_quoted[i] = true;
		transition_array.skip_comment[i] = true;
	}
	// For standard states we only care for delimiters \r and \n
	transition_array.skip_standard[delimiter_first_byte] = false;
	transition_array.skip_standard[static_cast<uint8_t>('\n')] = false;
	transition_array.skip_standard[static_cast<uint8_t>('\r')] = false;
	transition_array.skip_standard[comment] = false;
	if (enable_unquoted_escape) {
		transition_array.skip_standard[escape] = false;
	}

	// For quoted we only care about quote, escape and for delimiters \r and \n
	transition_array.skip_quoted[quote] = false;
	transition_array.skip_quoted[escape] = false;
	transition_array.skip_quoted[static_cast<uint8_t>('\n')] = false;
	transition_array.skip_quoted[static_cast<uint8_t>('\r')] = false;

	transition_array.skip_comment[static_cast<uint8_t>('\r')] = false;
	transition_array.skip_comment[static_cast<uint8_t>('\n')] = false;

	auto &stop_patterns = transition_array.stop_patterns;
	stop_patterns.push_back(SwarBlock::BytePattern::Byte(delimiter_first_byte));
	if (quote != '\0') {
		stop_patterns.push_back(SwarBlock::BytePattern::Byte(quote));
	}
	stop_patterns.push_back(SwarBlock::BytePattern::Either('\n', '\r'));
	if (comment != '\0') {
		stop_patterns.push_back(SwarBlock::BytePattern::Byte(comment));
	}
	if (escape != '\0' && escape != quote) {
		stop_patterns.push_back(SwarBlock::BytePattern::Byte(escape));
	}
	transition_array.block_scan = BlockScanEligible(transition_array, state_machine_options);
}

CSVStateMachineCache::CSVStateMachineCache() {
	auto default_quote_escape = DialectCandidates::GetDefaultQuoteEscapeCombination();
	auto default_delimiter = DialectCandidates::GetDefaultDelimiter();
	auto default_comment = DialectCandidates::GetDefaultComment();

	for (auto quote_escape : default_quote_escape) {
		for (const auto &delimiter : default_delimiter) {
			for (const auto &comment : default_comment) {
				for (const bool strict_mode : {true, false}) {
					Insert({delimiter, quote_escape.quote, quote_escape.escape, comment, NewLineIdentifier::SINGLE_N,
					        strict_mode});
					Insert({delimiter, quote_escape.quote, quote_escape.escape, comment, NewLineIdentifier::SINGLE_R,
					        strict_mode});
					Insert({delimiter, quote_escape.quote, quote_escape.escape, comment, NewLineIdentifier::CARRY_ON,
					        strict_mode});
				}
			}
		}
	}
}

const StateMachine &CSVStateMachineCache::Get(const CSVStateMachineOptions &state_machine_options) {
	// Custom State Machine, we need to create it and cache it first
	const lock_guard<mutex> parallel_lock(main_mutex);
	if (state_machine_cache.find(state_machine_options) == state_machine_cache.end()) {
		Insert(state_machine_options);
	}
	const auto &transition_array = state_machine_cache[state_machine_options];
	return transition_array;
}

CSVStateMachineCache &CSVStateMachineCache::Get(ClientContext &context) {
	auto &cache = ObjectCache::GetObjectCache(context);
	return *cache.GetOrCreate<CSVStateMachineCache>(CSVStateMachineCache::ObjectType());
}

} // namespace duckdb
