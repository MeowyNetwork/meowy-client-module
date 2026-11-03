**New in 1.3:** faster joins. At the end of loading a server's Lua, Garry's Mod throws away every material and loads them all again; on Meowy Network's map that is 5-15 seconds of every join. The pack now skips that second load (the materials stay as they were loaded seconds earlier). It's an in-memory change to the game that is only active while you join a Meowy Network server, it switches itself off if Garry's Mod updates that code, and `meowy_skipmatreload 0` in the console turns it off. It also now only works on Meowy Network servers. It checks which server the game is really connected to (inside the DLL, not in Lua, so a server can't fake it). Before this, any server could have read your recent crash reports and hardware description, or changed your Discord status. Discord buttons now only accept https links.

**New in 1.2:** crash reports also say where the game crashed (the module and offset, e.g. `engine.dll+0x200660`), and the pack can describe your hardware (CPU, GPU, RAM, Windows version) so the server can see what players' machines struggle with. No serial numbers or user names are collected; the description is stored on our server with your Steam account ID so it can be matched with performance stats.

**New in 1.1:** when you join, the server receives the Lua part of any game crash reports since your last visit, so crashes caused by addons can be found and fixed. Only the "Lua Stack Traces" section is sent; nothing else from the report leaves your PC.

Download **MeowyClientPack.exe**, close Garry's Mod, run it and click **Install**.

The installer isn't code-signed yet: if Windows shows "Windows protected your PC", click **More info** → **Run anyway**.
File hashes are in `SHA256SUMS.txt`, and every file has a GitHub build attestation
(`gh attestation verify <file> --repo MeowyNetwork/meowy-client-module`).

Manual install: unzip `meowy-client-module-manual.zip` into `garrysmod/lua/bin/`.
