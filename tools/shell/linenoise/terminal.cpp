#include "terminal.hpp"
#include "history.hpp"
#include "linenoise.hpp"
#if defined(_WIN32) || defined(WIN32)
#include <io.h>
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define isatty        _isatty
#define read          _read
#define write         _write
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

namespace duckdb {

static int mlmode = 1; /* Multi line mode. Default is multi line. */
#if defined(_WIN32) || defined(WIN32)
static HANDLE console_in = nullptr;
static DWORD old_mode;
#else
static struct termios orig_termios; /* In order to restore at exit.*/
#endif
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int rawmode = 0;           /* For atexit() function to check if restore is needed*/
static bool mouse_tracking = false;
static const char *unsupported_term[] = {"dumb", "cons25", "emacs", NULL};

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
	Terminal::DisableRawMode();
	History::Free();
}

#if defined(_WIN32) || defined(WIN32)
HANDLE Terminal::GetConsoleInput() {
	return console_in;
}
#endif

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
int Terminal::IsUnsupportedTerm() {
#if defined(_WIN32) || defined(WIN32)
#else
	char *term = getenv("TERM");
	int j;

	if (!term) {
		return 0;
	}
	for (j = 0; unsupported_term[j]; j++) {
		if (!strcasecmp(term, unsupported_term[j])) {
			return 1;
		}
	}
#endif
	return 0;
}

#if !defined(_WIN32) && !defined(WIN32)
int EnableRawModeInternal(int fd) {
	if (tcgetattr(fd, &orig_termios) == -1) {
		errno = ENOTTY;
		return -1;
	}

	auto raw = orig_termios; /* modify the original mode */
	/* input modes: no break, no CR to NL, no parity check, no strip char,
	 * no start/stop output control. */
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	/* output modes - disable post processing */
	raw.c_oflag &= ~(OPOST);
#ifdef IUTF8
	/* control modes - set 8 bit chars */
	raw.c_iflag |= IUTF8;
#endif
	raw.c_cflag |= CS8;
	/* local modes - echoing off, canonical off, no extended functions,
	 * no signal chars (^Z,^C) */
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	/* control chars - set return condition: min number of bytes and timer.
	 * We want read to return every single byte, without timeout. */
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

	/* put terminal in raw mode after flushing */
	if (tcsetattr(fd, TCSADRAIN, &raw) < 0) {
		errno = ENOTTY;
		return -1;
	}
	rawmode = 1;
	return 0;
}

void DisableRawModeInternal(int fd) {
	/* Don't even check the return value as it's too late. */
	if (rawmode && tcsetattr(fd, TCSADRAIN, &orig_termios) != -1) {
		rawmode = 0;
	}
}

#endif

/* Raw mode: 1960 magic shit. */
int Terminal::EnableRawMode() {
#if defined(_WIN32) || defined(WIN32)
	if (console_in) {
		// already in raw mode
		return 0;
	}
	console_in = GetStdHandle(STD_INPUT_HANDLE);

	GetConsoleMode(console_in, &old_mode);
	auto new_mode = old_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
	SetConsoleMode(console_in, new_mode);
#else
	int fd = STDIN_FILENO;

	if (!isatty(fd)) {
		errno = ENOTTY;
		return -1;
	}
	if (!atexit_registered) {
		atexit(linenoiseAtExit);
		atexit_registered = 1;
	}
	return EnableRawModeInternal(fd);
#endif
	return 0;
}

void Terminal::DisableRawMode() {
	Terminal::DisableMouseTracking();
#if defined(_WIN32) || defined(WIN32)
	if (console_in) {
		// restore old mode
		SetConsoleMode(console_in, old_mode);
		console_in = nullptr;
	}
#else
	int fd = STDIN_FILENO;
	DisableRawModeInternal(fd);
#endif
}

