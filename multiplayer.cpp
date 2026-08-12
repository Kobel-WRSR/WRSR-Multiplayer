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
#include "zstd.h"

extern "C" {
#include "bsdiff.h"
#include "bspatch.h"
}

#pragma comment(lib, "ws2_32.lib")

#define MAX_PLAYERS 4
#define MSG_SAVE      1
#define MSG_PING      2
#define MSG_PONG      3
#define MSG_CHAT      4
#define MSG_SAVE_FULL 5
#define MSG_SAVE_DIFF 6

static const TsmHost* H = nullptr;
static char g_saveDir[MAX_PATH];
static char g_syncDir[MAX_PATH];
static char g_playerName[64];
static char g_mode[32];
static char g_hostIp[64];
static int  g_port = 7777;

struct Player {
    SOCKET sock;
    char name[64];
    DWORD ping;
    bool connected;
    bool hasFullSave;
};

static Player g_players[MAX_PLAYERS];
static int g_playerCount = 0;
static HANDLE g_watchThread  = NULL;
static HANDLE g_serverThread = NULL;
static CRITICAL_SECTION g_lock;

static char g_prevSaveDir[MAX_PATH];

struct MsgHeader {
    BYTE type;
    DWORD size;
};

static void SendMsg(SOCKET sock, BYTE type, const void* data, DWORD size)
{
    MsgHeader hdr;
    hdr.type = type;
    hdr.size = size;
    send(sock, (char*)&hdr, sizeof(hdr), 0);
    if (data && size > 0)
        send(sock, (char*)data, size, 0);
}

static bool RecvMsg(SOCKET sock, MsgHeader* hdr, char* buf, DWORD bufSize)
{
    int got = recv(sock, (char*)hdr, sizeof(MsgHeader), MSG_WAITALL);
    if (got <= 0) return false;
    if (hdr->size > 0 && hdr->size < bufSize) {
        got = recv(sock, buf, hdr->size, MSG_WAITALL);
        if (got <= 0) return false;
    }
    return true;
}

static char* ReadFile(const char* path, DWORD* outSize)
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

static void SendCompressed(SOCKET sock, const char* data, DWORD size)
{
    size_t cap = ZSTD_compressBound(size);
    char* dst = (char*)malloc(cap);
    size_t cSize = ZSTD_compress(dst, cap, data, size, 3);
    send(sock, (char*)&size, sizeof(DWORD), 0);
    DWORD cs = (DWORD)cSize;
    send(sock, (char*)&cs, sizeof(DWORD), 0);
    send(sock, dst, cs, 0);
    free(dst);
}

struct DiffStream {
    char* buf;
    size_t size;
    size_t cap;
};

static int DiffWrite(struct bsdiff_stream* stream, const void* buffer, int size)
{
    DiffStream* ds = (DiffStream*)stream->opaque;
    if (ds->size + size > ds->cap) {
        ds->cap = (ds->size + size) * 2;
        ds->buf = (char*)realloc(ds->buf, ds->cap);
    }
    memcpy(ds->buf + ds->size, buffer, size);
    ds->size += size;
    return 0;
}

static void SendFileFull(SOCKET sock, const char* path)
{
    DWORD size;
    char* data = ReadFile(path, &size);
    if (!data) {
        DWORD z = 0;
        send(sock, (char*)&z, sizeof(DWORD), 0);
        send(sock, (char*)&z, sizeof(DWORD), 0);
        return;
    }
    SendCompressed(sock, data, size);
    free(data);
}

