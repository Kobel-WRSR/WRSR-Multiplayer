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

#include "mp_shared.h"
#pragma comment(lib, "ws2_32.lib")

#define MP_VERSION_MAJOR  0
#define MP_VERSION_MINOR  4
#define MP_VERSION_PATCH  1
#define MP_PROTOCOL_VER   3

#define MSG_SAVE          1
#define MSG_PING          2
#define MSG_PONG          3
#define MSG_CHAT          4
#define MSG_SAVE_FULL     5
#define MSG_SAVE_DIFF     6
#define MSG_BUILD         7
#define MSG_RESOURCE_REQ  8
#define MSG_RESOURCE_RESP 9
#define MSG_PLAYER_LIST   10
#define MSG_DEMOLISH      11
#define MSG_ROAD          12
#define MSG_HANDSHAKE     13
#define MSG_KICK          14
#define MSG_HEARTBEAT     15

#define HOOK_BUILD_RVA    0x6F8CC0
#define HOOK_PLACE_RVA    0x4461F0
#define HOOK_DEMOL_RVA    0x6F8E00

#define MAX_PLAYERS           4
#define MAX_CHAT_HISTORY     200
#define MAX_BUILD_HISTORY    500
#define DIFF_SIZE_LIMIT      (10 * 1024 * 1024)
#define PING_INTERVAL_MS     5000
#define HEARTBEAT_INTERVAL_MS 10000
#define KICK_TIMEOUT_MS      30000
#define MAX_BUILD_RATE        10
#define SEND_LOCK_TIMEOUT    2000

#pragma pack(push, 1)

struct MsgHeader {
    BYTE  type;
    DWORD size;
    WORD  checksum;
};

struct HandshakePacket {
    BYTE  protocolVersion;
    BYTE  versionMajor;
    BYTE  versionMinor;
    BYTE  versionPatch;
    char  playerName[64];
    char  gameMode[16];
};

struct BuildCmd {
    float x, z;
    float rotation;
    char  typeName[128];
    char  playerName[64];
    DWORD timestamp;
    DWORD sequenceId;
};

struct DemolishCmd {
    float x, z;
    char  playerName[64];
    DWORD timestamp;
};

struct RoadCmd {
    float x1, z1;
    float x2, z2;
    char  roadType[64];
    char  playerName[64];
    DWORD timestamp;
};

struct PlayerListEntry {
    char  name[64];
    DWORD ping;
    BYTE  connected;
    BYTE  isHost;
};

struct PlayerListPacket {
    BYTE            count;
    PlayerListEntry entries[MAX_PLAYERS];
};

struct ResourceReq {
    char  fromPlayer[64];
    char  toPlayer[64];
    char  resource[64];
    int   amount;
    int   price;
    DWORD requestId;
};

struct KickPacket {
    char reason[128];
};

#pragma pack(pop)

struct HookInfo {
    const char* name;
    DWORD_PTR   rva;
    void*       hookFn;
    void**      origFn;
    BYTE*       origBytes;
    int         hookSize;
    bool        installed;
};

typedef void (*BuildHandlerFn)(__int64, char*);
typedef void (*PlaceHandlerFn)(void*, float, float, int);
typedef void (*DemolHandlerFn)(__int64, char*);

static BuildHandlerFn g_origBuildHandler = nullptr;
static PlaceHandlerFn g_origPlaceHandler = nullptr;
static DemolHandlerFn g_origDemolHandler = nullptr;

static BYTE g_buildOrigBytes[16] = {0};
static BYTE g_placeOrigBytes[16] = {0};
static BYTE g_demolOrigBytes[16] = {0};

static HMODULE g_hExe = nullptr;

struct Player {
    SOCKET            sock;
    char              name[64];
    DWORD             ping;
    ULONGLONG         pingStart;
    ULONGLONG         lastActivity;
    bool              connected;
    bool              hasFullSave;
    bool              handshakeDone;
    char              prevSaveDir[MAX_PATH];
    CRITICAL_SECTION  netLock;
    DWORD             buildCount;
    DWORD             chatCount;
    ULONGLONG         lastBuildTime;
    int               buildRateCounter;
    ULONGLONG         buildRateWindow;
};

struct BuildDedupEntry {
    char     typeName[128];
    float    x, z;
    ULONGLONG timestamp;
};

#define DEDUP_HISTORY 32
static BuildDedupEntry g_dedupHistory[DEDUP_HISTORY] = {};
static int g_dedupIdx = 0;

static bool IsDuplicate(const char* type, float x, float z)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < DEDUP_HISTORY; i++) {
        if (!g_dedupHistory[i].typeName[0]) continue;
        if (now - g_dedupHistory[i].timestamp > 1000) continue;
        if (strcmp(g_dedupHistory[i].typeName, type) != 0) continue;
        if (fabsf(g_dedupHistory[i].x - x) < 0.5f &&
            fabsf(g_dedupHistory[i].z - z) < 0.5f)
            return true;
    }
    return false;
}

static void RecordBuild(const char* type, float x, float z)
{
    strncpy(g_dedupHistory[g_dedupIdx].typeName, type, 127);
    g_dedupHistory[g_dedupIdx].x = x;
    g_dedupHistory[g_dedupIdx].z = z;
    g_dedupHistory[g_dedupIdx].timestamp = GetTickCount64();
    g_dedupIdx = (g_dedupIdx + 1) % DEDUP_HISTORY;
}

static const TsmHost* H = nullptr;
static char g_saveDir[MAX_PATH];
static char g_syncDir[MAX_PATH];
static char g_playerName[64];
static char g_mode[32];
static char g_hostIp[64];
static int  g_port = 7777;
static bool g_enableDemolishSync = true;
static bool g_enableRoadSync = false;
static bool g_enableAntiSpam = true;
static int  g_maxBuildRate = MAX_BUILD_RATE;

static Player         g_players[MAX_PLAYERS];
static int            g_playerCount = 0;
static HANDLE         g_watchThread  = NULL;
static HANDLE         g_serverThread = NULL;
static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_typeNameLock;

static char           g_lastTypeName[128] = {0};
static volatile bool  g_inBuildMode = false;
static DWORD          g_sequenceId = 0;

static SharedMemory   g_shm;
static HANDLE         g_shmThread = NULL;

