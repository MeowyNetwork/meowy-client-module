#!/bin/bash
# /opt/observer/game-ctl: everything the supervisor does to the game, run AS THE
# GAME USER. The supervisor (user obsvisor, which holds the token) runs it
# through the one sudo rule it has (/etc/sudoers.d/meowy-observer):
#
#   sudo -n -u observer /opt/observer/game-ctl <command>
#
# sudo starts it with a fresh environment, and it builds its own below, so
# nothing of the supervisor's (the token above all) reaches Steam or the game.
#
#   installed   exit 0 once Garry's Mod is installed
#   prepare     at the supervisor's start: settings, no observer files at rest
#   arm|disarm  the loader and menu.lua's require line in, or out again
#   reset       before a visit: empty console.log and garrysmod/data
#   start       launch GMod through the running Steam, click through its prompt
#   quit|kill   SIGTERM or SIGKILL to the game
set -eu
umask 0027
ME=$(id -un)
HOME=$(getent passwd "$ME" | cut -d: -f6)
export HOME
cd "$HOME"
SHARED=/var/lib/meowy-observer
# ~/.steam/steam links to the real Steam folder (Ubuntu's package puts it in
# ~/.steam/debian-installation, Valve's in ~/.local/share/Steam).
GMOD="$HOME/.steam/steam/steamapps/common/GarrysMod"
GAME_PROCESS='hl2_linux|gmod'

# The game's side of supervisor.mjs, in an environment made here.
game_side() {
	exec env -i HOME="$HOME" USER="$ME" LOGNAME="$ME" SHELL=/bin/bash LANG=C.UTF-8 \
		PATH=/usr/local/bin:/usr/bin:/bin:/usr/games DISPLAY=:99 \
		MEOWY_OBSERVER_DIR="$SHARED" MEOWY_OBSERVER_NEED_STEAM=1 \
		node /opt/observer/supervisor.mjs --game "$1"
}

case "${1:-}" in
installed)
	[ -d "$GMOD/garrysmod/lua" ]
	;;
prepare)
	[ -d "$GMOD/garrysmod/lua" ] || { echo "game-ctl: Garry's Mod isn't installed yet" >&2; exit 1; }
	# The module finds the job folder through ~/.meowy-observer when Steam
	# doesn't pass MEOWY_OBSERVER_DIR on to the game (setup.sh makes the link).
	[ -e "$HOME/.meowy-observer" ] || [ -L "$HOME/.meowy-observer" ] || ln -s "$SHARED" "$HOME/.meowy-observer"
	# The supervisor reads console.log as a member of this user's group: let the
	# group through every folder on the way (the link's path and the real one).
	for p in "$GMOD/garrysmod" "$(realpath "$GMOD/garrysmod")"; do
		while [ "$p" != "$HOME" ] && [ "$p" != / ] && [ -n "$p" ]; do
			[ -L "$p" ] || chmod g+x "$p" 2>/dev/null || true
			p=$(dirname "$p")
		done
	done
	chmod g+x "$HOME"
	chmod g+r "$GMOD/garrysmod/console.log" 2>/dev/null || true
	# Any server's client Lua can list and read the game's folders. The real
	# module stays in the job folder's bin/, outside them (src/loader.cpp); the
	# small loader and menu.lua's require line are put in by the supervisor only
	# while the game starts, and taken out once the module has loaded. At rest,
	# nothing of the observer is in the game's folders: earlier installs' files go.
	rm -f "$GMOD"/garrysmod/lua/bin/gmsv_meowy_observer_*.dll "$GMOD"/garrysmod/lua/bin/gmsv_ldr_*.dll
	MENU="$GMOD/garrysmod/lua/includes/menu.lua"
	[ -f "$MENU" ] && sed -i '/meowy_observer/d; /^pcall(require, "ldr")$/d' "$MENU"
	# Settings the game applies itself at every start (options on the launch line
	# don't reliably survive being forwarded to the Steam that's already running).
	# Lowest textures and models, and of a server's own downloads only the map:
	# nothing here looks at any of it, and on big servers it's most of the memory
	# (11.5 GB seen with full textures under software rendering).
	AUTOEXEC="$GMOD/garrysmod/cfg/autoexec.cfg"
	mkdir -p "$(dirname "$AUTOEXEC")"
	# This game is the observer's alone, so the file is written whole, with
	# nothing in it naming the observer: any server's Lua can read cfg/.
	cat > "$AUTOEXEC" <<'EOF'
con_logfile console.log
mat_picmip 2
r_rootlod 2
cl_downloadfilter mapsonly
fps_max 15
mat_queue_mode 0
EOF
	;;
arm | disarm | reset | start)
	game_side "$1"
	;;
quit)
	pkill -TERM -u "$(id -u)" -x "$GAME_PROCESS" || true
	;;
kill)
	pkill -KILL -u "$(id -u)" -x "$GAME_PROCESS" || true
	;;
*)
	echo "usage: game-ctl installed|prepare|arm|disarm|reset|start|quit|kill" >&2
	exit 2
	;;
esac
