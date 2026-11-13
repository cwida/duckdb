#!/bin/bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "Usage: $0 <signed_extensions_dir> <public_key_file>"
	exit 1
fi

signed_extensions_dir="$1"
public_key_file="$2"
script_dir="$(dirname "$(readlink -f "$0")")"
signature_size=256

get_file_size() {
	if stat -c%s "$1" >/dev/null 2>&1; then
		stat -c%s "$1"
	else
		stat -f%z "$1"
	fi
}

if [ ! -d "$signed_extensions_dir" ]; then
	echo "Signed extensions directory '$signed_extensions_dir' does not exist"
	exit 1
fi

if [ ! -f "$public_key_file" ]; then
	echo "Public key file '$public_key_file' does not exist"
	exit 1
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/duckdb-extension-verify.XXXXXX")
file_list="$work_dir/signed_extensions.list"

cleanup() {
	rm -rf "$work_dir"
}

trap cleanup EXIT

find "$signed_extensions_dir" -type f \( -name '*.duckdb_extension' -o -name '*.duckdb_extension.wasm' \) -print0 | sort -z > "$file_list"

verified_count=$(tr -cd '\0' < "$file_list" | wc -c | tr -d '[:space:]')

if [ "$verified_count" -eq 0 ]; then
	echo "No signed extensions found under '$signed_extensions_dir'"
	exit 1
fi

cat "$file_list" | xargs -0 -n 1 -P "${CI_CPU_COUNT:-1}" bash -c '
	set -euo pipefail

	script_dir="$1"
	signed_extensions_dir="$2"
	public_key_file="$3"
	signature_size="$4"
	work_dir="$5"
	signed_extension="$6"

	get_file_size() {
		if stat -c%s "$1" >/dev/null 2>&1; then
			stat -c%s "$1"
		else
			stat -f%z "$1"
		fi
	}

	relative_path="${signed_extension#"$signed_extensions_dir"/}"
	job_dir=$(mktemp -d "$work_dir/job.XXXXXX") || exit 1
	log_file=$(mktemp "$work_dir/log.XXXXXX") || exit 1
	trap '\''rm -rf "$job_dir" "$log_file"'\'' EXIT

	unsigned_extension="$job_dir/extension.unsigned"
	signature_file="$job_dir/extension.signature"
	hash_file="$job_dir/extension.hash"

	if ! {
		file_size=$(get_file_size "$signed_extension")
		if [ "$file_size" -lt "$signature_size" ]; then
			echo "Signed extension '\''$signed_extension'\'' is smaller than the signature footer"
			exit 1
		fi

		unsigned_size=$((file_size - signature_size))
		head -c "$unsigned_size" "$signed_extension" > "$unsigned_extension"
		tail -c "$signature_size" "$signed_extension" > "$signature_file"

		"$script_dir/compute-extension-hash.sh" "$unsigned_extension" > "$hash_file"
		# LibreSSL (macOS) exits 0 on a failed verification and only says so in the output, so the verdict is read
		# from the output rather than from the exit code
		verify_output=$(openssl pkeyutl -verify -pubin -inkey "$public_key_file" -sigfile "$signature_file" \
			-in "$hash_file" -pkeyopt digest:sha256 2>&1) || true
		# the last command of the block decides its status: the caller prints the log and exits on failure
		if [ "$verify_output" = "Signature Verified Successfully" ]; then
			echo "Verified signature: $relative_path"
		else
			echo "Signature verification failed for $relative_path: $verify_output"
			false
		fi
	} >"$log_file" 2>&1; then
		cat "$log_file"
		exit 1
	fi

	cat "$log_file"
	rm -rf "$job_dir" "$log_file"
	trap - EXIT
' bash "$script_dir" "$signed_extensions_dir" "$public_key_file" "$signature_size" "$work_dir"

echo "Verified $verified_count signed extensions"
