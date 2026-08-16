#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKFLOW="${1:-$REPO_ROOT/.github/workflows/ci.yml}"

if [[ ! -f "$WORKFLOW" ]]; then
    echo "ERROR: CI workflow not found: $WORKFLOW" >&2
    exit 1
fi

event_branches() {
    local event="$1"
    awk -v event="$event" '
        $0 == "  " event ":" { in_event = 1; next }
        in_event && $0 ~ /^  [[:alnum:]_-]+:/ { exit }
        in_event && $0 ~ /^    branches:[[:space:]]*\[/ {
            line = $0
            sub(/^    branches:[[:space:]]*\[/, "", line)
            sub(/\][[:space:]]*$/, "", line)
            gsub(/,/, " ", line)
            print line
            found = 1
            exit
        }
        END { if (!found) exit 1 }
    ' "$WORKFLOW"
}

for event in push pull_request; do
    if ! branches="$(event_branches "$event")"; then
        echo "ERROR: $event trigger must declare an inline branches list" >&2
        exit 1
    fi

    for required in main dev; do
        found=false
        for branch in $branches; do
            if [[ "$branch" == "$required" ]]; then
                found=true
                break
            fi
        done
        if [[ "$found" != true ]]; then
            echo "ERROR: $event trigger does not cover required branch '$required'" >&2
            exit 1
        fi
    done
done

echo "CI branch coverage contract satisfied: push and pull_request cover main and dev"
