#include "duckdb/common/filename_pattern.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

FileNameSegment::FileNameSegment(string data) : type(FileNameSegmentType::LITERAL), data(std::move(data)) {
}

FileNameSegment::FileNameSegment(FileNameSegmentType type) : type(type) {
}

FilenamePattern::FilenamePattern() {
	segments.emplace_back("data_");
	segments.emplace_back(FileNameSegmentType::OFFSET);
}

FilenamePattern::FilenamePattern(string base, idx_t pos, bool uuid, vector<FileNameSegment> segments_p) {
	if (!segments_p.empty()) {
		segments = std::move(segments_p);
		return;
	}
	// deserialize from legacy pattern
	if (pos > 0) {
		segments.emplace_back(base.substr(0, pos));
	}
	if (uuid) {
		segments.emplace_back(FileNameSegmentType::UUID_V4);
	} else {
		segments.emplace_back(FileNameSegmentType::OFFSET);
	}
	if (pos == 0 && !base.empty()) {
		segments.emplace_back(std::move(base));
	} else if (pos < base.size()) {
		segments.emplace_back(base.substr(pos, base.size() - pos));
	}
}

void FilenamePattern::SetFilenamePattern(const string &pattern) {
	struct StringPattern {
		StringPattern(string pattern_p, FileNameSegmentType type) : pattern(std::move(pattern_p)), type(type) {
		}

		string pattern;
		FileNameSegmentType type;
	};
	vector<StringPattern> patterns;
	patterns.emplace_back("{i}", FileNameSegmentType::OFFSET);
	patterns.emplace_back("{uuid}", FileNameSegmentType::UUID_V4);
	patterns.emplace_back("{uuidv4}", FileNameSegmentType::UUID_V4);
	patterns.emplace_back("{uuidv7}", FileNameSegmentType::UUID_V7);

	idx_t current_pos = 0;
	segments.clear();
	for (idx_t i = 0; i < pattern.size(); i++) {
		if (pattern[i] != '{') {
			continue;
		}
		// found a { - check if this is one of the supported pattern
		idx_t remaining_length = pattern.size() - i;
		for (auto &pat : patterns) {
			if (remaining_length < pat.pattern.size()) {
				// cannot match
				continue;
			}
			if (memcmp(pat.pattern.c_str(), pattern.c_str() + i, pat.pattern.size()) != 0) {
				// no match
				continue;
			}
			// found a match!
			if (i > current_pos) {
				// add a literal pattern
				segments.emplace_back(pattern.substr(current_pos, i - current_pos));
			}
			// add the pattern here
			segments.emplace_back(pat.type);

			// advance the search forward
			i += pat.pattern.size() - 1;
			current_pos = i + 1;
			break;
		}
	}
	// add the final pattern (if any)
	if (current_pos < pattern.size()) {
		segments.emplace_back(pattern.substr(current_pos));
	}
	if (segments.size() == 1 && segments[0].type == FileNameSegmentType::LITERAL) {
		// if we have ONLY a literal we add an offset at the end
		segments.emplace_back(FileNameSegmentType::OFFSET);
	}
	if (segments.empty()) {
		// an explicitly empty pattern is stored as one empty literal: an empty segment list is the
		// legacy marker in deserialization, which would reconstruct the offset segment it implies
		segments.emplace_back(string());
	}
}

string FilenamePattern::CreateFilename(FileSystem &fs, const string &path, const string &extension,
                                       idx_t offset) const {
	string result;
	for (auto &segment : segments) {
		switch (segment.type) {
		case FileNameSegmentType::LITERAL:
			result += segment.data;
			break;
		case FileNameSegmentType::UUID_V4:
			result += UUID::ToString(UUID::GenerateRandomUUID());
			break;
		case FileNameSegmentType::UUID_V7:
			result += UUID::ToString(UUIDv7::GenerateRandomUUID());
			break;
		case FileNameSegmentType::OFFSET:
			result += std::to_string(offset);
			break;
		default:
			throw InternalException("Unsupported type for Filename Pattern");
		}
	}
	return fs.JoinPath(path, result + "." + extension);
}

bool FilenamePattern::HasUUID() const {
	for (auto &segment : segments) {
		if (segment.type == FileNameSegmentType::UUID_V4 || segment.type == FileNameSegmentType::UUID_V7) {
			return true;
		}
	}
	return false;
}

struct LegacyFilenamePattern {
	string base;
	idx_t pos;
};

bool SupportsLegacyFilenamePattern(const vector<FileNameSegment> &segments) {
	idx_t non_literal_count = 0;
	for (auto &segment : segments) {
		if (segment.type == FileNameSegmentType::UUID_V7) {
			// UUID v7 is not supported in legacy mode
			return false;
		}
		if (segment.type != FileNameSegmentType::LITERAL) {
			non_literal_count++;
		}
	}
	if (non_literal_count != 1) {
		// legacy mode requires exactly one non-literal
		return false;
	}
	return true;
}

LegacyFilenamePattern GetLegacyFilenamePattern(const vector<FileNameSegment> &segments) {
	LegacyFilenamePattern pattern;
	// construct the base + pos
	for (auto &segment : segments) {
		if (segment.type == FileNameSegmentType::LITERAL) {
			// add the literal to the base
			pattern.base += segment.data;
		} else {
			// non-literal: set the position to the current location
			pattern.pos = pattern.base.size();
		}
	}
	return pattern;
}

string FilenamePattern::SerializeBase() const {
	if (!SupportsLegacyFilenamePattern(segments)) {
		return string();
	}
	return GetLegacyFilenamePattern(segments).base;
}

idx_t FilenamePattern::SerializePos() const {
	if (!SupportsLegacyFilenamePattern(segments)) {
		return 0;
	}
	return GetLegacyFilenamePattern(segments).pos;
}

vector<FileNameSegment> FilenamePattern::SerializeSegments() const {
	if (SupportsLegacyFilenamePattern(segments)) {
		// for legacy mode we don't serialize the segments
		return vector<FileNameSegment>();
	}
	return segments;
}

} // namespace duckdb
