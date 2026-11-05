#include "history.hpp"
#include "linenoise.hpp"
#include "terminal.hpp"
#include "utf8proc_wrapper.hpp"
#include "shell_state.hpp"
#include <sys/stat.h>

#if defined(_WIN32) || defined(WIN32)
#define strdup _strdup
#endif

namespace duckdb {
#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 1000
static idx_t history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static idx_t history_len = 0;
static char **history = nullptr;
static char *history_file = nullptr;

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
void History::Free() {
	if (history) {
		for (idx_t j = 0; j < history_len; j++) {
			free(history[j]);
		}
		free(history);
	}
}

idx_t History::GetLength() {
	return history_len;
}

const char *History::GetEntry(idx_t index) {
	if (!history || index >= history_len) {
		// FIXME: print debug message
		return "";
	}
	return history[index];
}

void History::Overwrite(idx_t index, const char *new_entry) {
	if (!history || index >= history_len) {
		// FIXME: print debug message
		return;
	}

	free(history[index]);
	history[index] = strdup(new_entry);
}

void History::RemoveLastEntry() {
	history_len--;
	free(history[history_len]);
}

int History::Add(const char *line) {
	return History::Add(line, strlen(line));
}

int History::Add(const char *line, idx_t len) {
	char *linecopy;

	if (history_max_len == 0) {
		return 0;
	}

	/* Initialization on first call. */
	if (history == nullptr) {
		history = (char **)malloc(sizeof(char *) * history_max_len);
		if (history == nullptr) {
			return 0;
		}
		memset(history, 0, (sizeof(char *) * history_max_len));
	}

	/* Don't add duplicated lines. */
	if (history_len && !strcmp(history[history_len - 1], line)) {
		return 0;
	}

	if (!Utf8Proc::IsValid(line, len)) {
		// don't add invalid UTF8 to history
		return 0;
	}

	/* Add an heap allocated copy of the line in the history.
	 * If we reached the max length, remove the older line. */
	if (!Terminal::IsMultiline()) {
		// replace all newlines with spaces
		linecopy = strdup(line);
		if (!linecopy) {
			return 0;
		}
		for (auto ptr = linecopy; *ptr; ptr++) {
			if (*ptr == '\n' || *ptr == '\r') {
				*ptr = ' ';
			}
		}
	} else {
		// replace all '\n' with '\r\n'
		idx_t replaced_newline_count = 0;
		idx_t len;
		for (len = 0; line[len]; len++) {
			if (line[len] == '\r' && line[len + 1] == '\n') {
				// \r\n - skip past the \n
				len++;
			} else if (line[len] == '\n') {
				replaced_newline_count++;
			}
		}
		linecopy = (char *)malloc((len + replaced_newline_count + 1) * sizeof(char));
		idx_t pos = 0;
		for (len = 0; line[len]; len++) {
			if (line[len] == '\r' && line[len + 1] == '\n') {
				// \r\n - skip past the \n
				linecopy[pos++] = '\r';
				len++;
			} else if (line[len] == '\n') {
				linecopy[pos++] = '\r';
			}
			linecopy[pos++] = line[len];
		}
		linecopy[pos] = '\0';
	}
	if (history_len == history_max_len) {
		free(history[0]);
		memmove(history, history + 1, sizeof(char *) * (history_max_len - 1));
		history_len--;
	}
	history[history_len] = linecopy;
	history_len++;
	if (history_file && strlen(line) > 0) {
		// if there is a history file that we loaded from
		// append to the history
		// this way we can recover history in case of a crash
		FILE *fp;

		fp = fopen(history_file, "a");
		if (fp == nullptr) {
			return 1;
		}
		fprintf(fp, "%s\n", line);
		fclose(fp);
	}
	return 1;
}

int History::SetMaxLength(idx_t len) {
	char **new_entry;

	if (len < 1) {
		return 0;
	}
	if (history) {
		idx_t tocopy = history_len;

		new_entry = (char **)malloc(sizeof(char *) * len);
		if (new_entry == nullptr) {
			return 0;
		}

		/* If we can't copy everything, free the elements we'll not use. */
		if (len < tocopy) {
			for (idx_t j = 0; j < tocopy - len; j++) {
				free(history[j]);
			}
			tocopy = len;
		}
		memset(new_entry, 0, sizeof(char *) * len);
		memcpy(new_entry, history + (history_len - tocopy), sizeof(char *) * tocopy);
		free(history);
		history = new_entry;
	}
	history_max_len = len;
	if (history_len > history_max_len) {
		history_len = history_max_len;
	}
	return 1;
}

int History::Save(const char *filename) {
#if !defined(_WIN32) && !defined(WIN32)
	mode_t old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
#endif
	FILE *fp;

	fp = fopen(filename, "w");
#if !defined(_WIN32) && !defined(WIN32)
	umask(old_umask);
#endif
	if (fp == nullptr) {
		return -1;
	}
#if !defined(_WIN32) && !defined(WIN32)
	chmod(filename, S_IRUSR | S_IWUSR);
#endif
	for (idx_t j = 0; j < history_len; j++) {
		fprintf(fp, "%s\n", history[j]);
	}
	fclose(fp);
	return 0;
}

struct LineReader {
	static constexpr idx_t LINE_BUFFER_SIZE = LINENOISE_MAX_LINE * 2ULL;

public:
	LineReader() : fp(nullptr), filename(nullptr), end_of_file(false), position(0), capacity(0), total_read(0) {
		line_buffer[LINENOISE_MAX_LINE] = '\0';
		data_buffer[LINE_BUFFER_SIZE] = '\0';
	}

