#!/bin/bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "Usage: $0 <signed_extensions_dir> <public_key_file>"
	exit 1
fi

signed_extensions_dir="$1"
public_key_file="$2"
script_dir="$(dirname "$(readlink -f "$0")")"
# Minimum footer size (legacy metadata + signature); the per-file signature size is detected from the footer marker.
min_footer_size=512

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
	min_footer_size="$4"
	work_dir="$5"
	signed_extension="$6"

	get_file_size() {
		if stat -c%s "$1" >/dev/null 2>&1; then
			stat -c%s "$1"
		else
			stat -f%z "$1"
		fi
	}

	# Hex dump of $3 bytes at offset $2.
	bytes_hex() {
		dd if="$1" bs=1 skip="$2" count="$3" status=none | od -An -v -tx1 | tr -d '\'' \n'\''
	}

	# Hex dump of 32-byte metadata field $3 (0-based from block start $2).
	field_hex() {
		bytes_hex "$1" $(( $2 + $3 * 32 )) 32
	}

	# Hex of $1 NUL-padded to 32 bytes, the only accepted encoding of a metadata field.
	padded_hex() {
		local hex
		hex=$(printf '\''%s'\'' "$1" | od -An -v -tx1 | tr -d '\'' \n'\'')
		printf '\''%s'\'' "$hex"
		printf '\''%0*d'\'' $(( 64 - ${#hex} )) 0
	}

	# Mirror the engine verifier: tail metadata at size-512, marker (FIELD7) empty = 256, else [M1][sig][M2 = M1][256 zeros].
	detect_signature_size() {
		local file="$1" size="$2" tail_offset probe m1 m2 zeros magic marker
		tail_offset=$(( size - 512 ))
		magic=$(field_hex "$file" "$tail_offset" 7) || return 1
		if [ "$magic" != "$(padded_hex 4)" ]; then echo "could not find extension metadata magic in the footer"; return 1; fi
		marker=$(field_hex "$file" "$tail_offset" 1) || return 1
		if [ "$marker" = "$(padded_hex "")" ]; then echo 256; return 0; fi
		for probe in 256 384 512; do
			if [ "$probe" = 256 ] || [ "$marker" != "$(padded_hex "$probe")" ]; then continue; fi
			if [ "$size" -lt $(( 768 + probe )) ]; then echo "file is too small for its ${probe}-byte signature footer"; return 1; fi
			zeros=$(bytes_hex "$file" $(( size - 256 )) 256) || return 1
			if [ "$zeros" != "$(printf '\''%0512d'\'' 0)" ]; then echo "trailing 256-byte slot of the ${probe}-byte signature footer is not zero"; return 1; fi
			m1=$(bytes_hex "$file" $(( size - 768 - probe )) 256) || return 1
			m2=$(bytes_hex "$file" "$tail_offset" 256) || return 1
			if [ "${#m1}" != 512 ] || [ "$m1" != "$m2" ]; then echo "the two metadata copies of the ${probe}-byte signature footer differ"; return 1; fi
			echo "$probe"; return 0
		done
		echo "invalid signature-size marker in the footer"
		return 1
	}

	relative_path="${signed_extension#"$signed_extensions_dir"/}"
	job_dir=$(mktemp -d "$work_dir/job.XXXXXX") || exit 1
	log_file=$(mktemp "$work_dir/log.XXXXXX") || exit 1
	trap '\''rm -rf "$job_dir" "$log_file"'\'' EXIT

	unsigned_extension="$job_dir/extension.unsigned"
	signature_file="$job_dir/extension.signature"
	hash_file="$job_dir/extension.hash"

	# Subshell so a failed step aborts only this file and its log is still printed by the handler below.
	if ! (
		file_size=$(get_file_size "$signed_extension") || exit 1
		if [ "$file_size" -lt "$min_footer_size" ]; then
			echo "Signed extension '\''$signed_extension'\'' is smaller than the signature footer"
			exit 1
		fi

		if ! signature_size=$(detect_signature_size "$signed_extension" "$file_size"); then
			echo "Failed to detect signature size for '\''$signed_extension'\'': $signature_size"
			exit 1
		fi

		# Signed region is the plain prefix before the signature: [0, size-256) legacy, [0, size-512-V) otherwise.
		if [ "$signature_size" = 256 ]; then
			unsigned_size=$((file_size - 256))
		else
			unsigned_size=$((file_size - 512 - signature_size))
		fi
		head -c "$unsigned_size" "$signed_extension" > "$unsigned_extension" || exit 1
		dd if="$signed_extension" of="$signature_file" bs=1 skip="$unsigned_size" count="$signature_size" status=none || exit 1

		"$script_dir/compute-extension-hash.sh" "$unsigned_extension" > "$hash_file" || exit 1
		openssl pkeyutl -verify -pubin -inkey "$public_key_file" -sigfile "$signature_file" -in "$hash_file" \
			-pkeyopt digest:sha256 >/dev/null || exit 1

		echo "Verified signature: $relative_path"
	) >"$log_file" 2>&1; then
		cat "$log_file"
		exit 1
	fi

	cat "$log_file"
	rm -rf "$job_dir" "$log_file"
	trap - EXIT
' bash "$script_dir" "$signed_extensions_dir" "$public_key_file" "$min_footer_size" "$work_dir"

echo "Verified $verified_count signed extensions"