void Terminal::EnableMouseTracking() {
#if !defined(_WIN32) && !defined(WIN32)
	if (!rawmode) {
		return;
	}
	// Enable XTerm mouse tracking (normal tracking)
	printf("\x1b[?1000h");
	fflush(stdout);
	mouse_tracking = true;
#endif
}
void Terminal::DisableMouseTracking() {
#if !defined(_WIN32) && !defined(WIN32)
	if (!mouse_tracking) {
		return;
	}
	printf("\x1b[?1000l");
	fflush(stdout);
	mouse_tracking = false;
#endif
}

bool Terminal::IsMultiline() {
	return mlmode;
}

bool Terminal::IsAtty() {
	return isatty(STDIN_FILENO);
}

/* This function is called when linenoise() is called with the standard
 * input file descriptor not attached to a TTY. So for example when the
 * program using linenoise is called in pipe or with a file redirected
 * to its standard input. In this case, we want to be able to return the
 * line regardless of its length (by default we are limited to 4k). */
char *Terminal::EditNoTTY() {
	char *line = NULL;
	size_t len = 0, maxlen = 0;

	while (1) {
		if (len == maxlen) {
			if (maxlen == 0)
				maxlen = 16;
			maxlen *= 2;
			char *oldval = line;
			line = (char *)realloc(line, maxlen);
			if (line == NULL) {
				if (oldval)
					free(oldval);
				return NULL;
			}
		}
		int c = fgetc(stdin);
		if (c == EOF || c == '\n') {
			if (c == EOF && len == 0) {
				free(line);
				return NULL;
			} else {
				line[len] = '\0';
				return line;
			}
		} else {
			line[len] = c;
			len++;
		}
	}
}

/* This function calls the line editing function linenoiseEdit() using
 * the STDIN file descriptor set in raw mode. */
int Terminal::EditRaw(char *buf, size_t buflen, const char *prompt) {
	int count;

	if (buflen == 0) {
		errno = EINVAL;
		return -1;
	}

	if (Terminal::EnableRawMode() == -1) {
		return -1;
	}
	Linenoise l(STDIN_FILENO, STDOUT_FILENO, buf, buflen, prompt);
	count = l.Edit();
	Terminal::BufferAvailableInput();
	Terminal::DisableRawMode();
	printf("\n");
	return count;
}

// returns true if there is more data available to read in a particular stream
int Terminal::HasMoreData(int fd, idx_t timeout_micros) {
#if defined(_WIN32) || defined(WIN32)
	return false;
#else
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	struct timeval tv;
	tv.tv_sec = static_cast<time_t>(timeout_micros / 1000000);
	tv.tv_usec = static_cast<int>(timeout_micros % 1000000);
	return select(fd + 1, &rfds, NULL, NULL, &tv);
#endif
}

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void Terminal::SetMultiLine(int ml) {
	mlmode = ml;
}

static int parseInt(const char *s, int *offset = nullptr) {
	int result = 0;
	int idx;
	for (idx = 0; s[idx]; idx++) {
		char c = s[idx];
		if (c < '0' || c > '9') {
			break;
		}
		result = result * 10 + c - '0';
		if (result > 1000000) {
			result = 1000000;
		}
	}
	if (offset) {
		*offset = idx;
	}
	return result;
}

static int tryParseEnv(const char *env_var) {
	char *s;
	s = getenv(env_var);
	if (!s) {
		return 0;
	}
	return parseInt(s);
}

/* Use the ESC [6n escape sequence to query the cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
TerminalSize Terminal::GetCursorPosition() {
	int ifd = STDIN_FILENO;
	int ofd = STDOUT_FILENO;
	TerminalSize ws;

	char buf[32];
	unsigned int i = 0;

	/* Report cursor location */
	if (write(ofd, "\x1b[6n", 4) != 4) {
		return ws;
	}

	/* Read the response: ESC [ rows ; cols R */
	while (i < sizeof(buf) - 1) {
		if (read(ifd, buf + i, 1) != 1) {
			break;
		}
		if (buf[i] == 'R') {
			break;
		}
		i++;
	}
	buf[i] = '\0';

	/* Parse it. */
	if (buf[0] != ESC || buf[1] != '[') {
		return ws;
	}
	int offset = 2;
	int new_offset;
	ws.ws_row = parseInt(buf + offset, &new_offset);
	offset += new_offset;
	if (buf[offset] != ';') {
		return ws;
	}
	offset++;
	ws.ws_col = parseInt(buf + offset);
	return ws;
}

TerminalSize Terminal::TryMeasureTerminalSize() {
	int ofd = STDOUT_FILENO;
	/* ioctl() failed. Try to query the terminal itself. */
	TerminalSize start, result;

	/* Get the initial position so we can restore it later. */
	start = GetCursorPosition();
	if (!start.ws_col) {
		return result;
	}

	/* Go to bottom-right margin */
	if (write(ofd, "\x1b[999;999f", 10) != 10) {
		return result;
	}
	result = GetCursorPosition();
	if (!result.ws_col) {
		return result;
	}

	/* Restore position. */
	char seq[32];
	snprintf(seq, 32, "\x1b[%d;%df", start.ws_row, start.ws_col);
	if (write(ofd, seq, strlen(seq)) == -1) {
		/* Can't recover... */
	}
	return result;
}

bool ParseTerminalColor(TerminalColor &color, const char *buf, idx_t buflen) {
	/* Parse it. */
	// expected format is: \x1b]11;rgb:1e1e/1e1e/1e1e
	static const char rgb_format[] = "\x1b]11;rgb:";
	idx_t rgb_length = sizeof(rgb_format) - 1;
	idx_t offset;
	for (offset = 0; offset + rgb_length < buflen; offset++) {
		if (memcmp(buf + offset, rgb_format, rgb_length) == 0) {
			break;
		}
		// not part of the rgb code - buffer the keypress
		BufferedKeyPresses::BufferKeyPress((KEY_ACTION)buf[offset]);
	}
	// now parse the actual r/g/b values
	offset += rgb_length;
	if (offset >= buflen) {
		return false;
	}
	uint8_t values[3];
	memset(values, 0, sizeof(values));

	for (idx_t k = 0; k < 3; k++) {
		if (k > 0) {
			// expected a "/"
			if (offset >= buflen || buf[offset] != '/') {
				return false;
			}
			offset++;
		}
		// parse the hexadecimal value
		// note that these values are from 0...65535, not from 0...255
		uint32_t value = 0;
		idx_t end_pos = offset + 4;
		for (; offset < end_pos; offset++) {
			if (offset >= buflen) {
				return false;
			}
			auto c = buf[offset];
			if (c == '/') {
				// found a slash early - done
				break;
			}
			uint32_t current_value;
			if (c >= 'A' && c <= 'F') {
				current_value = 10 + (c - 'A');
			} else if (c >= 'a' && c <= 'f') {
				current_value = 10 + (c - 'a');
			} else if (c >= '0' && c <= '9') {
				current_value = c - '0';
			} else {
				// unsupported hex value
				return false;
			}
			value = value * 16 + current_value;
		}
		// normalize from
		values[k] = static_cast<uint8_t>(value >> 8);
	}
	// found the r/g/b
	color.r = values[0];
	color.g = values[1];
	color.b = values[2];
	return true;
}

void Terminal::BufferAvailableInput() {
	// consume available input and add it to the buffer
	int ifd = STDIN_FILENO;
	if (!isatty(ifd)) {
		return;
	}
	while (HasMoreData(ifd)) {
		char buf[1];
		if (read(ifd, buf, 1) != 1) {
			break;
		}
		BufferedKeyPresses::BufferKeyPress((KEY_ACTION)buf[0]);
	}
}

