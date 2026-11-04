#!/bin/bash
# Switch the VM's Garry's Mod to the x86-64 branch. The 32-bit game runs out of
# address space on big servers (its built-in browser aborts with "Out of
# memory"), and Steam's Properties window doesn't open on this display, so the
# branch is set in the app manifest while Steam is stopped.
#
# Run in the VM as the admin (obsadmin): sudo bash use-x86-64.sh
# Steam then downloads the branch; start the observer once it's done:
# sudo systemctl start observer
set -eu
if [ "$(id -u)" != 0 ]; then
	exec sudo bash "$0" "$@"
fi
GAME=observer
GAME_HOME=$(getent passwd "$GAME" | cut -d: -f6)
M="$GAME_HOME/.steam/steam/steamapps/appmanifest_4000.acf"
systemctl stop observer observer-desktop
# Edited as the game user: the manifest stays its own.
sudo -u "$GAME" python3 - "$M" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
user = re.search(r'"UserConfig"\s*\{[^}]*\}', s)
if user and '"BetaKey"' in user.group(0):
    block = re.sub(r'"BetaKey"\s*"[^"]*"', '"BetaKey"\t\t"x86-64"', user.group(0))
    s = s[:user.start()] + block + s[user.end():]
else:
    s = re.sub(r'("UserConfig"\s*\{)', r'\1\n\t\t"BetaKey"\t\t"x86-64"', s, count=1)
open(p, 'w').write(s)
EOF
grep -n BetaKey "$M"
systemctl start observer-desktop
echo "Steam is starting and will update Garry's Mod to x86-64 (watch Downloads in the browser view)."
echo "When it's done: sudo systemctl start observer"
