#!/bin/bash
# Run ON THE VPS HOST as root, once: creates the observer VM (Ubuntu 24.04,
# 4 vCPU, 6 GB RAM, 60 GB disk) on libvirt's private NAT network. Nothing in
# the VM is reachable from the internet; you reach its display through an SSH
# tunnel (see README.md).
#
#   sudo bash create-vm.sh /path/to/observer-folder
#
# The observer folder (this repo's observer/, already on the VPS at
# /opt/stacks/claude-staging/meowy-observer) is copied into the VM at first boot.
set -euo pipefail
SRC="${1:-/opt/stacks/claude-staging/meowy-observer}"
NAME=meowy-observer
IMG=/var/lib/libvirt/images

apt-get update
apt-get install -y --no-install-recommends qemu-system-x86 qemu-utils libvirt-daemon-system virtinst cloud-image-utils genisoimage dnsmasq-base
systemctl enable --now libvirtd
virsh net-autostart default >/dev/null 2>&1 || true
virsh net-start default >/dev/null 2>&1 || true

cd "$IMG"
[ -f noble-server-cloudimg-amd64.img ] || curl -fLO https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
qemu-img create -f qcow2 -F qcow2 -b noble-server-cloudimg-amd64.img "$NAME.qcow2" 60G

# First boot: an "observer" user that logs in with the same SSH keys as root
# here, and the observer files.
tar -C "$SRC" -czf /tmp/observer-files.tgz .
KEYS=$(cat /root/.ssh/authorized_keys 2>/dev/null | sed 's/^/      - /')
cat > /tmp/user-data <<EOF
#cloud-config
hostname: $NAME
users:
  - name: observer
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    ssh_authorized_keys:
$KEYS
write_files:
  - path: /opt/observer-files.tgz
    encoding: b64
    content: $(base64 -w0 /tmp/observer-files.tgz)
runcmd:
  - mkdir -p /opt/observer-src && tar -C /opt/observer-src -xzf /opt/observer-files.tgz
  - bash /opt/observer-src/vm/setup.sh
EOF
cloud-localds "$NAME-seed.iso" /tmp/user-data

virt-install --name "$NAME" --memory 6144 --vcpus 4 \
  --disk "$IMG/$NAME.qcow2",format=qcow2 --disk "$IMG/$NAME-seed.iso",device=cdrom \
  --osinfo detect=on,require=off --network network=default --graphics none \
  --import --noautoconsole --autostart

echo "VM started. Its address (give it a minute):"
sleep 30
virsh domifaddr "$NAME" || true
