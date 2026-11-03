/*
 * winlaunch.c -- stands in for Mystic for Windows, to test amigadoor.exe without a BBS (from the Duke Nukem 3D door).
 *
 *     winlaunch.exe <door.exe> [args before the drop folder...]
 *
 * Makes a real telnet-style TCP connection on 127.0.0.1 (nothing listens beyond this machine and only for one
 * connect), writes DOOR32.SYS with comm type 2 and the door's end of that connection as an inheritable socket handle,
 * the way Mystic does, and starts the door with the drop folder as its last argument. The other end of the connection
 * is relayed to this program's stdin/stdout, so a test can play the caller through pipes: 0xFF is doubled towards the
 * door and undoubled from it (telnet), and a few telnet commands are sent first, which the door must swallow.
 * Exits with the door's exit code, or 4 if the door's exit dropped the caller: like Mystic, this keeps its own copy of
 * the door's end and, once the door has gone, checks the connection still carries a line.
 *
 * Build: i686-w64-mingw32-gcc -O2 winlaunch.c -o winlaunch.exe -lws2_32 -static.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static SOCKET caller = INVALID_SOCKET;

/* stdin -> door, with 0xFF doubled */
static DWORD WINAPI to_door(LPVOID unused)
{
    (void)unused;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    unsigned char buf[8192], out[16384];
    DWORD n;
    while (ReadFile(in, buf, sizeof buf, &n, NULL) && n > 0) {
        size_t m = 0;
        for (DWORD i = 0; i < n; i++) {
            out[m++] = buf[i];
            if (buf[i] == 0xFF)
                out[m++] = 0xFF;
        }
        for (size_t off = 0; off < m;) {
            int s = send(caller, (const char *)out + off, (int)(m - off), 0);
            if (s <= 0)
                return 0;
            off += (size_t)s;
        }
    }
    shutdown(caller, SD_SEND);
    return 0;
}

/* door -> stdout, with doubled 0xFF undoubled (the door sends no other telnet commands). Its result is how the
 * connection ended: 0 a clean close, anything else a reset */
static DWORD WINAPI from_door(LPVOID unused)
{
    (void)unused;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    unsigned char buf[16384], plain[16384];
    int pending_ff = 0, n;
    while ((n = recv(caller, (char *)buf, sizeof buf, 0)) > 0) {
        DWORD m = 0, w;
        for (int i = 0; i < n; i++) {
            if (pending_ff) {
                pending_ff = 0;
                plain[m++] = 0xFF;
                if (buf[i] == 0xFF)
                    continue;
                fprintf(stderr, "winlaunch: lone 0xFF from the door, then %u\n", buf[i]);
            }
            if (buf[i] == 0xFF)
                pending_ff = 1;
            else
                plain[m++] = buf[i];
        }
        if (m && !WriteFile(out, plain, m, &w, NULL))
            break;
    }
    return n == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: winlaunch <door.exe> [args...]\n");
        return 2;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 2;

    /* One connection over loopback: listen, connect to ourselves, accept */
    SOCKET lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = sizeof a;
    if (bind(lis, (struct sockaddr *)&a, sizeof a) != 0 || listen(lis, 1) != 0 ||
        getsockname(lis, (struct sockaddr *)&a, &alen) != 0) {
        fprintf(stderr, "winlaunch: loopback listen failed (%d)\n", WSAGetLastError());
        return 2;
    }
    caller = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(caller, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "winlaunch: connect failed (%d)\n", WSAGetLastError());
        return 2;
    }
    SOCKET door = accept(lis, NULL, NULL);
    closesocket(lis);
    if (door == INVALID_SOCKET)
        return 2;
    SetHandleInformation((HANDLE)door, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation((HANDLE)caller, HANDLE_FLAG_INHERIT, 0);

    /* DOOR32.SYS in a fresh folder under %TEMP% */
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "winlaunch-%lu", (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\door32.sys", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return 2;
    fprintf(f, "2\n%llu\n38400\nwinlaunch\n1\nTest Player\nplayer\n100\n60\n1\n1\n", (unsigned long long)door);
    fclose(f);

    /* The door, with the drop folder last */
    char cmd[4096] = "";
    for (int i = 1; i < argc; i++)
        snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), "\"%s\" ", argv[i]);
    snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), "\"%s\"", dir);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "winlaunch: could not start the door (%lu)\n", GetLastError());
        return 2;
    }
    /* Our copy of the door's end stays open, as Mystic's does: Mystic carries on with the caller after the door */

    /* Telnet negotiation the door must not show or treat as keys: WILL ECHO, WILL SGA, DO NAWS + a NAWS reply */
    static const unsigned char hello[] = {255, 251, 1, 255, 251, 3, 255, 253, 31,
                                          255, 250, 31, 0, 80, 0, 25, 255, 240};
    send(caller, (const char *)hello, sizeof hello, 0);

    CreateThread(NULL, 0, to_door, NULL, 0, NULL);
    HANDLE relay = CreateThread(NULL, 0, from_door, NULL, 0, NULL);

    /* Once the door has gone, the BBS must still have the caller: a line down our copy has to arrive, then a clean
     * close. A door that shuts the connection on its way out (closesocket, or WSACleanup with the socket still
     * open) drops callers at the end of the door on a real BBS, and fails here with exit code 4 */
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1, relay_end = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    static const char probe[] = "\r\n[winlaunch: the BBS still has the caller]\r\n";
    int sent = send(door, probe, (int)strlen(probe), 0);
    shutdown(door, SD_SEND);
    WaitForSingleObject(relay, 10000);
    GetExitCodeThread(relay, &relay_end);
    closesocket(door);
    if (sent != (int)strlen(probe) || relay_end != 0) {
        fprintf(stderr, "winlaunch: the door's exit dropped the caller (send %d, error %d)\n", sent, WSAGetLastError());
        return 4;
    }
    return (int)code;
}
