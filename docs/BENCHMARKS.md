# Measuring the pack

What the pack changes should be shown with numbers anyone can get again on their own PC. This page says
how they are measured. The numbers for a release go under *Results* once a full run has been made for
it; none have been published yet.

## What is measured

- **Join time**: from the console's "Connecting to" line until the map is drawn ("Redownloading all
  lightmaps"), with the game started straight into the server (`+connect`).
- **How smoothly the game runs**: the pack's frame recorder (`meowy_frames_status`,
  [HOW-IT-WORKS](HOW-IT-WORKS.md)): the average frame rate, the 1% and 0.1% lows (the frame rate of the
  slowest frames, which is what stutter feels like), the median and 99th-percentile frame time, and
  hitches (a frame of 50 ms or more, or one of 25 ms or more that took four times as long as the frames
  around it).

## Configurations

| Name | What runs | Compared for |
|---|---|---|
| `stock` | Garry's Mod with the pack not loaded (`-meowy_safe`) | join time |
| `pack` | the pack as installed, your own settings | join time, frames |
| `no-speedups` | the pack with every join speed-up switched off | join time, against `pack` |
| `no-multicore` | the pack with multicore rendering off | frames, against `pack` |

`stock` has no frame numbers: the recorder is the pack's. Frame rates are compared between the pack's
own configurations, measured by the same instrument.

## Running it

```powershell
pwsh tools/bench/bench.ps1 -Game "D:\SteamLibrary\steamapps\common\GarrysMod" -Server 1.2.3.4:27015
```

Close the game first, and leave the PC alone while it runs: the recorder only counts frames while the
game's window is in front, and the script puts it there. The defaults are two counted runs of each
configuration with 60 seconds measured each, after one join that isn't counted, about 20 minutes in
all. `-Configs pack, no-multicore`, `-Runs`, `-Seconds`, `-Bits 32` and `-NoTurn` change that
(`Get-Help tools/bench/bench.ps1 -Full`).

It prints a table of each configuration's median and writes it, with every run, to `bench-out\` as
Markdown and JSON.

## What makes the numbers comparable, and what doesn't

- **Warm and cold joins differ a lot.** The first join after the PC starts reads everything from disk;
  later ones come from Windows' file cache. The script makes one join that isn't counted, then goes
  through the configurations in turn each round, so no configuration gets all the warm joins. The
  pack's biggest join gains are on the first join of a session, which this warm comparison doesn't
  show: for that, restart the PC, run one configuration with `-NoWarmUp -Runs 1`, and repeat for the
  other.
- **The view turns on the spot** for the measured time, so new things come into view the same way in
  every run. It isn't play: a busy server, a fight or driving across the map will be slower.
- **A live server changes between runs**: players, props and what the server's addons are doing. A
  quiet server, or the same minute-to-minute state, gives closer runs.
- **The game is 1600x900 in a window** unless `-ExtraArgs` says otherwise.
- **Your own settings stay as they are** in `pack`; the other configurations change only what the
  table above says.

## What it touches

The pack's settings file (`%LOCALAPPDATA%\MeowyClientPack\menu_settings.txt`) is copied before a
configuration changes it and put back at the end, also when a run fails. The recorder's kept sessions
(`frames.txt`) are put back too, so the benchmark's runs don't fill your own list. Two small files are
written to `garrysmod\cfg` for the camera turn and removed again.

## Results

None published yet.
