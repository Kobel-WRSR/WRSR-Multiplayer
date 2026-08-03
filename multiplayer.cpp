#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../../src/tesmio_api.h"
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static const TsmHost* H = nullptr;
static char g_saveDir[MAX_PATH];
static char g_syncDir[MAX_PATH];
static char g_mode[32];
static char g_hostIp[64];
static int  g_port = 7777;
static HANDLE g_watchThread  = NULL;
static HANDLE g_serverThread = NULL;
static SOCKET g_clientSocket = INVALID_SOCKET;
static CRITICAL_SECTION g_socketLock;

static void SendFile(SOCKET sock, const char* path)
{
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD fileSize = GetFileSize(hFile, NULL);
    send(sock, (char*)&fileSize, sizeof(DWORD), 0);
    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0)
        send(sock, buf, bytesRead, 0);
    CloseHandle(hFile);
}

static void RecvFile(SOCKET sock, const char* path)
{
    DWORD fileSize = 0;
    recv(sock, (char*)&fileSize, sizeof(DWORD), MSG_WAITALL);
    if (fileSize == 0 || fileSize > 512 * 1024 * 1024) return;
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    char buf[4096];
    DWORD remaining = fileSize;
    while (remaining > 0) {
        int toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        int got = recv(sock, buf, toRead, MSG_WAITALL);
        if (got <= 0) break;
        DWORD written;
        WriteFile(hFile, buf, got, &written, NULL);
        remaining -= got;
    }
    CloseHandle(hFile);
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

        char scriptPath[MAX_PATH];
        snprintf(scriptPath, MAX_PATH, "%s\\%s\\script.ini", g_saveDir, fd.cFileName);
        WIN32_FIND_DATAA fd2;
        HANDLE h2 = FindFirstFileA(scriptPath, &fd2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        FindClose(h2);

        if (CompareFileTime(&fd2.ftLastWriteTime, &latest) > 0) {
            latest = fd2.ftLastWriteTime;
            strcpy(outName, fd.cFileName);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

static void SyncSaveToClient(const char* saveName)
{
    EnterCriticalSection(&g_socketLock);
    if (g_clientSocket == INVALID_SOCKET) {
        LeaveCriticalSection(&g_socketLock);
        H->log("multiplayer  no client connected, skip sync");
        return;
    }
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\%s\\*", g_saveDir, saveName);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_socketLock);
        return;
    }
    int fileCount = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            fileCount++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    send(g_clientSocket, (char*)&fileCount, sizeof(int), 0);
    hFind = FindFirstFileA(pattern, &fd);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int nameLen = (int)strlen(fd.cFileName) + 1;
        send(g_clientSocket, (char*)&nameLen, sizeof(int), 0);
        send(g_clientSocket, fd.cFileName, nameLen, 0);
        char fullPath[MAX_PATH];
        snprintf(fullPath, MAX_PATH, "%s\\%s\\%s", g_saveDir, saveName, fd.cFileName);
        SendFile(g_clientSocket, fullPath);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    LeaveCriticalSection(&g_socketLock);
    H->log("multiplayer  save sent: %s", saveName);
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
    listen(listenSock, 1);
    H->log("multiplayer  server listening on port %d", g_port);
    while (true) {
        SOCKET client = accept(listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        H->log("multiplayer  client connected!");
        EnterCriticalSection(&g_socketLock);
        if (g_clientSocket != INVALID_SOCKET)
            closesocket(g_clientSocket);
        g_clientSocket = client;
        LeaveCriticalSection(&g_socketLock);
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
            char latestSave[MAX_PATH];
            GetLatestSave(latestSave);
            if (latestSave[0]) {
                H->log("multiplayer  detected save: %s", latestSave);
                SyncSaveToClient(latestSave);
            }
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
    info->version = "0.0.5";
    if (!H->configInt("plugins\\multiplayer.ini", "multiplayer", "enabled", 1))
        return 1;
    H->configString("plugins\\multiplayer.ini", "multiplayer", "mode",
                    g_mode, sizeof(g_mode), "host");
    H->configString("plugins\\multiplayer.ini", "multiplayer", "host_ip",
                    g_hostIp, sizeof(g_hostIp), "127.0.0.1");
    g_port = H->configInt("plugins\\multiplayer.ini", "multiplayer", "port", 7777);
    H->log("multiplayer  v0.0.5 loaded OK (mode: %s)", g_mode);
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
    InitializeCriticalSection(&g_socketLock);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_serverThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    g_watchThread  = CreateThread(NULL, 0, WatchThread,  NULL, 0, NULL);
    H->log("multiplayer  HOST mode started on port %d", g_port);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }