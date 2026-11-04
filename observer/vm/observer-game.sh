#!/bin/bash
# The observer service inside the VM, run as obsvisor (the one user that can
# read the token): waits for GMod, has the game user prepare it, and runs the
# supervisor. The supervisor starts GMod for each round of visits and quits it
# afterwards, so the account is only "in game" while it is actually visiting
# (anything else idling on the account, such as an hour booster that yields to
# play, gets it back in between).
#
# Steam and GMod run as the game user (observer), under observer-desktop.
# Everything done to them goes through game-ctl as that user (sudo -u, one
# rule); this side only reads the console log and the shared job folder.
set -u
GAME_USER="${MEOWY_OBSERVER_GAME_USER:-observer}"
GAME_HOME=$(getent passwd "$GAME_USER" | cut -d: -f6)
if [ -z "$GAME_HOME" ]; then
	echo "[observer] no user $GAME_USER (run setup.sh)"
	exit 1
fi
export MEOWY_OBSERVER_GAME_USER="$GAME_USER"
export MEOWY_OBSERVER_DIR="${MEOWY_OBSERVER_DIR:-/var/lib/meowy-observer}"
export MEOWY_OBSERVER_CONSOLE_LOG="${MEOWY_OBSERVER_CONSOLE_LOG:-$GAME_HOME/.steam/steam/steamapps/common/GarrysMod/garrysmod/console.log}"
CTL=(sudo -n -u "$GAME_USER" -- /opt/observer/game-ctl)

echo "[observer] waiting for GMod (log in to Steam as $GAME_USER and install it once)"
until "${CTL[@]}" installed; do sleep 30; done
"${CTL[@]}" prepare || { echo "[observer] game-ctl prepare failed"; exit 1; }

exec node /opt/observer/supervisor.mjs
