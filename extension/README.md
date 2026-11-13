This readme explains what types of extensions there are in DuckDB and how to build them.

# What are DuckDB extensions?
DuckDB extensions are libraries containing additional DuckDB functionality separate from the main codebase. These
extensions can provide added functionality to DuckDB that can/should not live in DuckDB main code for various reasons.
DuckDB extensions can be built in two ways. Firstly, they can be statically linked into DuckDBs executables (duckdb cli,
unittest binary, benchmark runner binary, etc). Doing so will automatically make them available when using these binaries.
Secondly, DuckDB has an extension loading mechanism to dynamically load extension binaries. 

# Extension Types
DuckDB Extensions can de divided into different types: In-tree extensions and out-of-tree extensions. These types refer 
to where the extensions live and who maintains them.

### In-tree extensions
In-tree extensions are extensions that live in the main DuckDB repository. These extensions are considered fundamental 
to DuckDB and/or tie into to DuckDB so deeply that changes to DuckDB are expected to regularly break them.  We aim to 
keep the amount of in-tree extensions to a minimum and strive to move extensions out-of-tree where possible.
### Out-of-tree Extensions (OOTEs)
Out-of-tree extensions live in separate repositories outside the main DuckDB repository. The reasons for moving extensions
out-of-tree can vary. Firstly, moving extensions out of the main DuckDB code-base keeps the core DuckDB code smaller
and less complex. Secondly, keeping extensions out-of-tree can be useful for licensing reasons.

There are two main types of OOTEs. Firstly, there are the **DuckDB Managed OOTEs**. These are distributed through the main
DuckDB CI. These extensions are signed using DuckDBs signing key and are maintained by the DuckDB team. Some examples are 
the `sqlite_scanner` and `postgres_scanner` extensions. The DuckDB Managed OOTEs are distributed automatically with every
release of DuckDB. For the current list of extensions in this category check out `.github/config/out_of_tree_extensions.cmake`

Secondly, there are **External OOTEs**. Extensions in this category are not tied to the DuckDB CI, but instead their CI/CD
runs in their own repository. The maintainer of the external OOTE repo is responsible for testing, distribution and making
sure that an up-to-date version of the extension is available. Depending on who maintains the extension, these extensions
may or may not be signed.

# Building extensions
Under the hood, all types of extensions are built the same way, which is using the DuckDB's root `CMakeLists.txt` file as root CMake file
and passing the extensions that should be build to it. DuckDB has various methods to configure which extensions to build. 
Additionally, we can configure for each extension how we want to build it: for example, whether to only
build the loadable extension, or also link the extension in the DuckDB binaries. There's different ways to load extensions
in DuckDB with various

## Makefile/CMake variables

Extension builds use two space- or semicolon-separated lists:

- `DUCKDB_USE` builds extensions or capabilities and links them into DuckDB.
- `DUCKDB_EXTEND` builds loadable extensions without linking them.

For example:

```bash
DUCKDB_EXTEND='httpfs ducklake' DUCKDB_USE='parquet json httplib' make
```

An extension in `DUCKDB_USE` is also built, so listing it in both variables is unnecessary. `DUCKDB_USE` also accepts
the capabilities `httplib`, `loadable_extensions`, and `local_extension_repository`; capabilities are not valid in
`DUCKDB_EXTEND`. If either variable is explicitly set, the two lists are an exact selection and an omitted list is empty.
Without either variable, DuckDB preserves its standard default build.

The same variables can be passed directly to CMake:

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_USE='core_functions;parquet;json;httplib' \
    -DDUCKDB_EXTEND='httpfs;ducklake'
