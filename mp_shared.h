#pragma once
#include <windows.h>

#define MP_SHARED_NAME    "WRSRMp_Control"
#define MP_SHARED_SIZE    16384
#define MP_MUTEX_NAME     "WRSRMp_Mutex"
#define MP_EVENT_CMD      "WRSRMp_CmdEvent"
#define MP_EVENT_STATUS   "WRSRMp_StatusEvent"

#define MP_CMD_NONE       0
#define MP_CMD_CONNECT    1
#define MP_CMD_DISCONNECT 2
#define MP_CMD_RELOAD_CFG 3
#define MP_CMD_KICK       4
#define MP_CMD_CHAT       5

#define MP_STATUS_OFFLINE     0
#define MP_STATUS_CONNECTING  1
#define MP_STATUS_CONNECTED   2
#define MP_STATUS_ERROR       3
#define MP_STATUS_HOST        4

#pragma pack(push, 1)

struct PlayerStatus {
    char  name[64];
    DWORD ping;
    BYTE  connected;
};

struct BuildNotify {
    char  playerName[64];
    char  typeName[128];
    float x, z;
    DWORD timestamp;
};

#define MAX_BUILD_NOTIFY 16

struct SharedBlock {
    DWORD   magic;
    DWORD   version;

    BYTE    command;
    char    cmdParam1[64];
    char    cmdParam2[64];
    char    cmdParam3[64];
    DWORD   cmdValue;

    BYTE    status;
    char    statusText[256];
    DWORD   ping;
    DWORD   playerCount;
    DWORD   totalBuilds;
    DWORD   bytesSent;
    DWORD   sessionUptime;

    PlayerStatus players[4];

    DWORD       buildNotifyCount;
    BuildNotify buildNotify[MAX_BUILD_NOTIFY];

    char    lastError[256];
    DWORD   pluginVersion;
};

#pragma pack(pop)

#define SHARED_MAGIC   0x5753524D
#define SHARED_VERSION 1

class SharedMemory {
public:
    HANDLE      hMap   = NULL;
    HANDLE      hMutex = NULL;
    HANDLE      hCmdEvent    = NULL;
    HANDLE      hStatusEvent = NULL;
    SharedBlock* block = nullptr;
    bool        owner  = false;

    bool Create(bool isOwner)
    {
        owner = isOwner;
        if (isOwner) {
            hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                PAGE_READWRITE, 0, MP_SHARED_SIZE, MP_SHARED_NAME);
        } else {
            for (int attempt = 0; attempt < 3; attempt++) {
                hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, MP_SHARED_NAME);
                if (hMap) break;
                Sleep(500);
            }
        }
        if (!hMap) return false;

        block = (SharedBlock*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, MP_SHARED_SIZE);
        if (!block) { CloseHandle(hMap); hMap = NULL; return false; }

        hMutex = CreateMutexA(NULL, FALSE, MP_MUTEX_NAME);
        hCmdEvent    = CreateEventA(NULL, FALSE, FALSE, MP_EVENT_CMD);
        hStatusEvent = CreateEventA(NULL, FALSE, FALSE, MP_EVENT_STATUS);

        if (isOwner) {
            memset(block, 0, MP_SHARED_SIZE);
            block->magic   = SHARED_MAGIC;
            block->version = SHARED_VERSION;
            block->status  = MP_STATUS_OFFLINE;
        }
        return true;
    }

    void Destroy()
    {
        if (block)        { UnmapViewOfFile(block); block = NULL; }
        if (hMap)         { CloseHandle(hMap);         hMap = NULL; }
        if (hMutex)       { CloseHandle(hMutex);       hMutex = NULL; }
        if (hCmdEvent)    { CloseHandle(hCmdEvent);    hCmdEvent = NULL; }
        if (hStatusEvent) { CloseHandle(hStatusEvent); hStatusEvent = NULL; }
    }

    bool Lock(DWORD ms = 2000)
    {
        return WaitForSingleObject(hMutex, ms) == WAIT_OBJECT_0;
    }

    void Unlock() { ReleaseMutex(hMutex); }

    void SendCommand(BYTE cmd, const char* p1 = "", const char* p2 = "",
                     const char* p3 = "", DWORD val = 0)
    {
        if (!Lock()) return;
        block->command = cmd;
        strncpy(block->cmdParam1, p1 ? p1 : "", 63);
        strncpy(block->cmdParam2, p2 ? p2 : "", 63);
        strncpy(block->cmdParam3, p3 ? p3 : "", 63);
        block->cmdValue = val;
        Unlock();
        SetEvent(hCmdEvent);
    }

    void SetStatus(BYTE status, const char* text = "")
    {
        if (!Lock()) return;
        block->status = status;
        strncpy(block->statusText, text ? text : "", 255);
        Unlock();
        SetEvent(hStatusEvent);
    }

    bool HasCommand()
    {
        return WaitForSingleObject(hCmdEvent, 0) == WAIT_OBJECT_0;
    }

    bool WaitStatus(DWORD ms = 100)
    {
        return WaitForSingleObject(hStatusEvent, ms) == WAIT_OBJECT_0;
    }
};