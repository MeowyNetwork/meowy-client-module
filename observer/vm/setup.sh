#!/bin/bash
# Runs INSIDE the observer VM as root (cloud-init calls it at first boot; safe
# to run again, and it migrates a VM set up before the users were split).
# Installs the Steam client, a virtual display with a local-only browser view,
# Node and a build of the observer module, and sets up three users:
#
#   observer   the game user: the display, Steam and GMod (observer-desktop).
#              No sudo, no SSH, can't read the token. Its Steam install and
#              GMod stay where they are (~observer/.steam).
#   obsvisor   the supervisor (observer.service): reads the token, talks to
#              the backend, and drives the game only through
#              `sudo -u observer /opt/observer/game-ctl ...` (its one sudo rule).
#   obsadmin   you: SSH keys and sudo. On a VM from before, it gets the keys
#              the observer user had.
#
# The job folder they share is /var/lib/meowy-observer (see README.md).
set -euo pipefail
SRC=/opt/observer-src
GAME=observer
VISOR=obsvisor
ADMIN="${OBSERVER_ADMIN:-obsadmin}"
SHARED=/var/lib/meowy-observer
# The VPS's public address: the game may reach it only on UDP and HTTP(S).
HOST_IP="${OBSERVER_HOST_IP:-138.201.62.52}"
export DEBIAN_FRONTEND=noninteractive

if [ "$(id -u)" != 0 ]; then
	echo "run it as root: sudo bash $0" >&2
	exit 1
fi

# --- users -------------------------------------------------------------------

id -u "$GAME" >/dev/null 2>&1 || useradd -m -s /bin/bash "$GAME"
id -u "$VISOR" >/dev/null 2>&1 || useradd --system --user-group --home-dir /nonexistent --no-create-home --shell /usr/sbin/nologin "$VISOR"
id -u "$ADMIN" >/dev/null 2>&1 || useradd -m -s /bin/bash "$ADMIN"
GAME_HOME=$(getent passwd "$GAME" | cut -d: -f6)
ADMIN_HOME=$(getent passwd "$ADMIN" | cut -d: -f6)
# The supervisor reads the game's console log (group $GAME) and the game's
# service log (journalctl -u observer-desktop); the admin reads the logs too.
usermod -aG "$GAME",systemd-journal "$VISOR"
usermod -aG adm,systemd-journal "$ADMIN"

# The admin's SSH keys: the game user's, on a VM from before the split.
AK="$ADMIN_HOME/.ssh/authorized_keys"
if [ ! -s "$AK" ] && [ -s "$GAME_HOME/.ssh/authorized_keys" ]; then
	install -d -o "$ADMIN" -g "$ADMIN" -m 700 "$ADMIN_HOME/.ssh"
	install -o "$ADMIN" -g "$ADMIN" -m 600 "$GAME_HOME/.ssh/authorized_keys" "$AK"
	echo "setup: $ADMIN now takes the SSH keys $GAME had"
fi
if [ ! -s "$AK" ]; then
	echo "setup: $ADMIN has no SSH key in $AK, so taking sudo away from $GAME would lock you out." >&2
	echo "setup: put your public key there (sudo install -D -o $ADMIN -g $ADMIN -m 600 <key file> $AK), then run this again." >&2
	exit 1
fi

