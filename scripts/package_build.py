import os
import sys
import shutil
import subprocess
from python_helpers import open_utf8
import re
import tempfile

excluded_objects = ['utf8proc_data.cpp']
# the built-in httplib client, registered like a static extension when it is packaged
httplib_source = os.path.join('src', 'main', 'http', 'http_client_httplib.cpp')
# installing and loading external extensions, registered like a static extension when it is packaged
loadable_extensions_sources = [
    os.path.join('src', 'main', 'extension', 'extension_install_dynamic.cpp'),
    os.path.join('src', 'main', 'extension', 'extension_load_dynamic.cpp'),
]


def third_party_includes():
    includes = []
    includes += [os.path.join('third_party', 'concurrentqueue')]
    includes += [os.path.join('third_party', 'fast_float')]
    includes += [os.path.join('third_party', 'fastpforlib')]
    includes += [os.path.join('third_party', 'fmt', 'include')]
    includes += [os.path.join('third_party', 'fsst')]
    includes += [os.path.join('third_party', 'httplib')]
    includes += [os.path.join('third_party', 'hyperloglog')]
    includes += [os.path.join('third_party', 'jaro_winkler')]
    includes += [os.path.join('third_party', 'jaro_winkler', 'details')]
    includes += [os.path.join('third_party', 'lz4')]
    includes += [os.path.join('third_party', 'brotli', 'include')]
    includes += [os.path.join('third_party', 'brotli', 'common')]
    includes += [os.path.join('third_party', 'brotli', 'dec')]
    includes += [os.path.join('third_party', 'brotli', 'enc')]
    includes += [os.path.join('third_party', 'mbedtls', 'include')]
    includes += [os.path.join('third_party', 'mbedtls', 'library')]
    includes += [os.path.join('third_party', 'miniz')]
    includes += [os.path.join('third_party', 'pcg')]
    includes += [os.path.join('third_party', 'pdqsort')]
    includes += [os.path.join('third_party', 're2')]
    includes += [os.path.join('third_party', 'ska_sort')]
    includes += [os.path.join('third_party', 'skiplist')]
    includes += [os.path.join('third_party', 'tdigest')]
    includes += [os.path.join('third_party', 'utf8proc')]
    includes += [os.path.join('third_party', 'utf8proc', 'include')]
    includes += [os.path.join('third_party', 'vergesort')]
    includes += [os.path.join('third_party', 'yyjson', 'include')]
    includes += [os.path.join('third_party', 'zstd', 'include')]
    includes += [os.path.join('third_party', 'jemalloc', 'include')]
    return includes


def third_party_sources():
    sources = []
    sources += [os.path.join('third_party', 'fmt')]
    sources += [os.path.join('third_party', 'fsst')]
    sources += [os.path.join('third_party', 'miniz')]
    sources += [os.path.join('third_party', 're2')]
    sources += [os.path.join('third_party', 'hyperloglog')]
    sources += [os.path.join('third_party', 'skiplist')]
    sources += [os.path.join('third_party', 'fastpforlib')]
    sources += [os.path.join('third_party', 'utf8proc')]
    sources += [os.path.join('third_party', 'mbedtls')]
    sources += [os.path.join('third_party', 'yyjson')]
    sources += [os.path.join('third_party', 'zstd')]
    sources += [os.path.join('third_party', 'jemalloc')]
    return sources


def file_is_lib(fname, libname):
    libextensions = ['.a', '.lib']
    libprefixes = ['', 'lib']
    for ext in libextensions:
        for prefix in libprefixes:
            potential_libname = prefix + libname + ext
            if fname == potential_libname:
                return True
    return False


def get_libraries(binary_dir, libraries, extensions):
    result_libs = []

    def find_library_recursive(search_dir, libname):
        flist = os.listdir(search_dir)
        for fname in flist:
            fpath = os.path.join(search_dir, fname)
            if os.path.isdir(fpath):
                entry = find_library_recursive(fpath, libname)
                if entry != None:
                    return entry
            elif os.path.isfile(fpath) and file_is_lib(fname, libname):
                return search_dir
        return None

    def find_library(search_dir, libname, result_libs, required=False):
        if libname == 'Threads::Threads':
            result_libs += [(None, 'pthread')]
            return
        libdir = find_library_recursive(binary_dir, libname)
        if libdir is None and required:
            raise Exception(f"Failed to locate required library {libname} in {binary_dir}")

        result_libs += [(libdir, libname)]

    duckdb_lib_name = 'duckdb_static'
    if os.name == 'nt':
        duckdb_lib_name = 'duckdb'
    find_library(os.path.join(binary_dir, 'src'), duckdb_lib_name, result_libs, True)
    for ext in extensions:
        find_library(os.path.join(binary_dir, 'extension', ext), ext + '_extension', result_libs, True)

    for libname in libraries:
        find_library(binary_dir, libname, result_libs)

    return result_libs


