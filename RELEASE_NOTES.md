**New in 1.2:** crash reports also say where the game crashed (the module and offset, e.g. `engine.dll+0x200660`), and the pack can describe your hardware (CPU, GPU, RAM, Windows version) so the server can see what players' machines struggle with. Nothing identifying is collected.

**New in 1.1:** when you join, the server receives the Lua part of any game crash reports since your last visit, so crashes caused by addons can be found and fixed. Only the "Lua Stack Traces" section is sent; nothing else from the report leaves your PC.

Download **MeowyClientPack.exe**, close Garry's Mod, run it and click **Install**.

The installer isn't code-signed yet: if Windows shows "Windows protected your PC", click **More info** → **Run anyway**.
File hashes are in `SHA256SUMS.txt`, and every file has a GitHub build attestation
(`gh attestation verify <file> --repo MeowyNetwork/meowy-client-module`).

Manual install: unzip `meowy-client-module-manual.zip` into `garrysmod/lua/bin/`.
