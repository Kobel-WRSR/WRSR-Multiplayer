# WRSR Multiplayer

First multiplayer mod for Workers & Resources: Soviet Republic.

## How it works

Turn-based cooperative multiplayer via save file sync over TCP.
Host saves the game → mod sends save to client → client loads it → plays → sends back.

## Requirements

- Workers & Resources: Soviet Republic v1.1.1.9
- TesmioLoader b0.3.6+
- Both players need the mod installed

## Installation

1. Download the latest release
2. Copy `multiplayer.dll` and `multiplayer.ini` to:
   `SovietRepublic\tesmioloader\build\plugins\`
3. Copy `mp_overlay.exe` anywhere convenient
4. Edit `multiplayer.ini`:
   - Host: set `mode = host`
   - Client: set `mode = client` and `host_ip = HOST_IP_HERE`

## Playing

### Host
1. Launch game via `tesmiolauncher.exe`
2. Load or create a map
3. Open `mp_overlay.exe`
4. Share your IP with the client (check at 2ip.ru)
5. Save the game — client receives it automatically

### Client
1. Launch game via `tesmiolauncher.exe`
2. Open `mp_overlay.exe`
3. Enter host IP and click Connect
4. Wait for save transfer
5. Load `mp_client` save in game

## Current version

v0.1.1 — TCP sync, zstd compression, overlay window, ping, chat

## Roadmap

- [ ] Delta sync
- [ ] Real-time multiplayer
- [ ] In-game UI

## License

GPL-3.0