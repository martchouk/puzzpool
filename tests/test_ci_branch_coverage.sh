#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECKER="$REPO_ROOT/scripts/check-ci-branch-coverage.sh"
WORKFLOW="$REPO_ROOT/.github/workflows/ci.yml"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

bash "$CHECKER" "$WORKFLOW"

expect_failure() {
    local name="$1"
    local input="$2"
    local pattern="$3"
    if bash "$CHECKER" "$input" >"$TMP_DIR/output" 2>&1; then
        echo "FAIL: checker accepted $name" >&2
        exit 1
    fi
    if ! grep -Fq "$pattern" "$TMP_DIR/output"; then
        echo "FAIL: checker rejected $name for the wrong reason" >&2
        cat "$TMP_DIR/output" >&2
        exit 1
    fi
    echo "PASS: checker rejects $name"
}

mutate_event_branches() {
    local event="$1"
    local mutation="$2"
    local output="$3"
    awk -v event="$event" -v mutation="$mutation" '
        $0 == "  " event ":" { in_event = 1 }
        in_event && $0 ~ /^    branches:/ {
            if (mutation == "remove-dev") sub(/, dev/, "")
            if (mutation == "remove-main") sub(/main, /, "")
            if (mutation == "remove-list") sub(/branches:/, "paths:")
            in_event = 0
        }
        { print }
    ' "$WORKFLOW" > "$output"
}

expect_failure "a missing workflow" "$TMP_DIR/does-not-exist.yml" "ERROR: CI workflow not found"

missing_list="$TMP_DIR/ci-without-push-branches.yml"
mutate_event_branches push remove-list "$missing_list"
expect_failure "push without a branches list" "$missing_list" "ERROR: push trigger must declare an inline branches list"

for event in push pull_request; do
    for branch in main dev; do
        mutated="$TMP_DIR/ci-without-${event}-${branch}.yml"
        mutate_event_branches "$event" "remove-${branch}" "$mutated"
        expect_failure "$event without $branch coverage" "$mutated" "ERROR: $event trigger does not cover required branch '$branch'"
    done
done
