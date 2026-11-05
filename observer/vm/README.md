# The observer in a VM

Steam's client needs user namespaces to sandbox itself, which Docker (and so Pelican) blocks. So
the observer runs in a small KVM virtual machine on the VPS instead: Steam gets what it needs,
and the host stays isolated from a game client that joins suspect servers.

## Who runs what

The game joins hostile servers, so it's treated as the part that can be taken over (a GMod client
exploit). It runs as a user that can't do anything else:

| User | Runs | Can |
|---|---|---|
| `observer` | the display (Xvfb, the browser view), Steam and GMod: `observer-desktop.service` | nothing beyond its own files: no sudo, no SSH, can't read the token; its traffic can't reach the VPS (except game ports and HTTP(S)) or private networks |
| `obsvisor` | the supervisor: `observer.service` | read the token (`/etc/meowy-observer/env`, `root:obsvisor` 640), talk to the backend, and run `/opt/observer/game-ctl` as `observer` (its only sudo rule) |
| `obsadmin` | you | SSH (the host root's keys) and sudo |

- **The supervisor never hands its environment to the game.** Everything it does to the game
  (start it, click through Steam's prompt, quit it, put the loader in and take it out, empty
  `console.log` and `data/`) goes through `sudo -n -u observer /opt/observer/game-ctl <command>`.
  sudo starts it with a fresh environment and game-ctl builds its own, so the token never reaches
  Steam or GMod. GMod is launched through the Steam that `observer-desktop` runs, inside that
  service and its memory limits.
- **The supervisor only reads what the game writes**: `console.log` (as a member of the
  `observer` group) and the job folder. Files the game could have swapped for a link are opened
  without following links, and only when they're owned by the game. The game can't point it at
  the token that way.
- **The job folder**, which the module and the supervisor share, is `/var/lib/meowy-observer`:
  `obsvisor:observer`, mode `3770`.
  - The setgid bit gives new files the `observer` group. Both services use umask `027`, so each
    side can read what the other writes.
  - The sticky bit stops the game from deleting or replacing the supervisor's files (`job.json`,
    `full-downloads.json`). It can only touch its own (`done.json`, `observer.log`,
    `native-state-*`).
  - `bin/`, which holds the real module, is root's: the game can load it but can't change it.
  - `~observer/.meowy-observer` links there, because the module falls back to that path when
    Steam doesn't pass `MEOWY_OBSERVER_DIR` on to the game.
- **Firewall** (`observer-firewall.service`, nftables, `vm/firewall.sh`). It applies only to the
  `observer` user's traffic:
  - allowed: loopback, DNS to the NAT gateway, and the VPS's public address on UDP (game servers)
    and TCP 80/443;
  - dropped: everything else to the VPS (SSH, Wings, databases), and all private, link-local and
    multicast ranges;
  - the internet stays open.

  `observer-desktop` requires this service, so the game doesn't run without it.

## 1. Create the VM (on the VPS, as root, once)

The observer files are already on the VPS at `/opt/stacks/claude-staging/meowy-observer`.

```bash
sudo bash /opt/stacks/claude-staging/meowy-observer/vm/create-vm.sh
```

It installs KVM/libvirt, creates `meowy-observer` (Ubuntu 24.04, 4 vCPU, 16 GB RAM, 60 GB disk) on
libvirt's private NAT network, and at first boot the VM installs everything itself
(`vm/setup.sh`, about 5-10 minutes). It prints the VM's address (usually `192.168.122.x`). The
VM's `obsadmin` user accepts the same SSH keys as the host's root.

## 2. The token (in the VM, as obsadmin)

```bash
ssh obsadmin@<vm address>            # from the VPS
echo 'SERVER_REP_OBSERVER_TOKEN=<the same token as the backend>' | sudo tee /etc/meowy-observer/env >/dev/null
sudo systemctl restart observer
```

`tee` keeps the file's owner and mode (`root:obsvisor`, 640).

## 3. Log in to Steam once (from your PC)

The VM's display listens only inside the VM (127.0.0.1:6080), so the tunnel goes through both
hops: your PC to the VPS, then the VPS to the VM (using the key that can log in to the VM):

```bash
ssh -t -L 6080:localhost:6080 <you>@138.201.62.52 ssh -i ~/.ssh/meowy_observer -L 6080:localhost:6080 obsadmin@<vm address>
```

Then open http://localhost:6080/vnc.html, Connect, log in with the observer account (Steam
Guard code), and install Garry's Mod (x86-64 branch if you can; `sudo bash
/opt/observer-src/vm/use-x86-64.sh` switches it later). Once it's installed, the observer starts
by itself. Its log: `journalctl -u observer -f` in the VM. The admin is in `systemd-journal`, so
this needs no sudo.

## Queueing a visit by hand

As `obsadmin` in the VM (it switches itself to `obsvisor` through sudo, the one user that can read
the token):

```bash
observer-queue 194.69.160.57:27050            # status only
observer-queue 194.69.160.57:27050 lua        # with the Lua sampler
observer-queue --local 138.201.62.52:27015    # a test visit the backend never hears of
```

The game user can't queue visits: it has no sudo, and the supervisor ignores a `local-job.json`
that isn't its own.

## When Steam stops launching the game

Steam can stop taking launches: the game's launch wrapper (`reaper SteamLaunch AppId=4000`)
outlives a game that died, so Steam counts it as still running, or a dialog sits in front.
Before each launch the supervisor clears a leftover wrapper. If a launch still goes unanswered
for three minutes, it restarts Steam (`game-ctl restart-steam`; `observer-desktop` brings the
display and Steam back), waits for it, and launches once more. These steps appear in the log as
`steam_restart`, `steam_restarted` and `steam_restart_ok`.

If launches fail even after a restart on three visits in a row, the supervisor logs
`launch_paused` and takes no visits for 20 minutes, so the queue isn't emptied into failures.
That usually needs a person: open the browser view (section 3) and check whether Steam is logged
out or showing a dialog.

```bash
journalctl -u observer --since -2h -o cat | grep -E 'game_start|steam_|launch_paused|visit_failed'
```

## Moving a VM from before the split (one `observer` user with sudo)

The Steam install and Garry's Mod stay where they are, in `~observer`, so nothing is
downloaded again. `observer` becomes the game user. `setup.sh` adds `obsadmin` (with the SSH keys
`observer` had, and sudo) and `obsvisor`, takes sudo and SSH away from `observer`, moves the job
folder, and restarts the services.

1. On the VPS, in its own shell: put this version of the repo's `observer/` folder at
   `/opt/stacks/claude-staging/meowy-observer` (as for the first install), then copy it into the
   VM:

   ```bash
   scp -i ~/.ssh/meowy_observer -r /opt/stacks/claude-staging/meowy-observer observer@<vm address>:/tmp/
   ```

2. In the VM's shell (`ssh -i ~/.ssh/meowy_observer observer@<vm address>` from the VPS; this
   is the last time you log in as `observer`):

   ```bash
   sudo rm -rf /opt/observer-src
   sudo cp -r /tmp/meowy-observer /opt/observer-src
   sudo bash /opt/observer-src/vm/setup.sh
   ```

   It ends with "observer VM ready". If it stops at "obsadmin has no SSH key", nothing has been
   taken away yet: do what it says and run it again. Keep this session open until step 3 works.

3. On the VPS, in a second terminal, log in as the admin and check:

   ```bash
   ssh -i ~/.ssh/meowy_observer obsadmin@<vm address>
   ```

   Then, in the VM:

   ```bash
   sudo -v && echo "admin sudo works"
   sudo -l -U observer                          # "User observer is not allowed to run sudo"
   sudo -u observer cat /etc/meowy-observer/env # Permission denied
   systemctl status observer-firewall observer-desktop observer --no-pager
   journalctl -u observer -n 30 --no-pager      # "ready", then visits
   sudo nft list table inet meowy_observer
   sudo -u observer curl -sS -m 5 -o /dev/null https://meowynetwork.com && echo "internet ok"
   sudo -u observer timeout 5 bash -c 'echo > /dev/tcp/192.168.122.1/22' && echo "NOT blocked" || echo "host blocked"
   ```

   Then close the `observer` session from step 2. From now on, log in as `obsadmin`, and use
   `obsadmin@` in the tunnel too (section 3).

4. Optional: until now the game user could read the token. If you want to be sure it never
   leaked, make a new one in the backend and write it as in section 2.

If Steam asks you to log in again after the restart, do it through the browser view (section 3).

## Files

| In the repo | In the VM | Runs as |
|---|---|---|
| `vm/setup.sh` | (run by hand or by cloud-init) | root |
| `vm/observer-game.sh` | `/opt/observer/observer-game.sh`, `observer.service` | obsvisor |
| `supervisor/supervisor.mjs` | `/opt/observer/supervisor.mjs` | obsvisor; and, as `--game <command>` from game-ctl, observer |
| `vm/game-ctl.sh` | `/opt/observer/game-ctl` | observer (via sudo, from the supervisor) |
| `vm/firewall.sh` | `/opt/observer/firewall.sh`, `observer-firewall.service` | root |
| `vm/queue.sh` | `/usr/local/bin/observer-queue` | obsadmin, which switches to obsvisor |
| `vm/use-x86-64.sh` | (run by hand) | obsadmin with sudo |