bool Terminal::TryGetBackgroundColor(TerminalColor &color) {
#if defined(_WIN32) || defined(WIN32)
	// FIXME: always emit black background on Windows
	color.r = 0;
	color.g = 0;
	color.b = 0;
	return true;
#else
	int ifd = STDIN_FILENO;
	int ofd = STDOUT_FILENO;
	if (!isatty(ifd)) {
		// if stdin is not the terminal - we need to open stdin manually
		ifd = open("/dev/tty", O_RDWR);
		if (ifd < 0) {
			// failed to open stdin
			return false;
		}
		ofd = ifd;
	}

	if (EnableRawModeInternal(ifd) == -1) {
		if (ifd != STDIN_FILENO) {
			close(ifd);
		}
		return false;
	}

	bool success = false;
	// We send the OSC 11 query to request the background color, followed by a DA1 ("Primary Device Attributes")
	// Terminals answer queries in order, and virtually every terminal responds to DA1 - but not every
	// terminal responds to OSC 11. When we receive the DA1 response without a preceding OSC 11 response, we know
	// the terminal does not support OSC 11 - instead of blocking on a response that will never come.
	if (write(ofd, "\x1b]11;?\007\x1b[c", 10) == 10) {
		// Read the response: until the DA1 response ("\x1b[?<params>c")
		string buf;
		char read_buf[1];
		bool found_da1 = false;
		idx_t da1_start = 0;
		while (true) {
			// wait up until 5s
			if (!HasMoreData(ifd, 5000000)) {
				// we didn't get data for 5s...
				fprintf(stderr, "Timeout trying to read terminal background color (> 5s elapsed).\n");
				fprintf(
				    stderr,
				    "Disable terminal background color detection by using duckdb -dark-mode or duckdb -light-mode.\n");
				fprintf(stderr, "This likely means duckdb does not correctly support your CLI.\n");
				fprintf(stderr, "Please file an issue.\n");
				break;
			}
			if (read(ifd, read_buf, 1) != 1) {
				break;
			}
			char c = read_buf[0];
			buf += c;
			if (!found_da1) {
				if (buf.size() >= 3 && buf.compare(buf.size() - 3, 3, "\x1b[?") == 0) {
					// found the start of the DA1 response
					found_da1 = true;
					da1_start = buf.size() - 3;
				}
			} else if (c == 'c') {
				// the DA1 response is complete - stop reading
				break;
			}
		}
		// the OSC 11 response (if any) is everything before the DA1 response
		idx_t response_size = found_da1 ? da1_start : buf.size();
		success = ParseTerminalColor(color, buf.c_str(), response_size);
	}
	DisableRawModeInternal(ifd);
	if (ifd != STDIN_FILENO) {
		close(ifd);
	}
	return success;
#endif
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
TerminalSize Terminal::GetTerminalSize() {
	TerminalSize result;

#if defined(_WIN32) || defined(WIN32)
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	int rows;

	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	result.ws_col = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	result.ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
	// try ioctl first
	{
		struct winsize ws;
		ioctl(1, TIOCGWINSZ, &ws);
		result.ws_col = ws.ws_col;
		result.ws_row = ws.ws_row;
	}
	// try ROWS and COLUMNS env variables
	if (!result.ws_col) {
		result.ws_col = tryParseEnv("COLUMNS");
	}
	if (!result.ws_row) {
		result.ws_row = tryParseEnv("ROWS");
	}
	// if those fail measure the size by moving the cursor to the corner and fetching the position
	if (!result.ws_col || !result.ws_row) {
		TerminalSize measured_size = TryMeasureTerminalSize();
		Linenoise::Log("measured size col %d,row %d -- ", measured_size.ws_row, measured_size.ws_col);
		if (measured_size.ws_row) {
			result.ws_row = measured_size.ws_row;
		}
		if (measured_size.ws_col) {
			result.ws_col = measured_size.ws_col;
		}
	}
	// if all else fails use defaults (80,24)
	if (!result.ws_col) {
		result.ws_col = 80;
	}
	if (!result.ws_row) {
		result.ws_row = 24;
	}
#endif
	return result;
}

/* Clear the screen. Used to handle ctrl+l */
void Terminal::ClearScreen() {
	if (write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) <= 0) {
		/* nothing to do, just to avoid warning. */
	}
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
void Terminal::Beep() {
	fprintf(stderr, "\x7");
	fflush(stderr);
}

EscapeSequence Terminal::ReadEscapeSequence(int ifd, KeyPress &key_press) {
	char seq[5];
	idx_t length = ReadEscapeSequence(ifd, seq);
	if (length == 0) {
		return EscapeSequence::INVALID;
	}
	Linenoise::Log("escape of length %d\n", length);
	switch (length) {
	case 1:
		if (seq[0] >= 'a' && seq[0] <= 'z') {
			return EscapeSequence(idx_t(EscapeSequence::ALT_A) + (seq[0] - 'a'));
		}
		if (seq[0] >= 'A' && seq[0] <= 'Z') {
			return EscapeSequence(idx_t(EscapeSequence::ALT_A) + (seq[0] - 'A'));
		}
		switch (seq[0]) {
		case BACKSPACE:
			return EscapeSequence::ALT_BACKSPACE;
		case ESC: {
			// Double ESC - this might be ALT + arrow key
			// Read the next escape sequence
			auto next_escape = ReadEscapeSequence(ifd, key_press);
			switch (next_escape) {
			case EscapeSequence::LEFT:
				return EscapeSequence::ALT_LEFT_ARROW;
			case EscapeSequence::RIGHT:
				return EscapeSequence::ALT_RIGHT_ARROW;
			default:
				// Not an arrow key, just return ESCAPE
				return EscapeSequence::ESCAPE;
			}
		}
		case '<':
			return EscapeSequence::ALT_LEFT_ARROW;
		case '>':
			return EscapeSequence::ALT_RIGHT_ARROW;
		case '\\':
			return EscapeSequence::ALT_BACKSLASH;
		default:
			Linenoise::Log("unrecognized escape sequence of length 1 - %d\n", seq[0]);
			break;
		}
		break;
	case 2:
		if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'A': /* Up */
				return EscapeSequence::UP;
			case 'B': /* Down */
				return EscapeSequence::DOWN;
			case 'C': /* Right */
				return EscapeSequence::RIGHT;
			case 'D': /* Left */
				return EscapeSequence::LEFT;
			case 'H': /* Home */
				return EscapeSequence::HOME;
			case 'F': /* End*/
				return EscapeSequence::END;
			case 'c':
				return EscapeSequence::ALT_F;
			case 'd':
				return EscapeSequence::ALT_B;
			default:
				Linenoise::Log("unrecognized escape sequence (O) %d\n", seq[1]);
				break;
			}
		} else if (seq[0] == '[') {
			switch (seq[1]) {
			case 'A': /* Up */
				return EscapeSequence::UP;
			case 'B': /* Down */
				return EscapeSequence::DOWN;
			case 'C': /* Right */
				return EscapeSequence::RIGHT;
			case 'D': /* Left */
				return EscapeSequence::LEFT;
			case 'H': /* Home */
				return EscapeSequence::HOME;
			case 'F': /* End*/
				return EscapeSequence::END;
			case 'Z': /* Shift Tab */
				return EscapeSequence::SHIFT_TAB;
			default:
				Linenoise::Log("unrecognized escape sequence (seq[1]) %d\n", seq[1]);
				break;
			}
		} else {
			Linenoise::Log("unrecognized escape sequence of length %d (%d %d)\n", length, seq[0], seq[1]);
		}
		break;
	case 3:
		if (seq[2] == '~') {
			switch (seq[1]) {
			case '1':
				return EscapeSequence::HOME;
			case '3': /* Delete key. */
				return EscapeSequence::DELETE_KEY;
			case '4':
			case '8':
				return EscapeSequence::END;
			default:
				Linenoise::Log("unrecognized escape sequence (~) %d\n", seq[1]);
				break;
			}
		} else if (seq[1] == '5' && seq[2] == 'C') {
			return EscapeSequence::ALT_F;
		} else if (seq[1] == '5' && seq[2] == 'D') {
			return EscapeSequence::ALT_B;
		} else {
			Linenoise::Log("unrecognized escape sequence of length %d\n", length);
		}
		break;
	case 5:
		if (memcmp(seq, "[1;5A", 5) == 0) {
			// [1;5A: ctrl-up
			return EscapeSequence::CTRL_UP;
		} else if (memcmp(seq, "[1;5B", 5) == 0) {
			// [1;5B: ctrl-down
			return EscapeSequence::CTRL_DOWN;
		} else if (memcmp(seq, "[1;5C", 5) == 0 || memcmp(seq, "[1;3C", 5) == 0) {
			// [1;5C: move word right
			return EscapeSequence::CTRL_MOVE_FORWARDS;
		} else if (memcmp(seq, "[1;5D", 5) == 0 || memcmp(seq, "[1;3D", 5) == 0) {
			// [1;5D: move word left
			return EscapeSequence::CTRL_MOVE_BACKWARDS;
		} else if (memcmp(seq, "[M", 2) == 0) {
			// mouse event - consume it
			EscapeSequence result_sequence = EscapeSequence::UNKNOWN;
			if (seq[2] == ' ') {
				// left mouse click
				result_sequence = EscapeSequence::MOUSE_CLICK;
				// get the co-ordinates, these are X + 32, Y + 32
				key_press.position.ws_col = static_cast<uint8_t>(seq[3]);
				key_press.position.ws_row = static_cast<uint8_t>(seq[4]);
				if (key_press.position.ws_col <= 32 || key_press.position.ws_row < 32) {
					// out of bounds of the terminal
					result_sequence = EscapeSequence::UNKNOWN;
				} else {
					key_press.position.ws_col -= 33;
					key_press.position.ws_row -= 32;
				}
			}
			// get the cursor position and subtract it from the key press location
			auto cursor_position = Terminal::GetCursorPosition();
			key_press.position.ws_col -= cursor_position.ws_col;
			key_press.position.ws_row -= cursor_position.ws_row;
			// disable mouse tracking again - we only consume one mouse event at a time
			Terminal::DisableMouseTracking();
			return result_sequence;
		} else {
			Linenoise::Log("unrecognized escape sequence (;) %d\n", seq[1]);
		}
		break;
	default:
		Linenoise::Log("unrecognized escape sequence of length %d\n", length);
		break;
	}
	return EscapeSequence::UNKNOWN;
}

idx_t Terminal::ReadEscapeSequence(int ifd, char seq[]) {
	if (read(ifd, seq, 1) == -1) {
		return 0;
	}
	switch (seq[0]) {
	case 'O':
	case '[':
		// these characters have multiple bytes following them
		break;
	default:
		return 1;
	}
	if (read(ifd, seq + 1, 1) == -1) {
		return 0;
	}

	if (seq[0] != '[') {
		return 2;
	}
	if (seq[1] == 'M') {
		// mouse event - read 3 more bytes
		if (read(ifd, seq + 2, 3) == -1) {
			return 0;
		}
		return 5;
	}
	if (seq[1] < '0' || seq[1] > '9') {
		return 2;
	}
	/* Extended escape, read additional byte. */
	if (read(ifd, seq + 2, 1) == -1) {
		return 0;
	}
	if (seq[2] == ';') {
		// read 2 extra bytes
		if (read(ifd, seq + 3, 2) == -1) {
			return 0;
		}
		return 5;
	} else {
		return 3;
	}
}

} // namespace duckdb
