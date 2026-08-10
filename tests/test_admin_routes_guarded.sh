#!/usr/bin/env bash
# tests/test_admin_routes_guarded.sh
#
# End-to-end regression for the admin route guard, over ALL SIX /api/v1/admin/*
# routes.
#
# Why a script and not a Catch2 case: src/main.cpp is outside puzzpool_core
# (CMakeLists.txt), and tests/CMakeLists.txt links puzzpool_core only, so no test
# binary can see the route wiring. The six route bodies are near-identical copies,
# so a route that loses its `adminGuard(cfg, req)` line leaves the entire Catch2
# suite green while being publicly reachable. test_handler_validation does not
# catch it either — it calls PoolService methods directly, below the guard.
#
# Phase 1 (AC9, fail closed): with ADMIN_TOKEN, ADMIN_GITHUB_USERS and
#   SESSION_SIGNING_SECRET all unset, every route must answer 401. Anything else —
#   including 400 for a missing body — means the request reached the handler.
# Phase 2 (AC12, and the control that makes phase 1 meaningful): with ADMIN_TOKEN
#   set, every route must answer something OTHER than 401. Without this control a
#   server that 401s unconditionally, or that never started, would pass phase 1.
#
# Run:   bash tests/test_admin_routes_guarded.sh
# Exit:  0 = all checks passed, non-zero = at least one failure.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/build/bin/puzzpool}"

# Returns 0 when something is already listening on the given TCP port.
port_in_use() {
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null || return 1
    exec 3<&-
    return 0
}

# Bind a port nobody else is using. A fixed port makes this test fail for reasons
# that have nothing to do with the guard — a leftover server from an interrupted
# run, or a parallel invocation — and a false failure here trains people to ignore
# the one check that covers the admin routes end to end.
PORT="${PORT:-}"
if [ -z "${PORT}" ]; then
    for candidate in $(seq 18153 18199); do
        if ! port_in_use "${candidate}"; then
            PORT="${candidate}"
            break
        fi
    done
fi
if [ -z "${PORT}" ]; then
    echo "FAIL: no free TCP port in 18153-18199"
    exit 1
fi
BASE="http://127.0.0.1:${PORT}"

PASS=0
FAILURES=0

# A throwaway value generated here; never a real credential.
THROWAWAY_TOKEN="test-only-$$-$(date +%s)"

TMP_DIR="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null
        wait "${SERVER_PID}" 2>/dev/null
    fi
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

# Method and path of every guarded admin route. Keep in sync with src/main.cpp.
ADMIN_ROUTES=(
    "POST /api/v1/admin/activate-puzzle"
    "POST /api/v1/admin/set-puzzle"
    "POST /api/v1/admin/set-test-chunk"
    "GET  /api/v1/admin/puzzles"
    "POST /api/v1/admin/reclaim"
    "POST /api/v1/admin/import-ranges"
)

if [ ! -x "${SERVER_BIN}" ]; then
    echo "FAIL: server binary not found at ${SERVER_BIN}"
    echo "      build it first: cmake --build build --parallel"
    exit 1
fi

# ── Start the server with a clean environment and a throwaway database ─────────
# `env -i` guarantees no ambient ADMIN_TOKEN from the developer's shell leaks into
# phase 1 and turns a fail-open regression into a false pass.
start_server() {
    local admin_token="$1"
    local db_path="${TMP_DIR}/pool-$$.db"
    rm -f "${db_path}"
    # `exec` matters: without it the subshell stays alive as the parent of the
    # server, $! is the subshell, and killing it leaves the real server holding the
    # port. Phase 2 would then silently interrogate the phase-1 server and report
    # six failures that have nothing to do with the guard.
    (
        cd "${TMP_DIR}" || exit 1
        exec env -i PATH="${PATH}" HOME="${HOME}" \
            PORT="${PORT}" DB_PATH="${db_path}" \
            ADMIN_TOKEN="${admin_token}" \
            ADMIN_GITHUB_USERS="" SESSION_SIGNING_SECRET="" \
            "${SERVER_BIN}" > "${TMP_DIR}/server.log" 2>&1
    ) &
    SERVER_PID=$!
}