static void SendFileDiff(SOCKET sock, const char* newPath, const char* oldPath)
{
    DWORD newSize, oldSize;
    char* newData = ReadFile(newPath, &newSize);
    char* oldData = ReadFile(oldPath, &oldSize);

    if (!newData || !oldData || oldSize == 0) {
        if (newData) SendCompressed(sock, newData, newSize);
        else { DWORD z = 0; send(sock, (char*)&z, sizeof(DWORD), 0); send(sock, (char*)&z, sizeof(DWORD), 0); }
        free(newData); free(oldData);
        return;
    }

    DiffStream ds = {};
    ds.cap = 4096;
    ds.buf = (char*)malloc(ds.cap);

    struct bsdiff_stream stream = {};
    stream.opaque = &ds;
    stream.malloc = malloc;
    stream.free   = free;
    stream.write  = DiffWrite;

    send(sock, (char*)&newSize, sizeof(DWORD), 0);

    if (bsdiff((uint8_t*)oldData, oldSize, (uint8_t*)newData, newSize, &stream) == 0) {
        size_t cap = ZSTD_compressBound(ds.size);
        char* comp = (char*)malloc(cap);
        size_t cSize = ZSTD_compress(comp, cap, ds.buf, ds.size, 3);
        DWORD rawDiffSize = (DWORD)ds.size;
        DWORD compDiffSize = (DWORD)cSize;
        send(sock, (char*)&rawDiffSize, sizeof(DWORD), 0);
        send(sock, (char*)&compDiffSize, sizeof(DWORD), 0);
        send(sock, comp, compDiffSize, 0);
        free(comp);
    }

    free(ds.buf);
    free(newData);
    free(oldData);
}

static void GetLatestSave(char* outName)
{
    outName[0] = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", g_saveDir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    FILETIME latest = {0, 0};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0) continue;
        if (strcmp(fd.cFileName, "..") == 0) continue;
        if (strcmp(fd.cFileName, "mp_client") == 0) continue;
        if (strncmp(fd.cFileName, "autosave", 8) == 0) continue;
        H->log("multiplayer  found folder: %s", fd.cFileName);
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
    if (!latestSave[0]) {
        H->log("multiplayer  no save found");
        return;
    }

    char newSavePath[MAX_PATH];
    snprintf(newSavePath, MAX_PATH, "%s\\%s", g_saveDir, latestSave);

    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", newSavePath);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    int fileCount = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            fileCount++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    bool useDiff = p.hasFullSave && g_prevSaveDir[0];
    BYTE msgType = useDiff ? MSG_SAVE_DIFF : MSG_SAVE_FULL;
    SendMsg(p.sock, msgType, &fileCount, sizeof(int));

    hFind = FindFirstFileA(pattern, &fd);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int nameLen = (int)strlen(fd.cFileName) + 1;
        send(p.sock, (char*)&nameLen, sizeof(int), 0);
        send(p.sock, fd.cFileName, nameLen, 0);

        char newFile[MAX_PATH];
        snprintf(newFile, MAX_PATH, "%s\\%s", newSavePath, fd.cFileName);

        if (useDiff) {
            char oldFile[MAX_PATH];
            snprintf(oldFile, MAX_PATH, "%s\\%s\\%s", g_saveDir, g_prevSaveDir, fd.cFileName);
            SendFileDiff(p.sock, newFile, oldFile);
        } else {
            SendFileFull(p.sock, newFile);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    p.hasFullSave = true;
    strncpy(g_prevSaveDir, latestSave, MAX_PATH - 1);

    H->log("multiplayer  save sent (%s) to %s",
           useDiff ? "diff" : "full", p.name);
}

static void SyncSaveToAll()
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].connected)
            SyncSaveToPlayer(i);
    }
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI PingThread(LPVOID arg)
{
    int idx = (int)(intptr_t)arg;
    while (true) {
        Sleep(5000);
        EnterCriticalSection(&g_lock);
        if (!g_players[idx].connected) {
            LeaveCriticalSection(&g_lock);
            break;
        }
        DWORD t1 = GetTickCount();
        SendMsg(g_players[idx].sock, MSG_PING, nullptr, 0);
        LeaveCriticalSection(&g_lock);
        MsgHeader hdr;
        char buf[256];
        if (RecvMsg(g_players[idx].sock, &hdr, buf, sizeof(buf))) {
            if (hdr.type == MSG_PONG) {
                g_players[idx].ping = GetTickCount() - t1;
                H->log("multiplayer  ping %s: %dms",
                       g_players[idx].name, g_players[idx].ping);
            }
        }
    }
    return 0;
}

