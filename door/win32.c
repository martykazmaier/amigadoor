/*
 * win32.c -- the door on Windows (Mystic BBS for Windows). See win32.h.
 *
 * The connection part is the Duke Nukem 3D door's platform_win32.c, cut down to what this door uses.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telnet.h"
#include "win32.h"

enum { CONN_CONSOLE, CONN_PIPE, CONN_SOCKET };

static int       g_conn = CONN_CONSOLE;
static int       g_comm_type;
static long long g_handle;
static SOCKET    g_sock = INVALID_SOCKET;
static HANDLE    g_in, g_out;
static DWORD     g_in_mode, g_out_mode;
static bool      g_modes_saved;

bool conn_open(int comm_type, long long handle)
{
    /* A crash must never leave a "this program has stopped working" box waiting on the BBS machine */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    /* Waits to the millisecond rather than Windows' usual 15.6: the Amiga's frames are 20 ms apart */
    timeBeginPeriod(1);
    g_comm_type = comm_type;
    g_handle = handle;

    if (comm_type == 2 && handle > 0)
    {
        WSADATA wsa;
        int type = 0, type_len = sizeof(type), on = 1;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        g_sock = (SOCKET)(uintptr_t)handle;
        if (getsockopt(g_sock, SOL_SOCKET, SO_TYPE, (char *)&type, &type_len) != 0 || type != SOCK_STREAM)
            return false;                   /* not a socket we were given: the drop file is wrong */
        /* Small writes (a key's echo, a frame's last bytes) go at once rather than waiting to be joined */
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
        g_conn = CONN_SOCKET;
        telnet_reset();
        return true;
    }

    /* Local: the console, or pipes under a test harness */
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(g_in) == FILE_TYPE_PIPE)
        g_conn = CONN_PIPE;
    else
    {
        g_conn = CONN_CONSOLE;
        if (GetConsoleMode(g_in, &g_in_mode) && GetConsoleMode(g_out, &g_out_mode))
        {
            g_modes_saved = true;
            SetConsoleMode(g_out, g_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        SetConsoleOutputCP(437);
    }
    return true;
}

void conn_close(void)
{
    if (g_modes_saved)
    {
        SetConsoleMode(g_in, g_in_mode);
        SetConsoleMode(g_out, g_out_mode);
        g_modes_saved = false;
    }
    /* The socket is the BBS's: Mystic carries on with it after the door, so it's left alone here. That includes
     * WSACleanup(): it resets every socket the process still has open, the inherited one too, and the caller was
     * dropped the moment the door ended. Winsock is let go of when the process exits, which doesn't touch the
     * connection while Mystic holds its own handle. (tools/winlaunch.c now checks this.) */
}

void conn_describe(char *out, size_t size)
{
    snprintf(out, size, "%d,%lld", g_comm_type, g_handle);
}

/* One wait on the socket: >0 bytes (telnet taken out, may be 0 after that), 0 nothing in time, -1 hung up */
static int socket_read(unsigned char *buf, int max, int timeout_ms)
{
    fd_set fds;
    struct timeval tv, *wait = NULL;
    int ready, n;

    if (timeout_ms >= 0)
    {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        wait = &tv;
    }
    FD_ZERO(&fds);
    FD_SET(g_sock, &fds);
    ready = select(0, &fds, NULL, NULL, wait);
    if (ready == SOCKET_ERROR)
        return -1;
    if (ready == 0)
        return 0;
    n = recv(g_sock, (char *)buf, max, 0);
    if (n == 0)
        return -1;                          /* the caller hung up */
    if (n == SOCKET_ERROR)
        return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
    return (int)telnet_filter_in(buf, (size_t)n);
}

int conn_read(unsigned char *buf, int max, int timeout_ms)
{
    long deadline = win_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    for (;;)
    {
        long left = timeout_ms < 0 ? -1 : deadline - win_now_ms();
        int n = 0;

        if (timeout_ms >= 0 && left < 0)
            left = 0;
        if (g_conn == CONN_SOCKET)
        {
            n = socket_read(buf, max, (int)left);
            if (n != 0)
                return n;
            /* nothing, or only telnet commands: wait on, if there's time left */
        }
        else if (g_conn == CONN_PIPE)
        {
            DWORD avail = 0, got = 0;
            if (!PeekNamedPipe(g_in, NULL, 0, NULL, &avail, NULL))
                return -1;
            if (avail > 0)
            {
                if (!ReadFile(g_in, buf, avail < (DWORD)max ? avail : (DWORD)max, &got, NULL) || got == 0)
                    return -1;
                return (int)got;
            }
            Sleep(1);
        }
        else
        {
            while (n < max && _kbhit())
                buf[n++] = (unsigned char)_getch();
            if (n > 0)
                return n;
            Sleep(1);
        }
        if (timeout_ms == 0 || (timeout_ms > 0 && win_now_ms() >= deadline))
            return 0;
    }
}

static bool socket_send_all(const unsigned char *p, size_t len)
{
    while (len > 0)
    {
        int n = send(g_sock, (const char *)p, len > 65536 ? 65536 : (int)len, 0);
        if (n > 0)
        {
            p += n;
            len -= (size_t)n;
        }
        else if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set fds;
            struct timeval tv = { 0, 100000 };
            FD_ZERO(&fds);
            FD_SET(g_sock, &fds);
            select(0, NULL, &fds, NULL, &tv);
        }
        else
            return false;
    }
    return true;
}

