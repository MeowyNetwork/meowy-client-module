# Notifications: manual checks

A real toast can't be asserted in CI. `notify_smoke` covers the rules (kinds and their texts, gate,
switch, focus, rate limit); `uninstall_smoke` covers the Start-menu shortcut. These are by hand.

Build with `-DMEOWY_BUILD_TESTS=ON` (both `build32` and `build64`). Turn off Do Not Disturb first:
Windows holds back toasts and balloons while it's on, or while a full-screen app runs.

## Without the game
1. `notify_manual printer_attacked 20` with no "Meowy Client Pack" in the Start menu: prints
   `no Start-menu shortcut ..., balloon` and a balloon "Meowy Network / Your printer is under
   attack." appears with the cat icon. The stand-in window's taskbar button has the badge and
   flashes. Click the balloon: the stand-in window comes to the front, the badge goes, and the
   tray icon is gone. `shutdown took` stays well under 2000 ms.
2. Run the installer (`MeowyClientPack.exe`, Install), then `notify_manual --check-shortcut
   "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Meowy Client Pack.lnk"` prints `yes`.
3. `notify_manual ticket_reply 20`: prints `toast`; a toast with the title "Meowy Network", the text
   "Staff replied to your ticket." and the cat logo appears, attributed to "Meowy Client Pack".
   Clicking it brings the stand-in window forward (the result line then ends in `toast clicked`).
4. Settings > System > Notifications > Meowy Client Pack: off. Step 3 prints
   `toast suppressed by Windows settings` and shows nothing (no balloon either).
5. Do the same with `build32\Release\notify_manual.exe`.

## In the game (dev server)
1. Join the dev server windowed or borderless, alt-tab away, and have the server send a kind (for
   example a dev bot raiding your printer). A toast appears; the taskbar button has the badge and
   flashes. Click it: the game comes to the front and the badge clears.
2. Alt-tab back yourself instead: the badge and flashing stop on their own.
3. With the game in front, `lua_run_cl print(meowy.Notify("arrested"))` prints `false focused`.
4. `lua_run_cl print(meowy.Notify("payday"))` prints `false unknown kind`.
   `lua_run_cl timer.Simple(5, function() print(meowy.Notify("arrested")) print(meowy.Notify("arrested")) end)`,
   then alt-tab within 5 s: one toast, and the console shows `true` then `false rate limited`.
5. `meowy.SetNotifyEnabled(false)`, then a kind: `false disabled`; it stays off after restarting
   the game (`%LOCALAPPDATA%\MeowyClientPack\notify.txt` says `enabled=0`). Turn it back on.
6. Join another server (or start singleplayer): `meowy.Notify("arrested")` prints
   `false only works on Meowy Network servers`.
7. Disconnect while a badge is showing: the badge goes. Quit the game: it closes at once.
8. Uninstall with the installer: "Meowy Client Pack" is gone from the Start menu.
