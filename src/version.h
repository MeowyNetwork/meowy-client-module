#pragma once
// The pack's version, in one place: meowy.VERSION (module.cpp) and the version
// resource of both DLLs (meowy.rc, boot/boot.rc), so anything outside the game
// (the launcher, the installer) can read the installed copy's version from the
// file itself (GetFileVersionInfo) and a file can't disagree with the code.
// Plain #defines: the resource compiler reads this too.
#define MEOWY_VERSION_MAJOR 1
#define MEOWY_VERSION_MINOR 7
#define MEOWY_VERSION_PATCH 1
#define MEOWY_VERSION "1.7.1"
