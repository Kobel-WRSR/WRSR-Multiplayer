#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../../src/tesmio_api.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "zstd.h"

extern "C" {
#include "bsdiff.h"
#include "bspatch.h"
}

#pragma comment(lib, "ws2_32.lib")

#define MAX_PLAYERS   4
#define MSG_SAVE      1
#define MSG_PING      2
#define MSG_PONG      3
#define MSG_CHAT      4
#define MSG_SAVE_FULL 5
#define MSG_SAVE_DIFF 6
#define MSG_BUILD     7

#define DIFF_SIZE_LIMIT (10 * 1024 * 1024)

#define CLICK_FLAG_RVA  0xA54E91
#define HOOK_TARGET_RVA 0x6F8CC0

static const TsmHost* H = nullptr;
static char g_saveDir[MAX_PATH];
static char g_syncDir[MAX_PATH];
static char g_playerName[64];
static char g_mode[32];
static char g_hostIp[64];
static int  g_port = 7777;

#pragma pack(push, 1)
struct BuildCmd {
    float x, y, z;
    float rotation;
    char  typeName[64];
};
#pragma pack(pop)

struct Player {
    SOCKET sock;
    char   name[64];
    DWORD  ping;
    ULONGLONG pingStart;
    bool   connected;
    bool   hasFullSave;
    char   prevSaveDir[MAX_PATH];
    CRITICAL_SECTION netLock;
};

static Player g_players[MAX_PLAYERS];
static int    g_playerCount = 0;
static HANDLE g_watchThread  = NULL;
static HANDLE g_serverThread = NULL;
static CRITICAL_SECTION g_lock;

struct PlayerLockGuard {
    Player& p;
    PlayerLockGuard(Player& p) : p(p) { EnterCriticalSection(&p.netLock); }
    ~PlayerLockGuard() { LeaveCriticalSection(&p.netLock); }
};

typedef void (*BuildHandlerFn)(__int64, char*);
static BuildHandlerFn g_origBuildHandler = nullptr;
static BYTE g_hookOrigBytes[14];
static HMODULE g_hExe = nullptr;

struct MsgHeader {
    BYTE  type;
    DWORD size;
};