stop_server() {
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null
        wait "${SERVER_PID}" 2>/dev/null
    fi
    SERVER_PID=""
    # Do not proceed until the port is actually free, or the next phase would
    # attach to a server it did not configure.
    local i
    for i in $(seq 1 50); do
        if ! port_in_use "${PORT}"; then
            return 0
        fi
        sleep 0.2
    done
    echo "FAIL: a server is still listening on port ${PORT} after shutdown"
    exit 1
}

# Poll for readiness; never `sleep N` and hope. /api/v1/stats is unauthenticated,
# so readiness never depends on the guard under test. There is no /health route.
wait_for_server() {
    local i
    for i in $(seq 1 50); do
        if curl -fsS -o /dev/null "${BASE}/api/v1/stats" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            echo "FAIL: server exited during startup; log follows"
            cat "${TMP_DIR}/server.log"
            exit 1
        fi
        sleep 0.2
    done
    echo "FAIL: server did not become ready on port ${PORT}"
    cat "${TMP_DIR}/server.log"
    exit 1
}

# Issues one request and echoes the HTTP status, or the empty string on a
# transport error.
request_status() {
    local method="$1" path="$2"
    shift 2
    curl -s -o /dev/null -w '%{http_code}' \
         -X "${method}" -H 'Content-Type: application/json' -d '{}' \
         "$@" "${BASE}${path}" 2>/dev/null
}

# ── Phase 1 — fail closed (AC9) ───────────────────────────────────────────────
echo "── Phase 1: no auth configured — every admin route must return 401 ──"
start_server ""
wait_for_server

for route in "${ADMIN_ROUTES[@]}"; do
    method="${route%% *}"
    path="${route##* }"
    status="$(request_status "${method}" "${path}")"
    case "${status}" in
        401) echo "PASS: ${method} ${path} — 401"
             PASS=$((PASS + 1)) ;;
        ''|000) echo "FAIL: ${method} ${path} — no HTTP status (server down?)"
             FAILURES=$((FAILURES + 1)) ;;
        *)   echo "FAIL: ${method} ${path} — ${status}, expected 401 (route is not guarded)"
             FAILURES=$((FAILURES + 1)) ;;
    esac
done

# The startup diagnostic must name both variables so an operator can act on it.
if grep -q "ADMIN_TOKEN" "${TMP_DIR}/server.log" &&
   grep -q "ADMIN_GITHUB_USERS" "${TMP_DIR}/server.log"; then
    echo "PASS: startup warning names ADMIN_TOKEN and ADMIN_GITHUB_USERS"
    PASS=$((PASS + 1))
else
    echo "FAIL: startup warning did not name both admin mechanisms"
    FAILURES=$((FAILURES + 1))
fi

stop_server

# ── Phase 2 — sensitivity, and AC12 ───────────────────────────────────────────
# Asserts "not 401" rather than 200: most of these routes legitimately return 400
# for an empty body. The guard, not the handler, is what is under test.
echo
echo "── Phase 2: ADMIN_TOKEN configured — every admin route must get past the guard ──"
start_server "${THROWAWAY_TOKEN}"
wait_for_server

for route in "${ADMIN_ROUTES[@]}"; do
    method="${route%% *}"
    path="${route##* }"
    status="$(request_status "${method}" "${path}" -H "X-Admin-Token: ${THROWAWAY_TOKEN}")"
    case "${status}" in
        ''|000) echo "FAIL: ${method} ${path} — no HTTP status (server down?)"
             FAILURES=$((FAILURES + 1)) ;;
        401) echo "FAIL: ${method} ${path} — still 401 with a valid X-Admin-Token"
             FAILURES=$((FAILURES + 1)) ;;
        *)   echo "PASS: ${method} ${path} — ${status}"
             PASS=$((PASS + 1)) ;;
    esac
done

# A wrong token must still be refused while the mechanism is configured.
wrong_status="$(request_status "GET" "/api/v1/admin/puzzles" -H "X-Admin-Token: wrong-token")"
if [ "${wrong_status}" = "401" ]; then
    echo "PASS: GET /api/v1/admin/puzzles — 401 with a wrong token"
    PASS=$((PASS + 1))
else
    echo "FAIL: GET /api/v1/admin/puzzles — ${wrong_status} with a wrong token, expected 401"
    FAILURES=$((FAILURES + 1))
fi

stop_server

echo
echo "── Summary ──"
echo "passed: ${PASS}"
echo "failed: ${FAILURES}"

if [ "${FAILURES}" -ne 0 ]; then
    exit 1
fi
exit 0
