#!/usr/bin/env python3
# Writes a C source defining duckdb_register_static_extensions, which registers the named statically built
# extensions and capabilities (httplib, loadable_extensions). Each extension archive carries a describe function, duckdb_extension_<name>_describe; the generated
# function passes every one of them to duckdb_register_static_extension, which also pulls the extensions out of
# their archives. A named extension whose archive is missing fails the link. Call the function before opening a
# database, or compile extension/loader/static_extension_autoregister.cpp next to it to have it called before main.
#
#   STATICALLY_LINK_EXTENSIONS="parquet;json" make static_extension_loader     (or: python3 scripts/generate_static_extension_loader.py -o static_extension_loader.c parquet json)
#   cc -I duckdb/include main.c static_extension_loader.c libparquet_extension.a libjson_extension.a libduckdb_static.a -lstdc++
import argparse
import os
import re
import sys


def parse_names(values):
    names = []
    for value in values:
        for name in re.split(r'[;\s]+', value):
            if not name:
                continue
            if not re.fullmatch(r'[a-z0-9_]+', name):
                sys.exit(f'invalid extension name: {name!r}')
            if name not in names:
                names.append(name)
    return names


TEMPLATE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'extension', 'loader', 'static_extension_loader.c.in'
)


def generate(names):
    # mirrors duckdb_write_static_extension_loader in extension/extension_build_tools.cmake, which renders the same template
    describers = [f'duckdb_extension_{name}_describe' for name in names]
    values = {
        'LINK_EXTENSION_LIST': ' '.join(names) if names else '(none)',
        'DESCRIBE_DECLARATIONS': '\n'.join(
            f'int32_t {describe}(duckdb_extension_descriptor *descriptor);' for describe in describers
        ),
        'DESCRIBE_REGISTRATIONS': '\n'.join(
            f'\tif (duckdb_register_static_extension({describe}) != 0) {{\n\t\tresult = 1;\n\t}}'
            for describe in describers
        ),
    }
    with open(TEMPLATE) as f:
        content = f.read()
    for key, value in values.items():
        content = content.replace(f'@{key}@', value)
    return content


def main():
    parser = argparse.ArgumentParser(
        description='Generate a C source that registers the named static DuckDB extensions.'
    )
    parser.add_argument('extensions', nargs='*', help='extension names, separated by spaces or semicolons')
    parser.add_argument('--output', '-o', default='-', help='file to write, or - for stdout (default)')
    args = parser.parse_args()
    content = generate(parse_names(args.extensions))
    if args.output == '-':
        sys.stdout.write(content)
    else:
        with open(args.output, 'w') as f:
            f.write(content)


if __name__ == '__main__':
    main()
