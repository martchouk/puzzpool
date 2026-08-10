#!/usr/bin/env bash
# tests/test_admin_routes_smoke.sh
#
# End-to-end smoke test for route wiring. The unit suites cover the
# authorization decision itself; this covers the thing they cannot see — that
# main.cpp actually wires the guard to every admin route, and that the auth
# routes are reachable and fail closed on a real server.
#
# Run:   bash tests/test_admin_routes_smoke.sh <path-to-puzzpool-binary> [port]
# Exit:  0 = all checks passed, non-zero = at least one failure.

set -uo pipefail

BINARY="${1:?usage: test_admin_routes_smoke.sh <puzzpool-binary> [port]}"
PORT="${2:-18899}"
BASE="http://127.0.0.1:${PORT}"

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: '$BINARY' is not an executable puzzpool binary" >&2
    exit 1
fi

PASS=0
FAIL=0
SERVER_PID=""

WORK_DIR="$(mktemp -d)"
cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# The server reads .env from its working directory; an empty temp dir keeps a
# developer's local .env out of the test.
cd "$WORK_DIR" || exit 1

# ── Server lifecycle ──────────────────────────────────────────────────────────
# Args: KEY=VALUE ... — the complete auth environment for this server instance.
start_server() {
    stop_server
    rm -f "$WORK_DIR/pool.db" "$WORK_DIR/pool.db-wal" "$WORK_DIR/pool.db-shm"

    env -u ADMIN_TOKEN -u ADMIN_GITHUB_USERS -u SESSION_SIGNING_SECRET \
        -u GITHUB_OAUTH_CLIENT_ID -u GITHUB_OAUTH_CLIENT_SECRET \
        PORT="$PORT" DB_PATH="$WORK_DIR/pool.db" "$@" \
        "$BINARY" > "$WORK_DIR/server.log" 2>&1 &
    SERVER_PID=$!

    for _ in $(seq 1 100); do
        if curl -sf -o /dev/null "$BASE/api/v1/stats"; then return 0; fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "ERROR: server exited during startup" >&2
            cat "$WORK_DIR/server.log" >&2
            return 1
        fi
        sleep 0.1
    done

    echo "ERROR: server did not become ready on $BASE" >&2
    cat "$WORK_DIR/server.log" >&2
    return 1
}

stop_server() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    SERVER_PID=""
}

# ── Assertions ────────────────────────────────────────────────────────────────
_check_status() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "PASS: $name"
        (( PASS++ )) || true
    else
        echo "FAIL: $name — expected HTTP $expected, got $actual"
        (( FAIL++ )) || true
    fi
}

_check_contains() {
    local name="$1" needle="$2" haystack="$3"
    if [[ "$haystack" == *"$needle"* ]]; then
        echo "PASS: $name"
        (( PASS++ )) || true
    else
        echo "FAIL: $name — expected to find '$needle' in: $haystack"
        (( FAIL++ )) || true
    fi
}

_check_absent() {
    local name="$1" needle="$2" haystack="$3"
    if [[ "$haystack" != *"$needle"* ]]; then
        echo "PASS: $name"
        (( PASS++ )) || true
    else
        echo "FAIL: $name — unexpectedly found '$needle' in: $haystack"
        (( FAIL++ )) || true
    fi
}

# status_of GET|POST <path> [curl args...]
status_of() {
    local method="$1" path="$2"
    shift 2
    curl -s -o /dev/null -w '%{http_code}' -X "$method" "$@" "${BASE}${path}"
}

body_of() {
    local method="$1" path="$2"
    shift 2
    curl -s -X "$method" "$@" "${BASE}${path}"
}

# The complete admin surface. If a route is ever added without the guard, the
# unconfigured pass below turns it up as a 200.
ADMIN_GET_ROUTES=(/api/v1/admin/puzzles)
ADMIN_POST_ROUTES=(
    /api/v1/admin/activate-puzzle
    /api/v1/admin/set-puzzle
    /api/v1/admin/set-test-chunk
    /api/v1/admin/reclaim
    /api/v1/admin/import-ranges
)

# ── 1. Nothing configured: every admin route fails closed (AC9) ───────────────
echo "── unconfigured deployment ──"
start_server || exit 1

_check_status "public /api/v1/stats stays reachable" 200 "$(status_of GET /api/v1/stats)"

for route in "${ADMIN_GET_ROUTES[@]}"; do
    _check_status "GET $route denies with no credential" 401 "$(status_of GET "$route")"
done
for route in "${ADMIN_POST_ROUTES[@]}"; do
    _check_status "POST $route denies with no credential" 401 \
        "$(status_of POST "$route" -H 'Content-Type: application/json' -d '{}')"
