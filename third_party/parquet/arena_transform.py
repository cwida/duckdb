#!/usr/bin/env python3
"""Post-processes the thrift generated parquet_types.{h,cpp} so that every struct that owns a string, a vector or
another such struct allocates from an ArenaAllocator (see parquet_arena.h).

Such structs are constructed with an allocator (`FileMetaData metadata(arena);`) and hand it down to all of their
children (strings, vectors and nested structs) using uses-allocator construction. Usage:

    arena_transform.py parquet_types.h parquet_types.cpp
"""
import re
import sys

header_path, source_path = sys.argv[1], sys.argv[2]
header = open(header_path).read()
source = open(source_path).read()

CLASS_RE = re.compile(r'^class (\w+) : public virtual ::apache::thrift::TBase \{(.*?)^\};', re.S | re.M)

PRIMITIVES = {'bool', 'double', 'int8_t', 'int16_t', 'int32_t', 'int64_t', 'uint32_t'}


def is_arena_member_type(type_name, arena_classes):
    return type_name == 'std::string' or type_name.startswith('duckdb::vector<') or type_name in arena_classes


def parse_members(body, class_name):
    """Returns the data members of a class as (type, name) pairs, plus whether the class has an __isset member"""
    start = body.index('~' + class_name)
    stripped = re.sub(r'/\*.*?\*/', '', body[start:], flags=re.S)
    members = []
    has_isset = False
    for line in stripped.split('\n')[1:]:
        line = line.strip()
        if not line:
            continue
        if line.startswith('_%s__isset' % class_name):
            has_isset = True
            break
        if line.startswith(('void ', 'uint32_t read', 'uint32_t write', 'virtual ')):
            break
        m = re.match(r'^(.+?)\s+(\w+);$', line)
        assert m, (class_name, line)
        members.append((m.group(1).strip(), m.group(2)))
    return members, has_isset


# --- find the classes that need an arena (transitively) ---
classes = {}
for m in CLASS_RE.finditer(header):
    classes[m.group(1)] = parse_members(m.group(2), m.group(1))

arena_classes = set()
changed = True
while changed:
    changed = False
    for name, (members, _) in classes.items():
        if name in arena_classes:
            continue
        if any(is_arena_member_type(t, arena_classes) for t, _ in members):
            arena_classes.add(name)
            changed = True


def split_top_level(text):
    parts, depth, cur = [], 0, ''
    for c in text:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        parts.append(cur.strip())
    return parts


# --- header ---
header = header.replace('#include "windows_compatibility.h"\n', '#include "windows_compatibility.h"\n#include "parquet_arena.h"\n', 1)


def replace_vector_types(text):
    text = re.sub(r'duckdb::vector<([^<>]*?)>', lambda m: 'ParquetVector<%s>' % m.group(1).replace('std::string', 'ParquetString'),
                  text)
    return text


def transform_header_class(match):
    name = match.group(1)
    body = match.group(0)
    if name not in arena_classes:
        return body
    members, has_isset = classes[name]
    # string members
    body = re.sub(r'^(  )std::string (\w+);$', r'\1ParquetString \2;', body, flags=re.M)
    body = replace_vector_types(body)
    # constructors
    ctor_decl = '  %s() noexcept;\n' % name
    assert ctor_decl in body, name
    body = body.replace(ctor_decl, (
        '  typedef ParquetAllocator<char> allocator_type;\n'
        '  explicit %(n)s(const allocator_type &alloc) noexcept;\n'
        '  %(n)s(const %(n)s&, const allocator_type &alloc);\n'
        '  %(n)s(%(n)s&&, const allocator_type &alloc);\n') % {'n': name})
    body = body.replace('%s& operator=(%s&&) noexcept;' % (name, name), '%s& operator=(%s&&);' % (name, name))
    return body


header = CLASS_RE.sub(transform_header_class, header)
for name in arena_classes:
    header = header.replace('void swap(%s &a, %s &b);\n' % (name, name), '')
header = replace_vector_types(header)