	bool Init(const char *filename_p) {
		filename = filename_p;
		fp = fopen(filename, "r");
		return fp;
	}

	void Close() {
		if (fp) {
			fclose(fp);
			fp = nullptr;
		}
	}

	const char *GetLine() {
		return line_buffer;
	}

	idx_t GetNextNewline() {
		for (idx_t i = position; i < capacity; i++) {
			if (data_buffer[i] == '\r' || data_buffer[i] == '\n') {
				return i;
			}
		}
		return capacity;
	}

	void SkipNewline() {
		if (position >= capacity) {
			// we are already at the end - fill the buffer
			FillBuffer();
		}
		if (position < capacity && data_buffer[position] == '\n') {
			position++;
		}
	}

	bool NextLine() {
		idx_t line_size = 0;
		while (true) {
			// find the next newline in the current buffer (if any)
			idx_t i = GetNextNewline();
			// copy over the data and move to the next byte
			idx_t read_count = i - position;
			if (line_size + read_count > LINENOISE_MAX_LINE) {
				// exceeded max line size
				// move on to next line and don't add to history
				// skip to next newline
				bool found_next_newline = false;
				while (!found_next_newline && capacity > 0) {
					i = GetNextNewline();
					if (i < capacity) {
						found_next_newline = true;
					}
					if (!found_next_newline) {
						// read more data
						FillBuffer();
					}
				}
				if (!found_next_newline) {
					// no newline found - skip
					return false;
				}
				// newline found - adjust position and read next line
				position = i + 1;
				if (data_buffer[i] == '\r') {
					// \r\n - skip the next byte as well
					SkipNewline();
				}
				continue;
			}
			memcpy(line_buffer + line_size, data_buffer + position, read_count);
			line_size += read_count;

			if (i < capacity) {
				// we're still within the buffer - this means we found a newline in the buffer
				line_buffer[line_size] = '\0';
				position = i + 1;
				if (data_buffer[i] == '\r') {
					// \r\n - skip the next byte as well
					SkipNewline();
				}
				if (line_size == 0 || !Utf8Proc::IsValid(line_buffer, line_size)) {
					// line is empty OR not valid UTF8
					// move on to next line and don't add to history
					line_size = 0;
					continue;
				}
				return true;
			}
			// we need to read more data - fill up the buffer
			FillBuffer();
			if (capacity == 0) {
				// no more data available - return true if there is anything we copied over (i.e. part of the next line)
				return line_size > 0;
			}
		}
	}

	void FillBuffer() {
		if (end_of_file || !fp) {
			return;
		}
		size_t read_data = fread(data_buffer, 1, LINE_BUFFER_SIZE, fp);
		position = 0;
		capacity = read_data;
		total_read += read_data;
		data_buffer[read_data] = '\0';

		if (read_data == 0) {
			end_of_file = true;
		}
		if (total_read >= LINENOISE_MAX_HISTORY) {
			fprintf(stderr, "History file \"%s\" exceeds maximum history file size of %d MB - skipping full load\n",
			        filename, LINENOISE_MAX_HISTORY / 1024 / 1024);
			capacity = 0;
			end_of_file = true;
		}
	}

private:
	FILE *fp;
	const char *filename;
	char line_buffer[LINENOISE_MAX_LINE + 1];
	char data_buffer[LINE_BUFFER_SIZE + 1];
	bool end_of_file;
	idx_t position;
	idx_t capacity;
	idx_t total_read;
};

int History::Load(const char *filename) {
	LineReader reader;
	if (!reader.Init(filename)) {
		return -1;
	}

	std::string result;
	while (reader.NextLine()) {
		auto buf = reader.GetLine();
		if (result.empty() && buf[0] == '.') {
			// if the first character is a dot this is a dot command
			// add the full line to the history
			History::Add(buf);
			continue;
		}
		// else we are parsing a SQL statement
		result += buf;
		if (duckdb_shell::ShellState::SQLIsComplete(result.c_str())) {
			// this line contains a full SQL statement - add it to the history
			History::Add(result.c_str(), result.size());
			result = std::string();
			continue;
		}
		// the result does not contain a full SQL statement - add a newline deliminator and move on to the next line
		result += "\r\n";
	}
	reader.Close();

	history_file = strdup(filename);
	return 0;
}

} // namespace duckdb