static SOCKET         g_clientSock = INVALID_SOCKET;
static HANDLE         g_clientRecvThread = NULL;
static bool           g_clientConnected = false;
static ULONGLONG      g_clientLastPing = 0;

static ULONGLONG      g_lastHeartbeat = 0;
static ULONGLONG      g_sessionStart  = 0;
static DWORD          g_totalBuildsThisSession = 0;
static DWORD          g_totalMsgSent  = 0;
static DWORD          g_totalMsgRecv  = 0;
static DWORD          g_totalBytesSent = 0;

static WORD CalcChecksum(const void* data, DWORD size)
{
    const BYTE* p = (const BYTE*)data;
    WORD sum = 0;
    for (DWORD i = 0; i < size; i++)
        sum = (WORD)((sum * 31 + p[i]) & 0xFFFF);
    return sum;
}

struct PlayerLockGuard {
    Player& p;
    PlayerLockGuard(Player& p) : p(p) { EnterCriticalSection(&p.netLock); }
    ~PlayerLockGuard() { LeaveCriticalSection(&p.netLock); }
};

static bool SendAll(SOCKET sock, const char* buf, int size)
{
    int sent = 0;
    while (sent < size) {
        int r = send(sock, buf + sent, size - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    g_totalBytesSent += size;
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
    hdr.checksum = data ? CalcChecksum(data, size) : 0;

    PlayerLockGuard lock(p);
    if (!SendAll(p.sock, (char*)&hdr, sizeof(hdr))) return;
    if (data && size > 0)
        SendAll(p.sock, (char*)data, size);
    g_totalMsgSent++;
}

static bool RecvMsg(SOCKET sock, BYTE* type, DWORD* size, char* buf, DWORD bufSize)
{
    MsgHeader hdr;
    if (!RecvAll(sock, (char*)&hdr, sizeof(hdr))) return false;
    *type = hdr.type;
    *size = hdr.size;
    if (hdr.size > 0 && hdr.size < bufSize) {
        if (!RecvAll(sock, buf, hdr.size)) return false;

        WORD expected = CalcChecksum(buf, hdr.size);
        if (hdr.checksum != 0 && expected != hdr.checksum) {
            H->log("multiplayer  checksum mismatch type=%d", hdr.type);
        }
    }
    g_totalMsgRecv++;
    return true;
}

static void BroadcastToAll(BYTE type, const void* data, DWORD size,
                           int excludeSlot = -1)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].connected) continue;
        if (g_players[i].sock == INVALID_SOCKET) continue;
        if (i == excludeSlot) continue;
        if (!g_players[i].handshakeDone) continue;
        SendMsgToPlayer(g_players[i], type, data, size);
    }
    LeaveCriticalSection(&g_lock);
}

static void ClientSendBuild(const BuildCmd* cmd);

static void BroadcastBuild(const BuildCmd* cmd)
{
    ShmAddBuildNotify(cmd);
    if (strcmp(g_mode, "client") == 0) {
        ClientSendBuild(cmd);
    } else {
        BroadcastToAll(MSG_BUILD, cmd, sizeof(BuildCmd));
    }
}

static void BroadcastDemolish(const DemolishCmd* cmd)
{
    if (!g_enableDemolishSync) return;
    BroadcastToAll(MSG_DEMOLISH, cmd, sizeof(DemolishCmd));
}

static void BroadcastChat(const char* msg)
{
    BroadcastToAll(MSG_CHAT, msg, (DWORD)strlen(msg));
}

static void BroadcastPlayerList()
{
    PlayerListPacket pkt = {};
    EnterCriticalSection(&g_lock);
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS && n < MAX_PLAYERS; i++) {
        if (!g_players[i].connected) continue;
        strncpy(pkt.entries[n].name, g_players[i].name, 63);
        pkt.entries[n].ping = g_players[i].ping;
        pkt.entries[n].connected = 1;
        pkt.entries[n].isHost = 0;
        n++;
    }
    pkt.count = (BYTE)n;
    LeaveCriticalSection(&g_lock);
    BroadcastToAll(MSG_PLAYER_LIST, &pkt, sizeof(pkt));
}

static bool CheckBuildRate(Player& p)
{
    if (!g_enableAntiSpam) return true;
    ULONGLONG now = GetTickCount64();
    if (now - p.buildRateWindow > 1000) {
        p.buildRateWindow = now;
        p.buildRateCounter = 0;
    }
    if (p.buildRateCounter >= g_maxBuildRate) {
        H->log("multiplayer  rate limit: %s (builds/sec)", p.name);
        return false;
    }
    p.buildRateCounter++;
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
    Sleep(300);
}

static char* LoadFile(const char* path, DWORD* outSize)
{
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
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
}

static int CountSaveFiles(const char* savePath)
{
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s/*", savePath);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return count;
}

static void SyncSaveToPlayer(int idx)
{
    Player& p = g_players[idx];
    if (!p.connected || p.sock == INVALID_SOCKET || !p.handshakeDone) return;

    char latestSave[MAX_PATH];
    GetLatestSave(latestSave);
    if (!latestSave[0]) {
        H->log("multiplayer  no save found for sync");
        return;
    }

    char newSavePath[MAX_PATH];
    snprintf(newSavePath, MAX_PATH, "%s/%s", g_saveDir, latestSave);
    WaitForSaveReady(newSavePath);

    int fileCount = CountSaveFiles(newSavePath);
    if (fileCount == 0) return;

    bool useDiff = p.hasFullSave && p.prevSaveDir[0];
    BYTE msgType = useDiff ? MSG_SAVE_DIFF : MSG_SAVE_FULL;
    SendMsgToPlayer(p, msgType, &fileCount, sizeof(int));

    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s/*", newSavePath);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    DWORD bytesBefore = g_totalBytesSent;
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
            HANDLE hCheck = CreateFileA(oldFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
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

    DWORD bytesTransferred = g_totalBytesSent - bytesBefore;
    p.hasFullSave = true;
    strncpy(p.prevSaveDir, latestSave, MAX_PATH - 1);
    H->log("multiplayer  save -> %s (%s, %u KB)",
           p.name, useDiff ? "diff" : "full",
           bytesTransferred / 1024);
}

static void SyncSaveToAll()
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g_players[i].connected && g_players[i].handshakeDone)
            SyncSaveToPlayer(i);
    LeaveCriticalSection(&g_lock);
}

static void HookedBuildHandler(__int64 param_1, char* param_2)
{
    __try {
        if (param_2) {
            __int64 lVar5 = *(__int64*)(param_2 + 0x240);
            if (lVar5 > 0x10000) {
                __try {
                    __int64 lVar6 = *(__int64*)(lVar5 + 0x318);
                    if (lVar6 > 0x10000) {
                        char* fullName = (char*)lVar6;
                        if (fullName[0] >= 'A' && fullName[0] <= 'z') {
                            EnterCriticalSection(&g_typeNameLock);
                            strncpy(g_lastTypeName, fullName, 127);
                            LeaveCriticalSection(&g_typeNameLock);
                        }
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_inBuildMode = true;
    g_origBuildHandler(param_1, param_2);
    g_inBuildMode = false;
}

static void HookedPlaceHandler(void* param_1, float x, float z, int param_4)
{
    g_origPlaceHandler(param_1, x, z, param_4);

    if (!g_inBuildMode) return;

    static char  s_lastType[128] = {0};
    static bool  s_sent = false;

    EnterCriticalSection(&g_typeNameLock);
    char typeName[128] = {0};
    strncpy(typeName, g_lastTypeName, 127);
    LeaveCriticalSection(&g_typeNameLock);

    if (!typeName[0]) return;

    bool typeChanged = (strcmp(typeName, s_lastType) != 0);
    if (typeChanged) {
        s_sent = false;
        strncpy(s_lastType, typeName, 127);
    }

    if (s_sent) return;
    s_sent = true;

    if (IsDuplicate(typeName, x, z)) return;
    RecordBuild(typeName, x, z);

    BuildCmd cmd = {};
    cmd.x = x;
    cmd.z = z;
    cmd.rotation = 0.f;
    strncpy(cmd.typeName, typeName, sizeof(cmd.typeName) - 1);
    strncpy(cmd.playerName, g_playerName, sizeof(cmd.playerName) - 1);
    cmd.timestamp = GetTickCount();
    cmd.sequenceId = InterlockedIncrement(&g_sequenceId);

    H->log("multiplayer  PLACE %s at (%.0f, %.0f) seq=%u",
           typeName, x, z, cmd.sequenceId);

    g_totalBuildsThisSession++;
    BroadcastBuild(&cmd);
}

static void HookedDemolHandler(__int64 param_1, char* param_2)
{
    if (g_origDemolHandler)
        g_origDemolHandler(param_1, param_2);

    if (!g_enableDemolishSync) return;

    __try {
        if (param_2) {
            float x = 0, z = 0;
            __try {
                __int64 lVar5 = *(__int64*)(param_2 + 0x240);
                if (lVar5 > 0x10000) {

                    float dat = *(float*)((BYTE*)g_hExe + 0x992088);
                    x = *(float*)(param_2 + 0x28) + *(float*)(param_2 + 4) + dat * 135.0f;
                    z = *(float*)(param_2 + 0x2c) + *(float*)(param_2 + 8) + dat * 15.0f;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}

            if (x != 0 || z != 0) {
                DemolishCmd cmd = {};
                cmd.x = x; cmd.z = z;
                strncpy(cmd.playerName, g_playerName, 63);
                cmd.timestamp = GetTickCount();
                BroadcastDemolish(&cmd);
                H->log("multiplayer  DEMOLISH at (%.0f, %.0f)", x, z);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static bool InstallHook(DWORD_PTR rva, void* hookFn, void** origFn,
                        BYTE* origBytes, const char* name, int hookSize)
{
    BYTE* target = (BYTE*)((DWORD_PTR)g_hExe + rva);

    if (target[0] != 0x48 || target[1] != 0x8B || target[2] != 0xC4) {
        H->log("multiplayer  hook %s: mismatch %02X %02X %02X",
               name, target[0], target[1], target[2]);
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
        H->log("multiplayer  hook %s: trampoline failed", name);
        return false;
    }

    DWORD oldProt;
    if (!VirtualProtect(target, hookSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        H->log("multiplayer  hook %s: VirtualProtect failed", name);
        return false;
    }

    memcpy(origBytes, target, hookSize);
    memcpy(trampoline, origBytes, hookSize);
    trampoline[hookSize + 0] = 0xFF;
    trampoline[hookSize + 1] = 0x25;
    *(DWORD*)(trampoline + hookSize + 2) = 0;
    *(DWORD_PTR*)(trampoline + hookSize + 6) = (DWORD_PTR)(target + hookSize);

    *origFn = trampoline;

    target[0] = 0x48; target[1] = 0xB8;
    *(DWORD_PTR*)(target + 2) = (DWORD_PTR)hookFn;
    target[10] = 0xFF; target[11] = 0xE0;
    for (int i = 12; i < hookSize; i++) target[i] = 0x90;

    VirtualProtect(target, hookSize, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, hookSize);

    H->log("multiplayer  hook [%s] OK @ %p (%d bytes)", name, target, hookSize);
    return true;
}

static void RemoveHook(DWORD_PTR rva, BYTE* origBytes, int hookSize)
{
    BYTE* target = (BYTE*)((DWORD_PTR)g_hExe + rva);
    DWORD oldProt;
    if (VirtualProtect(target, hookSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(target, origBytes, hookSize);
        VirtualProtect(target, hookSize, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, hookSize);
    }
}

static DWORD WINAPI ClientThread(LPVOID arg)
{
    int idx = (int)(intptr_t)arg;
    Player& p = g_players[idx];

    {
        BYTE type; DWORD size; char buf[512];
        if (!RecvMsg(p.sock, &type, &size, buf, sizeof(buf)) ||
            type != MSG_HANDSHAKE || size != sizeof(HandshakePacket)) {
            H->log("multiplayer  %s: bad handshake", p.name);
            goto disconnect;
        }

        HandshakePacket* hs = (HandshakePacket*)buf;
        if (hs->protocolVersion != MP_PROTOCOL_VER) {
            H->log("multiplayer  %s: protocol mismatch (%d vs %d)",
                   p.name, hs->protocolVersion, MP_PROTOCOL_VER);
            KickPacket kick;
            snprintf(kick.reason, sizeof(kick.reason),
                     "Protocol version mismatch. Server: %d, Client: %d",
                     MP_PROTOCOL_VER, hs->protocolVersion);
            SendMsgToPlayer(p, MSG_KICK, &kick, sizeof(kick));
            goto disconnect;
        }

        strncpy(p.name, hs->playerName, 63);
        p.handshakeDone = true;
        H->log("multiplayer  %s handshake OK (v%d.%d.%d)",
               p.name, hs->versionMajor, hs->versionMinor, hs->versionPatch);
    }

    H->log("multiplayer  player joined: %s", p.name);
    SyncSaveToPlayer(idx);
    BroadcastPlayerList();

    {
        BYTE type; DWORD size; char buf[sizeof(BuildCmd) + 16];
        while (RecvMsg(p.sock, &type, &size, buf, sizeof(buf))) {
            p.lastActivity = GetTickCount64();

            switch (type) {
            case MSG_PING:
                if (size > 0) DrainSocket(p.sock, size);
                SendMsgToPlayer(p, MSG_PONG, nullptr, 0);
                break;

            case MSG_PONG:
                if (size > 0) DrainSocket(p.sock, size);
                p.ping = (DWORD)(GetTickCount64() - p.pingStart);
                break;

            case MSG_HEARTBEAT:
                if (size > 0) DrainSocket(p.sock, size);
                break;

            case MSG_CHAT:
                if (size > 0 && size < 512) {
                    buf[size] = 0;
                    H->log("multiplayer  [%s]: %s", p.name, buf);

                    BroadcastToAll(MSG_CHAT, buf, size, idx);
                    p.chatCount++;
                }
                break;

            case MSG_BUILD:
                if (size == sizeof(BuildCmd)) {
                    BuildCmd* cmd = (BuildCmd*)buf;
                    if (!CheckBuildRate(p)) break;

                    strncpy(cmd->playerName, p.name, 63);
                    p.buildCount++;
                    H->log("multiplayer  [BUILD relay] %s: %s at (%.0f,%.0f)",
                           p.name, cmd->typeName, cmd->x, cmd->z);

                    BroadcastToAll(MSG_BUILD, cmd, sizeof(BuildCmd), idx);
                }
                break;

            case MSG_DEMOLISH:
                if (size == sizeof(DemolishCmd)) {
                    DemolishCmd* cmd = (DemolishCmd*)buf;
                    strncpy(cmd->playerName, p.name, 63);
                    H->log("multiplayer  [DEMOLISH relay] %s at (%.0f,%.0f)",
                           p.name, cmd->x, cmd->z);
                    BroadcastToAll(MSG_DEMOLISH, cmd, sizeof(DemolishCmd), idx);
                }
                break;

            case MSG_RESOURCE_REQ:
                if (size == sizeof(ResourceReq)) {
                    ResourceReq* req = (ResourceReq*)buf;
                    strncpy(req->fromPlayer, p.name, 63);
                    H->log("multiplayer  [TRADE] %s requests %d x %s",
                           p.name, req->amount, req->resource);
                    BroadcastToAll(MSG_RESOURCE_REQ, req, sizeof(ResourceReq), idx);
                }
                break;

            default:
                if (size > 0) DrainSocket(p.sock, size);
                break;
            }
        }
    }

disconnect:
    EnterCriticalSection(&g_lock);
    H->log("multiplayer  player left: %s (builds=%u, chat=%u)",
           p.name, p.buildCount, p.chatCount);
    p.connected = false;
    p.handshakeDone = false;
    p.hasFullSave = false;
    p.prevSaveDir[0] = 0;
    closesocket(p.sock);
    p.sock = INVALID_SOCKET;
    g_playerCount--;

    char leaveMsg[128];
    snprintf(leaveMsg, sizeof(leaveMsg), "[SERVER] %s disconnected", p.name);
    LeaveCriticalSection(&g_lock);

    BroadcastChat(leaveMsg);
    BroadcastPlayerList();
    return 0;
}

static DWORD WINAPI PingThread(LPVOID arg)
{
    int idx = (int)(intptr_t)arg;
    while (true) {
        Sleep(PING_INTERVAL_MS);
        EnterCriticalSection(&g_lock);
        if (!g_players[idx].connected) { LeaveCriticalSection(&g_lock); break; }

        ULONGLONG now = GetTickCount64();
        if (g_players[idx].lastActivity > 0 &&
            now - g_players[idx].lastActivity > KICK_TIMEOUT_MS) {
            H->log("multiplayer  timeout: %s", g_players[idx].name);
            closesocket(g_players[idx].sock);
            LeaveCriticalSection(&g_lock);
            break;
        }

        g_players[idx].pingStart = GetTickCount64();
        SendMsgToPlayer(g_players[idx], MSG_PING, nullptr, 0);
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static DWORD WINAPI ServerThread(LPVOID)
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        H->log("multiplayer  ERROR: cannot create listen socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    setsockopt(listenSock, SOL_SOCKET, SO_KEEPALIVE, (char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        H->log("multiplayer  ERROR: bind failed on port %d", g_port);
        closesocket(listenSock);
        return 1;
    }

    listen(listenSock, MAX_PLAYERS);
    H->log("multiplayer  server listening on 0.0.0.0:%d (max %d players)",
           g_port, MAX_PLAYERS);

    while (true) {
        SOCKET client = accept(listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        EnterCriticalSection(&g_lock);
        if (g_playerCount >= MAX_PLAYERS) {
            LeaveCriticalSection(&g_lock);

            KickPacket kick;
            snprintf(kick.reason, sizeof(kick.reason),
                     "Server is full (%d/%d players)", g_playerCount, MAX_PLAYERS);
            MsgHeader hdr = {MSG_KICK, sizeof(kick), 0};
            send(client, (char*)&hdr, sizeof(hdr), 0);
            send(client, (char*)&kick, sizeof(kick), 0);
            closesocket(client);
            H->log("multiplayer  rejected: server full");
            continue;
        }

        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (!g_players[i].connected) { slot = i; break; }

        if (slot < 0) {
            LeaveCriticalSection(&g_lock);
            closesocket(client);
            continue;
        }

        HandshakePacket serverHs = {};
        serverHs.protocolVersion = MP_PROTOCOL_VER;
        serverHs.versionMajor = MP_VERSION_MAJOR;
        serverHs.versionMinor = MP_VERSION_MINOR;
        serverHs.versionPatch = MP_VERSION_PATCH;
        strncpy(serverHs.playerName, g_playerName, 63);
        strncpy(serverHs.gameMode, "host", 15);
        MsgHeader hsHdr = {MSG_HANDSHAKE, sizeof(serverHs),
                           CalcChecksum(&serverHs, sizeof(serverHs))};
        send(client, (char*)&hsHdr, sizeof(hsHdr), 0);
        send(client, (char*)&serverHs, sizeof(serverHs), 0);

        g_players[slot].sock = client;
        g_players[slot].ping = 0;
        g_players[slot].pingStart = 0;
        g_players[slot].lastActivity = GetTickCount64();
        g_players[slot].connected = true;
        g_players[slot].handshakeDone = false;
        g_players[slot].hasFullSave = false;
        g_players[slot].prevSaveDir[0] = 0;
        g_players[slot].buildCount = 0;
        g_players[slot].chatCount = 0;
        g_players[slot].buildRateCounter = 0;
        g_players[slot].buildRateWindow = 0;
        snprintf(g_players[slot].name, 64, "Player_%d", slot);
        g_playerCount++;

        H->log("multiplayer  new connection slot=%d (%d/%d)",
               slot, g_playerCount, MAX_PLAYERS);

        CreateThread(NULL, 0, ClientThread, (LPVOID)(intptr_t)slot, 0, NULL);
        CreateThread(NULL, 0, PingThread,   (LPVOID)(intptr_t)slot, 0, NULL);
        LeaveCriticalSection(&g_lock);
    }

    closesocket(listenSock);
    return 0;
}

static DWORD WINAPI WatchThread(LPVOID)
{
    HANDLE hWatch = FindFirstChangeNotificationA(
        g_saveDir, TRUE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

    if (hWatch == INVALID_HANDLE_VALUE) {
        H->log("multiplayer  ERROR: cannot watch save folder: %s", g_saveDir);
        return 1;
    }

    H->log("multiplayer  watching: %s", g_saveDir);

    while (true) {
        DWORD result = WaitForSingleObject(hWatch, INFINITE);
        if (result == WAIT_OBJECT_0) {
            Sleep(1500);
            EnterCriticalSection(&g_lock);
            bool anyConnected = false;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (g_players[i].connected && g_players[i].handshakeDone)
                    anyConnected = true;
            LeaveCriticalSection(&g_lock);

            if (anyConnected) {
                H->log("multiplayer  save changed, syncing...");
                SyncSaveToAll();
            }
            FindNextChangeNotification(hWatch);
        }
    }

    CloseHandle(hWatch);
    return 0;
}

static DWORD WINAPI HeartbeatThread(LPVOID)
{
    while (true) {
        Sleep(HEARTBEAT_INTERVAL_MS);
        ULONGLONG now = GetTickCount64();
        ULONGLONG uptime = (now - g_sessionStart) / 1000;

        EnterCriticalSection(&g_lock);
        int active = 0;
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (g_players[i].connected) active++;
        LeaveCriticalSection(&g_lock);

        H->log("multiplayer  heartbeat | uptime=%llus players=%d builds=%u sent=%uKB",
               uptime, active, g_totalBuildsThisSession,
               g_totalBytesSent / 1024);
    }
    return 0;
}

static void ReloadConfig()
{

    g_enableDemolishSync = H->configInt("plugins\\multiplayer.ini",
                                        "multiplayer", "demolish_sync", 1) != 0;
    g_enableAntiSpam     = H->configInt("plugins\\multiplayer.ini",
                                        "multiplayer", "anti_spam", 1) != 0;
    g_maxBuildRate       = H->configInt("plugins\\multiplayer.ini",
                                        "multiplayer", "max_build_rate",
                                        MAX_BUILD_RATE);
    H->log("multiplayer  config reloaded: demolish=%d spam=%d rate=%d",
           g_enableDemolishSync, g_enableAntiSpam, g_maxBuildRate);
}

static DWORD WINAPI ClientRecvThread(LPVOID)
{
    H->log("multiplayer  client recv thread started");
    while (g_clientConnected) {
        MsgHeader hdr;
        if (!RecvAll(g_clientSock, (char*)&hdr, sizeof(hdr))) {
            H->log("multiplayer  client: disconnected from host");
            g_clientConnected = false;
            break;
        }

        switch (hdr.type) {
        case MSG_PING:
            if (hdr.size > 0) DrainSocket(g_clientSock, hdr.size);
            {
                MsgHeader pong = {MSG_PONG, 0, 0};
                EnterCriticalSection(&g_typeNameLock);
                send(g_clientSock, (char*)&pong, sizeof(pong), 0);
                LeaveCriticalSection(&g_typeNameLock);
            }
            break;

        case MSG_PONG:
            if (hdr.size > 0) DrainSocket(g_clientSock, hdr.size);
            {
                DWORD ping = (DWORD)(GetTickCount64() - g_clientLastPing);
                H->log("multiplayer  client ping: %dms", ping);
            }
            break;

        case MSG_BUILD:
            if (hdr.size == sizeof(BuildCmd)) {
                BuildCmd cmd;
                RecvAll(g_clientSock, (char*)&cmd, sizeof(cmd));
                H->log("multiplayer  client recv BUILD: %s by %s at (%.0f,%.0f)",
                       cmd.typeName, cmd.playerName, cmd.x, cmd.z);
            } else DrainSocket(g_clientSock, hdr.size);
            break;

        case MSG_DEMOLISH:
            if (hdr.size == sizeof(DemolishCmd)) {
                DemolishCmd cmd;
                RecvAll(g_clientSock, (char*)&cmd, sizeof(cmd));
                H->log("multiplayer  client recv DEMOLISH by %s at (%.0f,%.0f)",
                       cmd.playerName, cmd.x, cmd.z);
            } else DrainSocket(g_clientSock, hdr.size);
            break;

        case MSG_CHAT:
            if (hdr.size > 0 && hdr.size < 512) {
                char buf[512] = {};
                RecvAll(g_clientSock, buf, hdr.size);
                H->log("multiplayer  client chat: %s", buf);
            } else DrainSocket(g_clientSock, hdr.size);
            break;

        case MSG_PLAYER_LIST:
            if (hdr.size == sizeof(PlayerListPacket)) {
                PlayerListPacket pkt;
                RecvAll(g_clientSock, (char*)&pkt, sizeof(pkt));
                H->log("multiplayer  client: %d players online", pkt.count);
                for (int i = 0; i < pkt.count; i++)
                    H->log("multiplayer    - %s (ping=%d)",
                           pkt.entries[i].name, pkt.entries[i].ping);
            } else DrainSocket(g_clientSock, hdr.size);
            break;

        case MSG_KICK:
            if (hdr.size == sizeof(KickPacket)) {
                KickPacket kick;
                RecvAll(g_clientSock, (char*)&kick, sizeof(kick));
                H->log("multiplayer  client KICKED: %s", kick.reason);
            } else DrainSocket(g_clientSock, hdr.size);
            g_clientConnected = false;
            break;

        case MSG_SAVE_FULL:
        case MSG_SAVE_DIFF: {
            bool diff = (hdr.type == MSG_SAVE_DIFF);
            int fc = 0;
            RecvAll(g_clientSock, (char*)&fc, sizeof(int));
            H->log("multiplayer  client recv save (%s, %d files)",
                   diff ? "diff" : "full", fc);
            SHCreateDirectoryExA(NULL, g_saveDir, NULL);
            for (int i = 0; i < fc; i++) {
                int nl = 0;
                RecvAll(g_clientSock, (char*)&nl, sizeof(int));
                char fn[MAX_PATH] = {};
                int rl = nl < MAX_PATH - 1 ? nl : MAX_PATH - 1;
                RecvAll(g_clientSock, fn, rl);
                if (nl > rl) DrainSocket(g_clientSock, nl - rl);
                char fp[MAX_PATH];
                snprintf(fp, MAX_PATH, "%s\\%s", g_saveDir, fn);
                if (diff) {

                    DWORD nSz = 0, rSz = 0, cSz = 0;
                    RecvAll(g_clientSock, (char*)&nSz, 4);
                    RecvAll(g_clientSock, (char*)&rSz, 4);
                    RecvAll(g_clientSock, (char*)&cSz, 4);
                    if (nSz && rSz && cSz) {
                        char* cd = (char*)malloc(cSz);
                        if (cd) { RecvAll(g_clientSock, cd, cSz); free(cd); }
                    }
                } else {

                    DWORD oSz = 0, cSz2 = 0;
                    RecvAll(g_clientSock, (char*)&oSz, 4);
                    RecvAll(g_clientSock, (char*)&cSz2, 4);
                    if (oSz && cSz2) {
                        char* cb = (char*)malloc(cSz2);
                        if (cb) {
                            RecvAll(g_clientSock, cb, cSz2);
                            char* ob = (char*)malloc(oSz);
                            if (ob) {
                                size_t r = ZSTD_decompress(ob, oSz, cb, cSz2);
                                if (!ZSTD_isError(r)) {
                                    HANDLE hf = CreateFileA(fp, GENERIC_WRITE, 0,
                                        NULL, CREATE_ALWAYS, 0, NULL);
                                    if (hf != INVALID_HANDLE_VALUE) {
                                        DWORD wr;
                                        WriteFile(hf, ob, (DWORD)r, &wr, NULL);
                                        CloseHandle(hf);
                                    }
                                }
                                free(ob);
                            }
                            free(cb);
                        }
                    }
                }
            }
            H->log("multiplayer  client save received OK");
            break;
        }

        default:
            if (hdr.size > 0) DrainSocket(g_clientSock, hdr.size);
            break;
        }
    }

    closesocket(g_clientSock);
    g_clientSock = INVALID_SOCKET;
    H->log("multiplayer  client recv thread exited");
    return 0;
}

static DWORD WINAPI ClientPingThread(LPVOID)
{
    while (g_clientConnected) {
        Sleep(PING_INTERVAL_MS);
        if (!g_clientConnected) break;
        g_clientLastPing = GetTickCount64();
        MsgHeader hdr = {MSG_PING, 0, 0};
        EnterCriticalSection(&g_typeNameLock);
        send(g_clientSock, (char*)&hdr, sizeof(hdr), 0);
        LeaveCriticalSection(&g_typeNameLock);
    }
    return 0;
}

static bool ClientConnect()
{
    H->log("multiplayer  client: connecting to %s:%d", g_hostIp, g_port);

    g_clientSock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_clientSock == INVALID_SOCKET) {
        H->log("multiplayer  client: socket() failed");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    inet_pton(AF_INET, g_hostIp, &addr.sin_addr);

    if (connect(g_clientSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        H->log("multiplayer  client: connect failed to %s:%d", g_hostIp, g_port);
        closesocket(g_clientSock);
        g_clientSock = INVALID_SOCKET;
        return false;
    }

    H->log("multiplayer  client: connected to host");

    MsgHeader hsHdr;
    if (!RecvAll(g_clientSock, (char*)&hsHdr, sizeof(hsHdr)) ||
        hsHdr.type != MSG_HANDSHAKE) {
        H->log("multiplayer  client: no server handshake");
        closesocket(g_clientSock);
        g_clientSock = INVALID_SOCKET;
        return false;
    }
    HandshakePacket serverHs;
    RecvAll(g_clientSock, (char*)&serverHs, sizeof(serverHs));
    H->log("multiplayer  client: server is %s (protocol v%d)",
           serverHs.playerName, serverHs.protocolVersion);

    if (serverHs.protocolVersion != MP_PROTOCOL_VER) {
        H->log("multiplayer  client: protocol mismatch (%d vs %d)",
               serverHs.protocolVersion, MP_PROTOCOL_VER);
        closesocket(g_clientSock); g_clientSock = INVALID_SOCKET;
        return false;
    }

    HandshakePacket hs = {};
    hs.protocolVersion = MP_PROTOCOL_VER;
    hs.versionMajor = MP_VERSION_MAJOR;
    hs.versionMinor = MP_VERSION_MINOR;
    hs.versionPatch = MP_VERSION_PATCH;
    strncpy(hs.playerName, g_playerName, 63);
    strncpy(hs.gameMode, "client", 15);

    MsgHeader sendHdr = {MSG_HANDSHAKE, sizeof(hs),
                         CalcChecksum(&hs, sizeof(hs))};
    send(g_clientSock, (char*)&sendHdr, sizeof(sendHdr), 0);
    send(g_clientSock, (char*)&hs, sizeof(hs), 0);

    g_clientConnected = true;
    g_clientRecvThread = CreateThread(NULL, 0, ClientRecvThread, NULL, 0, NULL);
    CreateThread(NULL, 0, ClientPingThread, NULL, 0, NULL);

    H->log("multiplayer  client: handshake complete, ready");
    return true;
}

static void ClientSendBuild(const BuildCmd* cmd)
{
    if (!g_clientConnected || g_clientSock == INVALID_SOCKET) return;
    MsgHeader hdr = {MSG_BUILD, sizeof(BuildCmd),
                     CalcChecksum(cmd, sizeof(BuildCmd))};
    EnterCriticalSection(&g_typeNameLock);
    send(g_clientSock, (char*)&hdr, sizeof(hdr), 0);
    send(g_clientSock, (char*)cmd, sizeof(BuildCmd), 0);
    LeaveCriticalSection(&g_typeNameLock);
    H->log("multiplayer  client sent BUILD: %s at (%.0f,%.0f)",
           cmd->typeName, cmd->x, cmd->z);
}


static void ShmUpdateStatus()
{
    if (!g_shm.block) return;
    if (!g_shm.Lock()) return;

    if (strcmp(g_mode, "host") == 0) {
        g_shm.block->status = (g_playerCount > 0) ? MP_STATUS_HOST : MP_STATUS_CONNECTED;
        snprintf(g_shm.block->statusText, 255,
                 "Host | %d/%d players | port %d", g_playerCount, MAX_PLAYERS, g_port);
    } else {
        g_shm.block->status = g_clientConnected ? MP_STATUS_CONNECTED : MP_STATUS_OFFLINE;
        snprintf(g_shm.block->statusText, 255,
                 g_clientConnected ? "Connected to %s:%d" : "Offline",
                 g_hostIp, g_port);
    }

    g_shm.block->playerCount  = g_playerCount;
    g_shm.block->totalBuilds  = g_totalBuildsThisSession;
    g_shm.block->bytesSent    = g_totalBytesSent;
    g_shm.block->sessionUptime = (DWORD)((GetTickCount64() - g_sessionStart) / 1000);

    int n = 0;
    for (int i = 0; i < MAX_PLAYERS && n < 4; i++) {
        if (!g_players[i].connected) continue;
        strncpy(g_shm.block->players[n].name, g_players[i].name, 63);
        g_shm.block->players[n].ping = g_players[i].ping;
        g_shm.block->players[n].connected = 1;
        n++;
    }
    for (int i = n; i < 4; i++) {
        memset(&g_shm.block->players[i], 0, sizeof(PlayerStatus));
    }

    g_shm.Unlock();
    SetEvent(g_shm.hStatusEvent);
}

static void ShmAddBuildNotify(const BuildCmd* cmd)
{
    if (!g_shm.block || !g_shm.Lock()) return;
    DWORD idx = g_shm.block->buildNotifyCount % MAX_BUILD_NOTIFY;
    BuildNotify& n = g_shm.block->buildNotify[idx];
    strncpy(n.playerName, cmd->playerName, 63);
    strncpy(n.typeName,   cmd->typeName,   127);
    n.x = cmd->x; n.z = cmd->z;
    n.timestamp = GetTickCount();
    g_shm.block->buildNotifyCount++;
    g_shm.Unlock();
}

static DWORD WINAPI ShmControlThread(LPVOID)
{
    H->log("multiplayer  shm control thread started");

    while (true) {
        if (g_shm.HasCommand()) {
            if (!g_shm.Lock()) { Sleep(100); continue; }
            BYTE cmd = g_shm.block->command;
            char p1[64], p2[64], p3[64];
            strncpy(p1, g_shm.block->cmdParam1, 63);
            strncpy(p2, g_shm.block->cmdParam2, 63);
            strncpy(p3, g_shm.block->cmdParam3, 63);
            DWORD val = g_shm.block->cmdValue;
            g_shm.block->command = MP_CMD_NONE;
            g_shm.Unlock();

            switch (cmd) {
            case MP_CMD_CONNECT:
                H->log("multiplayer  shm CMD_CONNECT: %s:%s as %s", p1, p2, p3);
                strncpy(g_hostIp, p1, 63);
                g_port = atoi(p2[0] ? p2 : "7777");
                if (p3[0]) strncpy(g_playerName, p3, 63);
                strncpy(g_mode, "client", 31);
                if (g_clientConnected) {
                    g_clientConnected = false;
                    if (g_clientSock != INVALID_SOCKET) {
                        closesocket(g_clientSock);
                        g_clientSock = INVALID_SOCKET;
                    }
                    Sleep(500);
                }
                g_shm.SetStatus(MP_STATUS_CONNECTING, "Connecting...");
                if (ClientConnect()) {
                    H->log("multiplayer  shm: connect OK");
                    ShmUpdateStatus();
                } else {
                    H->log("multiplayer  shm: connect FAILED");
                    g_shm.SetStatus(MP_STATUS_ERROR, "Connection failed");
                }
                break;

            case MP_CMD_DISCONNECT:
                H->log("multiplayer  shm CMD_DISCONNECT");
                if (g_clientConnected) {
                    g_clientConnected = false;
                    if (g_clientSock != INVALID_SOCKET) {
                        closesocket(g_clientSock);
                        g_clientSock = INVALID_SOCKET;
                    }
                }
                g_shm.SetStatus(MP_STATUS_OFFLINE, "Disconnected");
                break;

            case MP_CMD_CHAT:
                H->log("multiplayer  shm CMD_CHAT: %s", p1);
                if (strcmp(g_mode, "host") == 0) {
                    char msg[320];
                    snprintf(msg, 320, "[%s]: %s", g_playerName, p1);
                    BroadcastChat(msg);
                } else if (g_clientConnected) {
                    char msg[320];
                    snprintf(msg, 320, "[%s]: %s", g_playerName, p1);
                    MsgHeader hdr = {MSG_CHAT, (DWORD)strlen(msg), 0};
                    EnterCriticalSection(&g_typeNameLock);
                    send(g_clientSock, (char*)&hdr, sizeof(hdr), 0);
                    send(g_clientSock, msg, (int)strlen(msg), 0);
                    LeaveCriticalSection(&g_typeNameLock);
                }
                break;

            case MP_CMD_RELOAD_CFG:
                H->log("multiplayer  shm CMD_RELOAD_CFG");
                ReloadConfig();
                ShmUpdateStatus();
                break;

            case MP_CMD_KICK:
                H->log("multiplayer  shm CMD_KICK: slot %u", val);
                if (val < MAX_PLAYERS && g_players[val].connected) {
                    KickPacket kick;
                    snprintf(kick.reason, sizeof(kick.reason),
                             "Kicked by host: %s", p1[0] ? p1 : "no reason");
                    SendMsgToPlayer(g_players[val], MSG_KICK, &kick, sizeof(kick));
                    Sleep(200);
                    closesocket(g_players[val].sock);
                }
                break;
            }
        }

        ShmUpdateStatus();
        Sleep(500);
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
    info->version = "0.4.1";

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
    g_port             = H->configInt("plugins\\multiplayer.ini", "multiplayer", "port", 7777);
    g_enableDemolishSync = H->configInt("plugins\\multiplayer.ini", "multiplayer", "demolish_sync", 1) != 0;
    g_enableAntiSpam   = H->configInt("plugins\\multiplayer.ini", "multiplayer", "anti_spam", 1) != 0;
    g_maxBuildRate     = H->configInt("plugins\\multiplayer.ini", "multiplayer", "max_build_rate", MAX_BUILD_RATE);

    memset(g_players, 0, sizeof(g_players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_players[i].sock = INVALID_SOCKET;
        InitializeCriticalSection(&g_players[i].netLock);
    }
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_typeNameLock);

    H->log("multiplayer  v%d.%d.%d loaded | mode=%s name=%s port=%d",
           MP_VERSION_MAJOR, MP_VERSION_MINOR, MP_VERSION_PATCH,
           g_mode, g_playerName, g_port);
    H->log("multiplayer  config: demolish=%d anti_spam=%d max_rate=%d",
           g_enableDemolishSync, g_enableAntiSpam, g_maxBuildRate);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    H->log("multiplayer  starting...");
    g_sessionStart = GetTickCount64();

    if (g_shm.Create(true)) {
        H->log("multiplayer  shared memory created: %s", MP_SHARED_NAME);
        g_shm.block->pluginVersion =
            (MP_VERSION_MAJOR << 16) | (MP_VERSION_MINOR << 8) | MP_VERSION_PATCH;
        g_shmThread = CreateThread(NULL, 0, ShmControlThread, NULL, 0, NULL);
    } else {
        H->log("multiplayer  shared memory failed (non-fatal)");
    }

    CreateDirectoryA(g_syncDir, NULL);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (strcmp(g_mode, "host") == 0) {
        g_serverThread = CreateThread(NULL, 0, ServerThread,  NULL, 0, NULL);
        g_watchThread  = CreateThread(NULL, 0, WatchThread,   NULL, 0, NULL);
        CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);
        H->log("multiplayer  host mode: server started on port %d", g_port);
    } else {
        H->log("multiplayer  client mode: connecting to %s:%d", g_hostIp, g_port);

        Sleep(2000);
        if (!ClientConnect()) {
            H->log("multiplayer  client: connection failed, will retry in background");

            CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                for (int attempt = 1; attempt <= 5; attempt++) {
                    Sleep(5000);
                    H->log("multiplayer  client: retry %d/5...", attempt);
                    if (ClientConnect()) return 0;
                }
                H->log("multiplayer  client: gave up after 5 attempts");
                return 0;
            }, NULL, 0, NULL);
        }
    }

    g_hExe = GetModuleHandleA("SOVIET64.exe");
    if (!g_hExe) {
        H->log("multiplayer  ERROR: SOVIET64.exe not found");
        return 0;
    }

    DWORD_PTR exeBase = (DWORD_PTR)g_hExe;
    H->log("multiplayer  exe base: %p", g_hExe);

    bool hookBuild = InstallHook(HOOK_BUILD_RVA,
        (void*)HookedBuildHandler,
        (void**)&g_origBuildHandler,
        g_buildOrigBytes, "build", 14);

    bool hookPlace = InstallHook(HOOK_PLACE_RVA,
        (void*)HookedPlaceHandler,
        (void**)&g_origPlaceHandler,
        g_placeOrigBytes, "place", 16);

    bool hookDemol = false;
    if (g_enableDemolishSync) {
        BYTE* demolTarget = (BYTE*)(exeBase + HOOK_DEMOL_RVA);
        if (demolTarget[0] == 0x48 && demolTarget[1] == 0x8B && demolTarget[2] == 0xC4) {
            hookDemol = InstallHook(HOOK_DEMOL_RVA,
                (void*)HookedDemolHandler,
                (void**)&g_origDemolHandler,
                g_demolOrigBytes, "demolish", 14);
        } else {
            H->log("multiplayer  demolish hook: offset not matching current build, skipped");
        }
    }

    H->log("multiplayer  hooks: build=%d place=%d demolish=%d",
           hookBuild, hookPlace, hookDemol);

    if (hookBuild && hookPlace) {
        H->log("multiplayer  v%d.%d.%d ACTIVE — realtime sync enabled",
               MP_VERSION_MAJOR, MP_VERSION_MINOR, MP_VERSION_PATCH);
    } else {
        H->log("multiplayer  WARNING: some hooks failed — save-sync only mode");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        g_shm.Destroy();

        if (g_origBuildHandler)
            RemoveHook(HOOK_BUILD_RVA, g_buildOrigBytes, 14);
        if (g_origPlaceHandler)
            RemoveHook(HOOK_PLACE_RVA, g_placeOrigBytes, 16);
        if (g_origDemolHandler)
            RemoveHook(HOOK_DEMOL_RVA, g_demolOrigBytes, 14);
    }
    return TRUE;
}