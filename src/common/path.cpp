#include "duckdb/common/path.hpp"

#include "duckdb/common/algorithm.hpp" // avoid MSVC warning on find_if_not
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// cleanly handle both separator types
static inline bool IsPathSeparator(char c) {
	return c == '/' || c == '\\';
}

// Appends "normal" segments, ignores ".", and consumes trailing segment via "..", popping "<segment>/.."
static void MaybeAppendSegment(vector<string> &segments, string::const_iterator begin, string::const_iterator end) {
	if (end == begin || (end == begin + 1 && *begin == '.')) {
		return;
	}
	const string segment(begin, end);
	if (segment == ".." && !segments.empty() && segments.back() != "..") {
		segments.pop_back();
	} else {
		segments.push_back(segment);
	}
}

static void SegmentAndNormalizePath(string::const_iterator begin, string::const_iterator end,
                                    vector<string> &segments) {
	auto prev_pos = begin;
	for (auto pos = prev_pos; pos != end; pos++) {
		if (IsPathSeparator(*pos)) {
			MaybeAppendSegment(segments, prev_pos, pos);
			prev_pos = pos + 1;
		}
	}
	MaybeAppendSegment(segments, prev_pos, end);
}

// ---------------------------------------------------------------------------
// Public methods (order matches path.hpp)
// ---------------------------------------------------------------------------

Path Path::FromString(const string &raw) {
	Path parsed;
#if defined(_WIN32)
	const auto first_slash_pos = raw.find_first_of(R"(/\)");
#else
	const auto first_slash_pos = raw.find('/');
#endif
	const auto scheme_pos = raw.find("://");
	const auto drive_leads = (false
#if defined(_WIN32)
	                          || (first_slash_pos == 1 && StringUtil::CharacterIsAlpha(raw[0]))
#endif
	);
	parsed.separator = first_slash_pos == string::npos ? '/' : raw[first_slash_pos];

	size_t path_offset;
	if (StringUtil::StartsWith(raw, "file:/")) {
		path_offset = ParseFileSchemes(raw, parsed);
	}
#if defined(_WIN32)
	else if (raw.size() >= 5 && StringUtil::StartsWith(raw, R"(\\)") && raw.find_first_of('\\', 3) != string::npos) {
		path_offset = ParseUNCScheme(raw, parsed);
	}
#endif
	else if (scheme_pos != string::npos && scheme_pos > 1 && scheme_pos < first_slash_pos && !drive_leads) {
		path_offset = ParseURIScheme(raw, parsed);
	} else {
		path_offset = ParseFilePathTail(raw, 0, parsed);
	}
	parsed.NormalizeSegments(raw, path_offset);
	if (!raw.empty() && IsPathSeparator(raw.back())) {
		parsed.has_trailing_separator = true;
	}
	D_ASSERT(parsed.HasScheme() || !parsed.HasAuthority());
	D_ASSERT(parsed.anchor.size() <= 4);
	return parsed;
}

string Path::ToString() const {
	string result = scheme + authority + anchor;
	for (size_t i = 0; i < segments.size(); i++) {
		if (i) {
			result += separator;
		}
		result += segments[i];
	}
	if (has_trailing_separator && !segments.empty()) {
		result += separator;
	}
	if (result.empty()) {
		result = '.';
	}
	return result;
}

string Path::GetBase() const {
	return scheme + authority + anchor;
}

string Path::GetPath() const {
	if (segments.empty()) {
		return (scheme.empty() && anchor.empty()) ? "." : "";
	}
	string result;
	for (size_t i = 0; i < segments.size(); i++) {
		if (i) {
			result += separator;
		}
		result += segments[i];
	}
	return result;
}

bool Path::HasDrive() const {
	return GetDriveChar() != 0;
}

char Path::GetDriveChar() const {
	const auto size = anchor.size();
	D_ASSERT(size <= 4);
	// must be one of {"C:", "C:\" or "\C:", "\C:\"}; switch on size & ':'
	if (size <= 1) {
		D_ASSERT(size == 0 || IsPathSeparator(anchor[0]));
		return 0;
	} else if (size == 2) {
		return anchor[0];
	} else if (size == 3) {
		return anchor[anchor[1] == ':' ? 0 : 1];
	} else /* (size == 4) */ {
		return anchor[1];
	}
}

Path Path::Join(const Path &rhs) const {
	Path lhs = *this;
#if defined(_WIN32)
	const bool win_local = (lhs.separator == '\\') || lhs.HasDrive();
	const auto auth_is_eq =
	    win_local ? StringUtil::CIEquals(lhs.authority, rhs.authority) : (lhs.authority == rhs.authority);
	const auto seg_prefix =
	    win_local ? (rhs.segments.size() >= lhs.segments.size() &&
	                 std::equal(lhs.segments.begin(), lhs.segments.end(), rhs.segments.begin(),
	                            [](const string &a, const string &b) { return StringUtil::CIEquals(a, b); }))
	              : (rhs.segments.size() >= lhs.segments.size() &&
	                 std::equal(lhs.segments.begin(), lhs.segments.end(), rhs.segments.begin()));
#else
	const auto auth_is_eq = (lhs.authority == rhs.authority);
	const auto seg_prefix = (rhs.segments.size() >= lhs.segments.size() &&
	                         std::equal(lhs.segments.begin(), lhs.segments.end(), rhs.segments.begin()));
#endif
	if (!rhs.IsAbsolute() && !rhs.HasDrive()) {
		lhs.segments.insert(lhs.segments.end(), rhs.segments.begin(), rhs.segments.end());
		vector<string> result;
		result.reserve(lhs.segments.size());
		for (auto &seg : lhs.segments) {
			if (seg == ".." && !result.empty() && result.back() != "..") {
				result.pop_back();
			} else {
				result.push_back(std::move(seg));
			}
		}
		while (lhs.IsAbsolute() && !result.empty() && result.front() == "..") {
			result.erase(result.begin());
		}
		lhs.segments = std::move(result);
	} else if (auth_is_eq && seg_prefix && lhs.scheme == rhs.scheme && lhs.anchor == rhs.anchor) {
		if (lhs.segments.size() < rhs.segments.size()) {
			lhs.segments = rhs.segments;
		}
	} else {
		throw InvalidInputException("Path: cannot join incompatible paths: \"%s\" onto \"%s\"", rhs.ToString(),
		                            lhs.ToString());
	}
	lhs.has_trailing_separator = rhs.has_trailing_separator;
	return lhs;
}

Path Path::Join(const string &rhs) const {
	return Join(Path::FromString(rhs));
}

// NOTE: instead of chopping off segments, apply '..'; this guards against traps with odd paths (leading ..,
// etc.) to make sure we always handle Parentage consistently, at a slight cost of cpu/tmp allocation.
Path Path::Parent(int n) const {
	if (n <= 0) {
		return *this;
	}
	string dots = "..";
	for (int i = 1; i < n; i++) {
		dots += "/..";
	}
	return Join(dots);
}

// ---------------------------------------------------------------------------
// Private methods (order matches path.hpp)
// ---------------------------------------------------------------------------

void Path::NormalizeSegments(const string &raw, size_t path_offset) {
	// normalize URI schemes to lowercase; UNC prefixes (\\, \\?, \\?\UNC\) are not URI schemes
	if (!scheme.empty() && !IsPathSeparator(scheme[0])) {
		scheme = StringUtil::Lower(scheme);
	}

	if (HasAnchor()) {
		string normal;
		if (IsPathSeparator(anchor[0])) {
			normal.append(1, separator);
		}
		auto drive_char = std::find_if_not(anchor.begin(), anchor.end(), IsPathSeparator);
		if (drive_char != anchor.end()) {
			normal.append(1, StringUtil::CharacterToUpper(*drive_char));
			normal.append(1, ':');
			if (IsPathSeparator(anchor.back())) {
				normal.append(1, separator);
			}
		}
		anchor = normal;
	}

	vector<string> raw_segs;
	D_ASSERT(path_offset <= raw.size());
	SegmentAndNormalizePath(raw.begin() + static_cast<string::difference_type>(path_offset), raw.end(), raw_segs);
	segments.clear();
	for (auto &seg : raw_segs) {
		if (IsAbsolute() && segments.empty() && seg == "..") {
			continue; // drop leading ".." on absolute paths (can't go above root)
		}
		segments.push_back(std::move(seg));
	}
}

//
// Parse path (only) proto://authority/path/to/file.txt
// - All URI paths are absolute, and URIs of the form proto://authority will be assigned path="/"
// - Not full URI parsing, only for URI paths (e.g., no query, fragment, etc.)
//
size_t Path::ParseURIScheme(const string &input, Path &parsed) {
	parsed.is_absolute = true;

	const size_t auth_begin = input.find("://") + 3;
	D_ASSERT(auth_begin >= 4); // non-empty protocol
	parsed.scheme = input.substr(0, auth_begin);

	const size_t path_begin = input.find('/', auth_begin);
	D_ASSERT(path_begin == string::npos || path_begin >= auth_begin);
	parsed.authority = input.substr(auth_begin, path_begin == string::npos ? string::npos : path_begin - auth_begin);
	parsed.anchor = '/';
	return path_begin == string::npos ? input.size() : path_begin + 1;
}

