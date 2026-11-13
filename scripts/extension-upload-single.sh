#!/bin/bash

# Main extension uploading script

# Note: use the DUCKDB_DEPLOY_SCRIPT_MODE variable to disable dryrun mode

# Usage: ./extension-upload-single.sh <name> <extension_version> <duckdb_version> <architecture> <s3_bucket> <copy_to_latest> <copy_to_versioned> [<path_to_ext>]
# <name>                : Name of the extension
# <extension_version>   : Version (commit / version tag) of the extension
# <duckdb_version>      : Version (commit / version tag) of DuckDB
# <architecture>        : Architecture target of the extension binary
# <s3_bucket>           : S3 bucket to upload to
# <copy_to_latest>      : Set this as the latest version ("true" / "false", default: "false")
# <copy_to_versioned>   : Set this as a versioned version that will not be overwritten
# <path_to_ext>         : (optional) Search this path for the extension

set -euo pipefail

if [ -z "${8:-}" ]; then
	BASE_EXT_DIR="/tmp/extension"
else
	BASE_EXT_DIR="$8"
fi

if [[ $4 == wasm* ]]; then
  ext="$BASE_EXT_DIR/$1.duckdb_extension.wasm"
else
  ext="$BASE_EXT_DIR/$1.duckdb_extension"
fi

script_dir="$(dirname "$(readlink -f "$0")")"
private_key_file=""

cleanup() {
	if [ -n "$private_key_file" ]; then
		rm -f "$private_key_file"
	fi
	rm -f "$ext.append" "$ext.hash" "$ext.sign" "$ext.compressed"
}

trap cleanup EXIT

get_file_size() {
	if stat -c%s "$1" >/dev/null 2>&1; then stat -c%s "$1"; else stat -f%z "$1"; fi
}

supported_signature_sizes="256 384 512"

# Hex dump of $3 bytes at offset $2.
bytes_hex() {
	dd if="$1" bs=1 skip="$2" count="$3" status=none | od -An -v -tx1 | tr -d ' \n'
}

# Hex dump of 32-byte metadata field $3 (0-based from block start $2).
field_hex() {
	bytes_hex "$1" $(( $2 + $3 * 32 )) 32
}

