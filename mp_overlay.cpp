#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "zstd.h"

extern "C" {
#include "bspatch.h"
}

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

#define ID_BTN_CONNECT  101
#define ID_BTN_CHAT     102
#define ID_EDIT_IP      103
#define ID_EDIT_NAME    104
#define ID_EDIT_CHAT    105
#define ID_LIST_PLAYERS 106
#define ID_RADIO_HOST   107
#define ID_RADIO_CLIENT 108
#define ID_PROGRESS     109
#define ID_LBL_PROGRESS 110
#define TIMER_PING      1001

#define MSG_SAVE      1
#define MSG_PING      2
#define MSG_PONG      3
#define MSG_CHAT      4
#define MSG_SAVE_FULL 5
#define MSG_SAVE_DIFF 6

#define WM_UPDATE_STATUS   (WM_USER + 1)
#define WM_UPDATE_PING     (WM_USER + 2)
#define WM_UPDATE_PROGRESS (WM_USER + 3)
#define WM_APPEND_CHAT     (WM_USER + 4)
#define WM_DISCONNECTED    (WM_USER + 5)

static const char* g_saveDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save\\mp_client";

static HWND g_hwnd;
static HWND g_btnConnect;
static HWND g_editIp;
static HWND g_editName;
static HWND g_editChat;
static HWND g_listPlayers;
static HWND g_radioHost;
static HWND g_radioClient;
static HWND g_lblStatus;
static HWND g_lblPing;
static HWND g_progress;
static HWND g_lblProgress;
static HFONT g_hFont = NULL;

static SOCKET g_sock = INVALID_SOCKET;
static bool g_connected = false;
static char g_playerName[64] = "Player";
static ULONGLONG g_lastPing = 0;
static HANDLE g_recvThread = NULL;
static CRITICAL_SECTION g_sendLock;

struct MsgHeader {
    BYTE type;
    DWORD size;
};

struct PatchStream {
    const char* buf;
    size_t pos;
    size_t size;
};

static int PatchRead(const struct bspatch_stream* stream, void* buffer, int length)
{
    PatchStream* ps = (PatchStream*)stream->opaque;
    if (ps->pos + (size_t)length > ps->size) return -1;
    memcpy(buffer, ps->buf + ps->pos, length);
    ps->pos += length;
    return 0;
}

static bool SendExact(SOCKET sock, const char* buf, DWORD size)
{
    DWORD sent = 0;
    while (sent < size) {
        int res = send(sock, buf + sent, size - sent, 0);
        if (res <= 0) return false;
        sent += res;
    }
    return true;
}

static bool RecvExact(SOCKET sock, char* buf, DWORD size)
{
    DWORD remaining = size;
    while (remaining > 0) {
        int got = recv(sock, buf + (size - remaining), remaining, 0);
        if (got <= 0) return false;
        remaining -= got;
    }
    return true;
}

static void SendMsg(SOCKET sock, BYTE type, const void* data, DWORD size)
{
    MsgHeader hdr; hdr.type = type; hdr.size = size;
    EnterCriticalSection(&g_sendLock);
    SendExact(sock, (char*)&hdr, sizeof(hdr));
    if (data && size > 0) SendExact(sock, (char*)data, size);
    LeaveCriticalSection(&g_sendLock);
}

static void PostStatus(const char* text)
{
    char* copy = _strdup(text);
    if (copy) PostMessage(g_hwnd, WM_UPDATE_STATUS, (WPARAM)copy, 0);
}

static void DrainSocket(SOCKET sock, DWORD size)
{
    char trash[1024];
    DWORD leftover = size;
    while (leftover > 0) {
        DWORD toRead = leftover > sizeof(trash) ? sizeof(trash) : leftover;
        if (!RecvExact(sock, trash, toRead)) break;
        leftover -= toRead;
    }
}

static char* ReadFileLocal(const char* path, DWORD* outSize)
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

static void WriteFileLocal(const char* path, const char* data, DWORD size)
{
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(hFile, data, size, &written, NULL);
    CloseHandle(hFile);
}

static bool RecvFileFull(SOCKET sock, const char* path)
{
    DWORD origSize = 0, compSize = 0;
    if (!RecvExact(sock, (char*)&origSize, sizeof(DWORD))) return false;
    if (!RecvExact(sock, (char*)&compSize, sizeof(DWORD))) return false;
    if (origSize == 0 || compSize == 0) return true;

    char* compBuf = (char*)malloc(compSize);
    if (!compBuf) {
        g_connected = false;
        PostMessage(g_hwnd, WM_DISCONNECTED, 0, 0);
        return false;
    }
    RecvExact(sock, compBuf, compSize);

    char* origBuf = (char*)malloc(origSize);
    if (!origBuf) {
        free(compBuf);
        g_connected = false;
        PostMessage(g_hwnd, WM_DISCONNECTED, 0, 0);
        return false;
    }

    size_t result = ZSTD_decompress(origBuf, origSize, compBuf, compSize);
    free(compBuf);

    if (!ZSTD_isError(result))
        WriteFileLocal(path, origBuf, (DWORD)result);

    free(origBuf);
    return true;
}

static bool RecvFileDiff(SOCKET sock, const char* path)
{
    DWORD newSize = 0, rawDiffSize = 0, compDiffSize = 0;
    if (!RecvExact(sock, (char*)&newSize, sizeof(DWORD))) return false;
    if (!RecvExact(sock, (char*)&rawDiffSize, sizeof(DWORD))) return false;
    if (!RecvExact(sock, (char*)&compDiffSize, sizeof(DWORD))) return false;
    if (newSize == 0 || rawDiffSize == 0 || compDiffSize == 0) return true;

    char* compDiff = (char*)malloc(compDiffSize);
    if (!compDiff) {
        g_connected = false;
        PostMessage(g_hwnd, WM_DISCONNECTED, 0, 0);
        return false;
    }
    RecvExact(sock, compDiff, compDiffSize);

    char* rawDiff = (char*)malloc(rawDiffSize);
    if (!rawDiff) {
        free(compDiff);
        g_connected = false;
        PostMessage(g_hwnd, WM_DISCONNECTED, 0, 0);
        return false;
    }

    size_t decompResult = ZSTD_decompress(rawDiff, rawDiffSize, compDiff, compDiffSize);
    free(compDiff);

    if (ZSTD_isError(decompResult)) { free(rawDiff); return true; }

    DWORD oldSize = 0;
    char* oldData = ReadFileLocal(path, &oldSize);
    if (!oldData) { free(rawDiff); return true; }

    char* newData = (char*)malloc(newSize);
    if (!newData) {
        free(rawDiff); free(oldData);
        g_connected = false;
        PostMessage(g_hwnd, WM_DISCONNECTED, 0, 0);
        return false;
    }

    PatchStream ps = { rawDiff, 0, rawDiffSize };
    struct bspatch_stream stream = {};
    stream.opaque = &ps;
    stream.read   = PatchRead;

    if (bspatch((uint8_t*)oldData, oldSize, (uint8_t*)newData, newSize, &stream) == 0)
        WriteFileLocal(path, newData, newSize);

    free(rawDiff);
    free(oldData);
    free(newData);
    return true;
}

static DWORD WINAPI RecvThread(LPVOID)
{
    while (g_connected) {
        MsgHeader hdr;
        if (!RecvExact(g_sock, (char*)&hdr, sizeof(MsgHeader))) {
            g_connected = false;
            PostMessage(g_hwnd, WM_DISCONNECTED, 0, 0);
            break;
        }

        if (hdr.type == MSG_PING) {
    if (hdr.size > 0) DrainSocket(g_sock, hdr.size);
    SendMsg(g_sock, MSG_PONG, nullptr, 0);
    continue;
}

if (hdr.type == MSG_PONG) {
    if (hdr.size > 0) DrainSocket(g_sock, hdr.size);
    DWORD ping = (DWORD)(GetTickCount64() - g_lastPing);
    PostMessage(g_hwnd, WM_UPDATE_PING, ping, 0);
    continue;
}

        if (hdr.type == MSG_SAVE_FULL || hdr.type == MSG_SAVE_DIFF) {
            bool isDiff = (hdr.type == MSG_SAVE_DIFF);
            int fileCount = 0;
            RecvExact(g_sock, (char*)&fileCount, sizeof(int));

            PostStatus(isDiff ? "Receiving delta..." : "Receiving save...");
            PostMessage(g_hwnd, WM_UPDATE_PROGRESS, 0, fileCount);

            SHCreateDirectoryExA(NULL, g_saveDir, NULL);

            bool ok = true;
            for (int i = 0; i < fileCount && ok; i++) {
                int nameLen = 0;
                RecvExact(g_sock, (char*)&nameLen, sizeof(int));

                char fileName[MAX_PATH] = {};
                int readLen = nameLen;
                if (readLen >= MAX_PATH) readLen = MAX_PATH - 1;
                RecvExact(g_sock, fileName, readLen);

                if (nameLen > readLen)
                    DrainSocket(g_sock, nameLen - readLen);

                char path[MAX_PATH];
                snprintf(path, MAX_PATH, "%s\\%s", g_saveDir, fileName);

                if (isDiff) ok = RecvFileDiff(g_sock, path);
                else        ok = RecvFileFull(g_sock, path);

                PostMessage(g_hwnd, WM_UPDATE_PROGRESS, i + 1, fileCount);
            }

            if (ok)
                PostStatus(isDiff ? "Delta applied! Reload save." : "Done! Load mp_client.");
            continue;
        }

        if (hdr.type == MSG_CHAT && hdr.size > 0) {
            if (hdr.size < 4096) {
                char* tmp = (char*)malloc(hdr.size + 1);
                if (tmp) {
                    RecvExact(g_sock, tmp, hdr.size);
                    tmp[hdr.size] = 0;
                    PostMessage(g_hwnd, WM_APPEND_CHAT, (WPARAM)tmp, 0);
                }
            } else {
                DrainSocket(g_sock, hdr.size);
            }
            continue;
        }

        if (hdr.size > 0)
            DrainSocket(g_sock, hdr.size);
    }
    return 0;
}

