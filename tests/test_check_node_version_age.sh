#!/usr/bin/env bash
# tests/test_check_node_version_age.sh
#
# Unit tests for scripts/check-node-version-age.sh.
# Uses wrapper scripts to mock 'node --version' and 'curl' without making
# real network calls. All other 'node' invocations pass through to the real binary.
#
# Run:   bash tests/test_check_node_version_age.sh
# Exit:  0 = all tests passed, non-zero = at least one failure.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-node-version-age.sh"

PASS=0
FAIL=0

# ── Temp workspace for wrapper scripts ────────────────────────────────────────
WRAP_DIR="$(mktemp -d)"
trap 'rm -rf "$WRAP_DIR"' EXIT

# ── Helper: build a minimal fake release index JSON ───────────────────────────
_fake_index() {
    local ver="$1" date="$2"
    printf '[{"version":"v%s","date":"%s","files":[],"npm":"10.0.0","v8":"","uv":"","zlib":"","openssl":"","modules":"","lts":false,"security":false}]' "$ver" "$date"
}

# ── Helper: install wrapper scripts into WRAP_DIR ─────────────────────────────
# Args:
#   $1  node version string to fake (e.g. "20.11.0")
#   $2  content that the fake curl should write to its first positional argument
#       (the script redirects: curl ... > "$TMPFILE", so the wrapper writes to $1)
#   $3  exit code for the fake curl (default 0)
_setup_wrappers() {
    local node_ver="$1"
    local curl_body="$2"
    local curl_exit="${3:-0}"
    local real_node
    real_node="$(command -v node)"

    # node wrapper: intercepts --version, delegates everything else to the real node
    cat > "$WRAP_DIR/node" <<NODEEOF
#!/usr/bin/env bash
if [[ "\${1:-}" == "--version" ]]; then
    echo "v${node_ver}"
    exit 0
fi
exec "${real_node}" "\$@"
NODEEOF
    chmod +x "$WRAP_DIR/node"

    # curl wrapper: writes the fake index body to the file given by the last
    # positional argument that follows '>' in the caller's redirect.
    # The script calls: curl ... > "$TMPFILE"
    # So the shell redirect is handled by the shell itself — curl just needs
    # to write to stdout and the shell writes it to the temp file.
    cat > "$WRAP_DIR/curl" <<CURLEOF
#!/usr/bin/env bash
# Ignore all flags; just emit the fake body and exit
printf '%s' $(printf '%q' "$curl_body")
exit ${curl_exit}
CURLEOF
    chmod +x "$WRAP_DIR/curl"
}

# ── Test runner ───────────────────────────────────────────────────────────────
_test() {
    local name="$1"
    local expected_exit="$2"
    local expected_pattern="${3:-}"
    shift 3
    # Remaining args are exported env vars (KEY=VAL)

    local output exit_code
    set +e
    output="$(PATH="$WRAP_DIR:$PATH" env "$@" bash "$SCRIPT" 2>&1)"
    exit_code=$?
    set -e

    local ok=true
    if [[ "$exit_code" -ne "$expected_exit" ]]; then
        ok=false
    fi
    if [[ -n "$expected_pattern" ]] && ! echo "$output" | grep -q "$expected_pattern"; then
        ok=false
    fi

    if $ok; then
        echo "PASS: $name"
        (( PASS++ )) || true
    else
        echo "FAIL: $name"
        echo "  expected exit=${expected_exit} pattern='${expected_pattern}', got exit=${exit_code}"
        echo "  output: $output"
        (( FAIL++ )) || true
    fi
}

# ── Tests ─────────────────────────────────────────────────────────────────────

# 1. Check disabled when MINIMUM_NODE_RELEASE_AGE=0
_setup_wrappers "20.11.0" "[]"
_test "MINIMUM_NODE_RELEASE_AGE=0 disables check" \
    0 "check disabled" \
    MINIMUM_NODE_RELEASE_AGE=0

# 2. Version old enough is accepted
_setup_wrappers "20.11.0" "$(_fake_index "20.11.0" "2015-06-01")"
_test "old version (2015) is accepted" \
    0 "OK:" \
    MINIMUM_NODE_RELEASE_AGE=1440

# 3. Version released today is rejected
_setup_wrappers "20.11.0" "$(_fake_index "20.11.0" "$(date +%Y-%m-%d)")"
_test "version released today is rejected" \
    1 "REJECTED:" \
    MINIMUM_NODE_RELEASE_AGE=1440

# 4. Rejection message includes release date and remaining time
_setup_wrappers "20.11.0" "$(_fake_index "20.11.0" "$(date +%Y-%m-%d)")"
_test "rejection message contains date and 'minutes'" \
    1 "minutes" \
    MINIMUM_NODE_RELEASE_AGE=1440

# 5. Network error (curl exits 1) causes exit 1
_setup_wrappers "20.11.0" "" 1
_test "network error causes exit 1 with error message" \
    1 "ERROR" \
    MINIMUM_NODE_RELEASE_AGE=1440

# 6. Version absent from index causes exit 1
_setup_wrappers "20.11.0" "$(_fake_index "18.0.0" "2020-01-01")"
_test "version not in index causes exit 1" \
    1 "ERROR" \
    MINIMUM_NODE_RELEASE_AGE=1440

# 7. Version 2 days old passes a 1440-minute threshold
if date -d "2 days ago" +%Y-%m-%d &>/dev/null 2>&1; then
    TWO_DAYS_AGO="$(date -d '2 days ago' +%Y-%m-%d)"
else
    TWO_DAYS_AGO="$(date -j -v-2d +%Y-%m-%d)"
fi
_setup_wrappers "20.11.0" "$(_fake_index "20.11.0" "$TWO_DAYS_AGO")"
_test "version 2 days old passes 1440-min threshold" \
    0 "OK:" \
    MINIMUM_NODE_RELEASE_AGE=1440

# 8. Custom threshold: version 5 days old is rejected when threshold is 10 days (14400 min)
if date -d "5 days ago" +%Y-%m-%d &>/dev/null 2>&1; then
    FIVE_DAYS_AGO="$(date -d '5 days ago' +%Y-%m-%d)"
else
    FIVE_DAYS_AGO="$(date -j -v-5d +%Y-%m-%d)"
fi
_setup_wrappers "20.11.0" "$(_fake_index "20.11.0" "$FIVE_DAYS_AGO")"
_test "version 5 days old rejected with 14400-min (10-day) threshold" \
    1 "REJECTED:" \
    MINIMUM_NODE_RELEASE_AGE=14400

# 9. Custom threshold of 0 accepts any version even released today
_setup_wrappers "20.11.0" "$(_fake_index "20.11.0" "$(date +%Y-%m-%d)")"
_test "MINIMUM_NODE_RELEASE_AGE=0 accepts even today's release" \
    0 "check disabled" \
    MINIMUM_NODE_RELEASE_AGE=0

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"

[[ "$FAIL" -eq 0 ]]
