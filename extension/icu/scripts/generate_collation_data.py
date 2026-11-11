#!/usr/bin/env python3
"""Generates the collation tables used by the ICU extension's built-in collator.

The generated tables are derived exclusively from Unicode Consortium data:

  * CLDR common/uca/FractionalUCA.txt - the root collation element table
  * CLDR common/uca/allkeys_CLDR.txt  - not used, FractionalUCA is authoritative
  * CLDR common/collation/*.xml       - per-locale tailoring rules
  * UCD UnicodeData.txt               - canonical decompositions and combining classes

Run as:
    python3 extension/icu/scripts/generate_collation_data.py

The versions above can be overridden with the CLDR_VERSION and UNICODE_VERSION
environment variables, see scripts/README.md for the steps to take when a new
release of the data comes out.

The downloaded source files are cached in ~/.cache/duckdb-collation-data, the
generated C++ file is written to extension/icu/collation/generated. The zstd
command line tool is used to compress the tables, it has to be installed to run
this script.
"""

import os
import re
import struct
import subprocess
import sys
import urllib.request

#! the versions of the data the tables are generated from, see scripts/README.md to update them
CLDR_VERSION = os.environ.get("CLDR_VERSION", "release-48")
UNICODE_VERSION = os.environ.get("UNICODE_VERSION", "17.0.0")

CLDR_BASE = "https://raw.githubusercontent.com/unicode-org/cldr/%s/common" % CLDR_VERSION
UCD_BASE = "https://www.unicode.org/Public/%s/ucd" % UNICODE_VERSION

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
EXTENSION_DIR = os.path.dirname(SCRIPT_DIR)
OUTPUT_DIR = os.path.join(EXTENSION_DIR, "collation", "generated")
CACHE_DIR = os.path.join(os.path.expanduser("~"), ".cache", "duckdb-collation-data", CLDR_VERSION)

# trie block size - code points are looked up as stage2[stage1[cp >> SHIFT] + (cp & MASK)]
TRIE_SHIFT = 6
TRIE_BLOCK = 1 << TRIE_SHIFT
TRIE_MASK = TRIE_BLOCK - 1
MAX_CODEPOINT = 0x110000

# trie value tags
TAG_SINGLE = 1
TAG_EXPANSION = 2
TAG_CONTEXTS = 3

CONTEXT_CONTRACTION = 0
CONTEXT_PREFIX = 1


def fetch(url, name):
    path = os.path.join(CACHE_DIR, name)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sys.stderr.write("downloading %s\n" % url)
    with urllib.request.urlopen(url) as response:
        data = response.read().decode("utf-8")
    with open(path, "w", encoding="utf-8") as f:
        f.write(data)
    return data


# ---------------------------------------------------------------------------
# collation elements
# ---------------------------------------------------------------------------


class CollationElement:
    """A single collation element: three weights, each left-aligned in its field."""

    def __init__(self, primary, secondary, tertiary, pure_tertiary=None):
        self.primary = primary
        self.secondary = secondary
        self.tertiary = tertiary
        # the tertiary weight without the case bits, which is what tailorings order by
        self.pure_tertiary = pure_tertiary if pure_tertiary is not None else tertiary & 0x3F3F

    def key(self):
        return (self.primary, self.secondary, self.tertiary)

    def encode(self):
        return (self.primary << 32) | (self.secondary << 16) | self.tertiary


def parse_weight(text, width):
    """Parses a space separated list of hex bytes into a left-aligned weight."""
    text = text.strip()
    if not text:
        return 0
    value = 0
    count = 0
    for byte in text.split():
        value = (value << 8) | int(byte, 16)
        count += 1
    if count > width:
        raise ValueError("weight %s is wider than %d bytes" % (text, width))
    return value << (8 * (width - count))


# ---------------------------------------------------------------------------
# implicit (computed) primary weights
# ---------------------------------------------------------------------------

# Han characters are ordered by radical/stroke. Their primaries are assigned
# sequentially from the Han primary range; unassigned code points use a formula
# over the code point itself.
HAN_FIRST_LEAD_BYTE = 0xFA
HAN_THIRD_BYTE_COUNT = 127
HAN_SECOND_BYTE_COUNT = 254
HAN_PER_LEAD_BYTE = HAN_THIRD_BYTE_COUNT * HAN_SECOND_BYTE_COUNT

UNASSIGNED_IMPLICIT_LEAD_BYTE = 0xFE

# the Han ranges are indexed by blocks of this many code points
HAN_BLOCK_SHIFT = 8

# FractionalUCA.txt places the trailing weights (U+FFFD, U+FFFF) in their own region,
# they use the single trailing lead byte at runtime
FILE_FIRST_TRAILING_LEAD_BYTE = 0xE5
TRAILING_LEAD_BYTE = 0xFF


def han_primary(index):
    lead = HAN_FIRST_LEAD_BYTE + index // HAN_PER_LEAD_BYTE
    rest = index % HAN_PER_LEAD_BYTE
    second = 0x02 + rest // HAN_THIRD_BYTE_COUNT
    third = 0x02 + 2 * (rest % HAN_THIRD_BYTE_COUNT)
    return (lead << 24) | (second << 16) | (third << 8)


def unassigned_primary(cp):
    cp += 1
    primary = 2 + (cp % 18) * 14
    cp //= 18
    primary |= (2 + (cp % 254)) << 8
    cp //= 254
    primary |= (4 + (cp % 251)) << 16
    return primary | (UNASSIGNED_IMPLICIT_LEAD_BYTE << 24)


def parse_han_order(text):
    """Extracts the radical/stroke ordered list of Han characters."""
    order = []
    for line in text.split("\n"):
        if not line.startswith("[radical ") or ":" not in line:
            continue
        body = line[line.index(":") + 1 : line.rindex("]")]
        i = 0
        while i < len(body):
            if i + 2 < len(body) and body[i + 1] == "-":
                for cp in range(ord(body[i]), ord(body[i + 2]) + 1):
                    order.append(cp)
                i += 3
            else:
                order.append(ord(body[i]))
                i += 1
    return order


def han_ranges(order):
    """Compresses the Han order into (start code point, length, first index) runs."""
    ranges = []
    start = 0
    for i in range(1, len(order) + 1):
        if i == len(order) or order[i] != order[i - 1] + 1:
            ranges.append((order[start], i - start, start))
            start = i
    ranges.sort()
    return ranges


# ---------------------------------------------------------------------------
# FractionalUCA.txt
# ---------------------------------------------------------------------------


class RootTable:
    def __init__(self):
        self.mappings = {}  # (code point, ...) -> [CollationElement]
        self.prefixes = {}  # (code point, ...) -> [CollationElement], first cp is the prefix
        self.compressible = [False] * 256
        self.variable_top = 0
        # reorder group ("SPACE", "PUNCTUATION", ...) -> (its first primary, whether that starts a new lead byte)
        self.group_starts = {}
        self.han_order = []
        self.anchors = {}  # "first regular" -> CollationElement
        self.lead_byte_scripts = {}  # lead byte -> [script or group name]
        # the weight ranges of elements that are ignorable at a higher level
        self.first_ignorable_secondary = 0
        self.first_ignorable_tertiary = 0


def parse_fractional_uca(text):
    table = RootTable()
    table.han_order = parse_han_order(text)
    han_index = {cp: i for i, cp in enumerate(table.han_order)}

    def implicit_primary(cp):
        if cp in han_index:
            return han_primary(han_index[cp])
        return unassigned_primary(cp)

    def parse_ces(spec):
        elements = []
        for match in re.finditer(r"\[([^\]]*)\]", spec):
            fields = [field.strip() for field in match.group(1).split(",")]
            if fields[0].startswith("U+"):
                # the primary is the computed implicit weight of the referenced code point,
                # the secondary and tertiary default to the common weight
                primary = implicit_primary(int(fields[0][2:], 16))
                if len(fields) == 3:
                    secondary, tertiary = parse_weight(fields[1], 2), parse_weight(fields[2], 2)
                elif len(fields) == 2:
                    secondary, tertiary = 0x0500, parse_weight(fields[1], 2)
                else:
                    secondary, tertiary = 0x0500, 0x0500
                elements.append(CollationElement(primary, secondary, tertiary))
            elif len(fields) == 3:
                primary = parse_weight(fields[0], 4)
                if (primary >> 24) >= FILE_FIRST_TRAILING_LEAD_BYTE:
                    primary = (TRAILING_LEAD_BYTE << 24) | (primary & 0x00FFFFFF)
                elements.append(CollationElement(primary, parse_weight(fields[1], 2), parse_weight(fields[2], 2)))
            else:
                raise ValueError("unrecognized collation element [%s]" % match.group(1))
        return elements

    for line in text.split("\n"):
        line = line.rstrip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("["):
            parse_bracket_line(table, line, parse_ces)
            continue
        if ";" not in line:
            continue
        left, right = line.split(";", 1)
        right = right.split("#")[0].strip()
        left = left.strip()
        if not right:
            continue
        is_prefix = "|" in left
        codepoints = tuple(int(cp, 16) for cp in left.replace("|", " ").split())
        # FDD0/FDD1 entries are boundary markers used when building tailorings
        if codepoints[0] in (0xFDD0, 0xFDD1):
            # e.g. "FDD1 263A; [0C 02 02, 05, 05] # SYMBOL first primary starts new lead byte"
            comment = line.split("#", 1)[1] if "#" in line else ""
            if codepoints[0] == 0xFDD1 and "first primary" in comment:
                group = comment.split()[0]
                table.group_starts[group] = (parse_ces(right)[0].primary, "starts new lead byte" in comment)
            continue
        elements = parse_ces(right)
        if is_prefix:
            table.prefixes[codepoints] = elements
        else:
            table.mappings[codepoints] = elements
    return table


def parse_bracket_line(table, line, parse_ces):
    body = line[1 : line.rindex("]")] if line.endswith("]") else line[1:]
    fields = body.split()
    if not fields:
        return
    if fields[0] == "fixed" and len(fields) >= 6 and fields[1] == "first" and fields[2] == "ignorable":
        if fields[3] == "secondary":
            table.first_ignorable_secondary = int(fields[5], 16) << 8
        elif fields[3] == "tertiary":
            table.first_ignorable_tertiary = int(fields[5], 16) << 8
    elif fields[0] == "top_byte":
        lead = int(fields[1], 16)
        table.compressible[lead] = "COMPRESS" in fields
        scripts = []
        for field in fields[2:]:
            if field in ("]", "#", "COMPRESS"):
                break
            scripts.append(field)
        table.lead_byte_scripts[lead] = scripts
    elif fields[0] == "variable" and len(fields) >= 4 and fields[1] == "top":
        table.variable_top = parse_weight(" ".join(fields[3:]), 4)
    elif fields[0] in ("first", "last"):
        # e.g. [first regular [0C 04, 05, 05]] - anchors with unspecified (X) weights are skipped
        start = line.index("[", 1)
        spec = line[start:]
        if "X" in spec:
            return
        elements = parse_ces(spec)
        if elements:
            table.anchors[" ".join(line[1:start].split())] = elements[0]


# ---------------------------------------------------------------------------
# canonical decomposition data
# ---------------------------------------------------------------------------


class NormalizationData:
    def __init__(self):
        self.decomposition = {}  # code point -> [code point]
        self.combining_class = {}  # code point -> ccc
        self.uppercase = set()
        self.titlecase = set()


def parse_unicode_data(text):
    data = NormalizationData()
    raw = {}
    for line in text.split("\n"):
        if not line:
            continue
        fields = line.split(";")
        cp = int(fields[0], 16)
        ccc = int(fields[3])
        if ccc:
            data.combining_class[cp] = ccc
        if fields[2] == "Lu":
            data.uppercase.add(cp)
        elif fields[2] == "Lt":
            data.titlecase.add(cp)
        decomposition = fields[5]
        if decomposition and not decomposition.startswith("<"):
            raw[cp] = [int(part, 16) for part in decomposition.split()]

    # recursively expand to the full canonical decomposition
    def expand(cp):
        if cp not in raw:
            return [cp]
        result = []
        for part in raw[cp]:
            result.extend(expand(part))
        return result

    for cp in raw:
        data.decomposition[cp] = expand(cp)
    return data


def build_fcd(data):
    """Computes the leading and trailing combining class of every code point.

    These are the combining classes of the first and last character of the canonical
    decomposition, and are used to detect text that needs to be normalized.
    """
    fcd = {}
    for cp, ccc in data.combining_class.items():
        fcd[cp] = (ccc << 8) | ccc
    for cp, decomposition in data.decomposition.items():
        lead = data.combining_class.get(decomposition[0], 0)
        trail = data.combining_class.get(decomposition[-1], 0)
        if lead or trail:
            fcd[cp] = (lead << 8) | trail
    return fcd


# ---------------------------------------------------------------------------
# tailoring rules
# ---------------------------------------------------------------------------

# relation strengths
PRIMARY = 0
SECONDARY = 1
TERTIARY = 2
QUATERNARY = 3
IDENTICAL = 4

# bidi marks appear in the rules of right-to-left scripts to make them readable
BIDI_MARKS = "\u200e\u200f\u061c"

STRENGTH_OF_RELATION = {"<": PRIMARY, "<<": SECONDARY, "<<<": TERTIARY, "<<<<": QUATERNARY, "=": IDENTICAL}


class Relation:
    def __init__(self, strength, text, extension, context):
        self.strength = strength
        self.text = text  # the string that is tailored
        self.extension = extension  # the string whose elements are appended, from "/"
        self.context = context  # the preceding string, from "|"


class Reset:
    def __init__(self, text, before_strength, position):
        self.text = text
        self.before_strength = before_strength
        self.position = position  # a logical position such as "last regular"


class Import:
    def __init__(self, target):
        self.target = target


class TailoringRules:
    def __init__(self):
        self.operations = []
        self.settings = {}
        self.reorder = []
        self.suppress_contractions = []


class RuleParser:
    """Parses the collation rule syntax of UTS #35."""

    def __init__(self, text):
        self.text = text
        self.pos = 0

    def error(self, message):
        raise ValueError("%s at offset %d: %s" % (message, self.pos, self.text[self.pos : self.pos + 40]))

    def skip_ignorable(self):
        while self.pos < len(self.text):
            char = self.text[self.pos]
            if char.isspace() or char in BIDI_MARKS:
                self.pos += 1
            elif char == "#":
                while self.pos < len(self.text) and self.text[self.pos] != "\n":
                    self.pos += 1
            else:
                break

    def parse_bracket(self):
        """Parses a [...] option, returning its contents."""
        assert self.text[self.pos] == "["
        depth = 0
        start = self.pos
        while self.pos < len(self.text):
            if self.text[self.pos] == "[":
                depth += 1
            elif self.text[self.pos] == "]":
                depth -= 1
                if depth == 0:
                    self.pos += 1
                    return self.text[start + 1 : self.pos - 1].strip()
            self.pos += 1
        self.error("unterminated option")

    def parse_string(self):
        """Parses the string of a reset or relation, stopping at the next operator."""
        result = []
        while self.pos < len(self.text):
            char = self.text[self.pos]
            if char in "&<=#|/" or char.isspace():
                break
            if char in BIDI_MARKS:
                # the rules use bidi marks for readability, they are not collated
                self.pos += 1
                continue
            if char == "'":
                # a quoted section, an empty one is a literal quote
                self.pos += 1
                quoted = 0
                while self.pos < len(self.text):
                    if self.text[self.pos] == "'":
                        if self.pos + 1 < len(self.text) and self.text[self.pos + 1] == "'":
                            result.append("'")
                            self.pos += 2
                            quoted += 1
                            continue
                        self.pos += 1
                        break
                    if self.text[self.pos] == "\\":
                        # escapes are processed inside quoted sections as well
                        result.append(self.parse_escape())
                    else:
                        result.append(self.text[self.pos])
                        self.pos += 1
                    quoted += 1
                if quoted == 0:
                    result.append("'")
                continue
            if char == "\\":
                result.append(self.parse_escape())
                continue
            result.append(char)
            self.pos += 1
        return "".join(result)

    def parse_escape(self):
        """Parses a backslash escape, which is the same inside and outside quotes."""
        self.pos += 1
        escape = self.text[self.pos]
        if escape == "u":
            value = chr(int(self.text[self.pos + 1 : self.pos + 5], 16))
            self.pos += 5
        elif escape == "U":
            value = chr(int(self.text[self.pos + 1 : self.pos + 9], 16))
            self.pos += 9
        else:
            value = escape
            self.pos += 1
        return value

    def parse_character_list(self):
        """Parses the character list of a starred relation, expanding a-z style ranges."""
        text = self.parse_string()
        result = []
        index = 0
        while index < len(text):
            if index + 2 < len(text) and text[index + 1] == "-":
                for codepoint in range(ord(text[index]), ord(text[index + 2]) + 1):
                    result.append(chr(codepoint))
                index += 3
                continue
            result.append(text[index])
            index += 1
        return result

    def parse(self):
        rules = TailoringRules()
        while True:
            self.skip_ignorable()
            if self.pos >= len(self.text):
                break
            char = self.text[self.pos]
            if char == "[":
                self.parse_option(rules)
            elif char == "&":
                self.pos += 1
                self.parse_reset(rules)
            elif char in "<=":
                self.parse_relation(rules)
            else:
                self.error("unexpected character")
        return rules

    def parse_option(self, rules):
        option = self.parse_bracket()
        fields = option.split(None, 1)
        name = fields[0]
        value = fields[1].strip() if len(fields) > 1 else ""
        if name == "import":
            rules.operations.append(Import(value))
        elif name == "reorder":
            rules.reorder = value.split()
        elif name == "suppressContractions":
            rules.suppress_contractions.append(value)
        elif name == "optimize":
            pass  # only a performance hint
        else:
            rules.settings[name] = value

    def parse_reset(self, rules):
        self.skip_ignorable()
        before_strength = None
        position = None
        text = ""
        while self.pos < len(self.text) and self.text[self.pos] == "[":
            option = self.parse_bracket()
            fields = option.split()
            if fields[0] == "before":
                before_strength = int(fields[1]) - 1
            else:
                position = " ".join(fields)
            self.skip_ignorable()
        if position is None:
            text = self.parse_string()
            if not text:
                self.error("empty reset")
        rules.operations.append(Reset(text, before_strength, position))

    def parse_relation(self, rules):
        operator = ""
        while self.pos < len(self.text) and self.text[self.pos] in "<=":
            operator += self.text[self.pos]
            self.pos += 1
        if operator not in STRENGTH_OF_RELATION:
            self.error("unknown relation %s" % operator)
        if self.pos < len(self.text) and self.text[self.pos] == "*":
            # a starred relation applies to every character of the following list
            self.pos += 1
            self.skip_ignorable()
            for char in self.parse_character_list():
                rules.operations.append(Relation(STRENGTH_OF_RELATION[operator], char, "", ""))
            return
        self.skip_ignorable()
        if self.pos < len(self.text) and self.text[self.pos] == "[":
            # a relation to a logical position, e.g. "&X < [variable top]"
            self.parse_bracket()
            return
        text = self.parse_string()
        extension = ""
        context = ""
        self.skip_ignorable()
        while self.pos < len(self.text) and self.text[self.pos] in "|/":
            separator = self.text[self.pos]
            self.pos += 1
            self.skip_ignorable()
            value = self.parse_string()
            if separator == "|":
                # the string before the bar is the context, the one after is tailored
                context = text
                text = value
            else:
                extension = value
            self.skip_ignorable()
        rules.operations.append(Relation(STRENGTH_OF_RELATION[operator], text, extension, context))


