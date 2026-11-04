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

# calculate SHA256 hash of extension binary
cat "$ext" > "$ext.append"

( command -v truncate >/dev/null 2>&1 && truncate -s -256 "$ext.append" ) || \
	( command -v gtruncate >/dev/null 2>&1 && gtruncate -s -256 "$ext.append" ) || exit 1

# (Optionally) Sign binary
if [ -n "${DUCKDB_EXTENSION_SIGNING_PK:-}" ]; then
	private_key_file=$(mktemp "${TMPDIR:-/tmp}/duckdb-extension-signing.XXXXXX.pem")
	printf '%s\n' "$DUCKDB_EXTENSION_SIGNING_PK" > "$private_key_file"
	"$script_dir/compute-extension-hash.sh" "$ext.append" > "$ext.hash"
	openssl pkeyutl -sign -in "$ext.hash" -inkey "$private_key_file" -pkeyopt digest:sha256 -out "$ext.sign"
else
	# Default to 256 zeros
	dd if=/dev/zero of="$ext.sign" bs=256 count=1 status=none
fi

# append signature to extension binary
cat "$ext.sign" >> "$ext.append"
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