```

### Project-owned extension revisions

The sync script and CMake use the same configuration precedence: explicit `EXTENSION_CONFIGS` files first, then named
configs for extensions in `DUCKDB_USE` and `DUCKDB_EXTEND`.
The first declaration of each extension wins, including `SOURCE_DIR` and `DONT_BUILD`
declarations. An extension project can therefore supply its own repository URLs and
`GIT_TAG` hashes without changing DuckDB's checked-in defaults:

```bash
EXTENSION_CONFIGS=/path/to/project/extension_config.cmake \
DUCKDB_EXTEND='httpfs;avro;aws' make reldebug
```

`EXTENSION_CONFIGS` is a semicolon-separated list; earlier files take precedence over later files. Configs can contribute
default selections with `set(DUCKDB_USE ...)` and `set(DUCKDB_EXTEND ...)`; defaults from multiple configs are combined.
Explicit user lists replace all config defaults. Make passes `EXTENSION_CONFIGS` to CMake as
`DUCKDB_EXTENSION_CONFIGS`.

To own the entire directory of named configs, set `EXTENSION_CONFIG_BASE_DIR`. For example,
the following reads `httpfs.cmake` and `avro.cmake` from the project's directory:

```bash
EXTENSION_CONFIG_BASE_DIR=/path/to/project/extension-configs \
DUCKDB_EXTEND='httpfs;avro' make reldebug
```

The default is DuckDB's `.github/config/extensions`. A custom directory replaces that
lookup location; missing files do not fall back to DuckDB's pinned revisions. Both sync
and CMake resolve relative config paths from the DuckDB source directory. Absolute paths
are recommended for builds driven by another repository.

The sync script also accepts `--extension-configs` and `--extension-config-base-dir`.
CMake accepts `-DEXTENSION_CONFIG_BASE_DIR=...` or the environment variable. Build wrappers
must pass the same settings to both steps.

Direct CMake builds do not run the synchronization step automatically. Use the Make wrapper or run
`make sync_out_of_tree_extensions` before configuring an out-of-tree extension build.

Sync reads literal `duckdb_extension_load` declarations and follows absolute includes and
includes using `${CMAKE_CURRENT_LIST_DIR}` or `${EXTENSION_CONFIG_BASE_DIR}`. It does not
evaluate general CMake variables or conditionals; keep repository URLs and commit hashes
literal in configs used by sync. This controls extension Git revisions, not vcpkg registry
baselines.

## Config files
To have more control over how in-tree extensions are built, extension config files should be used. These config files 
are simply CMake files that are included by DuckDB's CMake build. There are 4 different places that will be searched 
for config files:

1) The base configuration `extension/extension_config.cmake`. The extensions specified here will be built every time DuckDB
is built. This configuration is always loaded.
2) (Optional) The client specific extensions specification in `tools/*/duckdb_extension_config.cmake`. These config specify
which extensions are built and linked into each client.
3) (Optional) The local configuration file `extension/extension_config_local.cmake` This is where you would specify extensions you need 
included in your local/custom/dev build of DuckDB. This file is gitignored and to be created by the developer.
4) (Optional) Additional configuration files passed to the `DUCKDB_EXTENSION_CONFIGS` parameter. This can be used to point DuckDB
to config files stored anywhere on the machine.

DuckDB will load these config files in reverse order and ignore subsequent calls to load an extension with the 
same name. This allows overriding the base configuration of an extension by providing a different configuration
in the local config. For example, the base config selects and describes the Parquet extension with:
```cmake
set(DUCKDB_USE ${DUCKDB_USE} parquet)
duckdb_extension_load(parquet)
```
Now say we want to build DuckDB with our custom parquet extension. We can achieve this creating the
`extension/extension_config_local.cmake` file and adding:
```cmake
duckdb_extension_load(parquet
    SOURCE_DIR /path/to/my/custom/parquet
)
```

# Building versus linking
`duckdb_extension_load` describes an extension's source and metadata. The resolved `DUCKDB_USE` and `DUCKDB_EXTEND`
lists decide whether it is built and linked. An extension built through `DUCKDB_EXTEND` is available in the build's
extension repository. It can be installed and loaded at runtime when `loadable_extensions` is also in `DUCKDB_USE`.

# Using extension config files
The `duckdb_extension_load` function is used in the configuration files to specify how an extension should
be loaded. There are 3 different ways this can be done. For some examples, check out `.github/config/*.cmake`. These are
the configurations used in DuckDBs CI to select which extensions are built.

## Automatic loading
The simplest way to load an extension is just passing the extension name. This will automatically try to load the extension.
```cmake
duckdb_extension_load(<extension_name>)
```
This configuration of `duckdb_extension_load` will search the `./extension` and `./extension_external` directories for
extensions and attempt to load them if possible. Note that the `extension_external` directory does not exist but should
be created and populated with the out-of-tree extensions that should be built. Extensions based on the
[extension-template](https://github.com/duckdb/extension-template) should work out of the box using this automatic
loading when placed in the `extension_external` directory.

## Custom path
When extensions are located in a  path or their project structure is different from that the
[extension-template](https://github.com/duckdb/extension-template), the `SOURCE_DIR` and `INCLUDE_DIR` variables can
be used to tell DuckDB how to load the extension:
```cmake
duckdb_extension_load(<extension_name>
    SOURCE_DIR <absolute_path_to_extension_root>
    (INCLUDE_DIR <absolute_path_to_extension_header>)
)
```

## Remote GitHub repo
Directly installing extensions from GitHub repositories is also supported. This will download the extension to the current
cmake build directory and build it from there: 
```cmake
duckdb_extension_load(postgres_scanner
    GIT_URL https://github.com/duckdb/postgres_scanner
    GIT_TAG cd043b49cdc9e0d3752535b8333c9433e1007a48
)
```

# Explicitly disabling extensions
Because the sometimes you may want to override extensions set by other configurations, explicitly disabling extensions is 
also possible using the `DONT_BUILD flag`. This will disable the extension from being built all together. For example, to build DuckDB without the parquet extension which is enabled by default, in `extension/extension_config_local.cmake` specify:
```cmake
duckdb_extension_load(parquet DONT_BUILD)
```
For command-line builds, use an exact `DUCKDB_USE`/`DUCKDB_EXTEND` selection instead of disabling a default extension.

# Reporting an extension version

An extension can report its version through the `EXT_VERSION_<NAME_UPPERCASE>` macro, which DuckDB defines to
the extension version configured at build time:
```cpp
std::string ParquetExtension::Version() const {
#ifdef EXT_VERSION_PARQUET
	return EXT_VERSION_PARQUET;
#else
	return "";
#endif
}
```
By default the macro is defined for every source file of the extension, including any third party code it
vendors. Since the version defaults to a git commit hash - DuckDB's for in-tree extensions, the extension's own
otherwise - that puts a value which changes on every commit on all of their compiler command lines, forcing a
recompile of the whole extension for a string that a single file reads.

An extension avoids that by declaring which of its sources actually read the macro, in its own `CMakeLists.txt`:
```cmake
project(ParquetExtension)

# Before any subdirectory or target, which would otherwise inherit the directory-wide definition
set_extension_version_sources(parquet parquet_extension.cpp)
```
Only the listed sources then get `EXT_VERSION_<NAME>`, and a version change recompiles those rather than
everything. It has to be called before the extension declares any targets or adds any subdirectories, because
those inherit the directory-wide definition at the point they are created. Out-of-tree extensions can use it
too; their version is their own commit hash, so it changes on every commit in their repository.

# VCPKG dependency management
DuckDB extensions can use [VCPKG](https://vcpkg.io/en/) to manage their dependencies. Check out the [Extension Template](https://github.com/duckdb/extension-template) for an example
on how to set up vcpkg in extensions. 

## Building DuckDB with multiple extensions that use vcpkg
To build duckdb with multiple extensions that all use vcpkg, some extra steps are required. This is due to the fact that each
extension will specify their own vcpkg.json manifest for their dependencies, but vcpkg allows only a single manifest. The workaround here
is to merge the dependencies from the manifests of all extensions being built. This repo contains a script to do automatically perform this merge.

### Example build with 2 extensions using vcpkg
For example, lets say we want to create a DuckDB binary which has two extensions statically linked that each use vcpkg. The first step is to add the two extensions
to `extension/extension_config_local.cmake`:
```cmake
duckdb_extension_load(extension_1
    GIT_URL https://github.com/example/extension_1
    GIT_TAG some_git_hash
)
duckdb_extension_load(extension_2
    GIT_URL https://github.com/example/extension_2
    GIT_TAG some_git_hash
)
```

Now to merge the vcpkg.json manifests from these two extension run:
```shell   
make extension_configuration
```
This will create a merged manifest in `./build/extension_configuration/vcpkg.json`.

Next, run:
```shell
VCPKG_TOOLCHAIN_PATH="/path/to/your/vcpkg/installation" make
```
The Make wrapper generates and uses the merged manifest automatically.

# Linking the static archives by hand
Outside of CMake, a program links one archive per extension it wants, compiles a generated static extension loader,
and puts the engine archive last:
```shell
LINK_EXTENSIONS="parquet;json" make static_extension_loader     # writes build/release/static_extension_loader.c
cc -I src/include main.c build/release/static_extension_loader.c libparquet_extension.a libjson_extension.a libduckdb_static.a -lstdc++
```
Each extension archive carries a describe function, `duckdb_extension_<name>_describe`, that fills in a
`duckdb_extension_descriptor` (see `duckdb_static_extension.h`). The loader is plain C: it defines
`duckdb_register_static_extensions()`, which passes every describe function to `duckdb_register_static_extension`, and that also
pulls the extensions out of their archives. Call it before opening a database, from any language that can call C. A
C++ program that wants this to happen before main compiles `extension/loader/static_extension_autoregister.cpp` next to
the loader, into the program itself rather than into an archive, or the linker drops it. Without `LINK_EXTENSIONS`
the loader registers every extension archive in the build. A program can also skip the loader and call
`duckdb_register_static_extension` on the describe functions it declares itself. An archive whose describe function is
not registered contributes nothing, and a registered one whose archive is missing fails the link. CMake targets get all of this from
`link_extension_libraries`.

A loadable extension is the same archive linked as a shared library with its entry point exported, next to the
engine archive when it is built to carry its own copy of DuckDB:
```shell
c++ -shared -o parquet.duckdb_extension libparquet_extension.a libduckdb_static.a -Wl,-exported_symbol,_parquet_duckdb_cpp_init
```
The describe function is never taken there, since nothing registers it.
