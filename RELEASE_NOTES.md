**New in 1.4.1:** when you launch the game straight into the server (Steam join links), the screen you see for the first second while the loading page starts now looks just like the page, so there's no jump from a different-looking screen.

**New in 1.4:** our own loading screen. When you join a Meowy Network server, the pack shows its own screen instead of Garry's Mod's. It never freezes (Garry's Mod's own screen does for most of a join), shows each stage and a clock time for when you'll be in, and has things to do while you wait: *Where in Downtown?* (find where a photo was taken on the city map), *Rooftop run* (a cat running over the city's real rooftops that arrives when your game is ready) and *the city cat*. There's also a map of the city to explore, and Cancel works even while the game is busy. It's our web page from meowynetwork.com, shown with WebView2 (the browser built into Windows), and it appears only for joins to our servers and only in windowed or borderless mode; `meowy_loadingscreen 0` in the console turns it off. To make this possible the pack now **starts with the game**: the installer adds one line to `lua/includes/menu.lua` (removed on uninstall), and holding Shift while the game starts skips it once.

**New in 1.3:** faster joins. At the end of loading a server's Lua, Garry's Mod throws away every material and loads them all again; on Meowy Network's map that is 5-15 seconds of every join. The pack now skips that second load (the materials stay as they were loaded seconds earlier). It's an in-memory change to the game that is only active while you join a Meowy Network server, it switches itself off if Garry's Mod updates that code, and `meowy_skipmatreload 0` in the console turns it off. It also now only works on Meowy Network servers. It checks which server the game is really connected to (inside the DLL, not in Lua, so a server can't fake it). Before this, any server could have read your recent crash reports and hardware description, or changed your Discord status. Discord buttons now only accept https links.

**New in 1.2:** crash reports also say where the game crashed (the module and offset, e.g. `engine.dll+0x200660`), and the pack can describe your hardware (CPU, GPU, RAM, Windows version) so the server can see what players' machines struggle with. No serial numbers or user names are collected; the description is stored on our server with your Steam account ID so it can be matched with performance stats.

**New in 1.1:** when you join, the server receives the Lua part of any game crash reports since your last visit, so crashes caused by addons can be found and fixed. Only the "Lua Stack Traces" section is sent; nothing else from the report leaves your PC.

Download **MeowyClientPack.exe**, close Garry's Mod, run it and click **Install**.

The installer isn't code-signed yet: if Windows shows "Windows protected your PC", click **More info** → **Run anyway**.
File hashes are in `SHA256SUMS.txt`, and every file has a GitHub build attestation
(`gh attestation verify <file> --repo MeowyNetwork/meowy-client-module`).

Manual install: unzip `meowy-client-module-manual.zip` into `garrysmod/lua/bin/`.