size_t Path::ParseFilePathTail(const string &input, size_t start, Path &parsed) {
	size_t pos = start;

#if defined(_WIN32)
	if (input.size() >= pos + 2 && input[pos + 1] == ':' && StringUtil::CharacterIsAlpha(input[pos])) {
		parsed.anchor.append(1, input[pos]);
		parsed.anchor.append(1, ':');
		pos += 2;
	}
#endif

	if (input.size() > pos && IsPathSeparator(input[pos])) {
		parsed.anchor.append(1, parsed.separator);
		parsed.is_absolute = true;
		pos += 1;
	}
	return pos;
}

//
// Parse and normalize file:/{1,3} schemes. See
// https://en.wikipedia.org/wiki/File_URI_scheme and our docs below.
//
// DuckDB supports using the file: protocol. It currently supports the following formats:
//
//  file:/some/path (host omitted completely)
//  file:///some/path (empty host)
//  file://localhost/some/path (localhost as host)
//
// Note that the following formats are not supported because they are non-standard:
//
//   file:some/relative/path (relative path)
//   file://some/path (double-slash path)
//
// Additionally, the file: protocol does not support remote (non-localhost) hosts.
//
size_t Path::ParseFileSchemes(const string &input, Path &parsed) {
	parsed.is_absolute = true;

	size_t input_len = input.size();
	size_t path_begin = string::npos;
	D_ASSERT(input_len >= 6 && StringUtil::StartsWith(input, "file:/"));

	if (/* file:/// */ input_len >= 8 && input[6] == '/' && input[7] == '/') {
		parsed.scheme = "file://";
		parsed.anchor = "/";
		path_begin = 8;
	} else if (/* file:// */ input_len >= 7 && input[6] == '/') {
		// NOTE: the anchor may be synthesized, so the offset must come from ParseURIScheme, not from the sizes
		path_begin = ParseURIScheme(input, parsed);
		if (StringUtil::Lower(parsed.authority) != "localhost") {
			throw InvalidInputException("Path: file:// scheme only supports localhost authority, got: %s",
			                            parsed.authority);
		}
	} else /* file:/ */ {
		parsed.scheme = "file:";
		parsed.anchor = "/";
		path_begin = 6;
	}

	D_ASSERT(parsed.scheme == "file:" || parsed.scheme == "file://");
	D_ASSERT(parsed.scheme == "file://" || !parsed.HasAuthority());
	return ParseFilePathTail(input, path_begin, parsed);
}

#if defined(_WIN32)
//
// UNC Scheme as we handle it:
// - scheme = "\\"
// - authority = server\share
// - anchor = "\"
// - path = < the rest >
//
size_t Path::ParseUNCScheme(const string &input, Path &parsed) {
	D_ASSERT(input.size() >= 4 && input[0] == '\\' && input[1] == '\\');

	static const char extended_prefix[] = R"(\\?\)";
	static const char extended_unc_prefix[] = R"(\\?\UNC\)";
	const bool extended = StringUtil::StartsWith(input, extended_prefix);
	const bool extended_unc = extended && StringUtil::StartsWith(input, extended_unc_prefix);

	parsed.separator = '\\';

	if (extended && !extended_unc) {
		parsed.anchor = '\\';
		parsed.scheme = R"(\\?)";
		return ParseFilePathTail(input, 4, parsed);
	}

	const auto server_begin = extended_unc ? sizeof(extended_unc_prefix) - 1 : 2;
	const auto share_begin = input.find_first_of('\\', server_begin) + 1;
	auto pos = input.find_first_of("/\\", share_begin);
	const auto share_len = (pos == string::npos ? input.size() : pos) - share_begin;
	if (share_begin - server_begin <= 1 || share_len == 0) {
		throw InvalidInputException("Path: UNC path missing server\\share: %s", input);
	}

	parsed.scheme = extended_unc ? extended_unc_prefix : R"(\\)";
	parsed.authority = input.substr(server_begin, share_begin + share_len - server_begin);
	parsed.anchor = '\\';
	parsed.is_absolute = true;
	return (pos == string::npos || pos + 1 >= input.size()) ? input.size() : pos + 1;
}
#endif

string Path::AddSuffixToPath(const string &path, const string &suffix) {
	// a remote URL may carry a "?query" suffix, which has to stay at the very end of the path
	// in a local path a question mark is a regular character
	auto scheme = path.find("://");
	if (scheme != string::npos) {
		auto query = path.find('?', scheme + 3);
		if (query != string::npos) {
			return path.substr(0, query) + suffix + path.substr(query);
		}
	}
	return path + suffix;
}

} // namespace duckdb