def includes(extensions):
    scripts_dir = os.path.dirname(os.path.abspath(__file__))
    # add includes for duckdb and extensions
    includes = []
    includes.append(os.path.join(scripts_dir, '..', 'src', 'include'))
    includes.append(os.path.join(scripts_dir, '..'))
    includes.append(os.path.join(scripts_dir, '..', 'third_party', 'utf8proc', 'include'))
    for ext in extensions:
        includes.append(os.path.join(scripts_dir, '..', 'extension', ext, 'include'))
    return includes


def include_flags(extensions):
    return ' ' + ' '.join(['-I' + x for x in includes(extensions)])


def convert_backslashes(x):
    return '/'.join(x.split(os.path.sep))


def get_relative_path(source_dir, target_file):
    source_dir = convert_backslashes(source_dir)
    target_file = convert_backslashes(target_file)

    # absolute path: try to convert
    if source_dir in target_file:
        target_file = target_file.replace(source_dir, "").lstrip('/')
    return target_file


def release_version():
    version_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ci', 'release_version.txt')
    with open_utf8(version_path, 'r') as version_file:
        version = version_file.read().strip()
    if re.fullmatch(r'[0-9]+\.[0-9]+', version) is None:
        raise ValueError("Invalid release version '{}' in {}".format(version, version_path))
    return version


