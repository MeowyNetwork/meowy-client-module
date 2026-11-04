# The observer in a VM

Steam's client needs user namespaces to sandbox itself, which Docker (and so Pelican) blocks. So
the observer runs in a small KVM virtual machine on the VPS instead: Steam gets what it needs,
and the host stays isolated from a game client that joins suspect servers.

## 1. Create the VM (on the VPS, as root, once)

The observer files are already on the VPS at `/opt/stacks/claude-staging/meowy-observer`.

```bash
sudo bash /opt/stacks/claude-staging/meowy-observer/vm/create-vm.sh
```

It installs KVM/libvirt, creates `meowy-observer` (Ubuntu 24.04, 4 vCPU, 16 GB RAM, 60 GB disk) on
libvirt's private NAT network, and at first boot the VM installs everything itself
(`vm/setup.sh`, about 5-10 minutes). It prints the VM's address (usually `192.168.122.x`). The
VM's `observer` user accepts the same SSH keys as the host's root.

## 2. The token (in the VM)

```bash
ssh observer@<vm address>            # from the VPS
echo 'SERVER_REP_OBSERVER_TOKEN=<the same token as the backend>' | sudo tee /etc/meowy-observer/env
sudo systemctl restart observer
```

## 3. Log in to Steam once (from your PC)

The VM's display listens only inside the VM (127.0.0.1:6080), so the tunnel goes through both
hops: your PC to the VPS, then the VPS to the VM (using the key that can log in to the VM):

```bash
ssh -t -L 6080:localhost:6080 <you>@138.201.62.52 ssh -i ~/.ssh/meowy_observer -L 6080:localhost:6080 observer@<vm address>
```

Then open http://localhost:6080/vnc.html, Connect, log in with the observer account (Steam
Guard code), and install Garry's Mod (x86-64 branch if you can). Once it's installed the observer
starts by itself. Its log: `journalctl -u observer -f` in the VM.