static bool SendAll(SOCKET sock, const char* buf, int size)
{
    int sent = 0;
    while (sent < size) {
        int r = send(sock, buf + sent, size - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

static bool RecvAll(SOCKET sock, char* buf, int size)
{
    int received = 0;
    while (received < size) {
        int r = recv(sock, buf + received, size - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

static void DrainSocket(SOCKET sock, DWORD size)
{
    char trash[1024];
    DWORD left = size;
    while (left > 0) {
        DWORD toRead = left > sizeof(trash) ? sizeof(trash) : left;
        if (!RecvAll(sock, trash, toRead)) break;
        left -= toRead;
    }
}

static void SendMsgToPlayer(Player& p, BYTE type, const void* data, DWORD size)
{
    MsgHeader hdr;
    hdr.type = type;
    hdr.size = size;
    PlayerLockGuard lock(p);
    SendAll(p.sock, (char*)&hdr, sizeof(hdr));
    if (data && size > 0)
        SendAll(p.sock, (char*)data, size);
}

static bool RecvMsg(SOCKET sock, BYTE* type, DWORD* size, char* buf, DWORD bufSize)
{
    MsgHeader hdr;
    if (!RecvAll(sock, (char*)&hdr, sizeof(hdr))) return false;
    *type = hdr.type;
    *size = hdr.size;
    if (hdr.size > 0 && hdr.size < bufSize)
        if (!RecvAll(sock, buf, hdr.size)) return false;
    return true;
}

static void SendBuildToAll(const BuildCmd* cmd)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].connected && g_players[i].sock != INVALID_SOCKET)
            SendMsgToPlayer(g_players[i], MSG_BUILD, cmd, sizeof(BuildCmd));
    }
    LeaveCriticalSection(&g_lock);
}

static bool SafeReadString(const char* ptr, char* out, int maxLen)
{
    if (!ptr) return false;
    __try {
        int i = 0;
        while (i < maxLen - 1) {
            char c = ptr[i];
            if (c == 0) break;
            if (c < 0x20 || c > 0x7E) return false;
            out[i] = c;
            i++;
        }
        out[i] = 0;
        return i >= 3;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static ULONGLONG s_lastBuildTime = 0;

static void HookedBuildHandler(__int64 param_1, char* param_2)
{
    bool shouldSend = false;
    BuildCmd cmd = {};

    if (g_hExe && param_2) {
        BYTE p2_20 = 0;
        __try { p2_20 = param_2[0x20]; } __except(EXCEPTION_EXECUTE_HANDLER) {}

        BYTE flag = 0;
        BYTE* clickFlag = (BYTE*)((DWORD_PTR)g_hExe + CLICK_FLAG_RVA);
        __try { flag = *clickFlag; } __except(EXCEPTION_EXECUTE_HANDLER) {}

        if (p2_20 != '\0' && flag != '\0') {
            __try {
                __int64 lVar5 = *(__int64*)((__int64)param_2 + 0x240);
                if (lVar5 > 0x10000) {
                    float x = *(float*)(lVar5 + 0x320);
                    float z = *(float*)(lVar5 + 0x328);
                    if (x > -100000 && x < 100000 && z > -100000 && z < 100000) {
                        cmd.x = x;
                        cmd.z = z;
                        shouldSend = true;
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    g_origBuildHandler(param_1, param_2);

    if (shouldSend) {
        ULONGLONG now = GetTickCount64();
        if (now - s_lastBuildTime > 1000) {
            s_lastBuildTime = now;
            H->log("multiplayer  BUILD at %.1f %.1f", cmd.x, cmd.z);
            SendBuildToAll(&cmd);
        }
    }
}
static bool InstallBuildHook()
{
    g_hExe = GetModuleHandleA("SOVIET64.exe");
    if (!g_hExe) {
        H->log("multiplayer  hook: SOVIET64.exe not found");
        return false;
    }

    BYTE* target = (BYTE*)((DWORD_PTR)g_hExe + HOOK_TARGET_RVA);

    if (target[0] != 0x48 || target[1] != 0x8B || target[2] != 0xC4) {
        H->log("multiplayer  hook: mismatch %02X %02X %02X",
               target[0], target[1], target[2]);
        return false;
    }

    BYTE* trampoline = nullptr;
    DWORD_PTR base = (DWORD_PTR)target;
    for (DWORD_PTR delta = 0x1000000; delta < 0x70000000; delta += 0x1000000) {
        trampoline = (BYTE*)VirtualAlloc((void*)(base - delta), 64,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (trampoline) break;
        trampoline = (BYTE*)VirtualAlloc((void*)(base + delta), 64,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (trampoline) break;
    }
    if (!trampoline)
        trampoline = (BYTE*)VirtualAlloc(NULL, 64,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        H->log("multiplayer  hook: trampoline alloc failed");
        return false;
    }

    DWORD oldProt;
    if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        H->log("multiplayer  hook: VirtualProtect failed");
        return false;
    }

    memcpy(g_hookOrigBytes, target, 14);
    memcpy(trampoline, g_hookOrigBytes, 14);

    trampoline[14] = 0xFF;
    trampoline[15] = 0x25;
    *(DWORD*)(trampoline + 16) = 0;
    *(DWORD_PTR*)(trampoline + 20) = (DWORD_PTR)(target + 14);

    g_origBuildHandler = (BuildHandlerFn)trampoline;

    DWORD_PTR hookAddr = (DWORD_PTR)HookedBuildHandler;
    target[0]  = 0x48;
    target[1]  = 0xB8;
    *(DWORD_PTR*)(target + 2) = hookAddr;
    target[10] = 0xFF;
    target[11] = 0xE0;
    target[12] = 0x90;
    target[13] = 0x90;

    VirtualProtect(target, 14, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, 14);

    H->log("multiplayer  build hook installed at %p", target);
    return true;
}

static bool IsFileReady(const char* path)
{
    HANDLE hFile = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); return true; }
    return false;
}

static void WaitForSaveReady(const char* savePath)
{
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s/buildings.bin", savePath);
    int attempts = 0;
    while (!IsFileReady(pattern) && attempts < 30) { Sleep(500); attempts++; }
    Sleep(500);
}

static char* LoadFile(const char* path, DWORD* outSize)
{
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { *outSize = 0; return nullptr; }
    DWORD size = GetFileSize(hFile, NULL);
    char* buf = (char*)malloc(size);
    if (!buf) { CloseHandle(hFile); *outSize = 0; return nullptr; }
    DWORD bytesRead;
    ReadFile(hFile, buf, size, &bytesRead, NULL);
    CloseHandle(hFile);
    *outSize = size;
    return buf;
}

static void SendCompressedToPlayer(Player& p, const char* data, DWORD size)
{
    size_t cap = ZSTD_compressBound(size);
    char* dst = (char*)malloc(cap);
    if (!dst) return;
    size_t cSize = ZSTD_compress(dst, cap, data, size, 3);
    {
        PlayerLockGuard lock(p);
        SendAll(p.sock, (char*)&size, sizeof(DWORD));
        DWORD cs = (DWORD)cSize;
        SendAll(p.sock, (char*)&cs, sizeof(DWORD));
        SendAll(p.sock, dst, (int)cSize);
    }
    free(dst);
}

static void SendFileFullToPlayer(Player& p, const char* path)
{
    DWORD size;
    char* data = LoadFile(path, &size);
    if (!data) {
        PlayerLockGuard lock(p);
        DWORD z = 0;
        SendAll(p.sock, (char*)&z, sizeof(DWORD));
        SendAll(p.sock, (char*)&z, sizeof(DWORD));
        return;
    }
    SendCompressedToPlayer(p, data, size);
    free(data);
}

struct DiffStream { char* buf; size_t size; size_t cap; };

static int DiffWrite(struct bsdiff_stream* stream, const void* buffer, int size)
{
    DiffStream* ds = (DiffStream*)stream->opaque;
    if (ds->size + size > ds->cap) {
        size_t newCap = (ds->size + size) * 2;
        char* newBuf = (char*)realloc(ds->buf, newCap);
        if (!newBuf) return -1;
        ds->buf = newBuf;
        ds->cap = newCap;
    }
    memcpy(ds->buf + ds->size, buffer, size);
    ds->size += size;
    return 0;
}

static void SendFileDiffToPlayer(Player& p, const char* newPath, const char* oldPath)
{
    DWORD newSize, oldSize;
    char* newData = LoadFile(newPath, &newSize);
    char* oldData = LoadFile(oldPath, &oldSize);

    if (!newData || !oldData || oldSize == 0) {
        if (newData) SendCompressedToPlayer(p, newData, newSize);
        else {
            PlayerLockGuard lock(p);
            DWORD z = 0;
            SendAll(p.sock, (char*)&z, sizeof(DWORD));
            SendAll(p.sock, (char*)&z, sizeof(DWORD));
        }
        free(newData); free(oldData);
        return;
    }

    DiffStream ds = {};
    ds.cap = 4096;
    ds.buf = (char*)malloc(ds.cap);
    if (!ds.buf) { free(newData); free(oldData); return; }

    struct bsdiff_stream stream = {};
    stream.opaque = &ds;
    stream.malloc = malloc;
    stream.free   = ::free;
    stream.write  = DiffWrite;

    if (bsdiff((uint8_t*)oldData, oldSize, (uint8_t*)newData, newSize, &stream) == 0) {
        size_t cap = ZSTD_compressBound(ds.size);
        char* comp = (char*)malloc(cap);
        if (comp) {
            size_t cSize = ZSTD_compress(comp, cap, ds.buf, ds.size, 3);
            PlayerLockGuard lock(p);
            DWORD raw = (DWORD)ds.size, cmp = (DWORD)cSize;
            SendAll(p.sock, (char*)&newSize, sizeof(DWORD));
            SendAll(p.sock, (char*)&raw, sizeof(DWORD));
            SendAll(p.sock, (char*)&cmp, sizeof(DWORD));
            SendAll(p.sock, comp, (int)cSize);
            free(comp);
        }
    }

    free(ds.buf); free(newData); free(oldData);
}

static void GetLatestSave(char* outName)
{
    outName[0] = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s/*", g_saveDir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    FILETIME latest = {0, 0};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (strcmp(fd.cFileName, "mp_client") == 0) continue;
        if (strncmp(fd.cFileName, "autosave", 8) == 0) continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &latest) > 0) {
            latest = fd.ftLastWriteTime;
            strcpy(outName, fd.cFileName);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    H->log("multiplayer  latest save: %s", outName[0] ? outName : "(none)");
}

static void SyncSaveToPlayer(int idx)
{
    Player& p = g_players[idx];
    if (!p.connected || p.sock == INVALID_SOCKET) return;

    char latestSave[MAX_PATH];
    GetLatestSave(latestSave);
    if (!latestSave[0]) { H->log("multiplayer  no save found"); return; }

    char newSavePath[MAX_PATH];
    snprintf(newSavePath, MAX_PATH, "%s/%s", g_saveDir, latestSave);
    WaitForSaveReady(newSavePath);

    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s/*", newSavePath);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    int fileCount = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) fileCount++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    bool useDiff = p.hasFullSave && p.prevSaveDir[0];
    BYTE msgType = useDiff ? MSG_SAVE_DIFF : MSG_SAVE_FULL;
    SendMsgToPlayer(p, msgType, &fileCount, sizeof(int));

    hFind = FindFirstFileA(pattern, &fd);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int nameLen = (int)strlen(fd.cFileName) + 1;
        {
            PlayerLockGuard lock(p);
            SendAll(p.sock, (char*)&nameLen, sizeof(int));
            SendAll(p.sock, fd.cFileName, nameLen);
        }
        char newFile[MAX_PATH];
        snprintf(newFile, MAX_PATH, "%s/%s", newSavePath, fd.cFileName);
        if (useDiff) {
            char oldFile[MAX_PATH];
            snprintf(oldFile, MAX_PATH, "%s/%s/%s", g_saveDir, p.prevSaveDir, fd.cFileName);
            DWORD oldFileSize = 0;
            HANDLE hCheck = CreateFileA(oldFile, GENERIC_READ, FILE_SHARE_READ,
                                        NULL, OPEN_EXISTING, 0, NULL);
            if (hCheck != INVALID_HANDLE_VALUE) {
                oldFileSize = GetFileSize(hCheck, NULL);
                CloseHandle(hCheck);
            }
            if (oldFileSize > DIFF_SIZE_LIMIT) SendFileFullToPlayer(p, newFile);
            else SendFileDiffToPlayer(p, newFile, oldFile);
        } else {
            SendFileFullToPlayer(p, newFile);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    p.hasFullSave = true;
    strncpy(p.prevSaveDir, latestSave, MAX_PATH - 1);
    H->log("multiplayer  save sent (%s) to %s",
           useDiff ? "diff+full" : "full", p.name);
}

static void SyncSaveToAll()
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g_players[i].connected) SyncSaveToPlayer(i);
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI PingThread(LPVOID arg)
{
    int idx = (int)(intptr_t)arg;
    while (true) {
        Sleep(5000);
        EnterCriticalSection(&g_lock);
        if (!g_players[idx].connected) { LeaveCriticalSection(&g_lock); break; }
        g_players[idx].pingStart = GetTickCount64();
        SendMsgToPlayer(g_players[idx], MSG_PING, nullptr, 0);
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static DWORD WINAPI ClientThread(LPVOID arg)
{
    int idx = (int)(intptr_t)arg;
    Player& p = g_players[idx];
    H->log("multiplayer  player joined: %s", p.name);

    BYTE type; DWORD size; char buf[256];
    while (RecvMsg(p.sock, &type, &size, buf, sizeof(buf))) {
        if (type == MSG_PING) {
            if (size > 0) DrainSocket(p.sock, size);
            SendMsgToPlayer(p, MSG_PONG, nullptr, 0);
        } else if (type == MSG_PONG) {
            if (size > 0) DrainSocket(p.sock, size);
            p.ping = (DWORD)(GetTickCount64() - p.pingStart);
            H->log("multiplayer  ping %s: %dms", p.name, p.ping);
        } else if (type == MSG_CHAT) {
            buf[size < sizeof(buf) ? size : sizeof(buf) - 1] = 0;
            H->log("multiplayer  [%s]: %s", p.name, buf);
        }
    }

    EnterCriticalSection(&g_lock);
    p.connected = false; p.hasFullSave = false; p.prevSaveDir[0] = 0;
    closesocket(p.sock); p.sock = INVALID_SOCKET;
    g_playerCount--;
    H->log("multiplayer  player left: %s", p.name);
    LeaveCriticalSection(&g_lock);
    return 0;
}

static DWORD WINAPI ServerThread(LPVOID)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        H->log("multiplayer  ERROR: cannot create socket"); return 1;
    }
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        H->log("multiplayer  ERROR: bind failed");
        closesocket(listenSock); return 1;
    }
    listen(listenSock, MAX_PLAYERS);
    H->log("multiplayer  server listening on port %d", g_port);

    while (true) {
        SOCKET client = accept(listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        EnterCriticalSection(&g_lock);
        if (g_playerCount >= MAX_PLAYERS) {
            LeaveCriticalSection(&g_lock);
            closesocket(client); continue;
        }

        int nameLen = 0;
        RecvAll(client, (char*)&nameLen, sizeof(int));
        char name[64] = "Unknown";
        if (nameLen > 0) {
            int readLen = nameLen < 63 ? nameLen : 63;
            RecvAll(client, name, readLen);
            name[readLen] = '\0';
            if (nameLen > readLen) DrainSocket(client, nameLen - readLen);
        }

        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (!g_players[i].connected) { slot = i; break; }

        if (slot >= 0) {
            g_players[slot].sock = client;
            g_players[slot].ping = 0;
            g_players[slot].pingStart = 0;
            g_players[slot].connected = true;
            g_players[slot].hasFullSave = false;
            g_players[slot].prevSaveDir[0] = 0;
            strncpy(g_players[slot].name, name, 63);
            g_playerCount++;
            H->log("multiplayer  connected: %s (%d/%d)",
                   name, g_playerCount, MAX_PLAYERS);
            SyncSaveToPlayer(slot);
            CreateThread(NULL, 0, ClientThread, (LPVOID)(intptr_t)slot, 0, NULL);
            CreateThread(NULL, 0, PingThread,   (LPVOID)(intptr_t)slot, 0, NULL);
        } else {
            closesocket(client);
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static DWORD WINAPI WatchThread(LPVOID)
{
    HANDLE hWatch = FindFirstChangeNotificationA(
        g_saveDir, TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (hWatch == INVALID_HANDLE_VALUE) {
        H->log("multiplayer  ERROR: cannot watch save folder"); return 1;
    }
    H->log("multiplayer  watching save folder...");
    while (true) {
        if (WaitForSingleObject(hWatch, INFINITE) == WAIT_OBJECT_0) {
            Sleep(1000);
            SyncSaveToAll();
            FindNextChangeNotification(hWatch);
        }
    }
    return 0;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(
    const TsmHost* host, TsmPluginInfo* info)
{
    H = host;
    info->name    = "multiplayer";
    info->version = "0.3.2";
    if (!H->configInt("plugins\\multiplayer.ini", "multiplayer", "enabled", 1))
        return 1;
    H->configString("plugins\\multiplayer.ini", "multiplayer", "mode",
                    g_mode, sizeof(g_mode), "host");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "host_ip",
                    g_hostIp, sizeof(g_hostIp), "127.0.0.1");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "name",
                    g_playerName, sizeof(g_playerName), "Player");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "save_dir",
                    g_saveDir, sizeof(g_saveDir), "media_soviet/save");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "sync_dir",
                    g_syncDir, sizeof(g_syncDir), "mp_sync");
    g_port = H->configInt("plugins\\multiplayer.ini", "multiplayer", "port", 7777);
    memset(g_players, 0, sizeof(g_players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_players[i].sock = INVALID_SOCKET;
        InitializeCriticalSection(&g_players[i].netLock);
    }
    H->log("multiplayer  v0.3.2 loaded OK (mode: %s, name: %s)",
           g_mode, g_playerName);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    H->log("multiplayer  start phase OK");
    CreateDirectoryA(g_syncDir, NULL);
    InitializeCriticalSection(&g_lock);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_serverThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    g_watchThread  = CreateThread(NULL, 0, WatchThread,  NULL, 0, NULL);

    if (InstallBuildHook())
        H->log("multiplayer  v0.3.2 realtime hook ACTIVE");
    else
        H->log("multiplayer  v0.3.2 realtime hook FAILED - save-sync only");

    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }