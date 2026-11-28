#pragma once

// Who runs this build of the pack: the one place the C++ names the site it
// talks to and the server it treats as its own. Everything else reads these.
//
// The same two facts are kept once each for the other two languages:
//   launcher/core/src/brand.rs   (Rust: the launcher)
//   installer/Installer.cs       (C#: Installer.ServerAddress)
// tests/brand_test.js checks that the three agree and that no other source
// file spells the host or the address out.
//
// MEOWY_BRAND_HOST: policy, updates, the server index, the server-list
// verdicts and reports, and the loading page all come from it, over HTTPS.
// MEOWY_BRAND_SERVER_*: the one game server whose Lua may call the pack's
// client-realm functions (server_gate.h). Only this port: the same host also
// runs throwaway test servers.
#define MEOWY_BRAND_HOST "meowynetwork.com"
#define MEOWY_BRAND_SERVER_IP "138.201.62.52"
#define MEOWY_BRAND_SERVER_PORT 27015

#define MEOWY_BRAND_WIDE2(x) L##x
#define MEOWY_BRAND_WIDE(x) MEOWY_BRAND_WIDE2(x)
#define MEOWY_BRAND_HOST_W MEOWY_BRAND_WIDE(MEOWY_BRAND_HOST)
#define MEOWY_BRAND_SITE "https://" MEOWY_BRAND_HOST
#define MEOWY_BRAND_SITE_W L"https://" MEOWY_BRAND_HOST_W