done

_check_contains "startup names ADMIN_TOKEN in its warning" \
    "ADMIN_TOKEN" "$(cat "$WORK_DIR/server.log")"
_check_contains "startup names ADMIN_GITHUB_USERS in its warning" \
    "ADMIN_GITHUB_USERS" "$(cat "$WORK_DIR/server.log")"
_check_contains "startup names SESSION_SIGNING_SECRET in its warning" \
    "SESSION_SIGNING_SECRET" "$(cat "$WORK_DIR/server.log")"

# Auth routes are wired and fail closed without a signing secret (AC13, AC26).
_check_status "GET /api/v1/auth/me is 503 without a signing secret" 503 \
    "$(status_of GET /api/v1/auth/me)"
_check_status "GET /api/v1/auth/github/login is 503 without a signing secret" 503 \
    "$(status_of GET /api/v1/auth/github/login)"
_check_status "GET /api/v1/auth/github/callback is 503 without a signing secret" 503 \
    "$(status_of GET '/api/v1/auth/github/callback?code=a&state=b')"
_check_status "POST /api/v1/auth/logout is 503 without a signing secret" 503 \
    "$(status_of POST /api/v1/auth/logout)"

# ── 2. ADMIN_TOKEN configured: the legacy mechanism still works (AC12) ────────
echo "── ADMIN_TOKEN configured ──"
start_server ADMIN_TOKEN=smoke-test-token || exit 1

for route in "${ADMIN_GET_ROUTES[@]}"; do
    _check_status "GET $route accepts a valid X-Admin-Token" 200 \
        "$(status_of GET "$route" -H 'X-Admin-Token: smoke-test-token')"
    _check_status "GET $route rejects a wrong X-Admin-Token" 401 \
        "$(status_of GET "$route" -H 'X-Admin-Token: wrong')"
done

# reclaim is the one admin POST that needs no request payload to succeed.
_check_status "POST /api/v1/admin/reclaim accepts a valid X-Admin-Token" 200 \
    "$(status_of POST /api/v1/admin/reclaim -H 'X-Admin-Token: smoke-test-token')"
for route in "${ADMIN_POST_ROUTES[@]}"; do
    _check_status "POST $route rejects a wrong X-Admin-Token" 401 \
        "$(status_of POST "$route" -H 'X-Admin-Token: wrong' \
                     -H 'Content-Type: application/json' -d '{}')"
done

# ── 3. Signing configured: /auth/* answers, and leaks nothing ────────────────
echo "── session signing configured ──"
start_server SESSION_SIGNING_SECRET=smoke-signing-secret \
             ADMIN_GITHUB_USERS=smoke-operator \
             GITHUB_OAUTH_CLIENT_ID=smoke-client-id \
             GITHUB_OAUTH_CLIENT_SECRET=smoke-client-secret || exit 1

me_body="$(body_of GET /api/v1/auth/me)"
_check_contains "/auth/me reports signed out" '"authenticated":false' "$me_body"
_check_absent "/auth/me does not leak the signing secret" "smoke-signing-secret" "$me_body"

_check_contains "/auth/me ignores a garbage cookie" '"authenticated":false' \
    "$(body_of GET /api/v1/auth/me --cookie 'pp_session=garbage')"

login_headers="$(curl -s -D - -o /dev/null "$BASE/api/v1/auth/github/login")"
_check_contains "login redirects to GitHub" "github.com/login/oauth/authorize" "$login_headers"
_check_contains "login sets the state cookie" "pp_oauth_state=" "$login_headers"
_check_contains "state cookie is HttpOnly" "HttpOnly" "$login_headers"
_check_contains "state cookie is Secure" "Secure" "$login_headers"
_check_contains "state cookie is SameSite=Lax" "SameSite=Lax" "$login_headers"
_check_absent "the client secret never reaches the redirect" \
    "smoke-client-secret" "$login_headers"

_check_status "callback rejects a state with no cookie" 400 \
    "$(status_of GET '/api/v1/auth/github/callback?code=a&state=b')"

_check_status "POST /api/v1/auth/logout succeeds" 200 "$(status_of POST /api/v1/auth/logout)"

# An allow-list alone still denies an unauthenticated admin request.
_check_status "admin route denies without a session" 401 "$(status_of GET /api/v1/admin/puzzles)"

_check_absent "no secret value reaches the server log" \
    "smoke-signing-secret" "$(cat "$WORK_DIR/server.log")"
_check_absent "no OAuth client secret reaches the server log" \
    "smoke-client-secret" "$(cat "$WORK_DIR/server.log")"

stop_server

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"

[[ "$FAIL" -eq 0 ]]
