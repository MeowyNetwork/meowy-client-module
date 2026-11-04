#!/bin/bash
# Runs INSIDE the observer VM (cloud-init calls it at first boot; safe to run
# again). Installs the Steam client, a virtual display with a local-only
# browser view, Node and a build of the observer module, and sets up two
# services as the "observer" user: the display stack (observer-desktop) and
# the observer itself (observer, which starts once GMod is installed).
set -euo pipefail
SRC=/opt/observer-src
export DEBIAN_FRONTEND=noninteractive

dpkg --add-architecture i386
sed -i 's/^Components: main$/Components: main restricted universe multiverse/; s/^Components: main restricted$/Components: main restricted universe multiverse/' /etc/apt/sources.list.d/ubuntu.sources
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl procps xvfb x11vnc novnc websockify openbox \
  libgl1-mesa-dri libgl1-mesa-dri:i386 libgl1:i386 libc6:i386 libstdc++6:i386 \
  libxtst6:i386 libxrandr2:i386 libxi6:i386 libsdl2-2.0-0:i386 libnss3 libnss3:i386 \
  zenity xdg-utils dbus-x11 nodejs git cmake make g++ g++-multilib
echo steam steam/question select "I AGREE" | debconf-set-selections
echo steam steam/license note '' | debconf-set-selections
apt-get install -y --no-install-recommends steam-installer
ln -sf /usr/games/steam /usr/local/bin/steam

# The observer module, both Linux builds.
cmake -S "$SRC" -B /tmp/b64 -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/b64
cmake -S "$SRC" -B /tmp/b32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32 && cmake --build /tmp/b32
mkdir -p /opt/observer
cp /tmp/b64/gmsv_meowy_observer_linux64.dll /tmp/b32/gmsv_meowy_observer_linux32.dll /opt/observer/
cp "$SRC/supervisor/supervisor.mjs" /opt/observer/
cp "$SRC/vm/observer-game.sh" /opt/observer/ && chmod +x /opt/observer/observer-game.sh

# The token lives here, readable by the observer user only (you write it; see README.md).
install -d -o observer -g observer -m 700 /etc/meowy-observer
[ -f /etc/meowy-observer/env ] || install -o observer -g observer -m 600 /dev/null /etc/meowy-observer/env

cat > /etc/systemd/system/observer-desktop.service <<'EOF'
[Unit]
Description=Meowy observer: virtual display, local browser view, Steam
After=network-online.target

[Service]
User=observer
Environment=DISPLAY=:99
ExecStartPre=/bin/bash -c 'Xvfb :99 -screen 0 1024x768x24 -nolisten tcp & sleep 2; openbox & x11vnc -display :99 -forever -shared -localhost -rfbport 5900 -nopw -quiet & websockify --web /usr/share/novnc 127.0.0.1:6080 localhost:5900 >/dev/null 2>&1 &'
ExecStart=/usr/local/bin/steam -no-browser
Restart=always
RestartSec=20

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/observer.service <<'EOF'
[Unit]
Description=Meowy observer: GMod with the observer module, and the supervisor
After=observer-desktop.service
Requires=observer-desktop.service

[Service]
User=observer
Environment=DISPLAY=:99 MEOWY_OBSERVER_DIR=/home/observer/observer
EnvironmentFile=/etc/meowy-observer/env
ExecStart=/opt/observer/observer-game.sh
Restart=always
RestartSec=60

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now observer-desktop.service observer.service
echo "observer VM ready: log in to Steam through the browser view (README.md)"