def git_commit_count():
    try:
        return subprocess.check_output(['git', 'rev-list', '--count', 'HEAD'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return '0'


def git_commit_hash():
    if 'SETUPTOOLS_SCM_PRETEND_HASH' in os.environ:
        return os.environ['SETUPTOOLS_SCM_PRETEND_HASH'][:10]
    if os.getenv('DUCKDB_COMMIT'):
        return os.environ['DUCKDB_COMMIT'][:10]
    try:
        return subprocess.check_output(['git', 'log', '-1', '--format=%H'], text=True).strip()[:10]
    except (OSError, subprocess.CalledProcessError):
        return "0123456789"


def prefix_version(version):
    """Make sure the version is prefixed with 'v' to be of the form vX.Y.Z"""
    if version.startswith('v'):
        return version
    return 'v' + version


def git_dev_version():
    if 'SETUPTOOLS_SCM_PRETEND_VERSION' in os.environ:
        return prefix_version(os.environ['SETUPTOOLS_SCM_PRETEND_VERSION'])
    if os.getenv('DUCKDB_VERSION'):
        return prefix_version(os.environ['DUCKDB_VERSION'])
    if os.getenv('OVERRIDE_GIT_DESCRIBE'):
        return prefix_version(os.environ['OVERRIDE_GIT_DESCRIBE'])
    if os.getenv('DUCKDB_EXPLICIT_VERSION'):
        return prefix_version(os.environ['DUCKDB_EXPLICIT_VERSION'])
    return 'v{}.0-dev{}'.format(release_version(), git_commit_count())


def capi_version():
    """The C API version this source tree offers, as vMAJOR.MINOR.PATCH"""
    header = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'include', 'duckdb_extension.h')
    with open_utf8(header, 'r') as f:
        text = f.read()
    parts = []
    for part in ['MAJOR', 'MINOR', 'PATCH']:
        match = re.search('#define DUCKDB_EXTENSION_API_VERSION_{} ([0-9]+)'.format(part), text)
        if not match:
            raise ValueError('could not find DUCKDB_EXTENSION_API_VERSION_{} in {}'.format(part, header))
        parts.append(match.group(1))
    return 'v{}.{}.{}'.format(*parts)


def normalized_duckdb_version():
    """What a build of this tree stamps as the DuckDB version, as DUCKDB_NORMALIZED_VERSION does"""
    version = git_dev_version()
    return git_commit_hash() if re.search('-dev[0-9]+$', version) else version


def include_package(pkg_name, pkg_dir, include_files, include_list, source_list):
    import amalgamation

    original_path = sys.path
    # append the directory
    sys.path.append(pkg_dir)
    ext_pkg = __import__(pkg_name + '_config')

    ext_include_dirs = ext_pkg.include_directories
    ext_source_files = ext_pkg.source_files
    ext_kind = getattr(ext_pkg, 'extension_kind', 'CPP').upper()

    include_files += amalgamation.list_includes_files(ext_include_dirs)
    include_list += ext_include_dirs
    source_list += ext_source_files

    sys.path = original_path

    return ext_kind


def get_extension_linked_define(extension):
    return f'DUCKDB_EXTENSION_{extension.upper()}_LINKED'


def build_package(
    target_dir,
    extensions,
    linenumbers=False,
    unity_count=32,
    folder_name='duckdb',
    short_paths=False,
    default_linked_extensions=None,
    builtin_httplib=True,
    extension_load=True,
):
    if not os.path.isdir(target_dir):
        os.mkdir(target_dir)

    extensions = list(extensions)
    # Keep existing package_build behavior by default: all packaged extensions are linked.
    # Callers that package a superset can pass default_linked_extensions to emit a loader
    # that is controlled by DUCKDB_EXTENSION_<NAME>_LINKED compile definitions instead.
    if default_linked_extensions is None:
        default_linked_extensions = extensions
    default_linked_extensions = set(default_linked_extensions)
    packaged_extensions = set(extensions)
    unpackaged_linked_extensions = default_linked_extensions - packaged_extensions
    if unpackaged_linked_extensions:
        raise ValueError(
            "default_linked_extensions must be a subset of extensions: {}".format(
                ', '.join(sorted(unpackaged_linked_extensions))
            )
        )

    scripts_dir = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(scripts_dir)
    import amalgamation

    prev_wd = os.getcwd()
    os.chdir(os.path.join(scripts_dir, '..'))

    # obtain the list of source files from the amalgamation
    source_list = amalgamation.list_sources()
    builtin_httplib = builtin_httplib and extension_load
    excluded_sources = [] if extension_load else list(loadable_extensions_sources)
    if not builtin_httplib:
        excluded_sources.append(httplib_source)
    source_list = [x for x in source_list if x not in excluded_sources]
    include_list = amalgamation.list_include_dirs()
    include_files = amalgamation.list_includes()

    def copy_file(src, target_dir):
        # get the path
        full_path = src.split(os.path.sep)
        current_path = target_dir
        for i in range(len(full_path) - 1):
            current_path = os.path.join(current_path, full_path[i])
            if not os.path.isdir(current_path):
                os.mkdir(current_path)
        target_name = full_path[-1]
        target_file = os.path.join(current_path, target_name)
        amalgamation.copy_if_different(src, target_file)

    # include the main extension helper
    include_files += [os.path.join('src', 'include', 'duckdb', 'main', 'extension_helper.hpp')]
    include_files += [os.path.join('src', 'include', 'duckdb_static_extension.h')]
    # include the separate extensions, and generate their describe functions plus the object that registers the linked ones.
    # The registration only runs before main if the package's objects end up in the program: a consumer that archives
    # them first has to force-link the archive or call duckdb_register_static_extensions itself.
    ext_loader_defines = ''
    ext_describers = ''
    ext_registrations = ''
    # the httplib client and the loadable extensions support are not extensions, but register the same way, first
    registered_first = (['httplib'] if builtin_httplib else []) + (['loadable_extensions'] if extension_load else [])
    for name in registered_first:
        ext_describers += (
            f'extern "C" int32_t duckdb_extension_{name}_describe(duckdb_extension_descriptor *descriptor);\n\n'
        )
        ext_registrations += (
            f"\tif (duckdb_register_static_extension(duckdb_extension_{name}_describe) != 0) {{\n"
            "\t\tresult = 1;\n"
            "\t}\n"
        )
    with open(
        os.path.join(scripts_dir, '..', 'extension', 'loader', 'extension_describe.c.in')
    ) as describe_template_file:
        describe_template = describe_template_file.read()
    for ext in extensions:
        ext_path = os.path.join(scripts_dir, '..', 'extension', ext)
        ext_kind = include_package(ext, ext_path, include_files, include_list, source_list)

        ext_linked_define = get_extension_linked_define(ext)
        ext_linked_default = 1 if ext in default_linked_extensions else 0

        ext_loader_defines += (
            f"#ifndef {ext_linked_define}\n" f"#define {ext_linked_define} {ext_linked_default}\n" "#endif\n\n"
        )

        # the same describe function duckdb_add_extension_describe generates in extension/extension_build_tools.cmake
        if ext_kind == 'CAPI':
            entry_name, entry_field = f'{ext}_init_c_api', 'entry_capi_v1'
            # takes a duckdb_extension_info and a duckdb_extension_access pointer, returns bool
            entry_declaration = f'extern "C" int {entry_name}(void *info, void *access);'
        elif ext_kind == 'CAPI_V2':
            entry_name, entry_field = f'{ext}_init_c_api_v2', 'entry_capi_v2'
            # takes a duckdb_v2_extension_input pointer
            entry_declaration = f'extern "C" void {entry_name}(void *input);'
        else:
            entry_name, entry_field = f'{ext}_duckdb_cpp_init', 'entry_cpp'
            entry_declaration = f'extern "C" void {entry_name}(duckdb::ExtensionLoader &loader);'
        version_define = f'EXT_VERSION_{ext.upper()}'
        # what the entrypoint was built against, as extension_build_tools.cmake stamps it
        api_version = capi_version() if ext_kind in ('CAPI', 'CAPI_V2') else normalized_duckdb_version()
        describe = describe_template
        for key, value in {
            'NAME': ext,
            'ENTRY_DECLARATION': entry_declaration,
            'ENTRY_NAME': entry_name,
            'ENTRY_FIELD': entry_field,
            'EXTENSION_VERSION': version_define,
            'API_VERSION': '"{}"'.format(api_version),
        }.items():
            describe = describe.replace(f'@{key}@', value)

        ext_describers += (
            f"#if {ext_linked_define}\n"
            f"#ifndef {version_define}\n"
            f'#define {version_define} ""\n'
            "#endif\n"
            f"{describe}"
            "#endif\n\n"
        )
        ext_registrations += (
            f"#if {ext_linked_define}\n"
            f"\tif (duckdb_register_static_extension(duckdb_extension_{ext}_describe) != 0) {{\n"
            "\t\tresult = 1;\n"
            "\t}\n"
            "#endif\n"
        )

    # the same shape as extension/loader/static_extension_loader.c.in, with the describe functions inlined above it
    loader_code = (
        "// Generated by package_build.py. Do not edit.\n"
        + ext_loader_defines
        + '#include "duckdb/main/extension/extension_loader.hpp"\n'
        + '#include "duckdb_static_extension.h"\n\n'
        + ext_describers
        + "#ifndef DUCKDB_STATIC_EXTENSION_LOADER_API\n"
        + "#if defined(__GNUC__) || defined(__clang__)\n"
        + '#define DUCKDB_STATIC_EXTENSION_LOADER_API __attribute__((visibility("hidden")))\n'
        + "#else\n"
        + "#define DUCKDB_STATIC_EXTENSION_LOADER_API\n"
        + "#endif\n"
        + "#endif\n\n"
        + 'extern "C" DUCKDB_STATIC_EXTENSION_LOADER_API int32_t duckdb_register_static_extensions(void) {\n'
        + "\tint32_t result = 0;\n"
        + ext_registrations
        + "\treturn result;\n"
        + "}\n"
    )

    loader_name = 'generated_extension_loader_package_build.cpp'
    f = open(loader_name, 'wb')
    f.write(loader_code.encode('utf8'))
    f.close()

    # the static initializer that calls the loader before main is already in the amalgamation source list, and only
    # runs if its object is linked into the program
    source_list += [loader_name]

    for src in source_list:
        copy_file(src, target_dir)

    for inc in include_files:
        copy_file(inc, target_dir)

    # handle pragma_version.cpp: paste #define DUCKDB_SOURCE_ID and DUCKDB_VERSION there
    curdir = os.getcwd()
    os.chdir(os.path.join(scripts_dir, '..'))
    githash = git_commit_hash()
    dev_version = git_dev_version()
    dev_v_parts = dev_version.lstrip('v').split('.')
    os.chdir(curdir)
    # open the file and read the current contents
    fpath = os.path.join(target_dir, 'src', 'function', 'table', 'version', 'pragma_version.cpp')
    with open_utf8(fpath, 'r') as f:
        text = f.read()
    # now add the DUCKDB_SOURCE_ID define, if it is not there already
    found_hash = False
    found_dev = False
    found_major = False
    found_minor = False
    found_patch = False
    lines = text.split('\n')
    for i in range(len(lines)):
        if '#define DUCKDB_SOURCE_ID ' in lines[i]:
            lines[i] = '#define DUCKDB_SOURCE_ID "{}"'.format(githash)
            found_hash = True
        if '#define DUCKDB_VERSION ' in lines[i]:
            lines[i] = '#define DUCKDB_VERSION "{}"'.format(dev_version)
            found_dev = True
        if '#define DUCKDB_MAJOR_VERSION ' in lines[i]:
            lines[i] = '#define DUCKDB_MAJOR_VERSION {}'.format(int(dev_v_parts[0]))
            found_major = True
        if '#define DUCKDB_MINOR_VERSION ' in lines[i]:
            lines[i] = '#define DUCKDB_MINOR_VERSION {}'.format(int(dev_v_parts[1]))
            found_minor = True
        if '#define DUCKDB_PATCH_VERSION ' in lines[i]:
            lines[i] = '#define DUCKDB_PATCH_VERSION "{}"'.format(dev_v_parts[2])
            found_patch = True
    if not found_hash:
        lines = ['#ifndef DUCKDB_SOURCE_ID', '#define DUCKDB_SOURCE_ID "{}"'.format(githash), '#endif'] + lines
    if not found_dev:
        lines = ['#ifndef DUCKDB_VERSION', '#define DUCKDB_VERSION "{}"'.format(dev_version), '#endif'] + lines
    if not found_major:
        lines = [
            '#ifndef DUCKDB_MAJOR_VERSION',
            '#define DUCKDB_MAJOR_VERSION {}'.format(int(dev_v_parts[0])),
            '#endif',
        ] + lines
    if not found_minor:
        lines = [
            '#ifndef DUCKDB_MINOR_VERSION',
            '#define DUCKDB_MINOR_VERSION {}'.format(int(dev_v_parts[1])),
            '#endif',
        ] + lines
    if not found_patch:
        lines = [
            '#ifndef DUCKDB_PATCH_VERSION',
            '#define DUCKDB_PATCH_VERSION "{}"'.format(dev_v_parts[2]),
            '#endif',
        ] + lines
    text = '\n'.join(lines)
    with open_utf8(fpath, 'w+') as f:
        f.write(text)

    def file_is_excluded(fname):
        for entry in excluded_objects:
            if entry in fname:
                return True
        return False

    def generate_unity_build(entries, unity_name, linenumbers):
        ub_file = os.path.join(target_dir, unity_name)
        with open_utf8(ub_file, 'w+') as f:
            for entry in entries:
                if linenumbers:
                    f.write('#line 0 "{}"\n'.format(convert_backslashes(entry)))
                f.write('#include "{}"\n\n'.format(convert_backslashes(entry)))
        return ub_file

    def generate_unity_builds(source_list, nsplits, linenumbers):
        files_per_directory = {}
        for source in source_list:
            dirname = os.path.dirname(source)
            if dirname not in files_per_directory:
                files_per_directory[dirname] = []
            files_per_directory[dirname].append(source)

        new_source_files = []
        for dirname in files_per_directory.keys():
            current_files = files_per_directory[dirname]
            cmake_file = os.path.join(dirname, 'CMakeLists.txt')
            unity_files = []
            if os.path.isfile(cmake_file) and len(current_files) > 1:
                with open(cmake_file, 'r') as f:
                    text = f.read()
                    # Find the unity files in groups
                    pos = 0
                    end = len(text)
                    while pos < end:
                        lib = text.find('add_library_unity', pos)
                        if lib == -1:
                            break
                        pos = text.find(')', lib)
                        if pos == -1:
                            break
                        filenames = [x[0] for x in re.findall('([a-zA-Z0-9_]+[.](cpp|cc|c|cxx))', text[lib:pos])]
                        # Remove the unity files from the CMake list
                        unity_set = set(filenames)
                        unity_files += [x for x in current_files if os.path.basename(x) in unity_set]
                        current_files = [x for x in current_files if os.path.basename(x) not in unity_set]
            if current_files:
                if short_paths:
                    # replace source files with "__"
                    for file in current_files:
                        unity_filename = os.path.basename(file)
                        new_source_files.append(generate_unity_build([file], unity_filename, linenumbers))
                else:
                    # directly use the source files
                    new_source_files += [os.path.join(folder_name, file) for file in current_files]
            if unity_files:
                unity_files.sort()
                unity_base = dirname.replace(os.path.sep, '_')
                unity_name = f'ub_{unity_base}.cpp'
                new_source_files.append(generate_unity_build(unity_files, unity_name, linenumbers))
        return new_source_files

    original_sources = source_list
    source_list = generate_unity_builds(source_list, unity_count, linenumbers)

    os.chdir(prev_wd)
    return (
        [convert_backslashes(x) for x in source_list if not file_is_excluded(x)],
        [convert_backslashes(x) for x in include_list],
        [convert_backslashes(x) for x in original_sources],
    )
