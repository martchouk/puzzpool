#!/usr/bin/env bash
# scripts/check-node-version-age.sh
#
# Supply-chain guard: validates that the installed Node.js release is old enough
# to have been vetted by the community before it is used in a build or deploy.
#
# When a new Node.js release is published there is a window where malicious or
# accidentally broken packages can slip in. Enforcing a minimum age ensures that
# only releases that have had time to be scrutinised are used.
#
# Environment variables:
#   MINIMUM_NODE_RELEASE_AGE   Minimum age in minutes a Node.js release must
#                              have before it is considered safe to use.
#                              Default: 1440 (= 24 hours).
#                              Set to 0 to disable the check entirely.
#
# Exit codes:
#   0  Version meets the age requirement (or check is disabled).
#   1  Version is too new, or the release date could not be determined.

set -euo pipefail

MINIMUM_NODE_RELEASE_AGE="${MINIMUM_NODE_RELEASE_AGE:-1440}"

# ── Allow disabling the check ──────────────────────────────────────────────────
if [[ "$MINIMUM_NODE_RELEASE_AGE" -eq 0 ]]; then
    echo "[node-age] check disabled (MINIMUM_NODE_RELEASE_AGE=0)"
    exit 0
fi

# ── Resolve installed Node.js version ─────────────────────────────────────────
if ! command -v node &>/dev/null; then
    echo "[node-age] ERROR: 'node' not found in PATH" >&2
    exit 1
fi

NODE_VERSION="$(node --version | sed 's/^v//')"
echo "[node-age] installed Node.js: v${NODE_VERSION}"
echo "[node-age] minimum release age: ${MINIMUM_NODE_RELEASE_AGE} minutes"

# ── Fetch the official Node.js release index ───────────────────────────────────
RELEASE_INDEX_URL="https://nodejs.org/dist/index.json"
echo "[node-age] fetching release index from ${RELEASE_INDEX_URL} ..."

# Write to a temp file to avoid shell quoting issues with large JSON blobs
TMPFILE="$(mktemp)"
trap 'rm -f "$TMPFILE"' EXIT

if ! curl -sSf --max-time 20 "$RELEASE_INDEX_URL" > "$TMPFILE" 2>/dev/null; then
    echo "[node-age] ERROR: failed to fetch Node.js release index (network error or timeout)" >&2
    exit 1
fi

if [[ ! -s "$TMPFILE" ]]; then
    echo "[node-age] ERROR: Node.js release index returned empty response" >&2
    exit 1
fi

# ── Extract release date using Node.js, reading the index from the temp file ──
NODE_SCRIPT="
const fs = require('fs');
const data = JSON.parse(fs.readFileSync(process.argv[1], 'utf8'));
const version = 'v' + process.argv[2];
const entry = data.find(function(e) { return e.version === version; });
if (!entry) {
  process.stderr.write('[node-age] ERROR: ' + version + ' not found in Node.js release index\n');
  process.exit(1);
}
process.stdout.write(entry.date);
"

RELEASE_DATE="$(node -e "$NODE_SCRIPT" "$TMPFILE" "$NODE_VERSION")"

if [[ -z "$RELEASE_DATE" ]]; then
    echo "[node-age] ERROR: could not determine release date for v${NODE_VERSION}" >&2
    exit 1
fi

echo "[node-age] release date: ${RELEASE_DATE}"

# ── Compute age in minutes using Node.js for cross-platform date parsing ───────
AGE_SCRIPT="
var released = new Date(process.argv[1]).getTime();
var now = Date.now();
var minutes = Math.floor((now - released) / 60000);
process.stdout.write(String(minutes));
"
AGE_MINUTES="$(node -e "$AGE_SCRIPT" "$RELEASE_DATE")"

echo "[node-age] release age: ${AGE_MINUTES} minutes"

# ── Enforce the threshold ─────────────────────────────────────────────────────
if [[ "$AGE_MINUTES" -lt "$MINIMUM_NODE_RELEASE_AGE" ]]; then
    REMAINING=$(( MINIMUM_NODE_RELEASE_AGE - AGE_MINUTES ))
    echo "" >&2
    echo "[node-age] REJECTED: v${NODE_VERSION} released on ${RELEASE_DATE} is only ${AGE_MINUTES} min old." >&2
    echo "[node-age]           Minimum required age: ${MINIMUM_NODE_RELEASE_AGE} minutes." >&2
    echo "[node-age]           This version will pass the check in ${REMAINING} more minutes." >&2
    echo "[node-age]           To override (not recommended in production), set MINIMUM_NODE_RELEASE_AGE=0." >&2
    echo "" >&2
    exit 1
fi

echo "[node-age] OK: v${NODE_VERSION} is ${AGE_MINUTES} minutes old — meets the minimum age of ${MINIMUM_NODE_RELEASE_AGE} minutes."
