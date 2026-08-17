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
#include <time.h>
#include "zstd.h"

extern "C" {
#include "bsdiff.h"
#include "bspatch.h"
}

#include <shlobj.h>
#include "mp_shared.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define MP_VERSION_MAJOR     0
#define MP_VERSION_MINOR     8
#define MP_VERSION_PATCH     0
#define MP_PROTOCOL_VER      5

#define MSG_SAVE             1
#define MSG_PING             2
#define MSG_PONG             3
#define MSG_CHAT             4
#define MSG_SAVE_FULL        5
#define MSG_SAVE_DIFF        6
#define MSG_BUILD            7
#define MSG_RESOURCE_REQ     8
#define MSG_RESOURCE_RESP    9
#define MSG_PLAYER_LIST      10
#define MSG_DEMOLISH         11
#define MSG_ROAD             12
#define MSG_HANDSHAKE        13
#define MSG_KICK             14
#define MSG_HEARTBEAT        15
#define MSG_RECONNECT        16
#define MSG_SERVER_INFO      17
#define MSG_TERRITORY        18
#define MSG_STATS_REQ        19
#define MSG_STATS_RESP       20
#define MSG_ADMIN            21
#define MSG_BUILD_APPLY      22
#define MSG_ROAD_APPLY       23
#define MSG_WELCOME          24
#define MSG_PLAYER_JOIN      25
#define MSG_PLAYER_LEAVE     26

#define HOOK_BUILD_RVA       0x6F8CC0
#define HOOK_PLACE_RVA       0x4461F0
#define HOOK_DEMOL_RVA       0x7FC880
#define HOOK_ROAD_RVA        0x786BE0
#define HOOK_PIPE_RVA        0x000000
#define HOOK_RAIL_RVA        0x000000

#define MAX_PLAYERS              4
#define MAX_BUILD_LOG          512
#define MAX_CHAT_LOG           256
#define DIFF_SIZE_LIMIT        (10 * 1024 * 1024)
#define PING_INTERVAL_MS       5000
#define HEARTBEAT_INTERVAL_MS  10000
#define KICK_TIMEOUT_MS        30000
#define MAX_BUILD_RATE         10
#define RECONNECT_INTERVAL_MS  8000
#define MAX_RECONNECT_ATTEMPTS 10
#define CONNECT_TIMEOUT_MS     5000
#define TERRITORY_GRID_SIZE    64
#define MAX_ADMIN_REASON       128

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
    char  password[64];
    DWORD flags;
};

struct BuildCmd {
    float x, z;
    float rotation;
    char  typeName[128];
    char  playerName[64];
    DWORD timestamp;
    DWORD sequenceId;
    BYTE  isFromClient;
};

struct DemolishCmd {
    float x, z;
    char  playerName[64];
    DWORD timestamp;
    DWORD sequenceId;
};

struct RoadCmd {
    float x1, z1;
    float x2, z2;
    char  roadType[64];
    char  playerName[64];
    DWORD timestamp;
    DWORD sequenceId;
    BYTE  flags;
    BYTE  category;
};

#define ROAD_CAT_ROAD  0
#define ROAD_CAT_PIPE  1
#define ROAD_CAT_RAIL  2
#define ROAD_CAT_WIRE  3

struct PlayerListEntry {
    char  name[64];
    DWORD ping;
    DWORD buildCount;
    DWORD bytesReceived;
    DWORD color;
    BYTE  connected;
    BYTE  isHost;
    BYTE  slot;
};

struct PlayerListPacket {
    BYTE            count;
    DWORD           serverUptime;
    PlayerListEntry entries[MAX_PLAYERS];
};

struct ResourceReq {
    char  fromPlayer[64];
    char  toPlayer[64];
    char  resource[64];
    int   amount;
    int   price;
    DWORD requestId;
    BYTE  priority;
};

struct ResourceResp {
    DWORD requestId;
    BYTE  accepted;
    char  reason[128];
};

struct KickPacket {
    char  reason[128];
    BYTE  canReconnect;
};

struct ServerInfoPacket {
    DWORD playerCount;
    DWORD maxPlayers;
    DWORD port;
    DWORD uptime;
    DWORD totalBuilds;
    char  hostName[64];
    char  gameMode[16];
    BYTE  passwordProtected;
};

struct TerritoryPacket {
    char  playerName[64];
    float x1, z1;
    float x2, z2;
    BYTE  action;
    DWORD color;
};

struct StatsPacket {
    DWORD buildCount;
    DWORD demolishCount;
    DWORD chatCount;
    DWORD bytesReceived;
    DWORD bytesSent;
    DWORD ping;
    DWORD sessionUptime;
    char  playerName[64];
};

struct AdminPacket {
    BYTE  command;
    char  target[64];
    char  param[128];
};

#pragma pack(pop)

#define ADMIN_CMD_KICK      1
#define ADMIN_CMD_MUTE      2
#define ADMIN_CMD_UNMUTE    3
#define ADMIN_CMD_BROADCAST 4
#define ADMIN_CMD_RESET     5

struct BuildLogEntry {
    char  playerName[64];
    char  typeName[128];
    float x, z, rotation;
    DWORD timestamp;
    DWORD sequenceId;
};

struct ChatLogEntry {
    char  playerName[64];
    char  message[256];
    DWORD timestamp;
};

struct TerritoryInfo {
    char  playerName[64];
    float x1, z1, x2, z2;
    DWORD color;
    bool  active;
};

typedef void (*BuildHandlerFn)(__int64, char*);
typedef void (*PlaceHandlerFn)(void*, float, float, int);
typedef void (*DemolHandlerFn)(__int64, char*);

typedef void (*RoadHandlerFn)(void*, float, float, float, float, int);

static BuildHandlerFn g_origBuildHandler = nullptr;
static PlaceHandlerFn g_origPlaceHandler = nullptr;
static DemolHandlerFn g_origDemolHandler = nullptr;
static RoadHandlerFn  g_origRoadHandler  = nullptr;
static RoadHandlerFn  g_origPipeHandler  = nullptr;
static RoadHandlerFn  g_origRailHandler  = nullptr;

static BYTE g_buildOrigBytes[16] = {0};
static BYTE g_placeOrigBytes[16] = {0};
static BYTE g_demolOrigBytes[16] = {0};
static BYTE g_roadOrigBytes[16]  = {0};
static BYTE g_pipeOrigBytes[16]  = {0};
static BYTE g_railOrigBytes[16]  = {0};

static HMODULE g_hExe = nullptr;

struct Player {
    SOCKET            sock;
    char              name[64];
    DWORD             ping;
    DWORD             pingMin;
    DWORD             pingMax;
    ULONGLONG         pingStart;
    ULONGLONG         lastActivity;
    ULONGLONG         connectTime;
    bool              connected;
    bool              hasFullSave;
    bool              handshakeDone;
    bool              isMuted;
    char              prevSaveDir[MAX_PATH];
    CRITICAL_SECTION  netLock;
    DWORD             buildCount;
    DWORD             demolishCount;
    DWORD             chatCount;
    DWORD             bytesReceived;
    DWORD             bytesSent;
    int               buildRateCounter;
    ULONGLONG         buildRateWindow;
    DWORD             packetsSent;
    DWORD             packetsRecv;
    DWORD             slot;
    DWORD             color;
    TerritoryInfo     territory;
};

static const DWORD g_slotColors[MAX_PLAYERS] = {
    0xFFE64B32,
    0xFF3C8CDC,
    0xFF32C850,
    0xFFDCB432,
};

#define DEDUP_HISTORY 64
struct BuildDedupEntry { char typeName[128]; float x, z; ULONGLONG timestamp; DWORD seqId; };
static BuildDedupEntry g_dedupHistory[DEDUP_HISTORY] = {};
static int g_dedupIdx = 0;
static CRITICAL_SECTION g_dedupLock;

static bool IsDuplicate(const char* type, float x, float z, DWORD seqId)
{
    EnterCriticalSection(&g_dedupLock);
    ULONGLONG now = GetTickCount64();
    bool found = false;
    for (int i = 0; i < DEDUP_HISTORY; i++) {
        if (!g_dedupHistory[i].typeName[0]) continue;
        if (now - g_dedupHistory[i].timestamp > 2000) continue;
        if (seqId > 0 && g_dedupHistory[i].seqId == seqId) { found = true; break; }
        if (strcmp(g_dedupHistory[i].typeName, type) != 0) continue;
        if (fabsf(g_dedupHistory[i].x - x) < 1.5f && fabsf(g_dedupHistory[i].z - z) < 1.5f) { found = true; break; }
    }
    LeaveCriticalSection(&g_dedupLock);
    return found;
}

static void RecordBuild(const char* type, float x, float z, DWORD seqId)
{
    EnterCriticalSection(&g_dedupLock);
    strncpy(g_dedupHistory[g_dedupIdx].typeName, type, 127);
    g_dedupHistory[g_dedupIdx].x = x;
    g_dedupHistory[g_dedupIdx].z = z;
    g_dedupHistory[g_dedupIdx].timestamp = GetTickCount64();
    g_dedupHistory[g_dedupIdx].seqId = seqId;
    g_dedupIdx = (g_dedupIdx + 1) % DEDUP_HISTORY;
    LeaveCriticalSection(&g_dedupLock);
}

static const TsmHost* H = nullptr;
static char g_saveDir[MAX_PATH];
static char g_syncDir[MAX_PATH];
static char g_logDir[MAX_PATH];
static char g_playerName[64];
static char g_mode[32];
static char g_hostIp[64];
static char g_password[64];
static int  g_port               = 7777;
static bool g_enableDemolishSync = true;
static bool g_enableRoadSync     = false;
static bool g_enablePipeSync     = false;
static bool g_enableRailSync     = false;
static bool g_enableWireSync     = false;
static bool g_enableAntiSpam     = true;
static bool g_enableTerritories  = false;
static bool g_enableSessionLog   = true;
static int  g_maxBuildRate       = MAX_BUILD_RATE;
static int  g_maxPlayers         = MAX_PLAYERS;

static Player           g_players[MAX_PLAYERS];
static int              g_playerCount = 0;
static HANDLE           g_watchThread   = NULL;
static HANDLE           g_serverThread  = NULL;
static HANDLE           g_statsThread   = NULL;
static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_typeNameLock;
static CRITICAL_SECTION g_logLock;

static char           g_lastTypeName[128]  = {0};
static volatile bool  g_inBuildMode        = false;
static DWORD          g_sequenceId         = 0;
static DWORD          g_demolSequenceId    = 0;
static DWORD          g_roadSequenceId     = 0;

static SharedMemory   g_shm;
static HANDLE         g_shmThread = NULL;

static SOCKET         g_clientSock            = INVALID_SOCKET;
static HANDLE         g_clientRecvThread      = NULL;
static bool           g_clientConnected       = false;
static ULONGLONG      g_clientLastPing        = 0;
static bool           g_clientShouldReconnect = false;
static int            g_reconnectAttempts     = 0;
static DWORD          g_clientPing            = 0;
static DWORD          g_clientBytesSent       = 0;
static DWORD          g_clientBytesRecv       = 0;

static ULONGLONG      g_sessionStart       = 0;
static DWORD          g_totalBuildsSession  = 0;
static DWORD          g_totalDemolSession   = 0;
static DWORD          g_totalMsgSent        = 0;
static DWORD          g_totalMsgRecv        = 0;
static DWORD          g_totalBytesSent      = 0;
static DWORD          g_totalBytesRecv      = 0;

static BuildLogEntry  g_buildLog[MAX_BUILD_LOG];
static int            g_buildLogCount = 0;
static CRITICAL_SECTION g_buildLogLock;

static ChatLogEntry   g_chatLog[MAX_CHAT_LOG];
static int            g_chatLogCount = 0;

static TerritoryInfo  g_territories[MAX_PLAYERS];
static int            g_territoryCount = 0;

static WORD CalcChecksum(const void* data, DWORD size)
{
    const BYTE* p = (const BYTE*)data;
    WORD sum = 0;
    for (DWORD i = 0; i < size; i++) sum = (WORD)((sum * 31 + p[i]) & 0xFFFF);
    return sum;
}

static void LogBuild(const char* player, const char* type, float x, float z, float rot, DWORD seq)
{
    if (!g_enableSessionLog) return;
    EnterCriticalSection(&g_buildLogLock);
    if (g_buildLogCount < MAX_BUILD_LOG) {
        BuildLogEntry& e = g_buildLog[g_buildLogCount++];
        strncpy(e.playerName, player, 63);
        strncpy(e.typeName, type, 127);
        e.x = x; e.z = z; e.rotation = rot;
        e.timestamp = GetTickCount();
        e.sequenceId = seq;
    }
    LeaveCriticalSection(&g_buildLogLock);
}

static void LogChat(const char* player, const char* msg)
{
    if (!g_enableSessionLog) return;
    EnterCriticalSection(&g_logLock);
    if (g_chatLogCount < MAX_CHAT_LOG) {
        ChatLogEntry& e = g_chatLog[g_chatLogCount++];
        strncpy(e.playerName, player, 63);
        strncpy(e.message, msg, 255);
        e.timestamp = GetTickCount();
    }
    LeaveCriticalSection(&g_logLock);
}

static void FlushSessionLog()
{
    if (!g_enableSessionLog || !g_buildLogCount) return;
    char path[MAX_PATH];
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(path, MAX_PATH, "%s\\session_%04d%02d%02d_%02d%02d%02d.log",
             g_logDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[256]; DWORD wr;
    EnterCriticalSection(&g_buildLogLock);
    for (int i = 0; i < g_buildLogCount; i++) {
        BuildLogEntry& e = g_buildLog[i];
        snprintf(line, sizeof(line), "[%u] %s placed %s at (%.0f,%.0f) seq=%u\r\n",
                 e.timestamp, e.playerName, e.typeName, e.x, e.z, e.sequenceId);
        WriteFile(h, line, (DWORD)strlen(line), &wr, NULL);
    }
    LeaveCriticalSection(&g_buildLogLock);
    CloseHandle(h);
    H->log("multiplayer  session log written: %d builds", g_buildLogCount);
}

struct PlayerTally {
    char  name[64];
    int   builds, demolishes, roads, chats;
};
static int JournalTally(PlayerTally* out,int maxOut);

static void WriteStatusHtml()
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\mp_status.html", g_logDir);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD up = (DWORD)((GetTickCount64() - g_sessionStart) / 1000);
    int uh = up/3600, um = (up%3600)/60, us = up%60;
    char buf[4096];
    int len = snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5'>"
        "<title>WRSR Multiplayer Server</title>"
        "<style>body{background:#12121a;color:#e0ded8;font-family:monospace;padding:20px}"
        "h1{color:#d9472e}table{border-collapse:collapse;margin-top:12px}"
        "td,th{border:1px solid #333;padding:6px 12px;text-align:left}"
        "th{background:#2a1010;color:#e88}.on{color:#5c5}.off{color:#c55}</style></head><body>"
        "<h1>WRSR Multiplayer</h1>"
        "<p>Status: <span class='on'>ONLINE</span> | Uptime: %dh %dm %ds | Port: %d</p>"
        "<p>Players: %d/%d | Total builds: %u | Total demolitions: %u</p>"
        "<table><tr><th>Slot</th><th>Player</th><th>Ping</th><th>Builds</th><th>Demolitions</th></tr>",
        uh, um, us, g_port, g_playerCount, (int)g_maxPlayers,
        g_totalBuildsSession, g_totalDemolSession);
    DWORD wr;
    WriteFile(h, buf, len, &wr, NULL);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].connected) continue;
        int rlen = snprintf(buf, sizeof(buf),
            "<tr><td>%d</td><td>%s</td><td>%ums</td><td>%u</td><td>%u</td></tr>",
            i, g_players[i].name, g_players[i].ping,
            g_players[i].buildCount, g_players[i].demolishCount);
        WriteFile(h, buf, rlen, &wr, NULL);
    }
    LeaveCriticalSection(&g_lock);
    const char* midTail = "</table>";
    WriteFile(h, midTail, (DWORD)strlen(midTail), &wr, NULL);

    PlayerTally tallies[16];
    int tn = JournalTally(tallies, 16);
    if (tn > 0) {
        const char* th2 = "<h2 style='color:#d9472e'>Activity</h2>"
            "<table><tr><th>Player</th><th>Builds</th><th>Demolitions</th><th>Roads</th><th>Chats</th></tr>";
        WriteFile(h, th2, (DWORD)strlen(th2), &wr, NULL);
        for (int i = 0; i < tn; i++) {
            int rl = snprintf(buf, sizeof(buf),
                "<tr><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td></tr>",
                tallies[i].name, tallies[i].builds, tallies[i].demolishes,
                tallies[i].roads, tallies[i].chats);
            WriteFile(h, buf, rl, &wr, NULL);
        }
        const char* te = "</table>";
        WriteFile(h, te, (DWORD)strlen(te), &wr, NULL);
    }

    const char* tail = "<p style='color:#666;margin-top:20px'>Auto-refreshes every 5s. "
                       "Full event log in mp_journal.json</p></body></html>";
    WriteFile(h, tail, (DWORD)strlen(tail), &wr, NULL);
    CloseHandle(h);
}

static bool NameInFile(const char* fileName, const char* playerName)
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\%s", g_logDir, fileName);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0 || sz > 65536) { CloseHandle(h); return false; }
    char* data = (char*)malloc(sz + 1);
    if (!data) { CloseHandle(h); return false; }
    DWORD rd; ReadFile(h, data, sz, &rd, NULL); CloseHandle(h);
    data[sz] = 0;
    bool found = false;
    char* line = strtok(data, "\r\n");
    while (line) {
        while (*line == ' ' || *line == '\t') line++;
        if (line[0] && line[0] != '#' && _stricmp(line, playerName) == 0) { found = true; break; }
        line = strtok(NULL, "\r\n");
    }
    free(data);
    return found;
}

static bool IsFileNonEmpty(const char* fileName)
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\%s", g_logDir, fileName);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, NULL);
    CloseHandle(h);
    return sz > 0;
}

static bool IsPlayerAllowed(const char* playerName, char* reasonOut, int reasonCap)
{
    if (NameInFile("blacklist.txt", playerName)) {
        snprintf(reasonOut, reasonCap, "You are blacklisted on this server");
        return false;
    }
    if (IsFileNonEmpty("whitelist.txt") && !NameInFile("whitelist.txt", playerName)) {
        snprintf(reasonOut, reasonCap, "This server is whitelist-only");
        return false;
    }
    reasonOut[0] = 0;
    return true;
}

static bool IsPlayerAdmin(const char* playerName)
{
    return NameInFile("admins.txt", playerName);
}

#define JOURNAL_CAPACITY 256

enum EventKind {
    EV_JOIN = 0, EV_LEAVE, EV_BUILD, EV_DEMOLISH, EV_ROAD,
    EV_CHAT, EV_KICK, EV_SAVE_SYNC, EV_ADMIN
};

struct JournalEntry {
    DWORD    id;
    DWORD    timestamp;
    BYTE     kind;
    char     player[64];
    char     detail[96];
    float    x, z;
};

static JournalEntry     g_journal[JOURNAL_CAPACITY];
static DWORD            g_journalHead    = 0;
static DWORD            g_journalCount   = 0;
static CRITICAL_SECTION g_journalLock;
static bool             g_journalInit    = false;

static const char* EventKindName(BYTE k)
{
    switch(k){
        case EV_JOIN:      return "join";
        case EV_LEAVE:     return "leave";
        case EV_BUILD:     return "build";
        case EV_DEMOLISH:  return "demolish";
        case EV_ROAD:      return "road";
        case EV_CHAT:      return "chat";
        case EV_KICK:      return "kick";
        case EV_SAVE_SYNC: return "save_sync";
        case EV_ADMIN:     return "admin";
        default:           return "unknown";
    }
}

static void JournalInit()
{
    if(g_journalInit)return;
    InitializeCriticalSection(&g_journalLock);
    memset(g_journal,0,sizeof(g_journal));
    g_journalHead=0; g_journalCount=0;
    g_journalInit=true;
}

static void JournalAdd(BYTE kind,const char* player,const char* detail,float x,float z)
{
    if(!g_journalInit)JournalInit();
    EnterCriticalSection(&g_journalLock);
    JournalEntry& e=g_journal[g_journalHead];
    e.id=g_journalCount;
    e.timestamp=GetTickCount();
    e.kind=kind;
    strncpy(e.player,player?player:"",63); e.player[63]=0;
    strncpy(e.detail,detail?detail:"",95); e.detail[95]=0;
    e.x=x; e.z=z;
    g_journalHead=(g_journalHead+1)%JOURNAL_CAPACITY;
    g_journalCount++;
    LeaveCriticalSection(&g_journalLock);
}

static void JsonEscape(const char* in,char* out,int cap)
{
    int o=0;
    for(int i=0;in[i]&&o<cap-2;i++){
        char c=in[i];
        if(c=='"'||c=='\\'){ if(o<cap-3){out[o++]='\\';out[o++]=c;} }
        else if(c=='\n'){ if(o<cap-3){out[o++]='\\';out[o++]='n';} }
        else if(c=='\r'){  }
        else if((unsigned char)c<0x20){  }
        else out[o++]=c;
    }
    out[o]=0;
}

static void JournalWriteJson()
{
    if(!g_journalInit)return;
    char path[MAX_PATH];
    snprintf(path,MAX_PATH,"%s\\mp_journal.json",g_logDir);
    HANDLE h=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(h==INVALID_HANDLE_VALUE)return;
    DWORD wr;
    const char* head="{\"events\":[\n";
    WriteFile(h,head,(DWORD)strlen(head),&wr,NULL);

    EnterCriticalSection(&g_journalLock);
    DWORD total=g_journalCount<JOURNAL_CAPACITY?g_journalCount:JOURNAL_CAPACITY;
    DWORD now=GetTickCount();
    char line[512],pe[128],de[192];
    for(DWORD i=0;i<total;i++){

        DWORD slot=(g_journalHead+JOURNAL_CAPACITY-1-i)%JOURNAL_CAPACITY;
        JournalEntry& e=g_journal[slot];
        JsonEscape(e.player,pe,sizeof(pe));
        JsonEscape(e.detail,de,sizeof(de));
        DWORD ageMs=now-e.timestamp;
        int len=snprintf(line,sizeof(line),
            "  {\"id\":%u,\"kind\":\"%s\",\"player\":\"%s\","
            "\"detail\":\"%s\",\"x\":%.1f,\"z\":%.1f,\"age_s\":%u}%s\n",
            e.id,EventKindName(e.kind),pe,de,e.x,e.z,ageMs/1000,
            (i+1<total)?",":"");
        WriteFile(h,line,len,&wr,NULL);
    }
    LeaveCriticalSection(&g_journalLock);

    const char* tail="]}\n";
    WriteFile(h,tail,(DWORD)strlen(tail),&wr,NULL);
    CloseHandle(h);
}

static int JournalTally(PlayerTally* out,int maxOut)
{
    if(!g_journalInit)return 0;
    int n=0;
    EnterCriticalSection(&g_journalLock);
    DWORD total=g_journalCount<JOURNAL_CAPACITY?g_journalCount:JOURNAL_CAPACITY;
    for(DWORD i=0;i<total;i++){
        JournalEntry& e=g_journal[i];
        if(!e.player[0])continue;
        int idx=-1;
        for(int j=0;j<n;j++)if(strcmp(out[j].name,e.player)==0){idx=j;break;}
        if(idx<0){
            if(n>=maxOut)continue;
            idx=n++;
            strncpy(out[idx].name,e.player,63); out[idx].name[63]=0;
            out[idx].builds=out[idx].demolishes=out[idx].roads=out[idx].chats=0;
        }
        switch(e.kind){
            case EV_BUILD:    out[idx].builds++;     break;
            case EV_DEMOLISH: out[idx].demolishes++; break;
            case EV_ROAD:     out[idx].roads++;      break;
            case EV_CHAT:     out[idx].chats++;      break;
            default: break;
        }
    }
    LeaveCriticalSection(&g_journalLock);
    return n;
}

struct PlayerLockGuard {
    Player& p;
    PlayerLockGuard(Player& p) : p(p) { EnterCriticalSection(&p.netLock); }
    ~PlayerLockGuard() { LeaveCriticalSection(&p.netLock); }
};

static bool SendAll(SOCKET s, const char* b, int n)
{
    int sent = 0;
    while (sent < n) { int r = send(s, b+sent, n-sent, 0); if (r<=0) return false; sent+=r; }
    g_totalBytesSent += n;
    return true;
}

static bool SendAllPlayer(Player& p, const char* b, int n)
{
    int sent = 0;
    while (sent < n) { int r = send(p.sock, b+sent, n-sent, 0); if (r<=0) return false; sent+=r; }
    p.bytesSent += n;
    g_totalBytesSent += n;
    return true;
}

static bool RecvAll(SOCKET s, char* b, int n)
{
    int got = 0;
    while (got < n) { int r = recv(s, b+got, n-got, 0); if (r<=0) return false; got+=r; }
    g_totalBytesRecv += n;
    return true;
}

static bool RecvAllPlayer(Player& p, char* b, int n)
{
    int got = 0;
    while (got < n) { int r = recv(p.sock, b+got, n-got, 0); if (r<=0) return false; got+=r; }
    p.bytesReceived += n;
    g_totalBytesRecv += n;
    return true;
}

static void DrainSocket(SOCKET s, DWORD n)
{
    char t[1024];
    while (n > 0) { DWORD k = n>1024?1024:n; if (!RecvAll(s,t,k)) break; n-=k; }
}

static void DrainSocketPlayer(Player& p, DWORD n)
{
    char t[1024];
    while (n > 0) { DWORD k = n>1024?1024:n; if (!RecvAllPlayer(p,t,k)) break; n-=k; }
}

static void SendMsgToPlayer(Player& p, BYTE type, const void* data, DWORD size)
{
    MsgHeader hdr; hdr.type=type; hdr.size=size; hdr.checksum=data?CalcChecksum(data,size):0;
    PlayerLockGuard lock(p);
    if (!SendAllPlayer(p,(char*)&hdr,sizeof(hdr))) return;
    if (data && size) SendAllPlayer(p,(char*)data,size);
    p.packetsSent++;
    g_totalMsgSent++;
}

static bool RecvMsg(SOCKET s, BYTE* type, DWORD* size, char* buf, DWORD cap)
{
    MsgHeader hdr;
    if (!RecvAll(s,(char*)&hdr,sizeof(hdr))) return false;
    *type=hdr.type; *size=hdr.size;
    if (hdr.size>0 && hdr.size<cap) {
        if (!RecvAll(s,buf,hdr.size)) return false;
        WORD ex=CalcChecksum(buf,hdr.size);
        if (hdr.checksum!=0 && ex!=hdr.checksum)
            H->log("multiplayer  WARNING: checksum mismatch type=%d expected=%04X got=%04X",hdr.type,hdr.checksum,ex);
    } else if (hdr.size >= cap) {
        H->log("multiplayer  WARNING: packet too large type=%d size=%u cap=%u",hdr.type,hdr.size,cap);
        DrainSocket(s,hdr.size);
        *size=0;
    }
    g_totalMsgRecv++;
    return true;
}

static bool RecvMsgPlayer(Player& p, BYTE* type, DWORD* size, char* buf, DWORD cap)
{
    MsgHeader hdr;
    if (!RecvAllPlayer(p,(char*)&hdr,sizeof(hdr))) return false;
    *type=hdr.type; *size=hdr.size;
    if (hdr.size>0 && hdr.size<cap) {
        if (!RecvAllPlayer(p,buf,hdr.size)) return false;
        WORD ex=CalcChecksum(buf,hdr.size);
        if (hdr.checksum!=0 && ex!=hdr.checksum)
            H->log("multiplayer  WARNING: checksum mismatch from %s type=%d",p.name,hdr.type);
    } else if (hdr.size >= cap) {
        DrainSocketPlayer(p,hdr.size); *size=0;
    }
    p.packetsRecv++;
    g_totalMsgRecv++;
    return true;
}

static bool IsInTerritory(const TerritoryInfo& t, float x, float z)
{
    if (!t.active) return false;
    float minX=t.x1<t.x2?t.x1:t.x2, maxX=t.x1>t.x2?t.x1:t.x2;
    float minZ=t.z1<t.z2?t.z1:t.z2, maxZ=t.z1>t.z2?t.z1:t.z2;
    return (x>=minX && x<=maxX && z>=minZ && z<=maxZ);
}

static bool CheckTerritoryConflict(const char* playerName, float x, float z)
{
    if (!g_enableTerritories) return false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_territories[i].active) continue;
        if (strcmp(g_territories[i].playerName, playerName) == 0) continue;
        if (IsInTerritory(g_territories[i], x, z)) {
            H->log("multiplayer  territory conflict: %s tried to build in %s territory", playerName, g_territories[i].playerName);
            return true;
        }
    }
    return false;
}

static void BroadcastToAll(BYTE type, const void* data, DWORD size, int excl=-1)
{
    EnterCriticalSection(&g_lock);
    for (int i=0;i<MAX_PLAYERS;i++) {
        if (!g_players[i].connected||g_players[i].sock==INVALID_SOCKET) continue;
        if (i==excl||!g_players[i].handshakeDone) continue;
        SendMsgToPlayer(g_players[i],type,data,size);
    }
    LeaveCriticalSection(&g_lock);
}

static void ClientSendBuild(const BuildCmd* cmd);
static void ShmAddBuildNotify(const BuildCmd* cmd);
static void ShmAddRoadNotify(const RoadCmd* cmd);

static void BroadcastBuild(const BuildCmd* cmd)
{
    ShmAddBuildNotify(cmd);
    LogBuild(cmd->playerName, cmd->typeName, cmd->x, cmd->z, cmd->rotation, cmd->sequenceId);
    JournalAdd(EV_BUILD, cmd->playerName, cmd->typeName, cmd->x, cmd->z);
    if (strcmp(g_mode,"client")==0) ClientSendBuild(cmd);
    else BroadcastToAll(MSG_BUILD,cmd,sizeof(BuildCmd));
}

static void ClientSendDemolish(const DemolishCmd* cmd);
static void BroadcastDemolish(const DemolishCmd* cmd)
{
    if (!g_enableDemolishSync) return;
    g_totalDemolSession++;
    JournalAdd(EV_DEMOLISH, cmd->playerName, "building", cmd->x, cmd->z);
    if (strcmp(g_mode,"client")==0) { ClientSendDemolish(cmd); return; }
    BroadcastToAll(MSG_DEMOLISH,cmd,sizeof(DemolishCmd));
}

static void ClientSendRoad(const RoadCmd* cmd);
static void BroadcastRoad(const RoadCmd* cmd)
{
    if (strcmp(g_mode,"client")==0) { ClientSendRoad(cmd); return; }
    BroadcastToAll(MSG_ROAD,cmd,sizeof(RoadCmd));
}

static void BroadcastChat(const char* msg)
{
    BroadcastToAll(MSG_CHAT,msg,(DWORD)strlen(msg));
}

static void BroadcastTerritory(const TerritoryPacket* t)
{
    BroadcastToAll(MSG_TERRITORY,t,sizeof(TerritoryPacket));
}

static void BroadcastPlayerList()
{
    PlayerListPacket pkt={};
    EnterCriticalSection(&g_lock);
    pkt.serverUptime=(DWORD)((GetTickCount64()-g_sessionStart)/1000);
    int n=0;
    for (int i=0;i<MAX_PLAYERS&&n<MAX_PLAYERS;i++) {
        if (!g_players[i].connected) continue;
        strncpy(pkt.entries[n].name,g_players[i].name,63);
        pkt.entries[n].ping=g_players[i].ping;
        pkt.entries[n].buildCount=g_players[i].buildCount;
        pkt.entries[n].bytesReceived=g_players[i].bytesReceived;
        pkt.entries[n].color=g_players[i].color?g_players[i].color:g_slotColors[i%MAX_PLAYERS];
        pkt.entries[n].connected=1; pkt.entries[n].isHost=0;
        pkt.entries[n].slot=(BYTE)i;
        n++;
    }
    pkt.count=(BYTE)n;
    LeaveCriticalSection(&g_lock);
    BroadcastToAll(MSG_PLAYER_LIST,&pkt,sizeof(pkt));
}

static bool CheckBuildRate(Player& p)
{
    if (!g_enableAntiSpam) return true;
    ULONGLONG now=GetTickCount64();
    if (now-p.buildRateWindow>1000){p.buildRateWindow=now;p.buildRateCounter=0;}
    if (p.buildRateCounter>=g_maxBuildRate){
        H->log("multiplayer  anti-spam: %s exceeded build rate (%d/s)",p.name,g_maxBuildRate);
        return false;
    }
    p.buildRateCounter++;
    return true;
}

static bool IsFileReady(const char* path)
{
    HANDLE h=CreateFileA(path,GENERIC_READ,0,NULL,OPEN_EXISTING,0,NULL);
    if (h!=INVALID_HANDLE_VALUE){CloseHandle(h);return true;}
    return false;
}

static void WaitForSaveReady(const char* savePath)
{
    char pat[MAX_PATH]; snprintf(pat,MAX_PATH,"%s/buildings.bin",savePath);
    int a=0;
    while (!IsFileReady(pat)&&a<30){Sleep(500);a++;}
    if (a>0) H->log("multiplayer  waited %d cycles for save ready",a);
    Sleep(300);
}

static char* LoadFile(const char* path, DWORD* outSize)
{
    HANDLE h=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if (h==INVALID_HANDLE_VALUE){*outSize=0;return nullptr;}
    DWORD sz=GetFileSize(h,NULL);
    if (sz==0||sz==INVALID_FILE_SIZE){CloseHandle(h);*outSize=0;return nullptr;}
    char* buf=(char*)malloc(sz);
    if (!buf){CloseHandle(h);*outSize=0;return nullptr;}
    DWORD rd; ReadFile(h,buf,sz,&rd,NULL); CloseHandle(h); *outSize=sz; return buf;
}

static void WriteFileTo(const char* path, const char* data, DWORD size)
{
    HANDLE h=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if (h==INVALID_HANDLE_VALUE){H->log("multiplayer  ERROR: cannot write %s err=%u",path,GetLastError());return;}
    DWORD wr; WriteFile(h,data,size,&wr,NULL); CloseHandle(h);
}

static void SendCompressedToPlayer(Player& p, const char* data, DWORD size)
{
    if (!data||!size) return;
    size_t cap=ZSTD_compressBound(size);
    char* dst=(char*)malloc(cap); if (!dst) return;
    size_t cSz=ZSTD_compress(dst,cap,data,size,3);
    if (ZSTD_isError(cSz)){H->log("multiplayer  compress error: %s",ZSTD_getErrorName(cSz));free(dst);return;}
    {
        PlayerLockGuard lock(p);
        SendAllPlayer(p,(char*)&size,4);
        DWORD cs=(DWORD)cSz; SendAllPlayer(p,(char*)&cs,4);
        SendAllPlayer(p,dst,(int)cSz);
    }
    free(dst);
}

static void SendFileFullToPlayer(Player& p, const char* path)
{
    DWORD sz; char* data=LoadFile(path,&sz);
    if (!data){
        PlayerLockGuard lock(p);DWORD z=0;SendAllPlayer(p,(char*)&z,4);SendAllPlayer(p,(char*)&z,4);return;
    }
    SendCompressedToPlayer(p,data,sz); free(data);
}

struct DiffStream{char* buf;size_t size;size_t cap;};
static int DiffWrite(struct bsdiff_stream* s,const void* b,int n)
{
    DiffStream* ds=(DiffStream*)s->opaque;
    if (ds->size+n>ds->cap){
        size_t nc=(ds->size+n)*2;
        char* nb=(char*)realloc(ds->buf,nc);if(!nb)return -1;
        ds->buf=nb;ds->cap=nc;
    }
    memcpy(ds->buf+ds->size,b,n);ds->size+=n;return 0;
}

static void SendFileDiffToPlayer(Player& p, const char* newPath, const char* oldPath)
{
    DWORD nSz,oSz;
    char* nData=LoadFile(newPath,&nSz);
    char* oData=LoadFile(oldPath,&oSz);
    if (!nData||!oData||!oSz){
        if(nData)SendCompressedToPlayer(p,nData,nSz);
        else{PlayerLockGuard lock(p);DWORD z=0;SendAllPlayer(p,(char*)&z,4);SendAllPlayer(p,(char*)&z,4);}
        free(nData);free(oData);return;
    }
    DiffStream ds={};ds.cap=8192;ds.buf=(char*)malloc(ds.cap);
    if (!ds.buf){free(nData);free(oData);return;}
    struct bsdiff_stream st={};st.opaque=&ds;st.malloc=malloc;st.free=::free;st.write=DiffWrite;
    if (bsdiff((uint8_t*)oData,oSz,(uint8_t*)nData,nSz,&st)==0){
        size_t cap=ZSTD_compressBound(ds.size);
        char* comp=(char*)malloc(cap);
        if (comp){
            size_t cSz=ZSTD_compress(comp,cap,ds.buf,ds.size,3);
            if (!ZSTD_isError(cSz)){
                PlayerLockGuard lock(p);
                DWORD raw=(DWORD)ds.size,cmp=(DWORD)cSz;
                SendAllPlayer(p,(char*)&nSz,4);SendAllPlayer(p,(char*)&raw,4);SendAllPlayer(p,(char*)&cmp,4);
                SendAllPlayer(p,comp,(int)cSz);
            }
            free(comp);
        }
    } else {
        H->log("multiplayer  bsdiff failed for %s, sending full",newPath);
        SendCompressedToPlayer(p,nData,nSz);
    }
    free(ds.buf);free(nData);free(oData);
}

static void GetLatestSave(char* out)
{
    out[0]=0;char pat[MAX_PATH];snprintf(pat,MAX_PATH,"%s/*",g_saveDir);
    WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pat,&fd);
    if (h==INVALID_HANDLE_VALUE)return;
    FILETIME lat={0,0};
    do {
        if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;
        if(!strcmp(fd.cFileName,".")||!strcmp(fd.cFileName,".."))continue;
        if(!strcmp(fd.cFileName,"mp_client")||!strncmp(fd.cFileName,"autosave",8))continue;
        if(CompareFileTime(&fd.ftLastWriteTime,&lat)>0){lat=fd.ftLastWriteTime;strcpy(out,fd.cFileName);}
    } while(FindNextFileA(h,&fd));
    FindClose(h);
}

static int CountSaveFiles(const char* savePath)
{
    char pat[MAX_PATH];snprintf(pat,MAX_PATH,"%s/*",savePath);
    WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pat,&fd);
    if(h==INVALID_HANDLE_VALUE)return 0;
    int c=0;
    do{if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))c++;}while(FindNextFileA(h,&fd));
    FindClose(h);return c;
}

