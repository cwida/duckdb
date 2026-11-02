#!/bin/bash

# Main extension uploading script

# Usage: ./scripts/upload-staging-asset.sh <folder> <file>*
# <folder>              : Folder to upload to
# <file>                : File to be uploaded

if [ -z "$1" ] || [ -z "$2" ]; then
    echo "Usage: ./scripts/upload-staging-asset.sh <folder> <file1> [... <fileN>]"
    exit 1
fi

set -eu -o pipefail

# skip if repo is not in duckdb organization
if [ "$GITHUB_REPOSITORY_OWNER" != "duckdb" ]; then
  echo "Repository is $GITHUB_REPOSITORY_OWNER (not duckdb)"
  exit 0
fi

FOLDER="$1"
DRY_RUN_PARAM=""

# dryrun if repo is not duckdb/duckdb
if [ "$GITHUB_REPOSITORY" != "duckdb/duckdb" ]; then
  echo "Repository is $GITHUB_REPOSITORY (not duckdb/duckdb)"
  DRY_RUN_PARAM="--dry-run"
fi
# dryrun if we are not in main
if [ "$GITHUB_REF" != "refs/heads/main" ]; then
  echo "git ref is $GITHUB_REF (not refs/heads/main)"
  DRY_RUN_PARAM="--dry-run"
fi

if [ "$GITHUB_EVENT_NAME" == "workflow_dispatch" ]; then
  echo "overriding DRY_RUN_PARAM, forcing upload"
  DRY_RUN_PARAM=""
fi

# Early exit if credentials are missing
if [ -z "${AWS_ACCESS_KEY_ID:-}" ] || [ -z "${AWS_SECRET_ACCESS_KEY:-}" ]; then
  echo "No access or secret key available"

  if [ "$DRY_RUN_PARAM" == "" ]; then
    exit 1
  else
    exit 0
  fi
fi

if [ "${DUCKDB_COMMIT:-}" ]; then
  TARGET=$(printf '%s' "$DUCKDB_COMMIT" | cut -c1-10)
else
  TARGET=$(git log -1 --format=%H | cut -c1-10)
fi

if [ "${UPLOAD_ASSETS_TO_STAGING_TARGET:-}" ]; then
  TARGET="$UPLOAD_ASSETS_TO_STAGING_TARGET"
fi

# decide target for staging
if [ "${DUCKDB_VERSION:-}" ]; then
  TARGET="$TARGET/$DUCKDB_VERSION"
fi

if ! command -v rclone >/dev/null 2>&1; then
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
      python3 scripts/ci/retry.py -- bash -c \
        'choco install rclone -y --limit-output --no-progress; command -v rclone >/dev/null 2>&1'
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

files_from="$(mktemp "${TMPDIR:-/tmp}/duckdb-staging-files.XXXXXX")"
cleanup() {
  rm -f "$files_from"
}
trap cleanup EXIT
printf '%s\n' "${@:2}" > "$files_from"

s3_provider="${S3_PROVIDER:-AWS}"
if [[ "${AWS_ENDPOINT_URL:-}" == *"r2.cloudflarestorage.com"* ]]; then
  s3_provider="Cloudflare"
fi

rclone_s3_args=(
  --s3-provider "${s3_provider}"
  --s3-endpoint "${AWS_ENDPOINT_URL:-}"
  --s3-access-key-id "${AWS_ACCESS_KEY_ID:-}"
  --s3-secret-access-key "${AWS_SECRET_ACCESS_KEY:-}"
)

set -x

rclone $DRY_RUN_PARAM copy \
  --no-traverse \
  --ignore-times \
  --ignore-checksum \
  --s3-no-check-bucket \
  --s3-no-head \
  "${rclone_s3_args[@]}" \
  --files-from "$files_from" \
  . \
  ":s3:duckdb-staging/$TARGET/$GITHUB_REPOSITORY/$FOLDER/"
