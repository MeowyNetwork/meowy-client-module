#!/bin/bash
# Ask for an observer visit by hand: the server goes to the front of the
# backend's queue and the observer picks it up within a minute.
#
#   observer-queue 194.69.160.57:27050                  a normal visit (status only:
#                                                       no code in its game)
#   observer-queue 194.69.160.57:27050 lua              with the Lua sampler in its game
#   observer-queue --local 138.201.62.52:27015 [lua]    a test visit the backend never hears
#                                                       of (any server, ours included);
#                                                       its result is only logged
#   observer-queue --local --quick <ip:port> [lua]      the same, 2 minutes instead of 10
set -eu
LOCAL=""
QUICK=""
if [ "${1:-}" = "--local" ]; then
	LOCAL=1
	shift
	if [ "${1:-}" = "--quick" ]; then
		QUICK=1
		shift
	fi
fi
ADDR="${1:-}"
MODE="${2:-}"
if ! [[ "$ADDR" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}:[0-9]{1,5}$ ]] || ! [[ -z "$MODE" || "$MODE" == status || "$MODE" == lua ]]; then
	echo "usage: observer-queue [--local [--quick]] <ip:port> [status|lua]" >&2
	exit 2
fi
BODY="{\"address\":\"$ADDR\"${MODE:+,\"mode\":\"$MODE\"}${QUICK:+,\"quick\":true}}"
if [ -n "$LOCAL" ]; then
	DIR="${MEOWY_OBSERVER_DIR:-$HOME/.meowy-observer}"
	printf '%s\n' "$BODY" > "$DIR/local-job.json"
	echo "queued locally: $BODY (the observer takes it within a minute)"
	exit 0
fi
TOKEN=$(sed -n 's/^SERVER_REP_OBSERVER_TOKEN=//p' /etc/meowy-observer/env)
API=$(sed -n 's/^MEOWY_OBSERVER_API=//p' /etc/meowy-observer/env)
API="${API:-https://meowynetwork.com}"
# The token goes in through a file descriptor, not the command line.
curl -sS -X POST -H @<(printf 'Authorization: Bearer %s\n' "$TOKEN") \
	-H 'Content-Type: application/json' -d "$BODY" \
	"$API/api/pack/observer/queue"
echo