static void SyncSaveToPlayer(int idx)
{
    Player& p=g_players[idx];
    if(!p.connected||p.sock==INVALID_SOCKET||!p.handshakeDone)return;
    char latSave[MAX_PATH];GetLatestSave(latSave);
    if(!latSave[0]){H->log("multiplayer  no save found for %s",p.name);return;}
    char newPath[MAX_PATH];snprintf(newPath,MAX_PATH,"%s/%s",g_saveDir,latSave);
    WaitForSaveReady(newPath);
    int fc=CountSaveFiles(newPath);if(!fc){H->log("multiplayer  save empty: %s",newPath);return;}
    bool useDiff=p.hasFullSave&&p.prevSaveDir[0];
    H->log("multiplayer  sync -> %s (%s, %d files, save=%s)",p.name,useDiff?"diff":"full",fc,latSave);
    BYTE msgType=useDiff?MSG_SAVE_DIFF:MSG_SAVE_FULL;
    SendMsgToPlayer(p,msgType,&fc,sizeof(int));
    char pat[MAX_PATH];snprintf(pat,MAX_PATH,"%s/*",newPath);
    WIN32_FIND_DATAA fd;HANDLE hf=FindFirstFileA(pat,&fd);
    if(hf==INVALID_HANDLE_VALUE)return;
    DWORD bb=g_totalBytesSent;
    do {
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        int nl=(int)strlen(fd.cFileName)+1;
        {PlayerLockGuard lock(p);SendAllPlayer(p,(char*)&nl,4);SendAllPlayer(p,fd.cFileName,nl);}
        char nf[MAX_PATH];snprintf(nf,MAX_PATH,"%s/%s",newPath,fd.cFileName);
        if(useDiff){
            char of[MAX_PATH];snprintf(of,MAX_PATH,"%s/%s/%s",g_saveDir,p.prevSaveDir,fd.cFileName);
            DWORD osz=0;HANDLE hc=CreateFileA(of,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
            if(hc!=INVALID_HANDLE_VALUE){osz=GetFileSize(hc,NULL);CloseHandle(hc);}
            if(osz>DIFF_SIZE_LIMIT||osz==0)SendFileFullToPlayer(p,nf);else SendFileDiffToPlayer(p,nf,of);
        } else SendFileFullToPlayer(p,nf);
    } while(FindNextFileA(hf,&fd));
    FindClose(hf);
    p.hasFullSave=true;strncpy(p.prevSaveDir,latSave,MAX_PATH-1);
    DWORD kb=(g_totalBytesSent-bb)/1024;
    H->log("multiplayer  sync complete -> %s (%uKB transferred)",p.name,kb);
    char sysMsg[128];
    snprintf(sysMsg,sizeof(sysMsg),"[SERVER] %s received %s (%uKB)",p.name,useDiff?"delta update":"full save",kb);
    BroadcastChat(sysMsg);
}

static void SyncSaveToAll()
{
    EnterCriticalSection(&g_lock);
    for(int i=0;i<MAX_PLAYERS;i++)
        if(g_players[i].connected&&g_players[i].handshakeDone)SyncSaveToPlayer(i);
    LeaveCriticalSection(&g_lock);
}

static void HookedBuildHandler(__int64 param_1, char* param_2)
{
    __try {
        if (param_2) {
            __int64 lVar5=*(__int64*)(param_2+0x240);
            if (lVar5>0x10000) {
                __try {
                    __int64 lVar6=*(__int64*)(lVar5+0x318);
                    if (lVar6>0x10000) {
                        char* n=(char*)lVar6;
                        if (n[0]>='A'&&n[0]<='z') {
                            EnterCriticalSection(&g_typeNameLock);
                            strncpy(g_lastTypeName,n,127);
                            LeaveCriticalSection(&g_typeNameLock);
                        }
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER){}
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
    g_inBuildMode=true;
    g_origBuildHandler(param_1,param_2);
    g_inBuildMode=false;
}

static void HookedPlaceHandler(void* param_1, float x, float z, int param_4)
{
    g_origPlaceHandler(param_1,x,z,param_4);
    if (!g_inBuildMode) return;
    static char s_last[128]={0};
    static bool s_sent=false;
    EnterCriticalSection(&g_typeNameLock);
    char typeName[128]={0};strncpy(typeName,g_lastTypeName,127);
    LeaveCriticalSection(&g_typeNameLock);
    if (!typeName[0]) return;
    bool changed=(strcmp(typeName,s_last)!=0);
    if (changed){s_sent=false;strncpy(s_last,typeName,127);}
    if (s_sent) return;
    s_sent=true;
    DWORD seqId=InterlockedIncrement(&g_sequenceId);
    if (IsDuplicate(typeName,x,z,seqId)) return;
    RecordBuild(typeName,x,z,seqId);
    if (CheckTerritoryConflict(g_playerName,x,z)) {
        H->log("multiplayer  BLOCKED: %s at (%.0f,%.0f) — territory conflict",typeName,x,z);
        return;
    }
    BuildCmd cmd={};
    cmd.x=x;cmd.z=z;cmd.rotation=0.f;
    strncpy(cmd.typeName,typeName,sizeof(cmd.typeName)-1);
    strncpy(cmd.playerName,g_playerName,sizeof(cmd.playerName)-1);
    cmd.timestamp=GetTickCount();
    cmd.sequenceId=seqId;
    cmd.isFromClient=0;
    H->log("multiplayer  PLACE %s at (%.0f,%.0f) seq=%u",typeName,x,z,seqId);
    g_totalBuildsSession++;
    BroadcastBuild(&cmd);
}

static void HookedDemolHandler(__int64 param_1, char* param_2)
{
    if (g_origDemolHandler) g_origDemolHandler(param_1,param_2);
    if (!g_enableDemolishSync) return;
    __try {
        if (param_2){
            float x=0,z=0;
            __try {
                __int64 lVar5=*(__int64*)(param_2+0x240);
                if (lVar5>0x10000){
                    float dat=*(float*)((BYTE*)g_hExe+0x992088);
                    x=*(float*)(param_2+0x28)+*(float*)(param_2+4)+dat*135.0f;
                    z=*(float*)(param_2+0x2c)+*(float*)(param_2+8)+dat*15.0f;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER){}
            if(x||z){
                DemolishCmd cmd={};cmd.x=x;cmd.z=z;
                strncpy(cmd.playerName,g_playerName,63);
                cmd.timestamp=GetTickCount();
                cmd.sequenceId=InterlockedIncrement(&g_demolSequenceId);
                BroadcastDemolish(&cmd);
                H->log("multiplayer  DEMOLISH at (%.0f,%.0f) seq=%u",x,z,cmd.sequenceId);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

static void EmitRoad(float x1,float z1,float x2,float z2,BYTE category,const char* typeName)
{
    RoadCmd cmd={};
    cmd.x1=x1; cmd.z1=z1; cmd.x2=x2; cmd.z2=z2;
    cmd.category=category;
    strncpy(cmd.roadType,typeName?typeName:"road",63);
    strncpy(cmd.playerName,g_playerName,63);
    cmd.timestamp=GetTickCount();
    cmd.sequenceId=InterlockedIncrement(&g_roadSequenceId);
    cmd.flags=0;
    BroadcastRoad(&cmd);
    ShmAddRoadNotify(&cmd);
    { const char* cn=category==ROAD_CAT_PIPE?"pipe":category==ROAD_CAT_RAIL?"rail":category==ROAD_CAT_WIRE?"wire":"road";
      JournalAdd(EV_ROAD,g_playerName,cn,x1,z1); }
    const char* catStr = category==ROAD_CAT_PIPE?"PIPE":category==ROAD_CAT_RAIL?"RAIL":category==ROAD_CAT_WIRE?"WIRE":"ROAD";
    H->log("multiplayer  %s (%.0f,%.0f)->(%.0f,%.0f) seq=%u",catStr,x1,z1,x2,z2,cmd.sequenceId);
}

static void HookedRoadHandler(void* param_1,float x1,float z1,float x2,float z2,int param_6)
{
    if(g_origRoadHandler)g_origRoadHandler(param_1,x1,z1,x2,z2,param_6);
    if(!g_enableRoadSync)return;
    __try {
        if(x1||z1||x2||z2)EmitRoad(x1,z1,x2,z2,ROAD_CAT_ROAD,"road");
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

static void HookedPipeHandler(void* param_1,float x1,float z1,float x2,float z2,int param_6)
{
    if(g_origPipeHandler)g_origPipeHandler(param_1,x1,z1,x2,z2,param_6);
    if(!g_enablePipeSync)return;
    __try {
        if(x1||z1||x2||z2)EmitRoad(x1,z1,x2,z2,ROAD_CAT_PIPE,"pipe");
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

static void HookedRailHandler(void* param_1,float x1,float z1,float x2,float z2,int param_6)
{
    if(g_origRailHandler)g_origRailHandler(param_1,x1,z1,x2,z2,param_6);
    if(!g_enableRailSync)return;
    __try {
        if(x1||z1||x2||z2)EmitRoad(x1,z1,x2,z2,ROAD_CAT_RAIL,"rail");
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

static bool InstallHook(DWORD_PTR rva,void* hookFn,void** origFn,BYTE* origBytes,const char* name,int hookSize)
{
    BYTE* target=(BYTE*)((DWORD_PTR)g_hExe+rva);
    if(target[0]!=0x48||target[1]!=0x8B||target[2]!=0xC4){
        H->log("multiplayer  hook %s: mismatch %02X %02X %02X",name,target[0],target[1],target[2]);return false;
    }
    BYTE* tramp=nullptr;DWORD_PTR base=(DWORD_PTR)target;
    for(DWORD_PTR d=0x1000000;d<0x70000000;d+=0x1000000){
        tramp=(BYTE*)VirtualAlloc((void*)(base-d),64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(tramp)break;
        tramp=(BYTE*)VirtualAlloc((void*)(base+d),64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(tramp)break;
    }
    if(!tramp)tramp=(BYTE*)VirtualAlloc(NULL,64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!tramp){H->log("multiplayer  hook %s: trampoline failed",name);return false;}
    DWORD oldProt;
    if(!VirtualProtect(target,hookSize,PAGE_EXECUTE_READWRITE,&oldProt)){
        VirtualFree(tramp,0,MEM_RELEASE);H->log("multiplayer  hook %s: VirtualProtect failed",name);return false;
    }
    memcpy(origBytes,target,hookSize);
    memcpy(tramp,origBytes,hookSize);
    tramp[hookSize+0]=0xFF;tramp[hookSize+1]=0x25;
    *(DWORD*)(tramp+hookSize+2)=0;
    *(DWORD_PTR*)(tramp+hookSize+6)=(DWORD_PTR)(target+hookSize);
    *origFn=tramp;
    target[0]=0x48;target[1]=0xB8;
    *(DWORD_PTR*)(target+2)=(DWORD_PTR)hookFn;
    target[10]=0xFF;target[11]=0xE0;
    for(int i=12;i<hookSize;i++)target[i]=0x90;
    VirtualProtect(target,hookSize,oldProt,&oldProt);
    FlushInstructionCache(GetCurrentProcess(),target,hookSize);
    H->log("multiplayer  hook [%s] OK @ %p (%d bytes)",name,target,hookSize);
    return true;
}

static void RemoveHook(DWORD_PTR rva,BYTE* origBytes,int hookSize)
{
    BYTE* target=(BYTE*)((DWORD_PTR)g_hExe+rva);
    DWORD oldProt;
    if(VirtualProtect(target,hookSize,PAGE_EXECUTE_READWRITE,&oldProt)){
        memcpy(target,origBytes,hookSize);
        VirtualProtect(target,hookSize,oldProt,&oldProt);
        FlushInstructionCache(GetCurrentProcess(),target,hookSize);
    }
}

static void HandleAdminCommand(Player& sender, const AdminPacket* pkt)
{
    H->log("multiplayer  admin cmd=%d target=%s from=%s",pkt->command,pkt->target,sender.name);
    switch(pkt->command){
    case ADMIN_CMD_KICK:{
        EnterCriticalSection(&g_lock);
        for(int i=0;i<MAX_PLAYERS;i++){
            if(!g_players[i].connected||strcmp(g_players[i].name,pkt->target)!=0)continue;
            KickPacket kick;snprintf(kick.reason,sizeof(kick.reason),"Kicked by %s: %s",sender.name,pkt->param[0]?pkt->param:"no reason");kick.canReconnect=0;
            SendMsgToPlayer(g_players[i],MSG_KICK,&kick,sizeof(kick));
            H->log("multiplayer  admin kick: %s kicked %s",sender.name,g_players[i].name);
            break;
        }
        LeaveCriticalSection(&g_lock);
        break;}
    case ADMIN_CMD_MUTE:{
        EnterCriticalSection(&g_lock);
        for(int i=0;i<MAX_PLAYERS;i++){
            if(!g_players[i].connected||strcmp(g_players[i].name,pkt->target)!=0)continue;
            g_players[i].isMuted=true;
            H->log("multiplayer  admin mute: %s muted %s",sender.name,pkt->target);
            break;
        }
        LeaveCriticalSection(&g_lock);
        break;}
    case ADMIN_CMD_UNMUTE:{
        EnterCriticalSection(&g_lock);
        for(int i=0;i<MAX_PLAYERS;i++){
            if(!g_players[i].connected||strcmp(g_players[i].name,pkt->target)!=0)continue;
            g_players[i].isMuted=false;
            break;
        }
        LeaveCriticalSection(&g_lock);
        break;}
    case ADMIN_CMD_BROADCAST:{
        char msg[320];snprintf(msg,sizeof(msg),"[ADMIN] %s",pkt->param);
        BroadcastChat(msg);
        break;}
    }
}

static DWORD WINAPI ClientThread(LPVOID arg)
{
    int idx=(int)(intptr_t)arg;
    Player& p=g_players[idx];
    p.connectTime=GetTickCount64();
    {
        BYTE type;DWORD size;char buf[512];
        if(!RecvMsgPlayer(p,&type,&size,buf,sizeof(buf))||type!=MSG_HANDSHAKE||size!=sizeof(HandshakePacket)){
            H->log("multiplayer  slot %d: bad handshake",idx);goto disconnect;
        }
        HandshakePacket* hs=(HandshakePacket*)buf;
        if(hs->protocolVersion!=MP_PROTOCOL_VER){
            H->log("multiplayer  slot %d: protocol mismatch (need %d got %d)",idx,MP_PROTOCOL_VER,hs->protocolVersion);
            KickPacket kick;snprintf(kick.reason,sizeof(kick.reason),"Protocol v%d required, you have v%d",MP_PROTOCOL_VER,hs->protocolVersion);kick.canReconnect=0;
            SendMsgToPlayer(p,MSG_KICK,&kick,sizeof(kick));goto disconnect;
        }
        if(g_password[0]){
            if(strncmp(hs->password,g_password,63)!=0){
                H->log("multiplayer  slot %d: wrong password",idx);
                KickPacket kick;snprintf(kick.reason,sizeof(kick.reason),"Wrong server password");kick.canReconnect=1;
                SendMsgToPlayer(p,MSG_KICK,&kick,sizeof(kick));goto disconnect;
            }
        }
        if(!hs->playerName[0])snprintf(hs->playerName,64,"Player_%d",idx);
        char denyReason[128]={};
        if(!IsPlayerAllowed(hs->playerName,denyReason,sizeof(denyReason))){
            H->log("multiplayer  slot %d: %s denied — %s",idx,hs->playerName,denyReason);
            KickPacket kick;strncpy(kick.reason,denyReason,sizeof(kick.reason)-1);kick.canReconnect=0;
            SendMsgToPlayer(p,MSG_KICK,&kick,sizeof(kick));goto disconnect;
        }
        strncpy(p.name,hs->playerName,63);
        p.color=g_slotColors[idx%MAX_PLAYERS];
        p.handshakeDone=true;
        H->log("multiplayer  slot %d: %s handshake OK (client v%d.%d.%d mode=%s)",idx,p.name,hs->versionMajor,hs->versionMinor,hs->versionPatch,hs->gameMode);
        ServerInfoPacket info={};
        info.playerCount=(DWORD)g_playerCount;info.maxPlayers=MAX_PLAYERS;
        info.port=(DWORD)g_port;info.uptime=(DWORD)((GetTickCount64()-g_sessionStart)/1000);
        info.totalBuilds=g_totalBuildsSession;
        strncpy(info.hostName,g_playerName,63);strncpy(info.gameMode,g_mode,15);
        SendMsgToPlayer(p,MSG_SERVER_INFO,&info,sizeof(info));
    }
    H->log("multiplayer  player joined: %s (slot %d)",p.name,idx);
    JournalAdd(EV_JOIN,p.name,"connected",0,0);
    {
        char joinMsg[128];snprintf(joinMsg,sizeof(joinMsg),"[SERVER] %s joined",p.name);
        BroadcastChat(joinMsg);
    }
    SyncSaveToPlayer(idx);
    BroadcastPlayerList();
    if(g_enableTerritories){
        for(int i=0;i<MAX_PLAYERS;i++){
            if(!g_territories[i].active)continue;
            TerritoryPacket tp={};
            strncpy(tp.playerName,g_territories[i].playerName,63);
            tp.x1=g_territories[i].x1;tp.z1=g_territories[i].z1;
            tp.x2=g_territories[i].x2;tp.z2=g_territories[i].z2;
            tp.color=g_territories[i].color;tp.action=1;
            SendMsgToPlayer(p,MSG_TERRITORY,&tp,sizeof(tp));
        }
    }
    {
        BYTE type;DWORD size;char buf[sizeof(BuildCmd)+64];
        while(RecvMsgPlayer(p,&type,&size,buf,sizeof(buf))){
            p.lastActivity=GetTickCount64();
            switch(type){
            case MSG_PING:
                if(size>0)DrainSocketPlayer(p,size);
                SendMsgToPlayer(p,MSG_PONG,nullptr,0);break;
            case MSG_PONG:
                if(size>0)DrainSocketPlayer(p,size);
                {DWORD np=(DWORD)(GetTickCount64()-p.pingStart);
                 p.ping=np;
                 if(np<p.pingMin||p.pingMin==0)p.pingMin=np;
                 if(np>p.pingMax)p.pingMax=np;}
                break;
            case MSG_HEARTBEAT:
                if(size>0)DrainSocketPlayer(p,size);break;
            case MSG_CHAT:
                if(size>0&&size<512){
                    buf[size]=0;
                    if(p.isMuted){
                        H->log("multiplayer  muted %s tried to chat: %s",p.name,buf);
                        break;
                    }
                    H->log("multiplayer  [%s]: %s",p.name,buf);
                    LogChat(p.name,buf);
                    BroadcastToAll(MSG_CHAT,buf,size,idx);
                    p.chatCount++;
                }break;
            case MSG_BUILD:
                if(size==sizeof(BuildCmd)){
                    BuildCmd* cmd=(BuildCmd*)buf;
                    if(!CheckBuildRate(p))break;
                    if(IsDuplicate(cmd->typeName,cmd->x,cmd->z,cmd->sequenceId))break;
                    if(CheckTerritoryConflict(p.name,cmd->x,cmd->z))break;
                    RecordBuild(cmd->typeName,cmd->x,cmd->z,cmd->sequenceId);
                    strncpy(cmd->playerName,p.name,63);cmd->isFromClient=1;
                    p.buildCount++;
                    H->log("multiplayer  [BUILD] %s: %s at (%.0f,%.0f) seq=%u",p.name,cmd->typeName,cmd->x,cmd->z,cmd->sequenceId);
                    LogBuild(p.name,cmd->typeName,cmd->x,cmd->z,cmd->rotation,cmd->sequenceId);
                    ShmAddBuildNotify(cmd);
                    BroadcastToAll(MSG_BUILD,cmd,sizeof(BuildCmd),idx);
                }break;
            case MSG_DEMOLISH:
                if(size==sizeof(DemolishCmd)){
                    DemolishCmd* cmd=(DemolishCmd*)buf;
                    strncpy(cmd->playerName,p.name,63);
                    H->log("multiplayer  [DEMOLISH] %s at (%.0f,%.0f) seq=%u",p.name,cmd->x,cmd->z,cmd->sequenceId);
                    p.demolishCount++;
                    BroadcastToAll(MSG_DEMOLISH,cmd,sizeof(DemolishCmd),idx);
                }break;
            case MSG_ROAD:
                if(size==sizeof(RoadCmd)){
                    RoadCmd* cmd=(RoadCmd*)buf;
                    strncpy(cmd->playerName,p.name,63);
                    const char* catStr=cmd->category==ROAD_CAT_PIPE?"PIPE":cmd->category==ROAD_CAT_RAIL?"RAIL":cmd->category==ROAD_CAT_WIRE?"WIRE":"ROAD";
                    H->log("multiplayer  [%s] %s (%.0f,%.0f)->(%.0f,%.0f)",catStr,p.name,cmd->x1,cmd->z1,cmd->x2,cmd->z2);
                    BroadcastToAll(MSG_ROAD,cmd,sizeof(RoadCmd),idx);
                }break;
            case MSG_RESOURCE_REQ:
                if(size==sizeof(ResourceReq)){
                    ResourceReq* req=(ResourceReq*)buf;
                    strncpy(req->fromPlayer,p.name,63);
                    H->log("multiplayer  [TRADE] %s requests %d x %s from %s",p.name,req->amount,req->resource,req->toPlayer[0]?req->toPlayer:"all");
                    BroadcastToAll(MSG_RESOURCE_REQ,req,sizeof(ResourceReq),idx);
                }break;
            case MSG_RESOURCE_RESP:
                if(size==sizeof(ResourceResp)){
                    ResourceResp* resp=(ResourceResp*)buf;
                    H->log("multiplayer  [TRADE RESP] req=%u %s",resp->requestId,resp->accepted?"accepted":"declined");
                    BroadcastToAll(MSG_RESOURCE_RESP,resp,sizeof(ResourceResp),idx);
                }break;
            case MSG_TERRITORY:
                if(size==sizeof(TerritoryPacket)&&g_enableTerritories){
                    TerritoryPacket* tp=(TerritoryPacket*)buf;
                    strncpy(tp->playerName,p.name,63);
                    EnterCriticalSection(&g_lock);
                    g_territories[idx].active=(tp->action==1);
                    strncpy(g_territories[idx].playerName,p.name,63);
                    g_territories[idx].x1=tp->x1;g_territories[idx].z1=tp->z1;
                    g_territories[idx].x2=tp->x2;g_territories[idx].z2=tp->z2;
                    g_territories[idx].color=tp->color;
                    LeaveCriticalSection(&g_lock);
                    H->log("multiplayer  [TERRITORY] %s %s",p.name,tp->action?"set":"cleared");
                    BroadcastToAll(MSG_TERRITORY,tp,sizeof(TerritoryPacket),idx);
                }break;
            case MSG_STATS_REQ:
                if(size>0)DrainSocketPlayer(p,size);
                {
                    StatsPacket sp={};
                    strncpy(sp.playerName,p.name,63);
                    sp.buildCount=p.buildCount;sp.demolishCount=p.demolishCount;
                    sp.chatCount=p.chatCount;sp.bytesReceived=p.bytesReceived;
                    sp.bytesSent=p.bytesSent;sp.ping=p.ping;
                    sp.sessionUptime=(DWORD)((GetTickCount64()-p.connectTime)/1000);
                    SendMsgToPlayer(p,MSG_STATS_RESP,&sp,sizeof(sp));
                }break;
            case MSG_ADMIN:
                if(size==sizeof(AdminPacket)){
                    AdminPacket* ap=(AdminPacket*)buf;
                    HandleAdminCommand(p,ap);
                }break;
            default:
                if(size>0)DrainSocketPlayer(p,size);break;
            }
        }
    }
disconnect:
    EnterCriticalSection(&g_lock);
    DWORD sessionSec=(DWORD)((GetTickCount64()-p.connectTime)/1000);
    H->log("multiplayer  player left: %s slot=%d builds=%u demolish=%u chat=%u ping=%u/%u session=%us",
           p.name,idx,p.buildCount,p.demolishCount,p.chatCount,p.pingMin,p.pingMax,sessionSec);
    p.connected=false;p.handshakeDone=false;p.hasFullSave=false;p.prevSaveDir[0]=0;
    p.isMuted=false;p.territory.active=false;
    g_territories[idx].active=false;
    closesocket(p.sock);p.sock=INVALID_SOCKET;g_playerCount--;
    char lm[128];snprintf(lm,sizeof(lm),"[SERVER] %s disconnected (session: %us)",p.name,sessionSec);
    LeaveCriticalSection(&g_lock);
    BroadcastChat(lm);BroadcastPlayerList();
    if(g_enableTerritories){
        TerritoryPacket tp={};strncpy(tp.playerName,p.name,63);tp.action=0;
        BroadcastTerritory(&tp);
    }
    return 0;
}

static DWORD WINAPI PingThread(LPVOID arg)
{
    int idx=(int)(intptr_t)arg;
    while(true){
        Sleep(PING_INTERVAL_MS);
        EnterCriticalSection(&g_lock);
        if(!g_players[idx].connected){LeaveCriticalSection(&g_lock);break;}
        ULONGLONG now=GetTickCount64();
        if(g_players[idx].lastActivity>0&&now-g_players[idx].lastActivity>KICK_TIMEOUT_MS){
            H->log("multiplayer  timeout kick: %s (inactive %llums)",g_players[idx].name,now-g_players[idx].lastActivity);
            KickPacket kick;snprintf(kick.reason,sizeof(kick.reason),"Connection timeout");kick.canReconnect=1;
            SendMsgToPlayer(g_players[idx],MSG_KICK,&kick,sizeof(kick));
            Sleep(200);closesocket(g_players[idx].sock);
            LeaveCriticalSection(&g_lock);break;
        }
        g_players[idx].pingStart=GetTickCount64();
        SendMsgToPlayer(g_players[idx],MSG_PING,nullptr,0);
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static DWORD WINAPI ServerThread(LPVOID)
{
    SOCKET ls=socket(AF_INET,SOCK_STREAM,0);
    if(ls==INVALID_SOCKET){H->log("multiplayer  ERROR: cannot create socket err=%d",WSAGetLastError());return 1;}
    int opt=1;
    setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,(char*)&opt,sizeof(opt));
    setsockopt(ls,SOL_SOCKET,SO_KEEPALIVE,(char*)&opt,sizeof(opt));
    int sndbuf=256*1024,rcvbuf=256*1024;
    setsockopt(ls,SOL_SOCKET,SO_SNDBUF,(char*)&sndbuf,sizeof(sndbuf));
    setsockopt(ls,SOL_SOCKET,SO_RCVBUF,(char*)&rcvbuf,sizeof(rcvbuf));
    sockaddr_in addr={};addr.sin_family=AF_INET;addr.sin_port=htons((u_short)g_port);addr.sin_addr.s_addr=INADDR_ANY;
    if(bind(ls,(sockaddr*)&addr,sizeof(addr))!=0){H->log("multiplayer  ERROR: bind failed port %d err=%d",g_port,WSAGetLastError());closesocket(ls);return 1;}
    listen(ls,MAX_PLAYERS);
    H->log("multiplayer  server listening on 0.0.0.0:%d (max %d players)",g_port,MAX_PLAYERS);
    while(true){
        sockaddr_in clientAddr={};int clientAddrLen=sizeof(clientAddr);
        SOCKET client=accept(ls,(sockaddr*)&clientAddr,&clientAddrLen);
        if(client==INVALID_SOCKET)continue;
        char clientIp[64]="";
        inet_ntop(AF_INET,&clientAddr.sin_addr,clientIp,sizeof(clientIp));
        H->log("multiplayer  incoming connection from %s:%d",clientIp,ntohs(clientAddr.sin_port));
        EnterCriticalSection(&g_lock);
        if(g_playerCount>=(int)g_maxPlayers){
            LeaveCriticalSection(&g_lock);
            KickPacket kick;snprintf(kick.reason,sizeof(kick.reason),"Server full (%d/%d)",g_playerCount,g_maxPlayers);kick.canReconnect=1;
            MsgHeader hdr={MSG_KICK,sizeof(kick),CalcChecksum(&kick,sizeof(kick))};
            send(client,(char*)&hdr,sizeof(hdr),0);send(client,(char*)&kick,sizeof(kick),0);
            closesocket(client);H->log("multiplayer  rejected %s: server full",clientIp);continue;
        }
        int slot=-1;for(int i=0;i<MAX_PLAYERS;i++)if(!g_players[i].connected){slot=i;break;}
        if(slot<0){LeaveCriticalSection(&g_lock);closesocket(client);continue;}
        int sndb=64*1024,rcvb=64*1024;
        setsockopt(client,SOL_SOCKET,SO_SNDBUF,(char*)&sndb,sizeof(sndb));
        setsockopt(client,SOL_SOCKET,SO_RCVBUF,(char*)&rcvb,sizeof(rcvb));
        HandshakePacket shs={};shs.protocolVersion=MP_PROTOCOL_VER;
        shs.versionMajor=MP_VERSION_MAJOR;shs.versionMinor=MP_VERSION_MINOR;shs.versionPatch=MP_VERSION_PATCH;
        strncpy(shs.playerName,g_playerName,63);strncpy(shs.gameMode,g_mode,15);shs.flags=0;
        MsgHeader hh={MSG_HANDSHAKE,sizeof(shs),CalcChecksum(&shs,sizeof(shs))};
        send(client,(char*)&hh,sizeof(hh),0);send(client,(char*)&shs,sizeof(shs),0);
        g_players[slot].sock=client;
        g_players[slot].ping=0;g_players[slot].pingMin=0;g_players[slot].pingMax=0;
        g_players[slot].pingStart=0;g_players[slot].lastActivity=GetTickCount64();
        g_players[slot].connectTime=GetTickCount64();
        g_players[slot].connected=true;g_players[slot].handshakeDone=false;
        g_players[slot].hasFullSave=false;g_players[slot].prevSaveDir[0]=0;
        g_players[slot].buildCount=0;g_players[slot].demolishCount=0;g_players[slot].chatCount=0;
        g_players[slot].bytesReceived=0;g_players[slot].bytesSent=0;
        g_players[slot].buildRateCounter=0;g_players[slot].buildRateWindow=0;
        g_players[slot].packetsSent=0;g_players[slot].packetsRecv=0;
        g_players[slot].isMuted=false;g_players[slot].territory.active=false;
        g_players[slot].slot=(DWORD)slot;
        snprintf(g_players[slot].name,64,"Player_%d",slot);
        g_playerCount++;
        H->log("multiplayer  new connection: %s slot=%d (%d/%d)",clientIp,slot,g_playerCount,g_maxPlayers);
        CreateThread(NULL,0,ClientThread,(LPVOID)(intptr_t)slot,0,NULL);
        CreateThread(NULL,0,PingThread,(LPVOID)(intptr_t)slot,0,NULL);
        LeaveCriticalSection(&g_lock);
    }
    closesocket(ls);return 0;
}

static DWORD WINAPI WatchThread(LPVOID)
{
    HANDLE hw=FindFirstChangeNotificationA(g_saveDir,TRUE,FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_FILE_NAME);
    if(hw==INVALID_HANDLE_VALUE){H->log("multiplayer  ERROR: cannot watch %s err=%d",g_saveDir,GetLastError());return 1;}
    H->log("multiplayer  watching save dir: %s",g_saveDir);
    while(true){
        if(WaitForSingleObject(hw,INFINITE)==WAIT_OBJECT_0){
            Sleep(1500);
            EnterCriticalSection(&g_lock);
            bool any=false;
            for(int i=0;i<MAX_PLAYERS;i++)if(g_players[i].connected&&g_players[i].handshakeDone)any=true;
            LeaveCriticalSection(&g_lock);
            if(any){H->log("multiplayer  save changed, syncing to all players...");SyncSaveToAll();}
            FindNextChangeNotification(hw);
        }
    }
    CloseHandle(hw);return 0;
}

static DWORD WINAPI HeartbeatThread(LPVOID)
{
    while(true){
        Sleep(HEARTBEAT_INTERVAL_MS);
        ULONGLONG up=(GetTickCount64()-g_sessionStart)/1000;
        EnterCriticalSection(&g_lock);
        int a=0;DWORD totalPing=0;
        for(int i=0;i<MAX_PLAYERS;i++){if(!g_players[i].connected)continue;a++;totalPing+=g_players[i].ping;}
        LeaveCriticalSection(&g_lock);
        DWORD avgPing=a?totalPing/a:0;
        H->log("multiplayer  [HB] uptime=%llus players=%d builds=%u demolish=%u avgping=%ums sent=%uKB recv=%uKB",
               up,a,g_totalBuildsSession,g_totalDemolSession,avgPing,g_totalBytesSent/1024,g_totalBytesRecv/1024);
        BroadcastPlayerList();
    }
    return 0;
}

static DWORD WINAPI StatsThread(LPVOID)
{
    DWORD tick=0;
    while(true){
        Sleep(5000);
        tick++;

        if(strcmp(g_mode,"host")==0){WriteStatusHtml();JournalWriteJson();}

        if(tick%12==0){
            if(g_buildLogCount>0)FlushSessionLog();
            if(g_shm.block&&g_shm.Lock()){
                g_shm.block->totalBuilds=g_totalBuildsSession;
                g_shm.block->bytesSent=g_totalBytesSent;
                g_shm.block->sessionUptime=(DWORD)((GetTickCount64()-g_sessionStart)/1000);
                g_shm.Unlock();
            }
        }
    }
    return 0;
}

static void ReloadConfig()
{
    g_enableDemolishSync =H->configInt("plugins\\multiplayer.ini","multiplayer","demolish_sync",1)!=0;
    g_enableRoadSync     =H->configInt("plugins\\multiplayer.ini","multiplayer","road_sync",0)!=0;
    g_enablePipeSync     =H->configInt("plugins\\multiplayer.ini","multiplayer","pipe_sync",0)!=0;
    g_enableRailSync     =H->configInt("plugins\\multiplayer.ini","multiplayer","rail_sync",0)!=0;
    g_enableWireSync     =H->configInt("plugins\\multiplayer.ini","multiplayer","wire_sync",0)!=0;
    g_enableAntiSpam     =H->configInt("plugins\\multiplayer.ini","multiplayer","anti_spam",1)!=0;
    g_enableTerritories  =H->configInt("plugins\\multiplayer.ini","multiplayer","territories",0)!=0;
    g_enableSessionLog   =H->configInt("plugins\\multiplayer.ini","multiplayer","session_log",1)!=0;
    g_maxBuildRate       =H->configInt("plugins\\multiplayer.ini","multiplayer","max_build_rate",MAX_BUILD_RATE);
    g_maxPlayers         =H->configInt("plugins\\multiplayer.ini","multiplayer","max_players",MAX_PLAYERS);
    if(g_maxPlayers<1)g_maxPlayers=1;if(g_maxPlayers>MAX_PLAYERS)g_maxPlayers=MAX_PLAYERS;
    H->log("multiplayer  config reloaded: demolish=%d road=%d spam=%d terr=%d rate=%d maxplayers=%d",
           g_enableDemolishSync,g_enableRoadSync,g_enableAntiSpam,g_enableTerritories,g_maxBuildRate,g_maxPlayers);
}

struct PatchStream{const char* buf;size_t pos;size_t size;};
static int PatchRead(const struct bspatch_stream* s,void* b,int n)
{
    PatchStream* ps=(PatchStream*)s->opaque;
    if(ps->pos+n>ps->size)return -1;
    memcpy(b,ps->buf+ps->pos,n);ps->pos+=n;return 0;
}

static bool ClientConnect();

static DWORD WINAPI ClientRecvThread(LPVOID)
{
    H->log("multiplayer  client recv thread started");
    while(g_clientConnected){
        MsgHeader hdr;
        if(!RecvAll(g_clientSock,(char*)&hdr,sizeof(hdr))){
            H->log("multiplayer  client: connection lost (err=%d)",WSAGetLastError());
            g_clientConnected=false;
            if(g_clientShouldReconnect)g_shm.SetStatus(MP_STATUS_CONNECTING,"Reconnecting...");
            else g_shm.SetStatus(MP_STATUS_OFFLINE,"Disconnected");
            break;
        }
        g_totalBytesRecv+=sizeof(hdr);
        g_clientBytesRecv+=sizeof(hdr);
        switch(hdr.type){
        case MSG_PING:
            if(hdr.size>0)DrainSocket(g_clientSock,hdr.size);
            {MsgHeader p={MSG_PONG,0,0};EnterCriticalSection(&g_typeNameLock);send(g_clientSock,(char*)&p,sizeof(p),0);LeaveCriticalSection(&g_typeNameLock);}
            break;
        case MSG_PONG:
            if(hdr.size>0)DrainSocket(g_clientSock,hdr.size);
            {g_clientPing=(DWORD)(GetTickCount64()-g_clientLastPing);H->log("multiplayer  client ping: %dms",g_clientPing);}
            break;
        case MSG_BUILD:
            if(hdr.size==sizeof(BuildCmd)){
                BuildCmd cmd;RecvAll(g_clientSock,(char*)&cmd,sizeof(cmd));g_clientBytesRecv+=sizeof(cmd);
                H->log("multiplayer  client BUILD: %s by %s at (%.0f,%.0f) seq=%u",cmd.typeName,cmd.playerName,cmd.x,cmd.z,cmd.sequenceId);
                ShmAddBuildNotify(&cmd);
                g_totalBuildsSession++;
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_DEMOLISH:
            if(hdr.size==sizeof(DemolishCmd)){
                DemolishCmd cmd;RecvAll(g_clientSock,(char*)&cmd,sizeof(cmd));g_clientBytesRecv+=sizeof(cmd);
                H->log("multiplayer  client DEMOLISH by %s at (%.0f,%.0f)",cmd.playerName,cmd.x,cmd.z);
                g_totalDemolSession++;
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_ROAD:
            if(hdr.size==sizeof(RoadCmd)){
                RoadCmd cmd;RecvAll(g_clientSock,(char*)&cmd,sizeof(cmd));g_clientBytesRecv+=sizeof(cmd);
                const char* cs=cmd.category==ROAD_CAT_PIPE?"PIPE":cmd.category==ROAD_CAT_RAIL?"RAIL":cmd.category==ROAD_CAT_WIRE?"WIRE":"ROAD";
                H->log("multiplayer  client %s by %s (%.0f,%.0f)->(%.0f,%.0f)",cs,cmd.playerName,cmd.x1,cmd.z1,cmd.x2,cmd.z2);
                ShmAddRoadNotify(&cmd);
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_CHAT:
            if(hdr.size>0&&hdr.size<512){
                char buf[512]={};RecvAll(g_clientSock,buf,hdr.size);g_clientBytesRecv+=hdr.size;
                H->log("multiplayer  client chat: %s",buf);
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_PLAYER_LIST:
            if(hdr.size==sizeof(PlayerListPacket)){
                PlayerListPacket pkt;RecvAll(g_clientSock,(char*)&pkt,sizeof(pkt));g_clientBytesRecv+=sizeof(pkt);
                H->log("multiplayer  player list: %d online (server up %us)",pkt.count,pkt.serverUptime);
                if(g_shm.block&&g_shm.Lock()){
                    g_shm.block->playerCount=pkt.count;
                    for(int i=0;i<pkt.count&&i<4;i++){
                        strncpy(g_shm.block->players[i].name,pkt.entries[i].name,63);
                        g_shm.block->players[i].ping=pkt.entries[i].ping;
                        g_shm.block->players[i].connected=1;
                    }
                    g_shm.Unlock();
                }
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_SERVER_INFO:
            if(hdr.size==sizeof(ServerInfoPacket)){
                ServerInfoPacket info;RecvAll(g_clientSock,(char*)&info,sizeof(info));g_clientBytesRecv+=sizeof(info);
                H->log("multiplayer  server info: host=%s players=%d/%d uptime=%us builds=%u",
                       info.hostName,info.playerCount,info.maxPlayers,info.uptime,info.totalBuilds);
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_TERRITORY:
            if(hdr.size==sizeof(TerritoryPacket)){
                TerritoryPacket tp;RecvAll(g_clientSock,(char*)&tp,sizeof(tp));g_clientBytesRecv+=sizeof(tp);
                H->log("multiplayer  client territory: %s %s",tp.playerName,tp.action?"set":"cleared");
            } else DrainSocket(g_clientSock,hdr.size);
            break;
        case MSG_KICK:
            if(hdr.size==sizeof(KickPacket)){
                KickPacket kick;RecvAll(g_clientSock,(char*)&kick,sizeof(kick));g_clientBytesRecv+=sizeof(kick);
                H->log("multiplayer  KICKED: %s (canReconnect=%d)",kick.reason,kick.canReconnect);
                if(!kick.canReconnect)g_clientShouldReconnect=false;
            } else DrainSocket(g_clientSock,hdr.size);
            g_clientConnected=false;
            g_shm.SetStatus(MP_STATUS_OFFLINE,"Kicked from server");
            break;
        case MSG_SAVE_FULL:
        case MSG_SAVE_DIFF:{
            bool diff=(hdr.type==MSG_SAVE_DIFF);
            int fc=0;RecvAll(g_clientSock,(char*)&fc,4);g_clientBytesRecv+=4;
            H->log("multiplayer  client recv save (%s, %d files)",diff?"diff":"full",fc);
            g_shm.SetStatus(MP_STATUS_CONNECTING,diff?"Receiving delta...":"Receiving save...");
            SHCreateDirectoryExA(NULL,g_saveDir,NULL);
            for(int i=0;i<fc;i++){
                int nl=0;RecvAll(g_clientSock,(char*)&nl,4);g_clientBytesRecv+=4;
                char fn[MAX_PATH]={};int rl=nl<MAX_PATH-1?nl:MAX_PATH-1;
                RecvAll(g_clientSock,fn,rl);g_clientBytesRecv+=rl;if(nl>rl)DrainSocket(g_clientSock,nl-rl);
                char fp[MAX_PATH];snprintf(fp,MAX_PATH,"%s\\%s",g_saveDir,fn);
                if(diff){
                    DWORD nSz=0,rSz=0,cSz=0;
                    RecvAll(g_clientSock,(char*)&nSz,4);RecvAll(g_clientSock,(char*)&rSz,4);RecvAll(g_clientSock,(char*)&cSz,4);
                    g_clientBytesRecv+=12;
                    if(nSz&&rSz&&cSz){
                        char* cd=(char*)malloc(cSz);
                        if(cd){
                            RecvAll(g_clientSock,cd,cSz);g_clientBytesRecv+=cSz;
                            char* rd=(char*)malloc(rSz);
                            if(rd){
                                size_t dr=ZSTD_decompress(rd,rSz,cd,cSz);
                                if(!ZSTD_isError(dr)){
                                    DWORD oSz=0;char* od=LoadFile(fp,&oSz);
                                    if(od){
                                        char* nd=(char*)malloc(nSz);
                                        if(nd){
                                            PatchStream ps={rd,0,rSz};
                                            struct bspatch_stream bs={};bs.opaque=&ps;bs.read=PatchRead;
                                            if(bspatch((uint8_t*)od,oSz,(uint8_t*)nd,nSz,&bs)==0)WriteFileTo(fp,nd,nSz);
                                            else H->log("multiplayer  bspatch failed for %s",fn);
                                            free(nd);
                                        }
                                        free(od);
                                    }
                                } else H->log("multiplayer  decompress failed for %s",fn);
                                free(rd);
                            }
                            free(cd);
                        }
                    }
                } else {
                    DWORD oSz=0,cSz2=0;
                    RecvAll(g_clientSock,(char*)&oSz,4);RecvAll(g_clientSock,(char*)&cSz2,4);g_clientBytesRecv+=8;
                    if(oSz&&cSz2){
                        char* cb=(char*)malloc(cSz2);
                        if(cb){
                            RecvAll(g_clientSock,cb,cSz2);g_clientBytesRecv+=cSz2;
                            char* ob=(char*)malloc(oSz);
                            if(ob){
                                size_t r=ZSTD_decompress(ob,oSz,cb,cSz2);
                                if(!ZSTD_isError(r))WriteFileTo(fp,ob,(DWORD)r);
                                else H->log("multiplayer  decompress failed for %s",fn);
                                free(ob);
                            }
                            free(cb);
                        }
                    }
                }
            }
            H->log("multiplayer  save received OK (%uKB)",g_clientBytesRecv/1024);
            g_shm.SetStatus(MP_STATUS_CONNECTED,diff?"Delta applied — reload save":"Done! Load mp_client save");
            break;}
        default:
            if(hdr.size>0)DrainSocket(g_clientSock,hdr.size);break;
        }
    }
    if(g_clientSock!=INVALID_SOCKET){closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;}
    H->log("multiplayer  client recv thread exited");
    return 0;
}

static DWORD WINAPI ClientPingThread(LPVOID)
{
    while(g_clientConnected){
        Sleep(PING_INTERVAL_MS);if(!g_clientConnected)break;
        g_clientLastPing=GetTickCount64();
        MsgHeader hdr={MSG_PING,0,0};
        EnterCriticalSection(&g_typeNameLock);send(g_clientSock,(char*)&hdr,sizeof(hdr),0);LeaveCriticalSection(&g_typeNameLock);
        g_clientBytesSent+=sizeof(hdr);
    }
    return 0;
}

static DWORD WINAPI ClientReconnectThread(LPVOID)
{
    while(g_clientShouldReconnect){
        Sleep(RECONNECT_INTERVAL_MS);
        if(!g_clientShouldReconnect||g_clientConnected)break;
        g_reconnectAttempts++;
        if(g_reconnectAttempts>MAX_RECONNECT_ATTEMPTS){
            H->log("multiplayer  client: max reconnect attempts (%d) reached",MAX_RECONNECT_ATTEMPTS);
            g_clientShouldReconnect=false;
            g_shm.SetStatus(MP_STATUS_OFFLINE,"Reconnect failed — try manually");
            break;
        }
        H->log("multiplayer  client: reconnect attempt %d/%d to %s:%d",g_reconnectAttempts,MAX_RECONNECT_ATTEMPTS,g_hostIp,g_port);
        char sm[128];snprintf(sm,128,"Reconnecting... (%d/%d)",g_reconnectAttempts,MAX_RECONNECT_ATTEMPTS);
        g_shm.SetStatus(MP_STATUS_CONNECTING,sm);
        if(ClientConnect()){g_reconnectAttempts=0;H->log("multiplayer  client: reconnect OK");break;}
    }
    return 0;
}

static bool ClientConnect()
{
    H->log("multiplayer  client: connecting to %s:%d...",g_hostIp,g_port);
    g_clientSock=socket(AF_INET,SOCK_STREAM,0);
    if(g_clientSock==INVALID_SOCKET){H->log("multiplayer  client: socket() failed err=%d",WSAGetLastError());return false;}
    int sndb=64*1024,rcvb=64*1024;
    setsockopt(g_clientSock,SOL_SOCKET,SO_SNDBUF,(char*)&sndb,sizeof(sndb));
    setsockopt(g_clientSock,SOL_SOCKET,SO_RCVBUF,(char*)&rcvb,sizeof(rcvb));
    DWORD timeout=CONNECT_TIMEOUT_MS;
    setsockopt(g_clientSock,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));
    sockaddr_in addr={};addr.sin_family=AF_INET;addr.sin_port=htons((u_short)g_port);
    if(inet_pton(AF_INET,g_hostIp,&addr.sin_addr)!=1){
        H->log("multiplayer  client: invalid IP: %s",g_hostIp);
        closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;return false;
    }
    if(connect(g_clientSock,(sockaddr*)&addr,sizeof(addr))!=0){
        H->log("multiplayer  client: connect failed to %s:%d err=%d",g_hostIp,g_port,WSAGetLastError());
        closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;return false;
    }
    timeout=0;setsockopt(g_clientSock,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));
    H->log("multiplayer  client: TCP connected to %s:%d",g_hostIp,g_port);
    MsgHeader hsHdr;
    if(!RecvAll(g_clientSock,(char*)&hsHdr,sizeof(hsHdr))||hsHdr.type!=MSG_HANDSHAKE){
        H->log("multiplayer  client: no server handshake");closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;return false;
    }
    HandshakePacket serverHs;RecvAll(g_clientSock,(char*)&serverHs,sizeof(serverHs));
    H->log("multiplayer  client: server=%s proto=%d mode=%s",serverHs.playerName,serverHs.protocolVersion,serverHs.gameMode);
    if(serverHs.protocolVersion!=MP_PROTOCOL_VER){
        H->log("multiplayer  client: protocol mismatch (%d vs %d)",serverHs.protocolVersion,MP_PROTOCOL_VER);
        closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;return false;
    }
    HandshakePacket hs={};hs.protocolVersion=MP_PROTOCOL_VER;
    hs.versionMajor=MP_VERSION_MAJOR;hs.versionMinor=MP_VERSION_MINOR;hs.versionPatch=MP_VERSION_PATCH;
    strncpy(hs.playerName,g_playerName,63);strncpy(hs.gameMode,"client",15);
    strncpy(hs.password,g_password,63);hs.flags=0;
    MsgHeader sh={MSG_HANDSHAKE,sizeof(hs),CalcChecksum(&hs,sizeof(hs))};
    send(g_clientSock,(char*)&sh,sizeof(sh),0);send(g_clientSock,(char*)&hs,sizeof(hs),0);
    g_clientConnected=true;g_clientBytesSent=0;g_clientBytesRecv=0;
    g_clientRecvThread=CreateThread(NULL,0,ClientRecvThread,NULL,0,NULL);
    CreateThread(NULL,0,ClientPingThread,NULL,0,NULL);
    H->log("multiplayer  client: connected OK as %s",g_playerName);
    return true;
}

static void ClientSendBuild(const BuildCmd* cmd)
{
    if(!g_clientConnected||g_clientSock==INVALID_SOCKET)return;
    MsgHeader hdr={MSG_BUILD,sizeof(BuildCmd),CalcChecksum(cmd,sizeof(BuildCmd))};
    EnterCriticalSection(&g_typeNameLock);
    send(g_clientSock,(char*)&hdr,sizeof(hdr),0);
    send(g_clientSock,(char*)cmd,sizeof(BuildCmd),0);
    LeaveCriticalSection(&g_typeNameLock);
    g_clientBytesSent+=sizeof(hdr)+sizeof(BuildCmd);
    H->log("multiplayer  client sent BUILD: %s at (%.0f,%.0f) seq=%u",cmd->typeName,cmd->x,cmd->z,cmd->sequenceId);
}

static void ClientSendRoad(const RoadCmd* cmd)
{
    if(!g_clientConnected||g_clientSock==INVALID_SOCKET)return;
    MsgHeader hdr={MSG_ROAD,sizeof(RoadCmd),CalcChecksum(cmd,sizeof(RoadCmd))};
    EnterCriticalSection(&g_typeNameLock);
    send(g_clientSock,(char*)&hdr,sizeof(hdr),0);
    send(g_clientSock,(char*)cmd,sizeof(RoadCmd),0);
    LeaveCriticalSection(&g_typeNameLock);
    g_clientBytesSent+=sizeof(hdr)+sizeof(RoadCmd);
    H->log("multiplayer  client sent ROAD cat=%d (%.0f,%.0f)->(%.0f,%.0f)",cmd->category,cmd->x1,cmd->z1,cmd->x2,cmd->z2);
}

static void ClientSendDemolish(const DemolishCmd* cmd)
{
    if(!g_clientConnected||g_clientSock==INVALID_SOCKET)return;
    MsgHeader hdr={MSG_DEMOLISH,sizeof(DemolishCmd),CalcChecksum(cmd,sizeof(DemolishCmd))};
    EnterCriticalSection(&g_typeNameLock);
    send(g_clientSock,(char*)&hdr,sizeof(hdr),0);
    send(g_clientSock,(char*)cmd,sizeof(DemolishCmd),0);
    LeaveCriticalSection(&g_typeNameLock);
    g_clientBytesSent+=sizeof(hdr)+sizeof(DemolishCmd);
    H->log("multiplayer  client sent DEMOLISH at (%.0f,%.0f)",cmd->x,cmd->z);
}

static void ShmUpdateStatus()
{
    if(!g_shm.block||!g_shm.Lock())return;
    if(strcmp(g_mode,"host")==0){
        g_shm.block->status=MP_STATUS_HOST;
        snprintf(g_shm.block->statusText,255,"Host %d/%d | port %d | up %us",
                 g_playerCount,(int)g_maxPlayers,g_port,(DWORD)((GetTickCount64()-g_sessionStart)/1000));
    } else {
        g_shm.block->status=g_clientConnected?MP_STATUS_CONNECTED:MP_STATUS_OFFLINE;
        if(g_clientConnected)snprintf(g_shm.block->statusText,255,"Connected to %s:%d | ping %ums",g_hostIp,g_port,g_clientPing);
        else if(g_clientShouldReconnect)snprintf(g_shm.block->statusText,255,"Reconnecting... (%d/%d)",g_reconnectAttempts,MAX_RECONNECT_ATTEMPTS);
        else strncpy(g_shm.block->statusText,"Offline",255);
    }
    g_shm.block->playerCount   =g_playerCount;
    g_shm.block->totalBuilds   =g_totalBuildsSession;
    g_shm.block->bytesSent     =g_totalBytesSent;
    g_shm.block->sessionUptime =(DWORD)((GetTickCount64()-g_sessionStart)/1000);
    int n=0;
    for(int i=0;i<MAX_PLAYERS&&n<4;i++){
        if(!g_players[i].connected)continue;
        strncpy(g_shm.block->players[n].name,g_players[i].name,63);
        g_shm.block->players[n].ping=g_players[i].ping;
        g_shm.block->players[n].connected=1;n++;
    }
    for(int i=n;i<4;i++)memset(&g_shm.block->players[i],0,sizeof(PlayerStatus));
    g_shm.Unlock();SetEvent(g_shm.hStatusEvent);
}

static void ShmAddBuildNotify(const BuildCmd* cmd)
{
    if(!g_shm.block||!g_shm.Lock())return;
    DWORD idx=g_shm.block->buildNotifyCount%MAX_BUILD_NOTIFY;
    BuildNotify& n=g_shm.block->buildNotify[idx];
    strncpy(n.playerName,cmd->playerName,63);strncpy(n.typeName,cmd->typeName,127);
    n.x=cmd->x;n.z=cmd->z;n.timestamp=GetTickCount();
    g_shm.block->buildNotifyCount++;
    g_shm.Unlock();
}

static void ShmAddRoadNotify(const RoadCmd* cmd)
{

    if(!g_shm.block||!g_shm.Lock())return;
    DWORD idx=g_shm.block->buildNotifyCount%MAX_BUILD_NOTIFY;
    BuildNotify& n=g_shm.block->buildNotify[idx];
    strncpy(n.playerName,cmd->playerName,63);
    const char* cs=cmd->category==ROAD_CAT_PIPE?"pipe":cmd->category==ROAD_CAT_RAIL?"rail":cmd->category==ROAD_CAT_WIRE?"wire":"road";
    strncpy(n.typeName,cs,127);
    n.x=cmd->x1;n.z=cmd->z1;n.timestamp=GetTickCount();
    g_shm.block->buildNotifyCount++;
    g_shm.Unlock();
}

static DWORD WINAPI ShmControlThread(LPVOID)
{
    H->log("multiplayer  shm control thread started");
    Sleep(500);
    if(!g_shm.block||g_shm.block->magic!=SHARED_MAGIC){H->log("multiplayer  shm block invalid");return 1;}
    while(true){
        if(g_shm.HasCommand()){
            if(!g_shm.Lock()){Sleep(100);continue;}
            BYTE cmd=g_shm.block->command;
            char p1[64],p2[64],p3[64];
            strncpy(p1,g_shm.block->cmdParam1,63);
            strncpy(p2,g_shm.block->cmdParam2,63);
            strncpy(p3,g_shm.block->cmdParam3,63);
            DWORD val=g_shm.block->cmdValue;
            g_shm.block->command=MP_CMD_NONE;
            g_shm.Unlock();
            switch(cmd){
            case MP_CMD_CONNECT:
                H->log("multiplayer  shm CMD_CONNECT ip=%s port=%s name=%s",p1,p2,p3);
                strncpy(g_hostIp,p1,63);g_port=atoi(p2[0]?p2:"7777");
                if(p3[0])strncpy(g_playerName,p3,63);
                strncpy(g_mode,"client",31);
                if(g_clientConnected){
                    g_clientShouldReconnect=false;g_clientConnected=false;
                    if(g_clientSock!=INVALID_SOCKET){closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;}
                    Sleep(500);
                }
                g_reconnectAttempts=0;g_clientShouldReconnect=true;
                g_shm.SetStatus(MP_STATUS_CONNECTING,"Connecting...");
                if(ClientConnect()){H->log("multiplayer  shm: connect OK");ShmUpdateStatus();}
                else{H->log("multiplayer  shm: connect FAILED");g_shm.SetStatus(MP_STATUS_CONNECTING,"Failed, retrying...");CreateThread(NULL,0,ClientReconnectThread,NULL,0,NULL);}
                break;
            case MP_CMD_DISCONNECT:
                H->log("multiplayer  shm CMD_DISCONNECT");
                g_clientShouldReconnect=false;
                if(g_clientConnected){g_clientConnected=false;if(g_clientSock!=INVALID_SOCKET){closesocket(g_clientSock);g_clientSock=INVALID_SOCKET;}}
                g_shm.SetStatus(MP_STATUS_OFFLINE,"Disconnected");break;
            case MP_CMD_CHAT:
                if(strcmp(g_mode,"host")==0){char msg[320];snprintf(msg,320,"[%s]: %s",g_playerName,p1);BroadcastChat(msg);}
                else if(g_clientConnected){
                    char msg[320];snprintf(msg,320,"[%s]: %s",g_playerName,p1);
                    MsgHeader hdr={MSG_CHAT,(DWORD)strlen(msg),0};
                    EnterCriticalSection(&g_typeNameLock);send(g_clientSock,(char*)&hdr,sizeof(hdr),0);send(g_clientSock,msg,(int)strlen(msg),0);LeaveCriticalSection(&g_typeNameLock);
                }
                break;
            case MP_CMD_RELOAD_CFG:ReloadConfig();ShmUpdateStatus();break;
            case MP_CMD_KICK:
                H->log("multiplayer  shm CMD_KICK slot=%u reason=%s",val,p1);
                if(val<MAX_PLAYERS&&g_players[val].connected){
                    KickPacket kick;snprintf(kick.reason,sizeof(kick.reason),"Kicked by host%s%s",p1[0]?": ":"",p1);kick.canReconnect=0;
                    SendMsgToPlayer(g_players[val],MSG_KICK,&kick,sizeof(kick));Sleep(200);closesocket(g_players[val].sock);
                }
                break;
            }
        }
        ShmUpdateStatus();Sleep(500);
    }
    return 0;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void){return TSM_API_VERSION;}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    H=host;info->name="multiplayer";info->version = "0.7.1";
    if(!H->configInt("plugins\\multiplayer.ini","multiplayer","enabled",1))return 1;
    H->configString("plugins\\multiplayer.ini","multiplayer","mode",g_mode,sizeof(g_mode),"host");
    H->configString("plugins\\multiplayer.ini","multiplayer","host_ip",g_hostIp,sizeof(g_hostIp),"127.0.0.1");
    H->configString("plugins\\multiplayer.ini","multiplayer","name",g_playerName,sizeof(g_playerName),"Player");
    H->configString("plugins\\multiplayer.ini","multiplayer","password",g_password,sizeof(g_password),"");
    H->configString("plugins\\multiplayer.ini","multiplayer","save_dir",g_saveDir,sizeof(g_saveDir),"media_soviet/save");
    H->configString("plugins\\multiplayer.ini","multiplayer","sync_dir",g_syncDir,sizeof(g_syncDir),"mp_sync");
    H->configString("plugins\\multiplayer.ini","multiplayer","log_dir",g_logDir,sizeof(g_logDir),"mp_logs");
    g_port              =H->configInt("plugins\\multiplayer.ini","multiplayer","port",7777);
    g_enableDemolishSync=H->configInt("plugins\\multiplayer.ini","multiplayer","demolish_sync",1)!=0;
    g_enableRoadSync    =H->configInt("plugins\\multiplayer.ini","multiplayer","road_sync",0)!=0;
    g_enablePipeSync    =H->configInt("plugins\\multiplayer.ini","multiplayer","pipe_sync",0)!=0;
    g_enableRailSync    =H->configInt("plugins\\multiplayer.ini","multiplayer","rail_sync",0)!=0;
    g_enableWireSync    =H->configInt("plugins\\multiplayer.ini","multiplayer","wire_sync",0)!=0;
    g_enableAntiSpam    =H->configInt("plugins\\multiplayer.ini","multiplayer","anti_spam",1)!=0;
    g_enableTerritories =H->configInt("plugins\\multiplayer.ini","multiplayer","territories",0)!=0;
    g_enableSessionLog  =H->configInt("plugins\\multiplayer.ini","multiplayer","session_log",1)!=0;
    g_maxBuildRate      =H->configInt("plugins\\multiplayer.ini","multiplayer","max_build_rate",MAX_BUILD_RATE);
    g_maxPlayers        =H->configInt("plugins\\multiplayer.ini","multiplayer","max_players",MAX_PLAYERS);
    if(g_maxPlayers<1)g_maxPlayers=1;if(g_maxPlayers>MAX_PLAYERS)g_maxPlayers=MAX_PLAYERS;
    if(!g_playerName[0])strncpy(g_playerName,"Player",63);
    memset(g_players,0,sizeof(g_players));memset(g_territories,0,sizeof(g_territories));memset(g_buildLog,0,sizeof(g_buildLog));memset(g_chatLog,0,sizeof(g_chatLog));
    for(int i=0;i<MAX_PLAYERS;i++){g_players[i].sock=INVALID_SOCKET;g_players[i].slot=(DWORD)i;InitializeCriticalSection(&g_players[i].netLock);}
    InitializeCriticalSection(&g_lock);InitializeCriticalSection(&g_typeNameLock);
    InitializeCriticalSection(&g_logLock);InitializeCriticalSection(&g_buildLogLock);InitializeCriticalSection(&g_dedupLock);
    JournalInit();
    H->log("multiplayer  v%d.%d.%d loaded | mode=%s name=%s port=%d maxplayers=%d",MP_VERSION_MAJOR,MP_VERSION_MINOR,MP_VERSION_PATCH,g_mode,g_playerName,g_port,g_maxPlayers);
    H->log("multiplayer  config: demolish=%d road=%d spam=%d terr=%d rate=%d log=%d",g_enableDemolishSync,g_enableRoadSync,g_enableAntiSpam,g_enableTerritories,g_maxBuildRate,g_enableSessionLog);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    H->log("multiplayer  starting...");
    g_sessionStart=GetTickCount64();
    if(g_enableSessionLog)CreateDirectoryA(g_logDir,NULL);
    if(g_shm.Create(true)){
        H->log("multiplayer  shared memory created: %s (size %d bytes)",MP_SHARED_NAME,MP_SHARED_SIZE);
        __try{
            g_shm.block->pluginVersion=(MP_VERSION_MAJOR<<16)|(MP_VERSION_MINOR<<8)|MP_VERSION_PATCH;
            g_shm.block->status=MP_STATUS_OFFLINE;g_shm.block->playerCount=0;g_shm.block->buildNotifyCount=0;
            memset(g_shm.block->statusText,0,sizeof(g_shm.block->statusText));
            memset(g_shm.block->players,0,sizeof(g_shm.block->players));
        }__except(EXCEPTION_EXECUTE_HANDLER){H->log("multiplayer  shm block init exception");}
        g_shmThread=CreateThread(NULL,0,ShmControlThread,NULL,0,NULL);
    } else H->log("multiplayer  shared memory failed (non-fatal)");
    CreateDirectoryA(g_syncDir,NULL);
    WSADATA wsa;WSAStartup(MAKEWORD(2,2),&wsa);
    if(strcmp(g_mode,"host")==0){
        g_serverThread=CreateThread(NULL,0,ServerThread,NULL,0,NULL);
        g_watchThread =CreateThread(NULL,0,WatchThread, NULL,0,NULL);
        g_statsThread =CreateThread(NULL,0,StatsThread,  NULL,0,NULL);
        CreateThread(NULL,0,HeartbeatThread,NULL,0,NULL);
        H->log("multiplayer  host mode: server started on port %d (max %d players)",g_port,g_maxPlayers);
    } else {
        H->log("multiplayer  client mode: will connect to %s:%d as %s",g_hostIp,g_port,g_playerName);
        g_clientShouldReconnect=true;
        Sleep(2000);
        if(!ClientConnect()){H->log("multiplayer  client: initial connect failed, starting reconnect thread");CreateThread(NULL,0,ClientReconnectThread,NULL,0,NULL);}
    }
    g_hExe=GetModuleHandleA("SOVIET64.exe");
    if(!g_hExe){H->log("multiplayer  ERROR: SOVIET64.exe not found");return 0;}
    H->log("multiplayer  exe base: %p",g_hExe);
    bool hookBuild=InstallHook(HOOK_BUILD_RVA,(void*)HookedBuildHandler,(void**)&g_origBuildHandler,g_buildOrigBytes,"build",14);
    bool hookPlace=InstallHook(HOOK_PLACE_RVA,(void*)HookedPlaceHandler,(void**)&g_origPlaceHandler,g_placeOrigBytes,"place",16);
    bool hookDemol=false;
    if(g_enableDemolishSync){
        BYTE* dt=(BYTE*)((DWORD_PTR)g_hExe+HOOK_DEMOL_RVA);
        if(dt[0]==0x48&&dt[1]==0x8B&&dt[2]==0xC4)
            hookDemol=InstallHook(HOOK_DEMOL_RVA,(void*)HookedDemolHandler,(void**)&g_origDemolHandler,g_demolOrigBytes,"demolish",14);
        else H->log("multiplayer  demolish hook: offset mismatch at RVA 0x%X got %02X %02X %02X",HOOK_DEMOL_RVA,dt[0],dt[1],dt[2]);
    }
    H->log("multiplayer  hooks: build=%d place=%d demolish=%d",hookBuild,hookPlace,hookDemol);

    bool hookRoad=false,hookPipe=false,hookRail=false;
    if(g_enableRoadSync){
        BYTE* rt=(BYTE*)((DWORD_PTR)g_hExe+HOOK_ROAD_RVA);
        if(rt[0]==0x48&&rt[1]==0x8B&&rt[2]==0xC4)
            hookRoad=InstallHook(HOOK_ROAD_RVA,(void*)HookedRoadHandler,(void**)&g_origRoadHandler,g_roadOrigBytes,"road",14);
        else H->log("multiplayer  road hook: offset mismatch at RVA 0x%X got %02X %02X %02X",HOOK_ROAD_RVA,rt[0],rt[1],rt[2]);
    }
    if(g_enablePipeSync&&HOOK_PIPE_RVA){
        BYTE* pt=(BYTE*)((DWORD_PTR)g_hExe+HOOK_PIPE_RVA);
        if(pt[0]==0x48&&pt[1]==0x8B&&pt[2]==0xC4)
            hookPipe=InstallHook(HOOK_PIPE_RVA,(void*)HookedPipeHandler,(void**)&g_origPipeHandler,g_pipeOrigBytes,"pipe",14);
        else H->log("multiplayer  pipe hook: offset mismatch at RVA 0x%X got %02X %02X %02X",HOOK_PIPE_RVA,pt[0],pt[1],pt[2]);
    }
    if(g_enableRailSync&&HOOK_RAIL_RVA){
        BYTE* lt=(BYTE*)((DWORD_PTR)g_hExe+HOOK_RAIL_RVA);
        if(lt[0]==0x48&&lt[1]==0x8B&&lt[2]==0xC4)
            hookRail=InstallHook(HOOK_RAIL_RVA,(void*)HookedRailHandler,(void**)&g_origRailHandler,g_railOrigBytes,"rail",14);
        else H->log("multiplayer  rail hook: offset mismatch at RVA 0x%X got %02X %02X %02X",HOOK_RAIL_RVA,lt[0],lt[1],lt[2]);
    }
    H->log("multiplayer  infra hooks: road=%d pipe=%d rail=%d",hookRoad,hookPipe,hookRail);

    if(hookBuild&&hookPlace){
        H->log("multiplayer  v%d.%d.%d ACTIVE — realtime sync ON",MP_VERSION_MAJOR,MP_VERSION_MINOR,MP_VERSION_PATCH);
        if(g_enableTerritories)H->log("multiplayer  territories: enabled");
        if(g_enableSessionLog)H->log("multiplayer  session log: %s",g_logDir);
    } else {
        H->log("multiplayer  WARNING: hooks failed — save-sync only mode");
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if(reason==DLL_PROCESS_DETACH){
        g_clientShouldReconnect=false;
        if(g_enableSessionLog&&g_buildLogCount>0)FlushSessionLog();
        g_shm.Destroy();
        if(g_origBuildHandler)RemoveHook(HOOK_BUILD_RVA,g_buildOrigBytes,14);
        if(g_origPlaceHandler)RemoveHook(HOOK_PLACE_RVA,g_placeOrigBytes,16);
        if(g_origDemolHandler)RemoveHook(HOOK_DEMOL_RVA,g_demolOrigBytes,14);
        if(g_origRoadHandler)RemoveHook(HOOK_ROAD_RVA,g_roadOrigBytes,14);
        if(g_origPipeHandler)RemoveHook(HOOK_PIPE_RVA,g_pipeOrigBytes,14);
        if(g_origRailHandler)RemoveHook(HOOK_RAIL_RVA,g_railOrigBytes,14);
    }
    return TRUE;
}