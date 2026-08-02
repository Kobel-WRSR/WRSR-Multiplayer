#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

int main()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7777);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    printf("Connecting to server...\n");
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("ERROR: cannot connect\n");
        return 1;
    }
    printf("Connected!\n");

    int fileCount = 0;
    recv(sock, (char*)&fileCount, sizeof(int), MSG_WAITALL);
    printf("Receiving %d files...\n", fileCount);

    const char* saveDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save\\mp_client";
    CreateDirectoryA(saveDir, NULL);

    for (int i = 0; i < fileCount; i++) {
        int nameLen = 0;
        recv(sock, (char*)&nameLen, sizeof(int), MSG_WAITALL);
        char fileName[MAX_PATH];
        recv(sock, fileName, nameLen, MSG_WAITALL);

        DWORD fileSize = 0;
        recv(sock, (char*)&fileSize, sizeof(DWORD), MSG_WAITALL);

        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\%s", saveDir, fileName);
        HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);

        char buf[4096];
        DWORD remaining = fileSize;
        while (remaining > 0) {
            int toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
            int got = recv(sock, buf, toRead, MSG_WAITALL);
            if (got <= 0) break;
            DWORD written;
            WriteFile(hFile, buf, got, &written, NULL);
            remaining -= got;
        }
        CloseHandle(hFile);
        printf("Saved: %s\n", fileName);
    }

    printf("\nDone! Save ready at: %s\n", saveDir);
    printf("Load 'mp_client' save in game!\n");
    closesocket(sock);
    WSACleanup();
    return 0;
}