class WeightAllocator:
    """Allocates weights in the gaps between the weights that are in use.

    Weights are byte sequences that are compared lexicographically, which is the same as
    comparing the left-aligned values. Every tailored weight opens a gap of its own, so
    that a later rule can insert weights directly after it. The rules are applied twice:
    the first pass records the gaps and how many weights each of them needs, the second
    pass spreads the weights evenly over the gaps.
    """

    def __init__(self, used, width, alphabet, continuation=None):
        self.used = sorted(used)
        self.width = width
        self.alphabet = alphabet
        self.continuation = continuation if continuation is not None else alphabet
        self.counting = True
        self.chain = 0  # rules that reset to the same weight are ordered by their chain
        self.children = {}  # gap -> the allocations in it, in order
        self.weights = {}  # allocation -> weight
        self.order = {}  # allocation -> the chain it belongs to and its place in it
        self.sizes = {}  # allocation -> the number of weights that depend on it
        self.position = 0  # the allocation the second pass is at

    def next_used(self, weight):
        """The smallest weight in use that is greater than the given weight."""
        lower = 0
        upper = len(self.used)
        while lower < upper:
            middle = (lower + upper) // 2
            if self.used[middle] <= weight:
                lower = middle + 1
            else:
                upper = middle
        return self.used[lower] if lower < len(self.used) else (1 << (8 * self.width))

    def previous_used(self, weight):
        """The largest weight in use that is smaller than the given weight."""
        lower = 0
        upper = len(self.used)
        while lower < upper:
            middle = (lower + upper) // 2
            if self.used[middle] < weight:
                lower = middle + 1
            else:
                upper = middle
        return self.used[lower - 1] if lower > 0 else 0

    def to_bytes(self, weight):
        result = []
        for shift in range(8 * (self.width - 1), -1, -8):
            byte = (weight >> shift) & 0xFF
            if byte == 0:
                break
            result.append(byte)
        return result

    def from_bytes(self, byte_list):
        weight = 0
        for byte in byte_list:
            weight = (weight << 8) | byte
        return weight << (8 * (self.width - len(byte_list)))

    @staticmethod
    def gap_after(weight, scope=None):
        """The gap that directly follows a weight, which may itself be a tailored weight.

        The scope keeps the gaps of weights that are never compared with each other apart,
        so that for example the tertiary weights of one primary do not use up the space of
        another primary.
        """
        return weight if isinstance(weight, tuple) else ("root", weight, scope)

    def gap_before(self, weight, scope=None):
        """The gap that directly precedes a weight.

        The space before a weight of the root collation is the same space as the one after
        the weight below it, so both use the same gap.
        """
        if isinstance(weight, tuple):
            return ("before", weight)
        return ("root", self.previous_used(weight), scope)

    def allocate(self, gap, before=False):
        """Allocates the next weight of a gap.

        A weight that a [before] rule allocates sorts after the weights that the other
        rules put in the same gap, so that it stays closest to the weight it precedes.
        """
        if self.counting:
            allocation = ("weight", len(self.weights))
            self.weights[allocation] = None
            self.order[allocation] = (before, self.chain, len(self.weights))
            self.children.setdefault(gap, []).append(allocation)
            return allocation
        allocation = ("weight", self.position)
        self.position += 1
        return self.weights[allocation]

    def start_chain(self):
        """Starts a new chain of relations, which is inserted before the earlier ones."""
        self.chain += 1

    def start_assignment(self):
        """Assigns a weight to every allocation of the first pass."""
        for gap in sorted(self.children, key=self.gap_order):
            if gap[0] == "root":
                self.assign(gap, gap[1], self.next_used(gap[1]))
        self.counting = False
        self.position = 0

    def gap_order(self, gap):
        return (0, gap[1]) if gap[0] == "root" else (1, 0)

    def subtree_size(self, allocation):
        """How many weights are allocated in the gaps that follow an allocation."""
        if allocation in self.sizes:
            return self.sizes[allocation]
        total = 1
        for gap in (allocation, ("before", allocation)):
            for child in self.children.get(gap, []):
                total += self.subtree_size(child)
        self.sizes[allocation] = total
        return total

    def assign(self, gap, lower, upper):
        """Spreads the weights of a gap over an interval and recurses into their own gaps."""
        allocations = self.children.get(gap, [])
        if not allocations:
            return
        # a rule inserts its weights directly after the weight it resets to, so the chains
        # that come later in the rules sort before the ones that come earlier
        allocations = sorted(
            allocations,
            key=lambda allocation: (
                self.order[allocation][0],
                -self.order[allocation][1],
                self.order[allocation][2],
            ),
        )
        # every weight gets a share of the interval that fits the weights allocated after it
        sizes = [self.subtree_size(allocation) for allocation in allocations]
        weights = self.spread(lower, upper, sizes)
        if weights is None:
            raise ValueError("no room for %d weights between 0x%X and 0x%X" % (len(allocations), lower, upper))
        for position, allocation in enumerate(allocations):
            self.weights[allocation] = weights[position]
            following = weights[position + 1] if position + 1 < len(weights) else upper
            preceding = weights[position - 1] if position > 0 else lower
            self.assign(allocation, weights[position], following)
            self.assign(("before", allocation), preceding, weights[position])

    #! a gap is filled with at most one weight per this many available weights, so that
    #! later rules can still insert weights between them
    SPACING = 16

    def spread(self, base, limit, sizes):
        """Distributes weights over a gap, giving every weight a share of the space.

        No weight may be a prefix of another one, otherwise the concatenated weights of a
        string would not compare correctly, so the weights of a gap all differ in the first
        byte that the two ends of the gap differ in, and all have the same length.
        """
        total = sum(sizes)
        offsets = []
        position = 0
        for size in sizes:
            offsets.append(position)
            position += size

        radix = len(self.continuation)
        fields = self.fields(base, limit)
        # when a gap has no room of its own the weights extend the weight below them, which
        # only orders them correctly against the strings that do not use that weight
        fallback = [(self.to_bytes(base), list(self.continuation))]
        for target, choices in ((total * self.SPACING, fields), (total, fields), (total, fallback)):
            for prefix, leads in choices:
                for digits in range(0, self.width - len(prefix)):
                    capacity = len(leads) * (radix**digits)
                    if capacity < target:
                        continue
                    weights = []
                    for offset in offsets:
                        value = offset * capacity // total
                        byte_list = list(prefix)
                        byte_list.append(leads[value // (radix**digits)])
                        remainder = value % (radix**digits)
                        for digit in range(digits - 1, -1, -1):
                            byte_list.append(self.continuation[(remainder // (radix**digit)) % radix])
                        weights.append(self.from_bytes(byte_list))
                    if all(base < weight < limit for weight in weights) and len(set(weights)) == len(weights):
                        return weights
        return None

    def fields(self, base, limit):
        """The byte positions a gap can be filled at, shortest weights first.

        Every field is a prefix of one end of the gap plus the byte values that are free at
        the position after it, so the weights differ from both ends within their own length.
        """
        base_bytes = self.to_bytes(base)
        limit_bytes = self.to_bytes(limit)
        # weights of different lead bytes belong to different scripts, an allocation that
        # cannot stay in the lead byte of the weight below it goes into the one above
        same_lead = bool(base_bytes) and bool(limit_bytes) and base_bytes[0] == limit_bytes[0]
        result = []
        for index in range(max(len(base_bytes), len(limit_bytes))):
            alphabet = self.alphabet if index == 0 else self.continuation
            shared = base_bytes[:index] == limit_bytes[:index]
            options = []
            if index < len(base_bytes):
                low = base_bytes[index] + 1
                high = limit_bytes[index] - 1 if shared and index < len(limit_bytes) else alphabet[-1]
                leads = [value for value in alphabet if low <= value <= high]
                if leads:
                    options.append((base_bytes[:index], leads))
            if index < len(limit_bytes):
                high = limit_bytes[index] - 1
                low = base_bytes[index] + 1 if shared and index < len(base_bytes) else alphabet[0]
                leads = [value for value in alphabet if low <= value <= high]
                if leads:
                    option = (limit_bytes[:index], leads)
                    options.insert(0, option) if not same_lead and index > 0 else options.append(option)
            result.extend(options)
        return result


# weight bytes that are reserved: 00 terminates a weight, 01 separates levels, 02 is the merge
# separator and 03 and FF terminate primary compression
PRIMARY_ALPHABET = list(range(0x04, 0xFF))
# secondary weights between the common weight and 0x46 are reserved for the compression of
# common weights, 0x04 is only reachable when allocating below the common weight
SECONDARY_ALPHABET = [0x02, 0x03, 0x04] + list(range(0x46, 0xFF))
# only the lower six bits of a tertiary byte are the weight, the upper bits hold the case
TERTIARY_ALPHABET = list(range(0x02, 0x40))
# the reserved values only apply to the first byte of a weight
CONTINUATION_ALPHABET = list(range(0x04, 0xFF))

CASE_LOWER = 0x00
CASE_MIXED = 0x40
CASE_UPPER = 0x80


HANGUL_BASE = 0xAC00
HANGUL_LEADING = 0x1100
HANGUL_VOWEL = 0x1161
HANGUL_TRAILING = 0x11A7
HANGUL_VOWEL_COUNT = 21
HANGUL_TRAILING_COUNT = 28
HANGUL_COUNT = 19 * HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT


def decompose_hangul(text):
    """Replaces Hangul syllables by the jamo they are collated as."""
    result = []
    for codepoint in text:
        if not HANGUL_BASE <= codepoint < HANGUL_BASE + HANGUL_COUNT:
            result.append(codepoint)
            continue
        index = codepoint - HANGUL_BASE
        result.append(HANGUL_LEADING + index // (HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT))
        result.append(HANGUL_VOWEL + (index % (HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT)) // HANGUL_TRAILING_COUNT)
        if index % HANGUL_TRAILING_COUNT:
            result.append(HANGUL_TRAILING + index % HANGUL_TRAILING_COUNT)
    return tuple(result)


class TailoringBuilder:
    """Turns collation rules into mappings from strings to collation elements."""

    def __init__(self, root, normalization, implicit_primary):
        self.root = root
        self.normalization = normalization
        self.implicit_primary = implicit_primary
        self.mappings = {}
        self.prefix_mappings = {}
        self.settings = {}
        self.reorder = []
        self.suppressed = set()
        self.gaps = []
        primaries = set()
        secondaries = set()
        tertiaries = set()
        for elements in list(root.mappings.values()) + list(root.prefixes.values()):
            for element in elements:
                primaries.add(element.primary)
                secondaries.add(element.secondary)
                tertiaries.add(element.tertiary & 0x3F3F)
        # the Han and unassigned weights follow the tailorable range
        primaries.add(HAN_FIRST_LEAD_BYTE << 24)
        self.primary_allocator = WeightAllocator(primaries, 4, PRIMARY_ALPHABET)
        self.secondary_allocator = WeightAllocator(secondaries, 2, SECONDARY_ALPHABET, CONTINUATION_ALPHABET)
        self.tertiary_allocator = WeightAllocator(tertiaries, 2, TERTIARY_ALPHABET)

    def start_assignment(self):
        """Prepares the second pass, which assigns the weights that the first pass recorded."""
        self.mappings = {}
        self.prefix_mappings = {}
        self.settings = {}
        self.reorder = []
        self.suppressed = set()
        self.gaps = []
        for allocator in (self.primary_allocator, self.secondary_allocator, self.tertiary_allocator):
            allocator.start_assignment()

    def root_elements(self, text):
        """The collation elements a string has in the root collation."""
        result = []
        position = 0
        while position < len(text):
            matched = None
            for length in range(min(8, len(text) - position), 0, -1):
                key = text[position : position + length]
                if key in self.root.mappings:
                    matched = (length, self.root.mappings[key])
                    break
            if matched:
                result.extend(matched[1])
                position += matched[0]
            else:
                result.append(CollationElement(self.implicit_primary(text[position]), 0x0500, 0x0500))
                position += 1
        return result

    def elements_for(self, text):
        """The current collation elements of a string, including the tailorings so far."""
        result = []
        position = 0
        while position < len(text):
            matched = None
            for length in range(len(text) - position, 0, -1):
                key = text[position : position + length]
                if key in self.mappings:
                    matched = (length, self.mappings[key])
                    break
            if matched:
                result.extend(matched[1])
                position += matched[0]
                continue
            # fall back to the root, one character at a time so tailored contractions win
            matched = None
            for length in range(min(8, len(text) - position), 0, -1):
                key = text[position : position + length]
                if key in self.root.mappings:
                    matched = (length, self.root.mappings[key])
                    break
            if matched:
                result.extend(matched[1])
                position += matched[0]
            else:
                result.append(CollationElement(self.implicit_primary(text[position]), 0x0500, 0x0500))
                position += 1
        return result

    def case_bits(self, text):
        """The case bits of a tailored string, following the case of its characters."""
        upper = [cp in self.normalization.uppercase for cp in text]
        if all(upper):
            return CASE_UPPER
        if upper[0] or any(cp in self.normalization.titlecase for cp in text):
            return CASE_MIXED
        return CASE_LOWER

    def build(self, rules, resolve_import):
        """Applies the rules, resolve_import returns the rules of an imported collation."""
        self.settings.update(rules.settings)
        if rules.reorder:
            self.reorder = rules.reorder
        for value in rules.suppress_contractions:
            self.suppressed |= parse_unicode_set(value)

        anchor = None  # the collation elements the next relation is placed after
        for operation in rules.operations:
            if isinstance(operation, Import):
                imported = resolve_import(operation.target)
                if imported is not None:
                    self.build(imported, resolve_import)
                anchor = None
                continue
            if isinstance(operation, Reset):
                anchor = self.reset_position(operation)
                self.start_chain(anchor[-1], operation.before_strength)
                continue
            if anchor is None:
                raise ValueError("relation without a reset")
            anchor = self.add_relation(operation, anchor)

    def start_chain(self, anchor, before_strength):
        """Remembers the gaps that the relations of a chain allocate their weights in."""
        allocators = (self.primary_allocator, self.secondary_allocator, self.tertiary_allocator)
        for allocator in allocators:
            allocator.start_chain()
        weights = (anchor.primary, anchor.secondary, anchor.pure_tertiary)
        self.gaps = []
        self.gap_is_before = [before_strength == level for level in range(3)]
        # elements without a primary or secondary weight use their own weight ranges
        if anchor.primary == 0 and isinstance(weights[TERTIARY], int):
            if weights[SECONDARY] == 0:
                weights = (0, 0, max(weights[TERTIARY], self.root.first_ignorable_tertiary))
            elif isinstance(weights[SECONDARY], int) and weights[SECONDARY] < self.root.first_ignorable_secondary:
                weights = (0, max(weights[SECONDARY], self.root.first_ignorable_secondary), weights[TERTIARY])
        # the secondary weights of a primary and the tertiary weights of a secondary are
        # only compared with each other, so every one of them gets its own gap
        scopes = (None, anchor.primary, (anchor.primary, weights[SECONDARY]))
        for level, allocator in enumerate(allocators):
            if before_strength == level:
                self.gaps.append(allocator.gap_before(weights[level], scopes[level]))
            else:
                self.gaps.append(allocator.gap_after(weights[level], scopes[level]))

    def reset_position(self, reset):
        if reset.position is not None:
            anchor = self.root.anchors.get(reset.position)
            if anchor is None:
                raise ValueError("unknown reset position [%s]" % reset.position)
            return [CollationElement(anchor.primary, anchor.secondary, anchor.tertiary)]
        return self.elements_for(self.canonical_order(decompose_hangul(tuple(ord(char) for char in reset.text))))

    def add_relation(self, relation, anchor):
        text = self.canonical_order(decompose_hangul(tuple(ord(char) for char in relation.text)))
        # a relation to a string of several collation elements keeps all but the last element,
        # so that the tailored string sorts directly after the string it is reset to
        prefix = anchor[:-1]
        base = anchor[-1]
        case = self.case_bits(text)
        if relation.strength == IDENTICAL:
            element = CollationElement(base.primary, base.secondary, base.tertiary, base.pure_tertiary)
        elif relation.strength == PRIMARY:
            primary = self.primary_allocator.allocate(self.gaps[PRIMARY], self.gap_is_before[PRIMARY])
            element = self.make_element(primary, 0x0500, 0x0500, case)
            # the weights below the new primary start over at the common weight
            self.gaps[SECONDARY] = self.secondary_allocator.gap_after(0x0500, primary)
            self.gaps[TERTIARY] = self.tertiary_allocator.gap_after(0x0500, (primary, 0x0500))
        elif relation.strength == SECONDARY:
            secondary = self.secondary_allocator.allocate(self.gaps[SECONDARY], self.gap_is_before[SECONDARY])
            element = self.make_element(base.primary, secondary, 0x0500, case)
            self.gaps[TERTIARY] = self.tertiary_allocator.gap_after(0x0500, (base.primary, secondary))
        elif relation.strength == QUATERNARY:
            # a quaternary difference lives in the two bits above the tertiary weight, so it
            # is invisible unless the collation compares at the quaternary level
            self.quaternary = self.quaternary + 1 if getattr(self, "quaternary_base", None) == base.tertiary else 1
            self.quaternary_base = base.tertiary
            tertiary = base.tertiary
            if isinstance(tertiary, int):
                tertiary += self.quaternary << 6
            element = CollationElement(base.primary, base.secondary, tertiary, base.pure_tertiary)
        else:
            tertiary = self.tertiary_allocator.allocate(self.gaps[TERTIARY], self.gap_is_before[TERTIARY])
            element = self.make_element(base.primary, base.secondary, tertiary, case)
        elements = list(prefix) + [element]
        mapping = elements
        if relation.extension:
            mapping = elements + self.elements_for(decompose_hangul(tuple(ord(char) for char in relation.extension)))
        if relation.context:
            # the mapping only applies when the character is preceded by the context
            context = decompose_hangul(tuple(ord(char) for char in relation.context))
            self.prefix_mappings[(context, text)] = mapping
        else:
            self.add_mapping(text, mapping)
        # the next relation of the chain is placed after this element, the extension is not part of it
        return elements

    def make_element(self, primary, secondary, tertiary, case):
        """Builds a tailored element, the case bits are only known in the second pass."""
        if isinstance(tertiary, tuple):
            return CollationElement(primary, secondary, tertiary, tertiary)
        return CollationElement(primary, secondary, tertiary | (case << 8), tertiary)

    def close_over_composites(self):
        """Extends the tailoring to the characters that compose a tailored string.

        A composite such as U+0104 A WITH OGONEK is canonically equivalent to its base
        followed by a combining mark, so it has to pick up the tailoring of that base.
        """
        composites = {}
        for codepoint, decomposition in self.normalization.decomposition.items():
            if len(decomposition) < 2:
                continue
            marks = decomposition[1:]
            if any(self.normalization.combining_class.get(mark, 0) == 0 for mark in marks):
                continue
            if not self.is_derived(codepoint, decomposition):
                continue
            composites.setdefault(decomposition[0], []).append((codepoint, marks))

        for text in sorted(self.mappings):
            for composite, marks in composites.get(text[0], []):
                match = self.match_composite(text, marks)
                if match is None:
                    continue
                remainder, unmatched = match
                closure = (composite,) + remainder
                if closure in self.mappings:
                    continue
                self.mappings[closure] = self.mappings[text] + self.elements_for(unmatched)
            if len(text) == 1:
                continue
            # the last character of a contraction can be composed with a following mark
            for composite, marks in composites.get(text[-1], []):
                closure = text[:-1] + (composite,)
                if closure in self.mappings:
                    continue
                self.mappings[closure] = self.mappings[text] + self.elements_for(tuple(marks))

        # a composite also has to pick up the tailoring of the combining marks it contains.
        # a tailored base character does not reach into its composites, they keep the
        # weights of the root collation.
        for codepoint, decomposition in sorted(self.normalization.decomposition.items()):
            if (codepoint,) in self.mappings:
                continue
            if not any((mark,) in self.mappings for mark in decomposition[1:]):
                continue
            if not self.is_derived(codepoint, decomposition):
                continue
            self.mappings[(codepoint,)] = self.generate_elements(tuple(decomposition))

    def is_derived(self, codepoint, decomposition):
        """Whether the root collates a composite exactly like its own decomposition.

        Composites with weights of their own, such as the Arabic letters with hamza, keep
        them when the tailoring changes the characters they decompose into.
        """
        composed = [element.key() for element in self.root_elements((codepoint,))]
        decomposed = []
        for part in decomposition:
            # every character on its own, the root also maps the decomposition as a whole
            decomposed.extend(element.key() for element in self.root_elements((part,)))
        return composed == decomposed

    def generate_elements(self, text):
        """The elements of a string, the way they are generated when it is collated.

        Contractions are matched the same way as at runtime, including across combining
        marks that do not block them.
        """
        result = []
        remaining = list(text)
        while remaining:
            match = self.match_longest(remaining)
            if match is None:
                result.extend(self.elements_for((remaining[0],)))
                remaining.pop(0)
                continue
            elements, positions = match
            result.extend(elements)
            for position in sorted(positions, reverse=True):
                remaining.pop(position)
            remaining.pop(0)
        return result

    def match_longest(self, text):
        """Matches the longest tailored mapping at the start of a string.

        Returns its elements and the positions the match consumed after the first one, or
        None when only the first character is mapped.
        """
        best = None
        for length in range(min(8, len(text)), 1, -1):
            key = tuple(text[:length])
            if key in self.mappings:
                return self.mappings[key], list(range(1, length))
        # a contraction can reach across combining marks that do not block it
        for key, elements in self.mappings.items():
            if len(key) < 2 or key[0] != text[0]:
                continue
            positions = []
            matched = 1
            skipped = False
            for position in range(1, len(text)):
                if matched == len(key):
                    break
                combining_class = self.normalization.combining_class.get(text[position], 0)
                if text[position] == key[matched]:
                    if skipped and combining_class == 0:
                        break
                    positions.append(position)
                    matched += 1
                    continue
                following = self.normalization.combining_class.get(key[matched], 0)
                if combining_class == 0 or combining_class >= following:
                    break
                skipped = True
            if matched == len(key) and (best is None or len(key) > best[0]):
                best = (len(key), elements, positions)
        return (best[1], best[2]) if best else None

    def match_composite(self, text, marks):
        """Matches a tailored string against the marks of a composite character.

        Returns the part of the string that has to follow the composite and the marks that
        keep their own weights, or None when the composite cannot pick up the tailoring.
        """
        matched = 1
        blocking = 0
        unmatched = []
        for mark in marks:
            if matched < len(text):
                if mark == text[matched]:
                    matched += 1
                    blocking = 0
                    continue
                # the mark is skipped, which is only possible when it does not block the
                # character that follows it in the tailored string
                combining_class = self.normalization.combining_class.get(mark, 0)
                following = self.normalization.combining_class.get(text[matched], 0)
                if combining_class == 0 or combining_class >= following or combining_class <= blocking:
                    return None
                blocking = combining_class
            unmatched.append(mark)
        return text[matched:], tuple(unmatched)

    def canonical_order(self, text):
        """Puts the combining marks of a string in canonical order.

        The rules write the marks in their natural order, the text that is collated has them
        in the order the normalization puts them in.
        """
        result = list(text)
        for index in range(1, len(result)):
            combining_class = self.normalization.combining_class.get(result[index], 0)
            if combining_class == 0:
                continue
            position = index
            while position > 0:
                previous = self.normalization.combining_class.get(result[position - 1], 0)
                if previous == 0 or previous <= combining_class:
                    break
                result[position - 1], result[position] = result[position], result[position - 1]
                position -= 1
        return tuple(result)

    def add_mapping(self, text, elements):
        self.mappings[text] = elements
        # the same weights apply to the canonically equivalent form of the string
        decomposed = []
        for codepoint in text:
            decomposed.extend(self.normalization.decomposition.get(codepoint, [codepoint]))
        decomposed = tuple(decomposed)
        if decomposed != text and decomposed not in self.mappings:
            self.mappings[decomposed] = elements


# ---------------------------------------------------------------------------
# script reordering
# ---------------------------------------------------------------------------

# the groups that come before the scripts, they keep their position unless the rules
# name them explicitly
SPECIAL_GROUPS = ["space", "punct", "symbol", "currency", "digit"]

SPECIAL_GROUP_NAMES = {
    "SPACE": "space",
    "PUNCTUATION": "punct",
    "SYMBOL": "symbol",
    "CURRENCY": "currency",
    "DIGIT": "digit",
}

# lead bytes that the reordered groups are laid out over, the Han range follows the scripts
REORDER_LEAD_BYTES = list(range(0x03, 0xFE))


def build_reorder_groups(table):
    """Groups consecutive lead bytes that belong to the same scripts.

    The Han lead bytes of the file's layout are not used at runtime, Han uses the lead
    bytes that follow the scripts instead.
    """
    groups = []
    for lead in range(0x03, 0x81):
        scripts = table.lead_byte_scripts.get(lead, [])
        if not scripts or scripts[0].startswith("REORDER_RESERVED") or scripts[0].startswith("Hani"):
            # reserved and unused lead bytes are free space that the groups are laid out over
            names = []
        elif scripts[0] in SPECIAL_GROUP_NAMES:
            names = [SPECIAL_GROUP_NAMES[script] for script in scripts if script in SPECIAL_GROUP_NAMES]
        elif scripts[0] in ("IMPLICIT", "TRAILING", "SPECIAL", "TERMINATOR", "LEVEL-SEPARATOR", "FIELD-SEPARATOR"):
            continue
        else:
            names = scripts
        if not names:
            # reserved and unused lead bytes are free space the groups can move into
            continue
        if groups and groups[-1][0] == names and groups[-1][1][-1] == lead - 1:
            groups[-1][1].append(lead)
        else:
            groups.append((names, [lead]))
    # the lead bytes above the scripts hold the weights that tailorings allocate after the
    # last regular script, which is where the Han characters of the CJK locales end up
    groups.append((["Hani", "Hans", "Hant"], list(range(0x81, 0xFE))))
    return groups


def build_reorder_table(table, reorder):
    """Builds the lead byte permutation for a [reorder ...] rule.

    The groups are laid out in the requested order, groups that are not named keep their
    relative order. Groups are moved as whole lead bytes, so only the lead byte of a
    primary weight changes.
    """
    groups = build_reorder_groups(table)
    requested = [code if code != "Zzzz" else "others" for code in reorder]

    def matches(group, code):
        return code in group[0]

    listed = []
    for code in requested:
        if code == "others":
            listed.append(None)
            continue
        for group in groups:
            if matches(group, code) and group not in listed:
                listed.append(group)
                break
    remaining = [group for group in groups if group not in listed]
    if None in listed:
        index = listed.index(None)
        order = listed[:index] + remaining + listed[index + 1 :]
    elif any(group and set(group[0]) & set(SPECIAL_GROUPS) for group in listed):
        order = listed + remaining
    else:
        # only scripts were named, the special groups keep their leading position
        specials = [group for group in remaining if set(group[0]) & set(SPECIAL_GROUPS)]
        others = [group for group in remaining if not set(group[0]) & set(SPECIAL_GROUPS)]
        order = specials + listed + others
    order = [group for group in order if group is not None]

    return lay_out_groups(order)


def lay_out_groups(order):
    """Assigns lead bytes to the groups, moving as few of them as possible.

    A group keeps its own lead bytes when they are still free and no group that has to sort
    after it sits below them, otherwise it moves to the lowest lead bytes that are free.
    """
    permutation = list(range(256))
    taken = [False] * 256
    for lead in range(0, 256):
        if lead not in REORDER_LEAD_BYTES:
            taken[lead] = True
    cursor = REORDER_LEAD_BYTES[0] - 1

    def fits(start, size):
        return start + size <= 256 and not any(taken[start : start + size])

    for index, group in enumerate(order):
        leads = group[1]
        size = len(leads)
        following = [other[1][0] for other in order[index + 1 :]]
        start = leads[0]
        if not (start > cursor and all(start < other for other in following) and fits(start, size)):
            # the group has to move, put it directly after the groups before it
            start = cursor + 1
            while not fits(start, size):
                start += 1
        for offset, lead in enumerate(leads):
            permutation[lead] = start + offset
            taken[start + offset] = True
        cursor = start + size - 1
    return permutation


# ---------------------------------------------------------------------------
# unicode sets
# ---------------------------------------------------------------------------


def parse_unicode_set(text):
    """Parses the subset of the UnicodeSet syntax that the collation rules use."""
    codepoints = set()
    text = text.strip()
    if text.startswith("[") and text.endswith("]"):
        text = text[1:-1]
    position = 0
    characters = []
    while position < len(text):
        char = text[position]
        if char == "\\" and position + 1 < len(text) and text[position + 1] == "u":
            characters.append(chr(int(text[position + 2 : position + 6], 16)))
            position += 6
            continue
        if char.isspace():
            characters.append(None)
            position += 1
            continue
        characters.append(char)
        position += 1
    index = 0
    while index < len(characters):
        if characters[index] is None:
            index += 1
            continue
        if index + 2 < len(characters) and characters[index + 1] == "-" and characters[index + 2] is not None:
            for codepoint in range(ord(characters[index]), ord(characters[index + 2]) + 1):
                codepoints.add(codepoint)
            index += 3
            continue
        codepoints.add(ord(characters[index]))
        index += 1
    return codepoints


def parse_collation_xml(text):
    """Returns the rules of every collation type in a CLDR collation file."""
    import xml.etree.ElementTree as ElementTree

    result = {}
    tree = ElementTree.fromstring(text)
    for default in tree.iter("defaultCollation"):
        result["default"] = default.text.strip()
    short = set()
    for collation in tree.iter("collation"):
        alt = collation.get("alt")
        if alt == "proposed":
            # a proposed change to the rules, which is not part of the collation yet
            continue
        collation_type = collation.get("type")
        if collation_type in short and alt is None:
            # the short rules of the Chinese collations replace the long ones
            continue
        rules_element = collation.find("cr")
        rule_text = rules_element.text if rules_element is not None and rules_element.text else ""
        rules = RuleParser(rule_text).parse()
        for settings in collation.iter("settings"):
            for key, value in settings.attrib.items():
                rules.settings[key] = value
        result[collation_type] = rules
        if alt == "short":
            short.add(collation_type)
    return result


def parse_import_target(target):
    """Splits an import target such as "de-u-co-phonebk" into a locale and a collation type."""
    if "-u-co-" in target:
        locale, collation_type = target.split("-u-co-", 1)
    else:
        locale, collation_type = target, "standard"
    locale = locale.replace("-", "_")
    if locale in ("und", "root"):
        locale = "root"
    return locale, collation_type


# ---------------------------------------------------------------------------
# table building
# ---------------------------------------------------------------------------


class TableBuilder:
    """Collects the flat arrays that make up a collation table."""

    def __init__(self):
        self.ces = []
        self.ce_offsets = {}
        self.expansions = []
        self.entries = []
        self.contexts = []
        self.context_chars = []
        self.values = {}  # code point -> trie value

    def add_ces(self, elements):
        key = tuple(element.key() for element in elements)
        if key in self.ce_offsets:
            return self.ce_offsets[key]
        offset = len(self.ces)
        self.ces.extend(elements)
        self.ce_offsets[key] = offset
        return offset

    def add_chars(self, codepoints):
        offset = len(self.context_chars)
        self.context_chars.extend(codepoints)
        return offset

    def value_for(self, elements):
        offset = self.add_ces(elements)
        if len(elements) == 1:
            return (TAG_SINGLE << 30) | offset
        self.expansions.append((offset, len(elements)))
        return (TAG_EXPANSION << 30) | (len(self.expansions) - 1)

    def add_contexts(self, cp, default_elements, contexts):
        """contexts is a list of (kind, codepoints, elements)."""
        ce_offset = self.add_ces(default_elements) if default_elements else 0
        ce_count = len(default_elements) if default_elements else 0
        context_offset = len(self.contexts)
        # match longer contexts first, and prefixes before contractions
        contexts.sort(key=lambda context: (-context[0], -len(context[1])))
        for kind, codepoints, elements in contexts:
            self.contexts.append(
                (self.add_chars(codepoints), len(codepoints), kind, self.add_ces(elements), len(elements))
            )
        self.entries.append((ce_offset, ce_count, context_offset, len(contexts)))
        self.values[cp] = (TAG_CONTEXTS << 30) | (len(self.entries) - 1)


def build_table(mappings, prefixes):
    """Builds the flat arrays for a set of mappings.

    mappings maps a code point sequence to its collation elements, prefixes maps
    a sequence whose first code points are the preceding context.
    """
    builder = TableBuilder()
    contexts = {}
    for codepoints, elements in mappings.items():
        if len(codepoints) == 1:
            continue
        contexts.setdefault(codepoints[0], []).append((CONTEXT_CONTRACTION, list(codepoints[1:]), elements))
    for codepoints, elements in prefixes.items():
        # the last code point carries the mapping, everything before it is the context
        # the context is stored in reverse order because it is matched backwards
        contexts.setdefault(codepoints[-1], []).append((CONTEXT_PREFIX, list(reversed(codepoints[:-1])), elements))

    for codepoints, elements in sorted(mappings.items()):
        if len(codepoints) != 1:
            continue
        cp = codepoints[0]
        if cp in contexts:
            continue
        builder.values[cp] = builder.value_for(elements)
    for cp, entry_contexts in sorted(contexts.items()):
        builder.add_contexts(cp, mappings.get((cp,)), entry_contexts)
    return builder


def build_tailoring_tables(mappings, root, suppressed, prefix_mappings=None):
    """Builds the flat arrays of a tailoring, merging the tailored and root contexts."""
    root_contractions = {}
    for codepoints, elements in root.mappings.items():
        if len(codepoints) > 1:
            root_contractions.setdefault(codepoints[0], []).append((codepoints, elements))
    root_prefixes = {}
    for codepoints, elements in root.prefixes.items():
        root_prefixes.setdefault(codepoints[-1], []).append((codepoints, elements))

    single = {}
    contractions = {}
    for codepoints, elements in mappings.items():
        if len(codepoints) == 1:
            single[codepoints[0]] = elements
        else:
            contractions.setdefault(codepoints[0], []).append((CONTEXT_CONTRACTION, list(codepoints[1:]), elements))
    for (context, codepoints), elements in (prefix_mappings or {}).items():
        if len(codepoints) != 1:
            # a context on a contraction is not supported, it becomes a plain contraction
            contractions.setdefault(codepoints[0], []).append((CONTEXT_CONTRACTION, list(codepoints[1:]), elements))
            continue
        # the context is stored in reverse order because it is matched backwards
        contractions.setdefault(codepoints[0], []).append((CONTEXT_PREFIX, list(reversed(context)), elements))

    builder = TableBuilder()
    for cp in sorted(set(single) | set(contractions)):
        contexts = list(contractions.get(cp, []))
        if cp not in suppressed:
            tailored = set(mappings)
            for codepoints, elements in root_contractions.get(cp, []):
                if codepoints not in tailored:
                    contexts.append((CONTEXT_CONTRACTION, list(codepoints[1:]), elements))
            for codepoints, elements in root_prefixes.get(cp, []):
                contexts.append((CONTEXT_PREFIX, list(reversed(codepoints[:-1])), elements))
        default = single.get(cp, root.mappings.get((cp,)))
        if contexts:
            builder.add_contexts(cp, default, contexts)
        elif default:
            builder.values[cp] = builder.value_for(default)
    return builder


def build_trie(values):
    """Builds a two stage trie over the code point space."""
    stage1 = []
    stage2 = []
    blocks = {}
    for base in range(0, MAX_CODEPOINT, TRIE_BLOCK):
        block = tuple(values.get(base + i, 0) for i in range(TRIE_BLOCK))
        if block in blocks:
            stage1.append(blocks[block])
            continue
        offset = len(stage2)
        blocks[block] = offset
        stage2.extend(block)
        stage1.append(offset)
    return stage1, stage2


# ---------------------------------------------------------------------------
# C++ output
# ---------------------------------------------------------------------------

HEADER = """//===----------------------------------------------------------------------===//
//                         DuckDB
//
// %s
//
// This file is generated by extension/icu/scripts/generate_collation_data.py
// from Unicode Consortium data (CLDR %s, UCD %s). Do not edit.
//
//===----------------------------------------------------------------------===//

"""


def pack(values, width):
    """Packs values into little endian bytes of the given width."""
    formats = {8: "<Q", 4: "<I", 2: "<H", 1: "<B"}
    mask = (1 << (width * 8)) - 1
    return b"".join(struct.pack(formats[width], value & mask) for value in values)


class Unit:
    """A group of arrays that is compressed and decompressed as a whole.

    The arrays are stored largest element first, so that every one of them is aligned once
    the counts at the start of the unit are. The counts let the loader find the arrays back.
    """

    def __init__(self):
        self.arrays = []

    def add(self, values, width, per_element=1):
        self.arrays.append((list(values), width, per_element))

    def serialize(self):
        counts = [len(values) // per_element for values, _, per_element in self.arrays]
        header = pack(counts, 4)
        header += b"\0" * (-len(header) % 8)
        return header + b"".join(pack(values, width) for values, width, _ in self.arrays)


def compress_units(units, dictionary_size=8192):
    """Compresses the units against a dictionary trained on the ones that are optional.

    Every unit is compressed on its own, so that a collation can be decompressed without
    decompressing the others, while the shared dictionary keeps the small ones small.
    """
    import shutil
    import tempfile

    directory = tempfile.mkdtemp(prefix="collation-data-")
    try:
        paths = []
        for index, (name, blob, trainable) in enumerate(units):
            path = os.path.join(directory, "%04d.bin" % index)
            with open(path, "wb") as f:
                f.write(blob)
            paths.append((path, trainable))
        dictionary_path = os.path.join(directory, "dictionary")
        subprocess.run(
            ["zstd", "--train", "-q", "-f", "--maxdict=%d" % dictionary_size, "-o", dictionary_path]
            + [path for path, trainable in paths if trainable],
            check=True,
            capture_output=True,
        )
        with open(dictionary_path, "rb") as f:
            dictionary = f.read()
        compressed = []
        for path, _ in paths:
            subprocess.run(
                ["zstd", "-q", "-f", "-19", "-D", dictionary_path, path, "-o", path + ".zst"],
                check=True,
                capture_output=True,
            )
            with open(path + ".zst", "rb") as f:
                compressed.append(f.read())
        return dictionary, compressed
    finally:
        shutil.rmtree(directory)


def format_bytes(name, data, per_line=24):
    lines = ["const uint8_t %s[] = {" % name]
    for start in range(0, len(data), per_line):
        lines.append("    " + " ".join("0x%02X," % byte for byte in data[start : start + per_line]))
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def root_unit(table, builder, stage1, stage2, han_range_list):
    """The tables of the root collation, which every collation uses."""
    # an index of the Han ranges by code point block, so that a lookup only searches the
    # ranges that can hold a code point of that block
    lower_bounds = []
    upper_bounds = []
    lower = 0
    upper = 0
    for block in range(0, (MAX_CODEPOINT >> HAN_BLOCK_SHIFT) + 1):
        start = block << HAN_BLOCK_SHIFT
        while lower < len(han_range_list) and han_range_list[lower][0] + han_range_list[lower][1] <= start:
            lower += 1
        while upper < len(han_range_list) and han_range_list[upper][0] < start + (1 << HAN_BLOCK_SHIFT):
            upper += 1
        lower_bounds.append(lower)
        upper_bounds.append(upper)

    unit = Unit()
    unit.add([ce.encode() for ce in builder.ces], 8)
    unit.add(stage1, 4)
    unit.add(stage2, 4)
    unit.add([value for expansion in builder.expansions for value in expansion], 4, 2)
    unit.add([value for entry in builder.entries for value in entry], 4, 4)
    unit.add([value for context in builder.contexts for value in context], 4, 5)
    unit.add(builder.context_chars, 4)
    unit.add(lower_bounds, 4)
    unit.add(upper_bounds, 4)
    unit.add([start for start, _, _ in han_range_list], 4)
    unit.add([index for _, _, index in han_range_list], 4)
    unit.add([length for _, length, _ in han_range_list], 2)
    unit.add([1 if value else 0 for value in table.compressible], 1)
    return unit


def normalization_unit(data):
    """The canonical decompositions and combining classes."""
    chars = []
    decompositions = []
    for codepoint in sorted(data.decomposition):
        decomposition = data.decomposition[codepoint]
        decompositions.append((codepoint, len(chars), len(decomposition)))
        chars.extend(decomposition)

    def ranges_of(values):
        result = []
        for codepoint in sorted(values):
            value = values[codepoint]
            if result and result[-1][1] + 1 == codepoint and result[-1][2] == value:
                result[-1] = (result[-1][0], codepoint, value)
            else:
                result.append((codepoint, codepoint, value))
        return result

    combining = ranges_of(data.combining_class)
    fcd = ranges_of(build_fcd(data))

    unit = Unit()
    unit.add(chars, 4)
    unit.add([codepoint for codepoint, _, _ in decompositions], 4)
    unit.add([offset for _, offset, _ in decompositions], 4)
    unit.add([start for start, _, _ in combining], 4)
    unit.add([end for _, end, _ in combining], 4)
    unit.add([start for start, _, _ in fcd], 4)
    unit.add([end for _, end, _ in fcd], 4)
    unit.add([value for _, _, value in fcd], 2)
    unit.add([length for _, _, length in decompositions], 1)
    unit.add([value for _, _, value in combining], 1)
    return unit


def tailoring_unit(tailoring):
    """The tables of one tailoring."""
    builder = tailoring.builder
    codepoints = sorted(builder.values)
    unit = Unit()
    unit.add([ce.encode() for ce in builder.ces], 8)
    unit.add([value for expansion in builder.expansions for value in expansion], 4, 2)
    unit.add([value for entry in builder.entries for value in entry], 4, 4)
    unit.add([value for context in builder.contexts for value in context], 4, 5)
    unit.add(builder.context_chars, 4)
    unit.add(codepoints, 4)
    unit.add([builder.values[codepoint] for codepoint in codepoints], 4)
    unit.add(tailoring.reorder_table or [], 1)
    return unit


# region variants that are exposed as separate collations, mapped to the CLDR locale that
# holds their tailoring
LOCALE_ALIASES = {
    "ar_sa": "ar",
    "he_il": "he",
    "id_id": "id",
    "nb_no": "nb",
    "pa_in": "pa",
    "sr_ba": "sr",
    "sr_me": "sr",
    "sr_rs": "sr",
    "zh_cn": "zh",
    "zh_hk": "zh_hant",
    "zh_mo": "zh_hant",
    "zh_sg": "zh",
    "zh_tw": "zh_hant",
}

# CLDR locales that are not exposed as collations of their own
UNEXPOSED_LOCALES = {"root", "en_us_posix"}

STRENGTH_VALUES = {
    "1": 0,
    "primary": 0,
    "2": 1,
    "secondary": 1,
    "3": 2,
    "tertiary": 2,
    "4": 3,
    "quaternary": 3,
    "I": 4,
    "identical": 4,
}


class Tailoring:
    def __init__(self, name, builder, settings, reorder_table, mappings=None):
        self.name = name
        self.builder = builder
        self.settings = settings
        self.reorder_table = reorder_table
        # the strings the rules map, which generate_collation_tests.py orders
        self.mappings = mappings or {}


def collation_settings(rules_settings):
    settings = {
        "strength": 2,
        "case_level": False,
        "case_first": 0,
        "alternate_shifted": False,
        "backward_secondary": False,
        "normalization": False,
    }
    for key, value in rules_settings.items():
        value = value.strip()
        if key == "strength":
            settings["strength"] = STRENGTH_VALUES.get(value, 2)
        elif key == "caseLevel":
            settings["case_level"] = value == "on"
        elif key == "caseFirst":
            settings["case_first"] = {"lower": 1, "upper": 2}.get(value, 0)
        elif key == "alternate":
            settings["alternate_shifted"] = value == "shifted"
        elif key == "backwards":
            settings["backward_secondary"] = value in ("2", "on")
        elif key == "normalization":
            settings["normalization"] = value == "on"
    return settings


def parse_parent_locales(text):
    """Reads the locales that inherit their data from another locale."""
    import xml.etree.ElementTree as ElementTree

    parents = {}
    tree = ElementTree.fromstring(text)
    for entry in tree.iter("parentLocale"):
        for locale in entry.get("locales", "").split():
            parents[locale.lower()] = entry.get("parent").lower()
    return parents


def build_tailorings(root, normalization, implicit_primary):
    """Builds the tailoring of every CLDR collation locale."""
    listing = fetch_collation_listing()
    collations = {}
    for name, text in listing.items():
        collations[name] = parse_collation_xml(text)
    parents = parse_parent_locales(fetch(CLDR_BASE + "/supplemental/supplementalData.xml", "supplementalData.xml"))

    def parents_of(name):
        """The locales a locale inherits from, most specific first."""
        result = []
        if name in parents:
            result.append(parents[name])
        if "_" in name:
            # a script or region variant also inherits the rules of its language
            result.append(name.rsplit("_", 1)[0])
        return [parent for parent in result if parent not in UNEXPOSED_LOCALES]

    def collation_type_of(name):
        """The collation type a locale uses, which its variants inherit."""
        while name:
            locale = collations.get(name, {})
            if "default" in locale:
                return locale["default"]
            name = name.rsplit("_", 1)[0] if "_" in name else None
        return "standard"

    def source_of(name):
        """The rules a collation uses: a locale and one of its collation types.

        A locale can name a collation type that only its parent has the rules for, which is
        how the traditional Chinese locales end up with the stroke order of zh.
        """
        name = LOCALE_ALIASES.get(name, name)
        for collation_type in (collation_type_of(name), "standard"):
            queue = [name]
            while queue:
                locale = queue.pop(0)
                if collation_type in collations.get(locale, {}):
                    return (locale, collation_type)
                queue.extend(parents_of(locale))
        return None

    def resolve_import(target):
        locale, collation_type = parse_import_target(target)
        rules = collations.get(locale.lower(), {}).get(collation_type)
        if isinstance(rules, str):
            rules = None
        if rules is None:
            sys.stderr.write("warning: unknown import target %s\n" % target)
        return rules

    sources = {}
    for name in sorted(collations):
        if name in UNEXPOSED_LOCALES:
            continue
        sources[name] = source_of(name)
    for alias in LOCALE_ALIASES:
        sources[alias] = source_of(alias)

    tailorings = {}
    for source in sorted(set(value for value in sources.values() if value is not None)):
        name = source[0] if source[1] == "standard" else "%s_%s" % source
        rules = collations[source[0]][source[1]]
        builder = TailoringBuilder(root, normalization, implicit_primary)
        # the first pass counts how many weights every gap needs, the second assigns them
        builder.build(rules, resolve_import)
        builder.start_assignment()
        builder.build(rules, resolve_import)
        builder.close_over_composites()
        reorder_table = build_reorder_table(root, builder.reorder) if builder.reorder else None
        tables = build_tailoring_tables(builder.mappings, root, builder.suppressed, builder.prefix_mappings)
        tailorings[name] = Tailoring(
            name, tables, collation_settings(builder.settings), reorder_table, builder.mappings
        )

    # the locales that use the root collation share an empty tailoring
    tailorings["root"] = Tailoring("root", build_tailoring_tables({}, root, set()), collation_settings({}), None)
    sources = {
        name: "root" if source is None else source[0] if source[1] == "standard" else "%s_%s" % source
        for name, source in sources.items()
    }
    return tailorings, sources


def fetch_collation_listing():
    """Downloads every CLDR collation file, returning the file contents by locale."""
    import json

    index = fetch(
        "https://api.github.com/repos/unicode-org/cldr/contents/common/collation?ref=" + CLDR_VERSION,
        "collation-index.json",
    )
    result = {}
    for entry in json.loads(index):
        name = entry["name"]
        if not name.endswith(".xml"):
            continue
        result[name[:-4].lower()] = fetch(CLDR_BASE + "/collation/" + name, "collation/" + name)
    return result


def variable_top_primaries(table):
    """The last variable primary for maxVariable space, punct, symbol and currency, ending each group as ICU does."""
    groups = ["SPACE", "PUNCTUATION", "SYMBOL", "CURRENCY", "DIGIT"]
    tops = []
    for next_group in groups[1:]:
        primary, new_lead_byte = table.group_starts[next_group]
        start = (primary >> 24) << 8 if new_lead_byte else primary >> 16
        tops.append((start << 16) - 1)
    # maxVariable=punct is the default, FractionalUCA.txt names its end as [variable top]
    if tops[1] != table.variable_top:
        raise ValueError("punctuation ends at 0x%08X, but [variable top] is 0x%08X" % (tops[1], table.variable_top))
    return tops


def write_collation_data(path, table, builder, stage1, stage2, han_range_list, normalization, tailorings, sources):
    """Writes the compressed tables and the metadata the loader needs to find them."""
    names = sorted(tailorings)
    units = [
        ("root", root_unit(table, builder, stage1, stage2, han_range_list).serialize(), False),
        ("normalization", normalization_unit(normalization).serialize(), False),
    ]
    for name in names:
        units.append((name, tailoring_unit(tailorings[name]).serialize(), True))
    dictionary, compressed = compress_units(units)

    # the collations that are registered, every one of them points at the locale it takes
    # its rules from
    registered = {}
    for name, source in sources.items():
        parts = name.split("_")
        if len(parts) > 1 and len(parts[1]) == 4:
            # a script variant, it is only reachable through a region alias
            continue
        registered[name] = source

    out = [HEADER % ("collation_data.cpp", CLDR_VERSION, UNICODE_VERSION)]
    out.append('#include "collation_data.hpp"\n')
    out.append("namespace duckdb {")
    out.append("namespace collation {")
    out.append("")
    out.append("// The tables are compressed with zstd. Every unit is compressed on its own so that")
    out.append("// a collation can be loaded without loading the others, against a dictionary that is")
    out.append("// trained on all of them so that the small ones stay small.")
    out.append("")
    out.append(format_bytes("collation_dictionary", dictionary))
    out.append("const uint32_t collation_dictionary_size = %d;" % len(dictionary))
    out.append("")
    for index, ((name, blob, _), data) in enumerate(zip(units, compressed)):
        out.append("// %s: %d bytes" % (name, len(blob)))
        out.append(format_bytes("collation_unit_%d" % index, data))
    out.append("const CollationUnit collation_units[] = {")
    for index, ((_, blob, _), data) in enumerate(zip(units, compressed)):
        out.append("    {collation_unit_%d, %d, %d}," % (index, len(data), len(blob)))
    out.append("};")
    out.append("")
    out.append("const uint32_t collation_unit_count = %d;" % len(units))
    out.append("")

    out.append("const CollationInfo collation_infos[] = {")
    for name in sorted(registered):
        tailoring = tailorings[registered[name]]
        settings = tailoring.settings
        # the first two units are the root collation and the normalization tables
        unit = 2 + names.index(registered[name])
        out.append(
            '    {"%s", %d, %d, %s, %d, %s, %s, %s},'
            % (
                name,
                unit,
                settings["strength"],
                "true" if settings["case_level"] else "false",
                settings["case_first"],
                "true" if settings["alternate_shifted"] else "false",
                "true" if settings["backward_secondary"] else "false",
                "true" if settings["normalization"] else "false",
            )
        )
    out.append("};")
    out.append("")
    out.append("const uint32_t collation_count = %d;" % len(registered))
    out.append(
        "const uint32_t variable_top_primaries[] = {%s};"
        % ", ".join("0x%08X" % top for top in variable_top_primaries(table))
    )
    out.append("")
    out.append("} // namespace collation")
    out.append("} // namespace duckdb")
    out.append("")
    with open(path, "w") as f:
        f.write("\n".join(out))
    sys.stderr.write(
        "data: %.1f KB in %d units, compressed to %.1f KB with a %.1f KB dictionary\n"
        % (
            sum(len(blob) for _, blob, _ in units) / 1024,
            len(units),
            sum(len(data) for data in compressed) / 1024,
            len(dictionary) / 1024,
        )
    )


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    fractional = fetch(CLDR_BASE + "/uca/FractionalUCA.txt", "FractionalUCA.txt")
    unicode_data = fetch(UCD_BASE + "/UnicodeData.txt", "UnicodeData.txt")

    table = parse_fractional_uca(fractional)
    sys.stderr.write(
        "root: %d mappings, %d prefix mappings, %d Han characters\n"
        % (len(table.mappings), len(table.prefixes), len(table.han_order))
    )

    # the implicit weights of unassigned code points share a single compressible lead byte
    table.compressible[UNASSIGNED_IMPLICIT_LEAD_BYTE] = True

    builder = build_table(table.mappings, table.prefixes)
    stage1, stage2 = build_trie(builder.values)
    sys.stderr.write("root: %d collation elements, %d trie entries\n" % (len(builder.ces), len(stage2)))

    normalization = parse_unicode_data(unicode_data)
    sys.stderr.write(
        "normalization: %d decompositions, %d combining classes\n"
        % (len(normalization.decomposition), len(normalization.combining_class))
    )
    han_index = {cp: i for i, cp in enumerate(table.han_order)}

    def implicit_primary(cp):
        if cp in han_index:
            return han_primary(han_index[cp])
        return unassigned_primary(cp)

    tailorings, sources = build_tailorings(table, normalization, implicit_primary)
    sys.stderr.write(
        "tailorings: %d rule sets for %d collations, %d mappings\n"
        % (len(tailorings), len(sources), sum(len(t.builder.values) for t in tailorings.values()))
    )
    write_collation_data(
        os.path.join(OUTPUT_DIR, "collation_data.cpp"),
        table,
        builder,
        stage1,
        stage2,
        han_ranges(table.han_order),
        normalization,
        tailorings,
        sources,
    )


if __name__ == "__main__":
    main()
