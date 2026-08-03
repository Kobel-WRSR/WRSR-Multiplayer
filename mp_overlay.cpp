#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
#define TIMER_PING      1001

#define MSG_SAVE 1
#define MSG_PING 2
#define MSG_PONG 3
#define MSG_CHAT 4

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

static SOCKET g_sock = INVALID_SOCKET;
static bool g_connected = false;
static char g_playerName[64] = "Player";
static DWORD g_lastPing = 0;

struct MsgHeader {
    BYTE type;
    DWORD size;
};

static void SetStatus(const char* text)
{
    SetWindowTextA(g_lblStatus, text);
}

static void SetPing(DWORD ms)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Ping: %dms", ms);
    SetWindowTextA(g_lblPing, buf);
}

static void AddPlayer(const char* name, DWORD ping)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s  [%dms]", name, ping);
    SendMessageA(g_listPlayers, LB_ADDSTRING, 0, (LPARAM)buf);
}

static void ClearPlayers()
{
    SendMessage(g_listPlayers, LB_RESETCONTENT, 0, 0);
}

static void SendMsg(SOCKET sock, BYTE type, const void* data, DWORD size)
{
    MsgHeader hdr;
    hdr.type = type;
    hdr.size = size;
    send(sock, (char*)&hdr, sizeof(hdr), 0);
    if (data && size > 0)
        send(sock, (char*)data, size, 0);
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

static DWORD WINAPI RecvThread(LPVOID)
{
    while (g_connected) {
        MsgHeader hdr;
        int got = recv(g_sock, (char*)&hdr, sizeof(MsgHeader), MSG_WAITALL);
        if (got <= 0) {
            g_connected = false;
            SetStatus("Disconnected");
            break;
        }

        if (hdr.type == MSG_PING) {
            SendMsg(g_sock, MSG_PONG, nullptr, 0);
            continue;
        }

        if (hdr.type == MSG_PONG) {
            DWORD ping = GetTickCount() - g_lastPing;
            SetPing(ping);
            continue;
        }

        if (hdr.size == 0) continue;

        if (hdr.type == MSG_SAVE) {
            int fileCount = 0;
            recv(g_sock, (char*)&fileCount, sizeof(int), MSG_WAITALL);
            SetStatus("Receiving save...");

            for (int i = 0; i < fileCount; i++) {
                int nameLen = 0;
                recv(g_sock, (char*)&nameLen, sizeof(int), MSG_WAITALL);
                char fileName[MAX_PATH] = {};
                recv(g_sock, fileName, nameLen, MSG_WAITALL);

                DWORD fileSize = 0;
                recv(g_sock, (char*)&fileSize, sizeof(DWORD), MSG_WAITALL);

                char savePath[MAX_PATH];
                snprintf(savePath, MAX_PATH,
                    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save\\mp_client\\%s",
                    fileName);

                CreateDirectoryA(
                    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save\\mp_client",
                    NULL);

                HANDLE hFile = CreateFileA(savePath, GENERIC_WRITE, 0,
                                           NULL, CREATE_ALWAYS, 0, NULL);

                char buf[65536];
                DWORD remaining = fileSize;
                while (remaining > 0) {
                    int toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
                    int r = recv(g_sock, buf, toRead, 0);
                    if (r <= 0) break;
                    DWORD written;
                    WriteFile(hFile, buf, r, &written, NULL);
                    remaining -= r;
                }
                CloseHandle(hFile);
            }

            SetStatus("Save received! Load mp_client");
            continue;
        }

        if (hdr.type == MSG_CHAT) {
            char* tmp = (char*)malloc(hdr.size + 1);
            if (tmp) {
                RecvExact(g_sock, tmp, hdr.size);
                tmp[hdr.size] = 0;
                SendMessageA(g_editChat, EM_REPLACESEL, FALSE, (LPARAM)tmp);
                SendMessageA(g_editChat, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
                free(tmp);
            }
            continue;
        }

        if (hdr.size > 0) {
            char* tmp = (char*)malloc(hdr.size);
            if (tmp) {
                RecvExact(g_sock, tmp, hdr.size);
                free(tmp);
            }
        }
    }
    return 0;
}

static void Connect()
{
    char ip[64] = {};
    char name[64] = {};
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

    SetStatus("Connecting...");
    if (connect(g_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        SetStatus("Connection failed");
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return;
    }

    int nameLen = (int)strlen(name) + 1;
    send(g_sock, (char*)&nameLen, sizeof(int), 0);
    send(g_sock, name, nameLen, 0);

    g_connected = true;
    SetStatus("Connected!");
    ClearPlayers();
    AddPlayer(name, 0);
    CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);
    SetTimer(g_hwnd, TIMER_PING, 5000, NULL);
    SetWindowTextA(g_btnConnect, "Disconnect");
}

static void Disconnect()
{
    g_connected = false;
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    KillTimer(g_hwnd, TIMER_PING);
    SetStatus("Disconnected");
    ClearPlayers();
    SetWindowTextA(g_lblPing, "Ping: --");
    SetWindowTextA(g_btnConnect, "Connect");
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
        HFONT hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");

        auto Make = [&](const char* cls, const char* txt, DWORD style,
                        int x, int y, int w, int h, int id) -> HWND {
            HWND hw = CreateWindowA(cls, txt, WS_CHILD|WS_VISIBLE|style,
                                    x, y, w, h, hwnd, (HMENU)(intptr_t)id, 0, 0);
            SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
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

        Make("STATIC", "Players online:", 0, 10, 185, 225, 20, 0);
        g_listPlayers = Make("LISTBOX", "", WS_BORDER|LBS_NOTIFY, 10, 205, 225, 80, ID_LIST_PLAYERS);

        Make("STATIC", "Chat:", 0, 10, 295, 225, 20, 0);
        g_editChat = Make("EDIT", "", WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL, 10, 315, 225, 80, 0);

        Make("EDIT", "", WS_BORDER, 10, 403, 170, 22, 200);
        Make("BUTTON", "Send", BS_PUSHBUTTON, 185, 401, 50, 26, ID_BTN_CHAT);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_CONNECT) {
            if (!g_connected) Connect();
            else Disconnect();
        }
        if (LOWORD(wp) == ID_BTN_CHAT) SendChat();
        break;
    case WM_TIMER:
        if (wp == TIMER_PING && g_connected) {
            g_lastPing = GetTickCount();
            SendMsg(g_sock, MSG_PING, nullptr, 0);
        }
        break;
    case WM_DESTROY:
        Disconnect();
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "WRSRMultiplayer";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA("WRSRMultiplayer", "WRSR Multiplayer v0.1",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        100, 100, 255, 470, NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}