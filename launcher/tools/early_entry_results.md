# Early entry (start-up #9): measured, shelved

2026-11-08, this PC, `launcher/tools/early_entry.ps1 -Rounds 5 -LazyFonts`: `bin\win64\gmod.exe` started
directly and suspended, with `-disableluarefresh` and the Workshop fonts set aside in both modes (what the
launcher's Faster start already does). Early mode loads the installed pack (this branch's build) and calls
`meowy_early_start` before resuming; plain mode resumes at once. Timed from resume to the pack's menu Lua.

| Round | Plain (s) | Early (s) | Early − plain |
|---|---|---|---|
| 1 | 3.80 | 3.26 | −0.54 |
| 2 | 3.58 | — (never reached the menu in 90 s; no crash report) | — |
| 3 | 3.72 | 2.82 | −0.90 |
| 4 | 3.61 | 2.75 | −0.86 |
| 5 | 3.63 | 2.82 | −0.81 |

- **Median −0.86 s**, under the 2 s gate (DESIGN §9). With lazy fonts and `-disableluarefresh`, which need no
  injection, already taking ~1.3 s and ~0.8 s, the injection would add less than a second.
- **What it did:** `fs_cache` was in place 47 ms after start and answered ~7,860 of ~8,260 opens "not there"
  from ~775 folder listings before the menu; `font_loads` saved 89 gdi32 loads.
- **1 of 5 early starts never reached the menu** (most likely a hang from hooking while the engine starts).
  That alone would need work before anything like this could ship.

**Decision:** shelved. The injection stays a prototype on this branch; the launcher keeps to file
operations and launch flags.
