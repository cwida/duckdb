# DuckDB Security Vulnerability Response Policy

Please see the "[Securing DuckDB](https://duckdb.org/docs/current/operations_manual/securing_duckdb/overview)" page first. To quote:
> DuckDB is a powerful analytical database engine. It can read and write files, access the network, load extensions, and use system resources. Like any powerful tool, these capabilities require appropriate configuration when working with sensitive data or in shared environments.

## Security Model

DuckDB is an embedded engine: it runs in the host process, with the privileges of that process. It has no internal privilege boundary and no notion of an untrusted user inside the engine. DuckDB trusts the embedding application to use its APIs correctly and control which SQL is executed. Column values processed by that SQL may be untrusted.

### SQL

SQL is executable code, comparable to Bash or Python. A query can read and write local files, open network connections, install and load extensions, and consume unbounded CPU, memory and disk. Executing untrusted SQL is therefore unsafe by design, and a query doing any of the above is not a vulnerability.

This also applies to non-SQL APIs: file paths, table names, `ATTACH` targets and filter expressions can all trigger file or network access.

To run untrusted SQL, sandbox at the OS level (container, VM, or [DuckDB-Wasm](https://duckdb.org/docs/stable/clients/wasm/overview)). The settings in [Securing DuckDB](https://duckdb.org/docs/current/operations_manual/securing_duckdb/overview) are defense in depth. Bypasses of these settings are bugs, but are not security vulnerabilities on their own.

### Data files

Format readers (Parquet, CSV, JSON, Arrow, Avro, Iceberg and Delta metadata, DuckDB database files) assume a well-formed file from a trusted writer. Crafted or corrupted files may crash the process, allocate unbounded memory, or read out of bounds. We fix these as bugs, but do not treat them as vulnerabilities. Even a well-formed DuckDB database file can contain executable SQL in stored objects such as macros, views and triggers. These objects can do anything DuckDB can do, such as reading arbitrary files and making network calls. `ATTACH` on a database file is closer to loading a shared library than to opening a document; the file format is not a security boundary. Do not open data files from untrusted parties.

Column values *inside* a well-formed file are a separate case and are in scope. These are frequently attacker controlled (user input, scraped text, log lines). Memory corruption, code execution, or unexpected file or network access caused by the values in a file rather than by its structure is a vulnerability, and is prioritized as such.

### Extensions

Loading an extension executes native code in the host process. Core extensions are signed by the DuckDB project and fall under this security model. Community extensions are third party code and are not covered by this policy. Please report security issues in them to their maintainers. A deliberately loaded malicious extension is not a vulnerability. Loading an extension that was not requested, or loading unsigned code while signature checking is enabled, is.

### In scope

When trusted SQL processes column values or well-formed files from trusted writers, the following issues are in scope:

* Memory safety issues (out-of-bounds access, use-after-free)
* Missing or incorrect use of cryptography, e.g. data assumed to be encrypted that is not.
* Disclosure of data or credentials to a party that should not have access to them, such as sending credentials to an unrelated host.

For example, a well-formed Parquet file written by Spark is in scope even if its column values come from an untrusted source.

### Not in scope

* Any effect reachable through untrusted SQL: file access, network access, resource exhaustion.
* Crashes, hangs and out-of-memory on malformed, truncated or crafted data files, including corrupted database files.
* Internal exceptions, assertion failures, and stack overflow on deeply nested input such as expressions or JSON.
* Resource exhaustion by an expensive query.
* Behavior of deliberately loaded third party extensions.

## Supported Versions

Please see our [release calendar](https://duckdb.org/release_calendar) or DuckDB's [`endoflife.date`](https://endoflife.date/duckdb) page to see which versions are currently supported.

## Reporting a Vulnerability

Please check the [Security Model](#security-model) above first: many reports we receive fall outside it.

Please use GitHub's [security reporting tool here](https://github.com/duckdb/duckdb/security/advisories/new) to report a potential security issue.
Our team will investigate and get back to you. There are three major outcomes of this investigation:

1. The issue is an actual security issue. For [example](https://github.com/duckdb/duckdb/security/advisories/GHSA-vmp8-hg63-v2hp), a return value of a cryptographic function is not checked, and that could lead to failure to encrypt.
1. The issue is a bug, but not really security related. For example, triggering an (internal) exception is generally not considered a security issue.
1. The issue is neither.

If the issue is determined to be an actual security issue, we will request a CVE. For the second case, our [general support policy](https://ducklabs.com/community_support_policy/) will be followed.

## Public Disclosure

We would prefer to only disclose the issue once a DuckDB release is published with a fix. We generally publish a new DuckDB release every few weeks.
Again, see the [release calendar](https://duckdb.org/release_calendar) for the planned release dates.