static void Connect()
{
    char ip[64] = {}, name[64] = {};
    GetWindowTextA(g_editIp, ip, sizeof(ip));
    GetWindowTextA(g_editName, name, sizeof(name));
    if (!name[0]) strcpy(name, "Player");
    strncpy(g_playerName, name, 63);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    g_sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7777);
    inet_pton(AF_INET, ip[0] ? ip : "127.0.0.1", &addr.sin_addr);

    SetWindowTextA(g_lblStatus, "Connecting...");
    if (connect(g_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        SetWindowTextA(g_lblStatus, "Connection failed");
        closesocket(g_sock); g_sock = INVALID_SOCKET;
        return;
    }

    int nameLen = (int)strlen(name) + 1;
    EnterCriticalSection(&g_sendLock);
    SendExact(g_sock, (char*)&nameLen, sizeof(int));
    SendExact(g_sock, name, nameLen);
    LeaveCriticalSection(&g_sendLock);

    g_connected = true;
    SetWindowTextA(g_lblStatus, "Connected!");
    SendMessage(g_listPlayers, LB_RESETCONTENT, 0, 0);
    SendMessageA(g_listPlayers, LB_ADDSTRING, 0, (LPARAM)name);
    g_recvThread = CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);
    SetTimer(g_hwnd, TIMER_PING, 5000, NULL);
    SetWindowTextA(g_btnConnect, "Disconnect");
}

static void Disconnect()
{
    g_connected = false;
    KillTimer(g_hwnd, TIMER_PING);

    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    if (g_recvThread) {
        WaitForSingleObject(g_recvThread, 1000);
        CloseHandle(g_recvThread);
        g_recvThread = NULL;
    }

    SetWindowTextA(g_lblStatus, "Disconnected");
    SendMessage(g_listPlayers, LB_RESETCONTENT, 0, 0);
    SetWindowTextA(g_lblPing, "Ping: --");
    SetWindowTextA(g_btnConnect, "Connect");
    SendMessage(g_progress, PBM_SETPOS, 0, 0);
    SetWindowTextA(g_lblProgress, "");
}