# --- source ---
source = replace_vector_types(source)
source = re.sub(r'xfer \+= iprot->readString\(this->', 'xfer += ReadParquetString(iprot, this->', source)
source = re.sub(r'xfer \+= iprot->readBinary\(this->', 'xfer += ReadParquetBinary(iprot, this->', source)
source = re.sub(r'xfer \+= oprot->writeString\(', 'xfer += WriteParquetString(oprot, ', source)
source = re.sub(r'xfer \+= oprot->writeBinary\(', 'xfer += WriteParquetBinary(oprot, ', source)

# setters of string members keep taking a std::string
source = re.sub(r'(::__set_\w+\(const std::string& val\) \{\n  this->(\w+)) = val;',
                r'\1.assign(val.data(), val.size());', source)


def member_init(member_type, member_name, old_inits, mode):
    """mode: 'default' | 'copy' | 'copy_alloc' | 'move' | 'move_alloc'"""
    arena = is_arena_member_type(member_type, arena_classes)
    if mode == 'default':
        if arena:
            return '%s(alloc)' % member_name
        return old_inits.get(member_name)
    src = 'o.%s' % member_name
    trivial = member_type in PRIMITIVES or member_type.endswith('::type')
    if mode.startswith('move') and not trivial:
        src = 'std::move(%s)' % src
    if mode.endswith('_alloc') and arena:
        return '%s(%s, alloc)' % (member_name, src)
    return '%s(%s)' % (member_name, src)


for name in arena_classes:
    members, has_isset = classes[name]

    # default constructor
    pattern = re.compile(r'^%s::%s\(\) noexcept(.*?)^\}\n' % (name, name), re.S | re.M)
    m = pattern.search(source)
    assert m, name
    old_inits = {}
    init_text = m.group(1)
    if ':' in init_text.split('{')[0]:
        raw = init_text.split('{')[0].split(':', 1)[1]
        for item in split_top_level(raw):
            old_inits[re.match(r'(\w+)\(', item).group(1)] = item
    inits = [member_init(t, n, old_inits, 'default') for t, n in members]
    inits = [i for i in inits if i]
    new_ctor = '%s::%s(const allocator_type &alloc) noexcept' % (name, name)
    if inits:
        new_ctor += '\n   : ' + ',\n     '.join(inits)
    new_ctor += ' {\n}\n'
    source = source[:m.start()] + new_ctor + source[m.end():]

    # copy and move constructors
    def ctor(sig, mode):
        inits = [member_init(t, n, {}, mode) for t, n in members]
        if has_isset:
            inits.append('__isset(o.__isset)')
        text = '%s::%s(%s)' % (name, name, sig)
        if mode.startswith('move') and not mode.endswith('_alloc'):
            text += ' noexcept'
        if inits:
            text += '\n   : ' + ',\n     '.join(inits)
        return text + ' {\n}\n'

    copy_re = re.compile(r'^%s::%s\(const %s& \w+\) \{.*?^\}\n' % (name, name, name), re.S | re.M)
    move_re = re.compile(r'^%s::%s\(%s&& \w+\) noexcept \{.*?^\}\n' % (name, name, name), re.S | re.M)
    m = copy_re.search(source)
    assert m, name
    source = source[:m.start()] + ctor('const %s& o' % name, 'copy') + \
        ctor('const %s& o, const allocator_type &alloc' % name, 'copy_alloc') + source[m.end():]
    m = move_re.search(source)
    assert m, name
    source = source[:m.start()] + ctor('%s&& o' % name, 'move') + \
        ctor('%s&& o, const allocator_type &alloc' % name, 'move_alloc') + source[m.end():]

    # move assignment may need to allocate in the arena of the target
    source = re.sub(r'(^%s& %s::operator=\(%s&& \w+\)) noexcept \{' % (name, name, name), r'\1 {', source, flags=re.M)

    # swap is not defined for objects that live in different arenas
    source = re.sub(r'^void swap\(%s &a, %s &b\) \{.*?^\}\n\n' % (name, name), '', source, flags=re.S | re.M)

open(header_path, 'w').write(header)
open(source_path, 'w').write(source)
