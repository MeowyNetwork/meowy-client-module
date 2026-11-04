#!/bin/bash
# The observer service inside the VM: waits for GMod, puts the observer module
# in place, keeps the game running, and runs the supervisor.
set -u
export MEOWY_OBSERVER_DIR="${MEOWY_OBSERVER_DIR:-$HOME/observer}"
mkdir -p "$MEOWY_OBSERVER_DIR"

GMOD="$HOME/.local/share/Steam/steamapps/common/GarrysMod"
echo "[observer] waiting for GMod at $GMOD (log in to Steam and install it once)"
until [ -d "$GMOD/garrysmod/lua" ]; do sleep 30; done

mkdir -p "$GMOD/garrysmod/lua/bin"
cp -f /opt/observer/gmsv_meowy_observer_linux64.dll /opt/observer/gmsv_meowy_observer_linux32.dll "$GMOD/garrysmod/lua/bin/"
MENU="$GMOD/garrysmod/lua/includes/menu.lua"
grep -q 'meowy_observer' "$MENU" || echo 'pcall(require, "meowy_observer") -- meowy observer' >> "$MENU"

(
	while true; do
		pgrep -f 'hl2_linux|gmod' >/dev/null || steam -applaunch 4000 -windowed -w 800 -h 600 -nosound -nojoy +fps_max 15 +mat_queue_mode 0
		sleep 60
	done
) &

exec node /opt/observer/supervisor.mjs