# Hex of $1 NUL-padded to 32 bytes, the only accepted encoding of a metadata field.
padded_hex() {
	local hex
	hex=$(printf '%s' "$1" | od -An -v -tx1 | tr -d ' \n')
	printf '%s' "$hex"
	printf '%0*d' $(( 64 - ${#hex} )) 0
}

# Mirror the engine verifier: tail metadata at size-512, marker (FIELD7) empty = 256, else [M1][sig][M2 = M1][256 zeros].
detect_signature_size() {
	local file="$1" size tail_offset probe m1 m2 zeros marker
	size=$(get_file_size "$file") || return 1
	if [ "$size" -lt 512 ]; then echo "extension '$file' is too small to contain a footer" >&2; return 1; fi
	tail_offset=$(( size - 512 ))
	if [ "$(field_hex "$file" "$tail_offset" 7)" != "$(padded_hex 4)" ]; then
		echo "could not find extension metadata magic in the footer of '$file'" >&2; return 1
	fi
	marker=$(field_hex "$file" "$tail_offset" 1) || return 1
	if [ "$marker" = "$(padded_hex "")" ]; then echo 256; return 0; fi
	for probe in $supported_signature_sizes; do
		if [ "$probe" = 256 ] || [ "$marker" != "$(padded_hex "$probe")" ]; then continue; fi
		if [ "$size" -lt $(( 768 + probe )) ]; then
			echo "extension '$file' is too small for its ${probe}-byte signature footer" >&2; return 1
		fi
		zeros=$(bytes_hex "$file" $(( size - 256 )) 256) || return 1
		if [ "$zeros" != "$(printf '%0512d' 0)" ]; then
			echo "trailing 256-byte slot of the ${probe}-byte signature footer in '$file' is not zero" >&2; return 1
		fi
		m1=$(bytes_hex "$file" $(( size - 768 - probe )) 256) || return 1
		m2=$(bytes_hex "$file" "$tail_offset" 256) || return 1
		if [ "${#m1}" != 512 ] || [ "$m1" != "$m2" ]; then
			echo "the two metadata copies of the ${probe}-byte signature footer in '$file' differ" >&2; return 1
		fi
		echo "$probe"; return 0
	done
	echo "invalid signature-size marker in the footer of '$file'" >&2
	return 1
}

# RSA PKCS#1 v1.5 signature length equals the key modulus in bytes (2048/3072/4096 bit -> 256/384/512).
key_signature_size() {
	local bits
	bits=$(openssl pkey -in "$1" -noout -text 2>/dev/null | grep -oE '\(([0-9]+) bit' | grep -oE '[0-9]+' | head -n1) || true
	case "$bits" in
		2048|3072|4096) echo $(( bits / 8 )) ;;
		*) echo "unsupported signing key size '${bits:-unknown}' bits: only 2048, 3072 and 4096 are supported" >&2; return 1 ;;
	esac
}

# The signature size baked into the footer is authoritative for where the signature goes.
signature_size=$(detect_signature_size "$ext") || exit 1

if [ -n "${DUCKDB_EXTENSION_SIGNING_PK:-}" ]; then
	private_key_file=$(mktemp "${TMPDIR:-/tmp}/duckdb-extension-signing.XXXXXX.pem")
	printf '%s\n' "$DUCKDB_EXTENSION_SIGNING_PK" > "$private_key_file"
	key_size=$(key_signature_size "$private_key_file") || exit 1
	# The key must match the slot the build produced, otherwise signing would corrupt the footer.
	if [ "$key_size" != "$signature_size" ]; then
		echo "Signing key produces a ${key_size}-byte signature but the extension footer has a ${signature_size}-byte slot; rebuild the extension with a matching signature size"
		exit 1
	fi
fi

# The signed region is the plain prefix before the signature: [0, size-256) legacy, [0, size-512-V) otherwise.
file_size=$(get_file_size "$ext")
if [ "$signature_size" = 256 ]; then
	signed_size=$(( file_size - 256 ))
else
	signed_size=$(( file_size - 512 - signature_size ))
fi
footer_tail_size=$(( file_size - signed_size - signature_size ))

# calculate SHA256 hash of extension binary
head -c "$signed_size" "$ext" > "$ext.append"

# (Optionally) Sign binary
if [ -n "${DUCKDB_EXTENSION_SIGNING_PK:-}" ]; then
	"$script_dir/compute-extension-hash.sh" "$ext.append" > "$ext.hash"
	openssl pkeyutl -sign -in "$ext.hash" -inkey "$private_key_file" -pkeyopt digest:sha256 -out "$ext.sign"
	# Guard against a key that passed the bit check but emits a different-length signature (e.g. non-RSA).
	actual_sig_size=$(get_file_size "$ext.sign")
	if [ "$actual_sig_size" != "$signature_size" ]; then
		echo "Produced signature is ${actual_sig_size} bytes but the footer slot is ${signature_size} bytes"
		rm -f "$ext.sign" "$ext.hash" "$ext.append"
		exit 1
	fi
else
	# Default to signature_size zeros
	dd if=/dev/zero of="$ext.sign" bs="$signature_size" count=1 status=none
fi

# append signature and the footer tail (M2 and the zero slot, empty for legacy) to extension binary
cat "$ext.sign" >> "$ext.append"
tail -c "$footer_tail_size" "$ext" >> "$ext.append"
rm "$ext.sign"
rm -f "$ext.hash"

if [ -n "${DUCKDB_EXTENSION_SIGNING_OUTPUT_DIR:-}" ]; then
	signed_output_file="$DUCKDB_EXTENSION_SIGNING_OUTPUT_DIR/$3/$4/$(basename "$ext")"
	mkdir -p "$(dirname "$signed_output_file")"
	cp "$ext.append" "$signed_output_file"
fi

# compress extension binary
if [[ $4 == wasm_* ]]; then
	brotli < "$ext.append" > "$ext.compressed"
else
	gzip < "$ext.append" > "$ext.compressed"
fi
rm "$ext.append"

set -e

if ! command -v rclone >/dev/null 2>&1; then
    case "$(uname -s)" in
      MINGW*|MSYS*|CYGWIN*)
        choco install rclone -y --limit-output --no-progress
        ;;
      *)
        install_runner=(bash)
        if command -v sudo >/dev/null 2>&1; then
          install_runner=(sudo bash)
        fi
        curl -fsSL --retry 5 https://rclone.org/install.sh | "${install_runner[@]}"
        ;;
    esac