# Sudo rules: the admin may do anything; the supervisor may run game-ctl as
# the game user, nothing else; the game user may not use sudo at all.
sudoers() { # <file> <content>: checked before it's put in place
	local tmp
	tmp=$(mktemp)
	printf '%s\n' "$2" > "$tmp"
	visudo -cqf "$tmp"
	install -o root -g root -m 440 "$tmp" "$1"
	rm -f "$tmp"
}
sudoers /etc/sudoers.d/meowy-admin "$ADMIN ALL=(ALL) NOPASSWD:ALL"
sudoers /etc/sudoers.d/meowy-observer "# The supervisor drives the game as the game user, through game-ctl only.
# sudo starts it with a fresh environment: none of the supervisor's (the
# token) reaches Steam or the game.
Defaults:$VISOR !lecture
$VISOR ALL=($GAME) NOPASSWD: /opt/observer/game-ctl"
# cloud-init's rule for the old observer user (NOPASSWD:ALL), and any other.
for f in /etc/sudoers.d/*; do
	[ -f "$f" ] || continue
	case "$f" in /etc/sudoers.d/meowy-*) continue ;; esac
	if grep -qE "^[[:space:]]*$GAME[[:space:]]" "$f"; then
		tmp=$(mktemp)
		sed -E "/^[[:space:]]*$GAME[[:space:]]/d" "$f" > "$tmp"
		visudo -cqf "$tmp"
		install -o root -g root -m 440 "$tmp" "$f"
		rm -f "$tmp"
		echo "setup: removed $GAME's sudo rule from $f"
	fi
done
for g in sudo admin adm lxd wheel systemd-journal; do
	if id -nG "$GAME" | tr ' ' '\n' | grep -qx "$g"; then
		gpasswd -d "$GAME" "$g" >/dev/null
		echo "setup: removed $GAME from group $g"
	fi
done
visudo -cq
# No SSH for the game user or the supervisor.
rm -f "$GAME_HOME/.ssh/authorized_keys"
usermod -L "$GAME"
printf '# Only the admin logs in (observer/vm/setup.sh).\nDenyUsers %s %s\n' "$GAME" "$VISOR" > /etc/ssh/sshd_config.d/meowy-observer.conf
install -d -m 755 /run/sshd # sshd -t wants it; socket-activated sshd may not have made it yet
if sshd -t; then
	systemctl try-reload-or-restart ssh.service 2>/dev/null || true
else
	echo "setup: sshd's config doesn't check out; see sshd -t" >&2
fi
# The supervisor goes through the game's home to its console log.
chgrp "$GAME" "$GAME_HOME"
chmod 750 "$GAME_HOME"

# --- packages ----------------------------------------------------------------

dpkg --add-architecture i386
sed -i 's/^Components: main$/Components: main restricted universe multiverse/; s/^Components: main restricted$/Components: main restricted universe multiverse/' /etc/apt/sources.list.d/ubuntu.sources
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl procps sudo nftables iproute2 xvfb x11vnc novnc websockify openbox \
  libgl1-mesa-dri libgl1-mesa-dri:i386 libgl1:i386 libc6:i386 libstdc++6:i386 \
  libxtst6:i386 libxrandr2:i386 libxi6:i386 libsdl2-2.0-0:i386 libnss3 libnss3:i386 \
  zenity xdg-utils dbus-x11 xdotool imagemagick nodejs git cmake make g++ g++-multilib
echo steam steam/question select "I AGREE" | debconf-set-selections
echo steam steam/license note '' | debconf-set-selections
apt-get install -y --no-install-recommends steam-installer
ln -sf /usr/games/steam /usr/local/bin/steam

# The observer module, both Linux builds.
cmake -S "$SRC" -B /tmp/b64 -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/b64
cmake -S "$SRC" -B /tmp/b32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32 && cmake --build /tmp/b32

# From here the old services must be down (their files move).
systemctl stop observer.service 2>/dev/null || true

# --- files -------------------------------------------------------------------

# Root's, so neither the game nor the supervisor can change what the other runs.
install -d -o root -g root -m 755 /opt/observer
install -o root -g root -m 644 /tmp/b64/meowy_observer_linux64.so /tmp/b32/meowy_observer_linux.so /tmp/b64/gmsv_ldr_linux64.dll /tmp/b32/gmsv_ldr_linux.dll /opt/observer/
install -o root -g root -m 644 "$SRC/supervisor/supervisor.mjs" /opt/observer/
install -o root -g root -m 755 "$SRC/vm/observer-game.sh" /opt/observer/observer-game.sh
install -o root -g root -m 755 "$SRC/vm/game-ctl.sh" /opt/observer/game-ctl
install -o root -g root -m 755 "$SRC/vm/firewall.sh" /opt/observer/firewall.sh
install -o root -g root -m 755 "$SRC/vm/queue.sh" /usr/local/bin/observer-queue

# The job folder the module (as the game) and the supervisor share:
# - owner obsvisor, group observer, mode 3770: both read and write in it, new
#   files get the group (setgid), and with the sticky bit the game can delete
#   or replace only its own files (done.json, observer.log, native-state-*),
#   never the supervisor's (job.json, full-downloads.json) or bin/;
# - bin/ holds the real module, root's: the game loads it but can't change it.
install -d -o "$VISOR" -g "$GAME" -m 3770 "$SHARED"
chown "$VISOR:$GAME" "$SHARED"
chmod 3770 "$SHARED"
install -d -o root -g root -m 755 "$SHARED/bin"
install -o root -g root -m 644 /opt/observer/meowy_observer_linux64.so /opt/observer/meowy_observer_linux.so "$SHARED/bin/"
# Before the split the folder was ~observer/.meowy-observer: what's worth
# keeping moves over (the native check's state, the log, the servers joined
# with every download), and the old folder becomes a link to the new one,
# which is also how the game finds it when Steam doesn't pass
# MEOWY_OBSERVER_DIR on (the module falls back to ~/.meowy-observer).
OLD="$GAME_HOME/.meowy-observer"
if [ -d "$OLD" ] && [ ! -L "$OLD" ]; then
	for f in "$OLD"/native-state-* "$OLD/observer.log" "$OLD/full-downloads.json"; do
		name=${f##*/}
		if [ -f "$f" ] && [ ! -L "$f" ] && [ ! -e "$SHARED/$name" ]; then
			mv "$f" "$SHARED/$name"
			if [ "$name" = full-downloads.json ]; then
				chown "$VISOR:$GAME" "$SHARED/$name"
			else
				chown "$GAME:$GAME" "$SHARED/$name"
			fi
			chmod 640 "$SHARED/$name"
			echo "setup: moved $name to $SHARED"
		fi
	done
	rm -rf "$OLD" # bin/ (now in $SHARED/bin) and the old job and done files
