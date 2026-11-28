// SPDX-License-Identifier: Apache-2.0
// DIAG-LOADHITCH (diagnostic, default off, observe only): where a slow frame's time goes when something loads
// (U 28/11/2026: a stutter on every battle character swap). Game-thread ReadFile / CreateFile time is summed per
// frame; frames over 25 ms log: frame time, game work (Sg_MainLoop pre to post), file read time, calls, bytes and
// the files opened.
#include "../fps60.h"
#include <stdio.h>
#include <string.h>

typedef BOOL(WINAPI* readfile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef HANDLE(WINAPI* createw_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* createa_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static readfile_t g_rf = nullptr;
static createw_t  g_cw = nullptr;
static createa_t  g_ca = nullptr;
static DWORD g_tid = 0;
static LARGE_INTEGER g_freq = {}, g_prev = {}, g_pre = {};
static double g_read_ms = 0, g_open_ms = 0, g_work_ms = 0;
static uint32_t g_reads = 0, g_opens = 0;
static uint64_t g_bytes = 0;
static char g_names[300];
static int  g_nk = 0;
static double now_ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_freq.QuadPart; }

static BOOL WINAPI h_rf(HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (GetCurrentThreadId() != g_tid) return g_rf(h, b, n, got, ov);
    double t = now_ms();
    BOOL r = g_rf(h, b, n, got, ov);
    g_read_ms += now_ms() - t; g_reads++; g_bytes += n;
    return r;
}
static void note(const char* s) {
    const char* b = s;
    for (const char* p = s; *p; p++) if (*p == 0x5C || *p == '/') b = p + 1;
    if (g_nk < 280) g_nk += _snprintf_s(g_names + g_nk, sizeof(g_names) - g_nk, _TRUNCATE, " %s", b);
}
static HANDLE WINAPI h_cw(LPCWSTR f, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD fl, HANDLE t) {
    if (GetCurrentThreadId() != g_tid) return g_cw(f, a, s, sa, c, fl, t);
    double t0 = now_ms();
    HANDLE r = g_cw(f, a, s, sa, c, fl, t);
    g_open_ms += now_ms() - t0; g_opens++;
    char n[MAX_PATH] = {};
    if (f) WideCharToMultiByte(CP_UTF8, 0, f, -1, n, sizeof(n) - 1, nullptr, nullptr);
    note(n);
    return r;
}
static HANDLE WINAPI h_ca(LPCSTR f, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD fl, HANDLE t) {
    if (GetCurrentThreadId() != g_tid) return g_ca(f, a, s, sa, c, fl, t);
    double t0 = now_ms();
    HANDLE r = g_ca(f, a, s, sa, c, fl, t);
    g_open_ms += now_ms() - t0; g_opens++;
    note(f ? f : "?");
    return r;
}

static bool g_hooked = false;
static void pre() {
    if (!g_hooked) {
        g_hooked = true;
        g_tid = GetCurrentThreadId();
        HMODULE k = GetModuleHandleA("kernel32.dll");
        hook_entry("DIAG-LOADHITCH ReadFile", (void*)GetProcAddress(k, "ReadFile"), (void*)&h_rf, (void**)&g_rf);
        hook_entry("DIAG-LOADHITCH CreateFileW", (void*)GetProcAddress(k, "CreateFileW"), (void*)&h_cw, (void**)&g_cw);
        hook_entry("DIAG-LOADHITCH CreateFileA", (void*)GetProcAddress(k, "CreateFileA"), (void*)&h_ca, (void**)&g_ca);
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (g_prev.QuadPart) {
        double fr = (t.QuadPart - g_prev.QuadPart) * 1000.0 / g_freq.QuadPart;
        static int lines = 0;
        if (fr > 25.0 && lines < 400) {
            lines++;
            log_line("DIAG-LOADHITCH: frame %.1f ms: game work %.1f ms, file reads %.1f ms (%u calls, %llu KB), opens %.1f ms (%u):%s",
                     fr, g_work_ms, g_read_ms, g_reads, (unsigned long long)(g_bytes / 1024), g_open_ms, g_opens, g_names);
        }
    }
    g_prev = t; g_pre = t;
    g_read_ms = g_open_ms = g_work_ms = 0; g_reads = g_opens = 0; g_bytes = 0; g_nk = 0; g_names[0] = 0;
}
static void post() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (g_pre.QuadPart) g_work_ms += (t.QuadPart - g_pre.QuadPart) * 1000.0 / g_freq.QuadPart;
}
static bool install() { QueryPerformanceFrequency(&g_freq); return true; }

Row row_DIAGLOADHITCH = {
    "DIAG-LOADHITCH",
    "diagnostic: slow frames split into game work and game-thread file I/O (load hitches)",
    0, install, pre, post, 0
};