static DWORD WINAPI ClientThread(LPVOID arg)
{
    int idx = (int)(intptr_t)arg;
    Player& p = g_players[idx];
    H->log("multiplayer  player joined: %s", p.name);
    MsgHeader hdr;
    char buf[256];
    while (RecvMsg(p.sock, &hdr, buf, sizeof(buf))) {
        if (hdr.type == MSG_PING)
            SendMsg(p.sock, MSG_PONG, nullptr, 0);
        else if (hdr.type == MSG_CHAT) {
            buf[hdr.size] = 0;
            H->log("multiplayer  [%s]: %s", p.name, buf);
        }
    }
    EnterCriticalSection(&g_lock);
    p.connected = false;
    p.hasFullSave = false;
    closesocket(p.sock);
    p.sock = INVALID_SOCKET;
    g_playerCount--;
    H->log("multiplayer  player left: %s", p.name);
    LeaveCriticalSection(&g_lock);
    return 0;
}

static DWORD WINAPI ServerThread(LPVOID)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        H->log("multiplayer  ERROR: cannot create socket");
        return 1;
    }
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        H->log("multiplayer  ERROR: bind failed");
        closesocket(listenSock);
        return 1;
    }
    listen(listenSock, MAX_PLAYERS);
    H->log("multiplayer  server listening on port %d", g_port);
    while (true) {
        SOCKET client = accept(listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        EnterCriticalSection(&g_lock);
        if (g_playerCount >= MAX_PLAYERS) {
            LeaveCriticalSection(&g_lock);
            closesocket(client);
            continue;
        }
        int nameLen = 0;
        recv(client, (char*)&nameLen, sizeof(int), MSG_WAITALL);
        char name[64] = "Unknown";
        if (nameLen > 0 && nameLen < 64)
            recv(client, name, nameLen, MSG_WAITALL);
        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!g_players[i].connected) { slot = i; break; }
        }
        if (slot >= 0) {
            g_players[slot].sock = client;
            g_players[slot].ping = 0;
            g_players[slot].connected = true;
            g_players[slot].hasFullSave = false;
            strncpy(g_players[slot].name, name, 63);
            g_playerCount++;
            H->log("multiplayer  client connected: %s (%d/%d)",
                   name, g_playerCount, MAX_PLAYERS);
            CreateThread(NULL, 0, ClientThread, (LPVOID)(intptr_t)slot, 0, NULL);
            CreateThread(NULL, 0, PingThread,   (LPVOID)(intptr_t)slot, 0, NULL);
            SyncSaveToPlayer(slot);
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
        H->log("multiplayer  ERROR: cannot watch save folder");
        return 1;
    }
    H->log("multiplayer  watching save folder...");
    while (true) {
        DWORD result = WaitForSingleObject(hWatch, INFINITE);
        if (result == WAIT_OBJECT_0) {
            Sleep(2000);
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
    info->version = "0.2.0";
    if (!H->configInt("plugins\\multiplayer.ini", "multiplayer", "enabled", 1))
        return 1;
    H->configString("plugins\\multiplayer.ini", "multiplayer", "mode",
                    g_mode, sizeof(g_mode), "host");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "host_ip",
                    g_hostIp, sizeof(g_hostIp), "127.0.0.1");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "name",
                    g_playerName, sizeof(g_playerName), "Player");
    g_port = H->configInt("plugins\\multiplayer.ini", "multiplayer", "port", 7777);
    memset(g_players, 0, sizeof(g_players));
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_players[i].sock = INVALID_SOCKET;
    g_prevSaveDir[0] = 0;
    H->log("multiplayer  v0.2.0 loaded OK (mode: %s, name: %s)",
           g_mode, g_playerName);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    H->log("multiplayer  start phase OK");
    snprintf(g_saveDir, MAX_PATH,
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save");
    snprintf(g_syncDir, MAX_PATH,
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\mp_sync");
    CreateDirectoryA(g_syncDir, NULL);
    InitializeCriticalSection(&g_lock);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_serverThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    g_watchThread  = CreateThread(NULL, 0, WatchThread,  NULL, 0, NULL);
    H->log("multiplayer  HOST mode started on port %d (zstd+bsdiff)", g_port);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }