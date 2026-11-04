#!/bin/bash
# The observer service inside the VM: waits for GMod, puts the observer module
# in place, and runs the supervisor. The supervisor starts GMod for each round
# of visits and quits it afterwards, so the account is only "in game" while it
# is actually visiting (anything else idling on the account, such as an hour
# booster that yields to play, gets it back in between).
set -u
# GMod is started by the Steam that observer-desktop runs, so it doesn't get
# this service's environment: keep to the default both sides fall back to.
export MEOWY_OBSERVER_DIR="${MEOWY_OBSERVER_DIR:-$HOME/.meowy-observer}"
mkdir -p "$MEOWY_OBSERVER_DIR"

# ~/.steam/steam links to the real Steam folder (Ubuntu's package puts it in
# ~/.steam/debian-installation, Valve's in ~/.local/share/Steam).
GMOD="$HOME/.steam/steam/steamapps/common/GarrysMod"
echo "[observer] waiting for GMod at $GMOD (log in to Steam and install it once)"
until [ -d "$GMOD/garrysmod/lua" ]; do sleep 30; done

# Any server's client Lua can list and read the game's folders. The real module
# stays in $MEOWY_OBSERVER_DIR/bin, outside them (src/loader.cpp); the small
# loader and menu.lua's require line are put in by the supervisor only while
# the game starts, and taken out once the module has loaded. At rest, nothing
# of the observer is in the game's folders: earlier installs' files go here.
mkdir -p "$MEOWY_OBSERVER_DIR/bin"
cp -f /opt/observer/meowy_observer_linux64.so /opt/observer/meowy_observer_linux.so "$MEOWY_OBSERVER_DIR/bin/"
rm -f "$GMOD"/garrysmod/lua/bin/gmsv_meowy_observer_*.dll "$GMOD"/garrysmod/lua/bin/gmsv_ldr_*.dll
MENU="$GMOD/garrysmod/lua/includes/menu.lua"
sed -i '/meowy_observer/d; /^pcall(require, "ldr")$/d' "$MENU"

# Settings the game applies itself at every start (options on the launch line
# don't reliably survive being forwarded to the Steam that's already running).
# Lowest textures and models, and of a server's own downloads only the map:
# nothing here looks at any of it, and on big servers it's most of the memory
# (11.5 GB seen with full textures under software rendering).
AUTOEXEC="$GMOD/garrysmod/cfg/autoexec.cfg"
mkdir -p "$(dirname "$AUTOEXEC")"
touch "$AUTOEXEC"
sed -i '/-- meowy observer --/,/-- meowy observer end --/d' "$AUTOEXEC"
cat >> "$AUTOEXEC" <<'EOF'
// -- meowy observer --
con_logfile console.log
mat_picmip 2
r_rootlod 2
cl_downloadfilter mapsonly
fps_max 15
mat_queue_mode 0
// -- meowy observer end --
EOF

exec node /opt/observer/supervisor.mjs
