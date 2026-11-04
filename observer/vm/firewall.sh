#!/bin/bash
# /opt/observer/firewall.sh, run as root by observer-firewall.service (which
# observer-desktop requires, so the game doesn't start without it). Keeps the
# game user's traffic off the VPS and off private networks; the internet stays
# open to it (game servers, Steam, meowynetwork.com).
#
#   firewall.sh [game user] [the VPS's public address]
#
# For the game user's packets only (the supervisor, the admin and the system
# are untouched):
# - loopback: allowed (the display's browser view, the local DNS resolver);
# - DNS to the NAT gateway (the VPS at 192.168.122.1): allowed; nothing else
#   to it;
# - the VPS's public address: UDP (game servers, ours too) and TCP 80/443
#   (downloads, the website) only, so not its SSH, Wings, databases...;
# - private, shared, link-local (the cloud metadata address), multicast and
#   reserved ranges, IPv4 and IPv6: dropped.
# A packet that isn't from a socket of the game user (or whose owner the
# kernel doesn't know) never enters the game chain, so replies on the admin's
# SSH session are never caught by it.
set -eu
GAME_USER="${1:-observer}"
HOST_IP="${2:-138.201.62.52}"
GAME_UID=$(id -u "$GAME_USER")
GW=$(ip -4 route show default | awk '{ for (i = 1; i < NF; i++) if ($i == "via") { print $(i + 1); exit } }')
case "$GW" in *[!0-9.]*) GW="" ;; esac
case "$HOST_IP" in "" | *[!0-9.]*) echo "firewall.sh: not an IPv4 address: $HOST_IP" >&2; exit 2 ;; esac

{
	# Replaced whole on every run.
	echo "table inet meowy_observer {}"
	echo "delete table inet meowy_observer"
	echo "table inet meowy_observer {"
	echo "  chain output {"
	echo "    type filter hook output priority filter; policy accept;"
	echo "    meta skuid $GAME_UID jump game"
	echo "  }"
	echo "  chain game {"
	echo "    oifname \"lo\" accept"
	if [ -n "$GW" ]; then
		echo "    ip daddr $GW udp dport 53 accept"
		echo "    ip daddr $GW tcp dport 53 accept"
	fi
	echo "    ip daddr $HOST_IP meta l4proto udp accept"
	echo "    ip daddr $HOST_IP tcp dport { 80, 443 } accept"
	echo "    ip daddr $HOST_IP counter drop"
	echo "    ip daddr { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/3 } counter drop"
	echo "    ip6 daddr { ::ffff:0:0/96, fc00::/7, fe80::/10, ff00::/8 } counter drop"
	echo "  }"
	echo "}"
} | nft -f -
echo "observer firewall: game user $GAME_USER ($GAME_UID), gateway ${GW:-none}, host $HOST_IP"
