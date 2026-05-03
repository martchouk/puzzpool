#!/usr/bin/env bash
set -euo pipefail

ENV_FILE=".env"

if [[ ! -f "$ENV_FILE" ]]; then
    echo "ERROR: $ENV_FILE not found"
    exit 1
fi

# Load variables from .env
set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

STAGE="${STAGE:-}"
STAGE="$(printf '%s' "$STAGE" | tr '[:lower:]' '[:upper:]')"

if [[ -z "$STAGE" ]]; then
    echo "ERROR: STAGE is not set in $ENV_FILE"
    exit 1
fi

case "$STAGE" in
    TEST)
        GIT_BRANCH="dev"
        SERVICE_NAME="puzzpool-test"
        ;;
    PROD)
        GIT_BRANCH="main"
        SERVICE_NAME="puzzpool"
        ;;
    *)
        echo "ERROR: Unsupported STAGE='$STAGE' in $ENV_FILE"
        echo "Allowed values: TEST or PROD"
        exit 1
        ;;
esac

echo "[update] stage=$STAGE"
echo "[update] branch=$GIT_BRANCH"
echo "[update] service=$SERVICE_NAME"

CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
CURRENT_COMMIT_BEFORE="$(git rev-parse --short HEAD)"

echo "[update] current git branch: $CURRENT_BRANCH"
echo "[update] current commit before pull: $CURRENT_COMMIT_BEFORE"

if [[ "$CURRENT_BRANCH" != "$GIT_BRANCH" ]]; then
    echo "[update] switching to branch $GIT_BRANCH"
    git checkout "$GIT_BRANCH"
fi

echo "[update] pulling latest changes from origin/$GIT_BRANCH"
git pull origin "$GIT_BRANCH"

CURRENT_COMMIT_AFTER="$(git rev-parse --short HEAD)"
echo "[update] current commit after pull: $CURRENT_COMMIT_AFTER"

echo "[update] building project"
bash build.sh

echo "[update] restarting service: $SERVICE_NAME"
sudo systemctl restart "$SERVICE_NAME"

sleep 2

if systemctl is-active --quiet "$SERVICE_NAME"; then
    echo "OK: $SERVICE_NAME running"
    systemctl --no-pager --full status "$SERVICE_NAME" | head -n 10
else
    echo "FAIL: $SERVICE_NAME is not running"
    echo "Check logs with:"
    echo "  journalctl -u $SERVICE_NAME -n 100 --no-pager"
    exit 1
fi
