#!/bin/bash
# The observer container's start-up: virtual display, a browser view of it for
# the one-time Steam login, the Steam client, GMod with the observer module,
# and the supervisor.
#
# Pelican variables:
#   SERVER_PORT                the allocation: the browser view (noVNC) of the display
#   VNC_PASSWORD               password for that view (you set it in the panel)
#   SERVER_REP_OBSERVER_TOKEN  the backend's observer token
#   MEOWY_OBSERVER_API         optional, default https://meowynetwork.com
set -u
cd /home/container

export DISPLAY=:99
export MEOWY_OBSERVER_DIR=/home/container/observer
mkdir -p "$MEOWY_OBSERVER_DIR"

Xvfb :99 -screen 0 1024x768x24 -nolisten tcp &
sleep 2

# A view of the display in the browser, for logging in to Steam once (Steam Guard).
if [ -n "${VNC_PASSWORD:-}" ]; then
	mkdir -p ~/.vnc && x11vnc -storepasswd "$VNC_PASSWORD" ~/.vnc/passwd >/dev/null
	x11vnc -display :99 -rfbauth ~/.vnc/passwd -forever -shared -localhost -rfbport 5900 -quiet &
	websockify --web /usr/share/novnc "${SERVER_PORT:-6080}" localhost:5900 >/dev/null 2>&1 &
	echo "[observer] display view on port ${SERVER_PORT:-6080} (password from VNC_PASSWORD)"
else
	echo "[observer] VNC_PASSWORD not set: no display view"
fi

steam -silent -no-browser &

GMOD="$HOME/.local/share/Steam/steamapps/common/GarrysMod"
echo "[observer] waiting for GMod to be installed at $GMOD (log in to Steam and install it once)"
until [ -d "$GMOD/garrysmod/lua" ]; do sleep 30; done

# The observer module and its line in menu.lua (the menu state loads gmsv_ modules).
mkdir -p "$GMOD/garrysmod/lua/bin"
cp -f /opt/observer/gmsv_meowy_observer_linux64.dll /opt/observer/gmsv_meowy_observer_linux32.dll "$GMOD/garrysmod/lua/bin/"
MENU="$GMOD/garrysmod/lua/includes/menu.lua"
grep -q 'meowy_observer' "$MENU" || echo 'pcall(require, "meowy_observer") -- meowy observer' >> "$MENU"

# The game: small window, low frame rate, no sound. Restarted if it exits.
(
	while true; do
		steam -applaunch 4000 -windowed -w 800 -h 600 -nosound -nojoy +fps_max 15 +mat_queue_mode 0
		sleep 60
		while pgrep -f 'hl2_linux|gmod' >/dev/null; do sleep 30; done
	done
) &

exec node /opt/observer/supervisor.mjs
