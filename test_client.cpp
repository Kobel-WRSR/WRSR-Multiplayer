#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "zstd.h"

extern "C" {
#include "bspatch.h"
}

#pragma comment(lib, "ws2_32.lib")

#define MSG_SAVE      1
#define MSG_PING      2
#define MSG_PONG      3
#define MSG_CHAT      4
#define MSG_SAVE_FULL 5
#define MSG_SAVE_DIFF 6

static const char* g_saveDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\media_soviet\\save\\mp_client";

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
    if (ps->pos + length > ps->size) return -1;
    memcpy(buffer, ps->buf + ps->pos, length);
    ps->pos += length;
    return 0;
}

static char* ReadFileLocal(const char* path, DWORD* outSize)
{
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { *outSize = 0; return nullptr; }
    DWORD size = GetFileSize(hFile, NULL);
    char* buf = (char*)malloc(size);
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

static void RecvFileFull(SOCKET sock, const char* path)
{
    DWORD origSize = 0, compSize = 0;
    recv(sock, (char*)&origSize, sizeof(DWORD), MSG_WAITALL);
    recv(sock, (char*)&compSize, sizeof(DWORD), MSG_WAITALL);
    if (origSize == 0 || compSize == 0) return;

    char* compBuf = (char*)malloc(compSize);
    DWORD remaining = compSize;
    char* ptr = compBuf;
    while (remaining > 0) {
        int got = recv(sock, ptr, remaining < 65536 ? remaining : 65536, 0);
        if (got <= 0) break;
        ptr += got; remaining -= got;
    }

    char* origBuf = (char*)malloc(origSize);
    size_t result = ZSTD_decompress(origBuf, origSize, compBuf, compSize);
    free(compBuf);

    if (!ZSTD_isError(result))
        WriteFileLocal(path, origBuf, (DWORD)result);
    free(origBuf);
}

static void RecvFileDiff(SOCKET sock, const char* path)
{
    DWORD newSize = 0, rawDiffSize = 0, compDiffSize = 0;
    recv(sock, (char*)&newSize, sizeof(DWORD), MSG_WAITALL);
    recv(sock, (char*)&rawDiffSize, sizeof(DWORD), MSG_WAITALL);
    recv(sock, (char*)&compDiffSize, sizeof(DWORD), MSG_WAITALL);

    if (newSize == 0 || rawDiffSize == 0 || compDiffSize == 0) return;

    char* compDiff = (char*)malloc(compDiffSize);
    DWORD remaining = compDiffSize;
    char* ptr = compDiff;
    while (remaining > 0) {
        int got = recv(sock, ptr, remaining < 65536 ? remaining : 65536, 0);
        if (got <= 0) break;
        ptr += got; remaining -= got;
    }

    char* rawDiff = (char*)malloc(rawDiffSize);
    ZSTD_decompress(rawDiff, rawDiffSize, compDiff, compDiffSize);
    free(compDiff);

    DWORD oldSize = 0;
    char* oldData = ReadFileLocal(path, &oldSize);

    if (!oldData) {
        free(rawDiff);
        return;
    }

    char* newData = (char*)malloc(newSize);

    PatchStream ps = { rawDiff, 0, rawDiffSize };
    struct bspatch_stream stream = {};
    stream.opaque = &ps;
    stream.read   = PatchRead;

    if (bspatch((uint8_t*)oldData, oldSize, (uint8_t*)newData, newSize, &stream) == 0)
        WriteFileLocal(path, newData, newSize);

    free(rawDiff);
    free(oldData);
    free(newData);
}

int main()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7777);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    printf("Connecting...\n");
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("ERROR: cannot connect\n");
        return 1;
    }
    printf("Connected!\n");

    char name[] = "TestClient";
    int nameLen = sizeof(name);
    send(sock, (char*)&nameLen, sizeof(int), 0);
    send(sock, name, nameLen, 0);

    CreateDirectoryA(g_saveDir, NULL);

    while (true) {
        MsgHeader hdr;
        int got = recv(sock, (char*)&hdr, sizeof(MsgHeader), MSG_WAITALL);
        if (got <= 0) break;

        if (hdr.type == MSG_PING) {
            MsgHeader pong = { MSG_PONG, 0 };
            send(sock, (char*)&pong, sizeof(pong), 0);
            continue;
        }

        if (hdr.type == MSG_SAVE_FULL || hdr.type == MSG_SAVE_DIFF) {
            bool isDiff = (hdr.type == MSG_SAVE_DIFF);
            int fileCount = 0;
            recv(sock, (char*)&fileCount, sizeof(int), MSG_WAITALL);
            printf("%s: %d files\n", isDiff ? "Delta" : "Full", fileCount);

            for (int i = 0; i < fileCount; i++) {
                int nameLen2 = 0;
                recv(sock, (char*)&nameLen2, sizeof(int), MSG_WAITALL);
                char fileName[MAX_PATH] = {};
                recv(sock, fileName, nameLen2, MSG_WAITALL);

                char path[MAX_PATH];
                snprintf(path, MAX_PATH, "%s\\%s", g_saveDir, fileName);

                if (isDiff)
                    RecvFileDiff(sock, path);
                else
                    RecvFileFull(sock, path);

                printf("  %s: %s\n", isDiff ? "patched" : "saved", fileName);
            }
            printf("Done! Load mp_client in game.\n");
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}