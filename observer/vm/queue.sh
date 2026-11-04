#!/bin/bash
# Ask for an observer visit by hand: the server goes to the front of the
# backend's queue and the observer picks it up within a minute.
#
#   observer-queue 194.69.160.57:27050          a normal visit
#   observer-queue 194.69.160.57:27050 status   status only: no code in its game
set -eu
ADDR="${1:-}"
MODE="${2:-}"
if ! [[ "$ADDR" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}:[0-9]{1,5}$ ]] || ! [[ -z "$MODE" || "$MODE" == status ]]; then
	echo "usage: observer-queue <ip:port> [status]" >&2
	exit 2
fi
TOKEN=$(sed -n 's/^SERVER_REP_OBSERVER_TOKEN=//p' /etc/meowy-observer/env)
API=$(sed -n 's/^MEOWY_OBSERVER_API=//p' /etc/meowy-observer/env)
API="${API:-https://meowynetwork.com}"
BODY="{\"address\":\"$ADDR\"${MODE:+,\"mode\":\"$MODE\"}}"
# The token goes in through a file descriptor, not the command line.
curl -sS -X POST -H @<(printf 'Authorization: Bearer %s\n' "$TOKEN") \
	-H 'Content-Type: application/json' -d "$BODY" \
	"$API/api/pack/observer/queue"
echo
