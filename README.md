WRSR Multiplayer

Show Image Show Image Show Image

The first multiplayer mod for Workers & Resources: Soviet Republic. Built from scratch using TesmioLoader hooks and reverse engineering the game executable with Ghidra.

This is alpha. Things work, things break. If you're here to test — thank you, seriously.

What actually works
Real-time building sync — place a building and it shows up in everyone's overlay instantly
Save sync — when a new player connects, they get the full save. After that, only changes are sent (delta compression with zstd)
Up to 4 players
Overlay app — chat, player list, build history, interactive map, resource import tab, session stats
No config editing — the overlay connects to the game plugin automatically and handles everything
What doesn't work yet
Buildings don't appear on the client's screen in real time — they appear after the next autosave syncs (~1-2 min). The overlay shows them instantly though
Road, rail, pipe sync
Territory mode — the UI exists, the logic doesn't
Resource delivery by train — same, UI only for now
Requirements
Workers & Resources: Soviet Republic (v1.1.1.9, other versions may work)
TesmioLoader b0.3.6 or newer
ZeroTier — if you want to play over the internet (free, 2 min setup)
Installation
Download multiplayer.dll, mp_overlay.exe and multiplayer.ini from this page
Drop multiplayer.dll and multiplayer.ini into your TesmioLoader plugins folder:
SovietRepublic/tesmioloader/build/plugins/
Done. No other setup needed on the host side.
How to play
Host
Start the game
Run mp_overlay.exe — it connects to the plugin automatically
Share your ZeroTier IP with friends
Play normally, your buildings sync to everyone
Client
Install the mod the same way as the host
Start the game, then run mp_overlay.exe
Enter the host's IP and your name, hit Connect
Wait for the save to sync, then load the mp_client save in-game
That's it
ZeroTier (internet play)
Download ZeroTier from zerotier.com
Host creates a free network at my.zerotier.com
Host shares the Network ID with friends
Everyone joins the same network
Use the IP shown in the ZeroTier tray app as the host IP in the overlay
Known issues
Solo tested only so far — real multiplayer testing is ongoing
All buildings sync to the same coordinates as on host (visual placement on client coming in v0.5)
Client needs to reload the save after the initial sync
Roadmap

v0.5

Visual building placement on client (no save reload)
Road and rail sync
Territory mode

v1.0

Resource contracts between players
Physical resource delivery by train/truck between territories
Full cooperative and territory game modes
How it works

The mod hooks into the game's build placement function at the binary level using TesmioLoader. When you place a building, the hook captures the building type and coordinates, then broadcasts them to all connected clients over TCP. Save file sync uses bsdiff for delta compression.

The overlay communicates with the in-game plugin through Windows shared memory — no network traffic between the overlay and the game, just a shared memory block that both sides read and write.

Building from source
cl /LD /MT /O2 /std:c++17 multiplayer.cpp bsdiff.c bspatch.c /Fe:multiplayer.dll /I. /IC:\vcpkg\installed\x64-windows-static\include /link /DLL ws2_32.lib shell32.lib C:\vcpkg\installed\x64-windows-static\lib\zstd.lib

cl /MT /O2 /std:c++17 mp_overlay.cpp bspatch.c /Fe:mp_overlay.exe /I. /IC:\vcpkg\installed\x64-windows-static\include /link /subsystem:windows ws2_32.lib d3d11.lib dxgi.lib C:\vcpkg\installed\x64-windows-static\lib\imgui.lib C:\vcpkg\installed\x64-windows-static\lib\zstd.lib user32.lib gdi32.lib shell32.lib

Dependencies: vcpkg with zstd and imgui[dx11-binding,win32-binding] (x64-windows-static)

License

MIT