fi
ln -sfn "$SHARED" "$OLD"
chown -h "$GAME:$GAME" "$OLD"

# The token: root's, readable by the supervisor only (you write it; see README.md).
install -d -o root -g "$VISOR" -m 750 /etc/meowy-observer
chown root:"$VISOR" /etc/meowy-observer
chmod 750 /etc/meowy-observer
[ -f /etc/meowy-observer/env ] || install -o root -g "$VISOR" -m 640 /dev/null /etc/meowy-observer/env
chown root:"$VISOR" /etc/meowy-observer/env
chmod 640 /etc/meowy-observer/env

# --- services ----------------------------------------------------------------

cat > /etc/systemd/system/observer-firewall.service <<EOF
[Unit]
Description=Meowy observer: keep the game user off the VPS and private networks
Wants=network-online.target
After=network-online.target
Before=observer-desktop.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/opt/observer/firewall.sh $GAME $HOST_IP
ExecStop=/usr/sbin/nft delete table inet meowy_observer

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/observer-desktop.service <<EOF
[Unit]
Description=Meowy observer: virtual display, local browser view, Steam (as $GAME)
Wants=network-online.target
After=network-online.target observer-firewall.service
# No game without the firewall.
Requires=observer-firewall.service

[Service]
User=$GAME
Group=$GAME
# Its files readable by its group (the supervisor reads the console log and
# done.json), by nobody else.
UMask=0027
Environment=DISPLAY=:99
Environment=MEOWY_OBSERVER_DIR=$SHARED
ExecStartPre=/bin/bash -c 'Xvfb :99 -screen 0 1600x900x24 -nolisten tcp & sleep 2; openbox & x11vnc -display :99 -forever -shared -localhost -rfbport 5900 -nopw -quiet & websockify --web /usr/share/novnc 127.0.0.1:6080 localhost:5900 >/dev/null 2>&1 &'
ExecStart=/usr/local/bin/steam -no-browser
# Steam and the game (with their browsers) can outgrow the VM; past this the
# kernel kills them rather than the whole VM stalling.
MemoryHigh=13G
MemoryMax=14G
Restart=always
RestartSec=20

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/observer.service <<EOF
[Unit]
Description=Meowy observer: the supervisor (as $VISOR), which runs GMod's visits
After=observer-desktop.service
Requires=observer-desktop.service

[Service]
User=$VISOR
Group=$VISOR
UMask=0027
EnvironmentFile=/etc/meowy-observer/env
Environment=MEOWY_OBSERVER_GAME_USER=$GAME
Environment=MEOWY_OBSERVER_DIR=$SHARED
ExecStart=/opt/observer/observer-game.sh
Restart=always
RestartSec=60

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable observer-firewall.service observer-desktop.service observer.service
# One transaction: the firewall first, then the display and Steam, then the
# supervisor (a VM from before gets the new users and settings now).
systemctl restart observer-firewall.service observer-desktop.service observer.service

if sudo -l -U "$GAME" 2>/dev/null | grep -q 'may run'; then
	echo "setup: WARNING: $GAME can still use sudo:" >&2
	sudo -l -U "$GAME" >&2
fi
echo "observer VM ready. Log in as $ADMIN from now on (ssh $ADMIN@<this VM>); $GAME has no SSH and no sudo."
echo "Then log in to Steam through the browser view if it isn't yet (README.md)."
