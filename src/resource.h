#pragma once
// Resources in gmcl_meowy (src/meowy.rc).
#define IDI_MEOWY 101        // the pack's icon: taskbar badge, tray icon, Start-menu shortcut (icon index 0)
#define IDR_NOTIFY_PNG 102   // the toast's logo (written to %LOCALAPPDATA%\MeowyClientPack\notify_icon.png)
#define IDR_RESILIENCE_LOGIC_LUA 103   // src/lua/resilience_logic.lua: connection resilience's rules (menu state)
#define IDR_RESILIENCE_LUA 104         // src/lua/resilience.lua: its menu glue and UI
#define IDR_DOCTOR_LOGIC_LUA 105       // src/lua/doctor_logic.lua: the PC doctor's rules (menu state)
#define IDR_DOCTOR_LUA 106             // src/lua/doctor.lua: its settings tab and meowy_doctor
#define IDR_UI_LUA 107                 // src/lua/ui.lua: the menu UI kit (cards, buttons, prompts)
#define IDR_HOME_LOGIC_LUA 108        // src/lua/home_logic.lua: the main menu's Home card, its rules
#define IDR_HOME_LUA 109              // src/lua/home.lua: the Home card on the menu page
#define IDR_MENU_PAGE_LUA 110         // src/lua/menu_page.lua: the Meowy home screen (preview), its Lua side
#define IDR_MENU_SHELL_JS 111         // src/menu/shell.js: that home, put into the stock menu page
#define IDR_CONSOLE_LOGIC_LUA 112     // src/lua/console_logic.lua: the Meowy console's rules
#define IDR_CONSOLE_LUA 113           // src/lua/console.lua: the Meowy console window (meowy_console)
#define IDR_MENU_BROWSER_JS 114       // src/menu/browser.js: Find servers, the pack's own (goes in with shell.js)
#define IDR_SETTINGS_GUARD_LOGIC_LUA 130   // src/lua/settings_guard_logic.lua: what servers changed in your settings, its rules
#define IDR_SETTINGS_GUARD_LUA 131         // src/lua/settings_guard.lua: its prompt, and the config.cfg copies' commands
#define IDR_CONTENT_LOGIC_LUA 115     // src/lua/content_logic.lua: the content platform's rules (adverts, plans, consent)