bool conn_write(const void *data, size_t len)
{
    const unsigned char *p = data;

    if (g_conn == CONN_SOCKET)
    {
        static unsigned char out[2 * 8192];
        while (len > 0)
        {
            size_t take = len > 8192 ? 8192 : len;
            size_t n = telnet_escape_out(p, take, out);
            if (!socket_send_all(out, n))
                return false;
            p += take;
            len -= take;
        }
        return true;
    }
    while (len > 0)
    {
        DWORD wrote = 0;
        if (!WriteFile(g_out, p, len > 65536 ? 65536 : (DWORD)len, &wrote, NULL) || wrote == 0)
            return false;
        p += wrote;
        len -= wrote;
    }
    return true;
}

/* From when the door started: a long is 32 bits on Windows, and a BBS machine's uptime can pass 24 days */
long win_now_ms(void)
{
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&now);
    return (long)((now.QuadPart - start.QuadPart) * 1000 / freq.QuadPart);
}

int win_setenv(const char *name, const char *value)
{
    char line[1200];
    snprintf(line, sizeof(line), "%s=%s", name, value);
    return _putenv(line);
}

void win_exe_dir(char *out, size_t size)
{
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)size);
    char *slash;

    if (len == 0 || len >= size)
    {
        snprintf(out, size, ".");
        return;
    }
    out[len] = '\0';
    if ((slash = strrchr(out, '\\')) != NULL || (slash = strrchr(out, '/')) != NULL)
        *slash = '\0';
}

void *win_lib_open(const char *path)
{
    /* With the altered search path, the DLLs it needs come from its own folder too (libjxl's, its C++ runtime) */
    return (void *)LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void *win_lib_sym(void *lib, const char *name)
{
    FARPROC f = GetProcAddress((HMODULE)lib, name);
    void *p;
    memcpy(&p, &f, sizeof(p));
    return p;
}

/* One argument for a command line, quoted the way the C runtime reads it back */
static void quote_arg(char *cmd, size_t size, const char *arg)
{
    size_t n = strlen(cmd);
    int slashes = 0;

    if (n + 4 >= size)
        return;
    if (n > 0)
        cmd[n++] = ' ';
    cmd[n++] = '"';
    for (const char *p = arg; *p != '\0' && n + 4 < size; p++)
    {
        if (*p == '\\')
            slashes++;
        else
        {
            if (*p == '"')
            {
                for (int i = 0; i <= slashes && n + 4 < size; i++)
                    cmd[n++] = '\\';
            }
            slashes = 0;
        }
        cmd[n++] = *p;
    }
    for (int i = 0; i < slashes && n + 3 < size; i++)
        cmd[n++] = '\\';                    /* backslashes before the closing quote are doubled */
    cmd[n++] = '"';
    cmd[n] = '\0';
}

int win_run_self(char *const argv[])
{
    char exe[MAX_PATH], cmd[8192] = "";
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;

    if (GetModuleFileNameA(NULL, exe, sizeof(exe)) == 0)
        return -1;
    quote_arg(cmd, sizeof(cmd), exe);
    for (int i = 0; argv[i] != NULL; i++)
        quote_arg(cmd, sizeof(cmd), argv[i]);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* Inheriting handles: the caller's socket goes to the Amiga side this way, the same number as here */
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

bool win_make_session_dir(char *out, size_t size)
{
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(temp), temp);

    if (n == 0 || n >= sizeof(temp))
        return false;
    for (int i = 0; i < 100; i++)
    {
        snprintf(out, size, "%samigadoor-%lu-%lu", temp, (unsigned long)GetCurrentProcessId(),
                 (unsigned long)(GetTickCount() + (DWORD)i));
        if (CreateDirectoryA(out, NULL))
            return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return false;
}

void win_remove_tree(const char *path)
{
    char pattern[MAX_PATH + 4], child[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if ((h = FindFirstFileA(pattern, &fd)) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                win_remove_tree(child);
            else
            {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

/* ---- crash guard ---- */

static char   g_reset[256];
static size_t g_reset_len;

static LONG WINAPI on_crash(EXCEPTION_POINTERS *info)
{
    (void)info;
    if (g_reset_len > 0)
        conn_write(g_reset, g_reset_len);
    conn_close();
    ExitProcess(3);
    return EXCEPTION_EXECUTE_HANDLER;
}

void win_crash_guard(const char *reset, size_t len)
{
    if (reset == NULL)
    {
        SetUnhandledExceptionFilter(NULL);
        return;
    }
    g_reset_len = len < sizeof(g_reset) ? len : sizeof(g_reset);
    memcpy(g_reset, reset, g_reset_len);
    SetUnhandledExceptionFilter(on_crash);
}

#endif /* _WIN32 */
