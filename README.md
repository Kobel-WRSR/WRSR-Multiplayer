# WRSR Multiplayer

[![Discord](https://img.shields.io/badge/Discord-Join-7289da)](https://discord.gg/d3JRzaY3s)

![version](https://img.shields.io/badge/version-0.7.1-red)
![status](https://img.shields.io/badge/status-alpha-orange)

The first multiplayer mod for Workers & Resources: Soviet Republic. Built from scratch using TesmioLoader hooks and reverse engineering the game executable with Ghidra.

This is alpha. Things work, things break. If you're here to test — thank you, seriously.

---

## What actually works

- Real-time building sync — place a building and it shows up in everyone's overlay instantly
- Save sync — when a new player connects, they get the full save. After that, only changes are sent (delta compression with zstd)
- Up to 4 players
- Overlay app — chat, player list, build history, demolish history, interactive map, resource import tab, ping graph, connection log, admin panel, settings
- No config editing — the overlay connects to the game plugin automatically

## What doesn't work yet

- Buildings don't appear on the client's screen in real time — they appear after the next save sync (~1-2 min). The overlay shows them instantly though
- Road, rail, pipe sync
- Territory mode — the UI exists, the logic doesn't
- Resource delivery between players

---

## Requirements

- Workers & Resources: Soviet Republic (v1.1.1.9, other versions may work)
- TesmioLoader b0.3.6 or newer
- ZeroTier — if you want to play over the internet (free, 2 min setup)

---

## Installation

1. Download `multiplayer.dll`, `mp_overlay.exe` and `multiplayer.ini` from this page
2. Drop `multiplayer.dll` and `multiplayer.ini` into your TesmioLoader plugins folder:
   ```
   SovietRepublic/tesmioloader/build/plugins/
   ```
3. Done. No other setup needed on the host side.

---

## Important: open port 7777 (host only)

Before hosting, open port 7777 in Windows Firewall:

1. Start → Windows Defender Firewall → Advanced Settings
2. Inbound Rules → New Rule
3. Port → TCP → 7777 → Allow the connection
4. Apply to all profiles → give it a name → Done

Without this, clients won't be able to connect.

---

## Important: launch order

**Always start the game first, then the overlay.** If you open the overlay before the game, it will lag until the game loads.

---

## How to play

### Host
1. Open port 7777 in Windows Firewall (see above)
2. Start the game via TesmioLoader
3. Load your save or start a new game
4. Run `mp_overlay.exe` — it connects to the plugin automatically and shows ONLINE
5. Share your ZeroTier IP with friends (shown in the overlay)
6. Play normally — buildings sync to everyone

### Client
1. Install the mod the same way as the host
2. Start the game via TesmioLoader
3. Run `mp_overlay.exe`
4. Select **Client** mode, enter host's ZeroTier IP, your name, click **Connect**
5. Wait for the save to sync — then load the `mp_client` save in-game

---

## ZeroTier (internet play)

1. Download ZeroTier from [zerotier.com](https://www.zerotier.com)
2. Host creates a free network at [my.zerotier.com](https://my.zerotier.com)
3. Host shares the Network ID with friends
4. Everyone joins the same network and gets authorized by the host
5. Use the IP shown in the ZeroTier tray app as the host IP in the overlay

---

## Changelog

### v0.7.1
- Overlay now shows IP and port when connected
- Your name is visible in the status bar
- Fixed status showing ONLINE when not actually connected
- Added host/player count display

### v0.7.0
- Admin tab: kick, mute, broadcast to all players
- Settings tab: saved servers, toggles for notifications/map/autoscroll
- Demolish history tab
- Ping graph in Stats
- Connection log in Stats
- Territory display on map
- Cyrillic font support
- Default language is English

### v0.5.0
- Initial public release
- Building sync, save sync, chat, player list, map, resource import

---

## Known issues

- Client may disconnect on first connection attempt — just reconnect
- Buildings appear on client side only after save sync, not in real time
- Territory mode UI exists but logic not implemented
- Two instances of the game on the same PC won't work — use two separate PCs

---

## Building from source

```
cl /LD /MT /O2 /std:c++17 multiplayer.cpp bsdiff.c bspatch.c /Fe:multiplayer.dll /I. /I..\..\src /IC:\vcpkg\installed\x64-windows-static\include /link /DLL ws2_32.lib shell32.lib C:\vcpkg\installed\x64-windows-static\lib\zstd.lib

cl /MT /O2 /std:c++17 /utf-8 mp_overlay.cpp bspatch.c /Fe:mp_overlay.exe /I. /IC:\vcpkg\installed\x64-windows-static\include /link /subsystem:windows ws2_32.lib d3d11.lib dxgi.lib iphlpapi.lib winmm.lib C:\vcpkg\installed\x64-windows-static\lib\imgui.lib C:\vcpkg\installed\x64-windows-static\lib\zstd.lib user32.lib gdi32.lib shell32.lib
```

Dependencies: vcpkg with `zstd` and `imgui[dx11-binding,win32-binding]` (x64-windows-static)

---

## Community

Join the Discord: https://discord.gg/d3JRzaY3s

## License

MIT