#!/bin/bash
# Run a command on the Steam Frame; FRAME_PASS (and optionally FRAME_HOST, FRAME_USER) come from trinity-engine/.env.
set -euo pipefail
set -a; . "$(dirname "$0")/../../.env"; set +a
export SSHPASS="$FRAME_PASS"
exec sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 "${FRAME_USER:-steamos}@${FRAME_HOST:-frame}" "$@"