static void SendChat()
{
    if (!g_connected) return;
    char msg[256] = {};
    HWND hInput = GetDlgItem(g_hwnd, 200);
    GetWindowTextA(hInput, msg, sizeof(msg));
    if (!msg[0]) return;
    char full[320];
    snprintf(full, sizeof(full), "[%s]: %s", g_playerName, msg);
    SendMsg(g_sock, MSG_CHAT, full, (DWORD)strlen(full));
    SendMessageA(g_editChat, EM_REPLACESEL, FALSE, (LPARAM)full);
    SendMessageA(g_editChat, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
    SetWindowTextA(hInput, "");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);

        g_hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");

        auto Make = [&](const char* cls, const char* txt, DWORD style,
                        int x, int y, int w, int h, int id) -> HWND {
            HWND hw = CreateWindowA(cls, txt, WS_CHILD|WS_VISIBLE|style,
                                    x, y, w, h, hwnd, (HMENU)(intptr_t)id, 0, 0);
            SendMessage(hw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            return hw;
        };

        Make("STATIC", "Mode:", 0, 10, 10, 50, 20, 0);
        g_radioHost   = Make("BUTTON", "Host",   BS_AUTORADIOBUTTON|WS_GROUP, 65, 10, 55, 20, ID_RADIO_HOST);
        g_radioClient = Make("BUTTON", "Client", BS_AUTORADIOBUTTON, 125, 10, 65, 20, ID_RADIO_CLIENT);
        SendMessage(g_radioHost, BM_SETCHECK, BST_CHECKED, 0);

        Make("STATIC", "Server IP:", 0, 10, 40, 75, 20, 0);
        g_editIp = Make("EDIT", "127.0.0.1", WS_BORDER, 90, 38, 145, 22, ID_EDIT_IP);

        Make("STATIC", "Name:", 0, 10, 70, 75, 20, 0);
        g_editName = Make("EDIT", "Player1", WS_BORDER, 90, 68, 145, 22, ID_EDIT_NAME);

        g_btnConnect = Make("BUTTON", "Connect", BS_PUSHBUTTON, 10, 100, 225, 28, ID_BTN_CONNECT);

        g_lblStatus = Make("STATIC", "Not connected", 0, 10, 138, 225, 20, 0);
        g_lblPing   = Make("STATIC", "Ping: --", 0, 10, 158, 225, 20, 0);

        g_lblProgress = Make("STATIC", "", 0, 10, 183, 225, 18, ID_LBL_PROGRESS);
        g_progress = Make(PROGRESS_CLASS, "", PBS_SMOOTH, 10, 203, 225, 18, ID_PROGRESS);
        SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        Make("STATIC", "Players online:", 0, 10, 230, 225, 20, 0);
        g_listPlayers = Make("LISTBOX", "", WS_BORDER|LBS_NOTIFY, 10, 250, 225, 70, ID_LIST_PLAYERS);

        Make("STATIC", "Chat:", 0, 10, 330, 225, 20, 0);
        g_editChat = Make("EDIT", "", WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL, 10, 350, 225, 70, 0);

        Make("EDIT", "", WS_BORDER, 10, 428, 170, 22, 200);
        Make("BUTTON", "Send", BS_PUSHBUTTON, 185, 426, 50, 26, ID_BTN_CHAT);
        break;
    }
    case WM_UPDATE_STATUS: {
        char* text = (char*)wp;
        SetWindowTextA(g_lblStatus, text);
        free(text);
        break;
    }
    case WM_UPDATE_PING: {
        char buf[64];
        snprintf(buf, sizeof(buf), "Ping: %dms", (DWORD)wp);
        SetWindowTextA(g_lblPing, buf);
        break;
    }
    case WM_UPDATE_PROGRESS: {
        int current = (int)wp;
        int total   = (int)lp;
        SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, total));
        SendMessage(g_progress, PBM_SETPOS, current, 0);
        char buf[64];
        snprintf(buf, sizeof(buf), "Files: %d / %d", current, total);
        SetWindowTextA(g_lblProgress, buf);
        break;
    }
    case WM_APPEND_CHAT: {
        char* text = (char*)wp;
        SendMessageA(g_editChat, EM_REPLACESEL, FALSE, (LPARAM)text);
        SendMessageA(g_editChat, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
        free(text);
        break;
    }
    case WM_DISCONNECTED:
        g_connected = false;
        SetWindowTextA(g_lblStatus, "Disconnected");
        SetWindowTextA(g_btnConnect, "Connect");
        SetWindowTextA(g_lblPing, "Ping: --");
        SendMessage(g_listPlayers, LB_RESETCONTENT, 0, 0);
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_CONNECT) {
            if (!g_connected) Connect(); else Disconnect();
        }
        if (LOWORD(wp) == ID_BTN_CHAT) SendChat();
        break;
    case WM_TIMER:
        if (wp == TIMER_PING && g_connected) {
            g_lastPing = GetTickCount64();
            SendMsg(g_sock, MSG_PING, nullptr, 0);
        }
        break;
    case WM_DESTROY:
        Disconnect();
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    InitializeCriticalSection(&g_sendLock);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "WRSRMultiplayer";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA("WRSRMultiplayer", "WRSR Multiplayer v0.2",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        100, 100, 255, 500, NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (GetFocus() == GetDlgItem(g_hwnd, 200)) {
                SendChat();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteCriticalSection(&g_sendLock);
    return 0;
}