fi

# Set dry run unless guard var is set
DRY_RUN_PARAM="--dry-run"
if [ "${DUCKDB_DEPLOY_SCRIPT_MODE:-}" == "for_real" ]; then
  DRY_RUN_PARAM=""
fi

# Abort if credentials are not set
if [ -z "${AWS_ACCESS_KEY_ID:-}" ] || [ -z "${AWS_SECRET_ACCESS_KEY:-}" ]; then
	echo "Missing AWS credentials: AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY are required"
	rm "$ext.compressed"
	if [ "$DRY_RUN_PARAM" == "" ]; then
		exit 1
	else
		exit 0
	fi
fi

dest_extension="gz"
extra_upload_args=()
if [[ $4 == wasm* ]]; then
  dest_extension="wasm"
  extra_upload_args=(
    --header-upload "Content-Encoding: br"
    --header-upload "Content-Type: application/wasm"
  )
fi

upload_extension() {
  local destination="$1"
  local s3_provider="${S3_PROVIDER:-AWS}"
  if [[ "${AWS_ENDPOINT_URL}" == *"r2.cloudflarestorage.com"* ]]; then
    s3_provider="Cloudflare"
  fi
  local rclone_s3_args=(
    --s3-provider "${s3_provider}"
    --s3-endpoint "${AWS_ENDPOINT_URL}"
  )

  # NOTE: flags largely try to simplify this to a PASS/FAIL PUT, skipping
  # checks that otherwise normally occur; HEADs are not allowed for this 
  # Write-Only token.
  # --no-check-dest -> no pre-write HEAD call
  # --s3-no-check-bucket -> don't check for bucket existence
  # --s3-no-head -> no post-write HEAD call/check
  #
  set -x
  rclone $DRY_RUN_PARAM copyto \
    --no-traverse \
    --ignore-times \
    --ignore-checksum \
    --no-check-dest \
    --s3-no-check-bucket \
    --s3-no-head \
    "${rclone_s3_args[@]}" \
    "${extra_upload_args[@]}" \
    "$ext.compressed" \
    ":s3,env_auth=true:${destination}"
}

# upload versioned version
if [[ $7 = 'true' ]]; then
  if [ -z "$3" ]; then
    echo "extension-upload-single.sh called with upload_versioned=true but no extension version was passed"
	rm "$ext.compressed"
	exit 1
  fi

  if [ "${DUCKDB_DEPLOY_SCRIPT_MODE:-}" == "for_real" ]; then
    upload_extension "$5/$1/$2/$3/$4/$1.duckdb_extension.$dest_extension"
  fi
fi

# upload to latest version
if [[ $6 = 'true' ]]; then
  if [ "${DUCKDB_DEPLOY_SCRIPT_MODE:-}" == "for_real" ]; then
    upload_extension "$5/$3/$4/$1.duckdb_extension.$dest_extension"
  fi
fi

# clean up
rm "$ext.compressed"
