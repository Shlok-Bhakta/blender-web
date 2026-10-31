#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Delete older Actions cache entries of one family on the current ref once a
# replacement has been saved, so superseded multi-GiB entries do not push the
# live LLVM and dependency caches out of the repository's 10 GB budget.
# Usage: prune-superseded-caches.sh <key-prefix> <saved-key>
set -euo pipefail
prefix=$1
saved=$2
ref=${GITHUB_REF:?}
list() {
  gh cache list --repo "$GITHUB_REPOSITORY" --ref "$ref" --key "$1" --limit 100 --json id,key
}
if [[ $saved != "$prefix"* ]]; then
  echo "Refusing to prune: $saved is not in family $prefix" >&2
  exit 1
fi
# Never remove the previous entry unless its replacement really exists.
if ! list "$saved" | jq -e --arg key "$saved" 'any(.[]; .key == $key)' > /dev/null; then
  echo "No saved entry $saved on $ref; keeping existing $prefix* caches"
  exit 0
fi
list "$prefix" | jq -r --arg key "$saved" '.[] | select(.key != $key) | "\(.id) \(.key)"' |
  while read -r id key; do
    echo "Deleting superseded cache $key"
    gh cache delete --repo "$GITHUB_REPOSITORY" "$id" || echo "Could not delete $key"
  done
