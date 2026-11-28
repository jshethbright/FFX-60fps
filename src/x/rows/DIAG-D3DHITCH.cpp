// SPDX-License-Identifier: Apache-2.0
// DIAG-D3DHITCH (diagnostic, default off, observe only): D3D11 calls in slow frames (U 28/11/2026: model loads hitch
// 30-40 ms with < 1 ms game work and no game-thread file I/O, so the time is in render/Present).
// Device and immediate-context vtable slots come from a throwaway device (the same vtables as the game device) and
// are timed per frame: CreateBuffer, CreateTexture2D, CreateShaderResourceView, CreateInputLayout, Create*Shader,
// Map, UpdateSubresource, Draw/DrawIndexed, Present. Frames over 20 ms log each one's time, count and bytes.
#include "../fps60.h"
#include <d3d11.h>
#include <stdio.h>

enum { C_BUF, C_TEX, C_SRV, C_IL, C_VS, C_PS, C_MAP, C_UPD, C_DRAW, C_N };
static const char* const NAMES[C_N] = { "CreateBuffer", "CreateTexture2D", "CreateSRV", "CreateInputLayout",
                                        "CreateVS", "CreatePS", "Map", "UpdateSubresource", "Draw" };
static double   g_ms[C_N], g_max[C_N];
static uint32_t g_n[C_N], g_other[C_N];
static double   g_oms[C_N];                  // time on threads other than the game thread (render, loader)
static uint64_t g_bytes[C_N];
static DWORD    g_tid = 0;
static LARGE_INTEGER g_freq = {}, g_prev = {};
static double tnow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_freq.QuadPart; }
static void acc(int c, double t0, uint64_t bytes) {
    double d = tnow() - t0;
    if (GetCurrentThreadId() != g_tid) { g_other[c]++; g_oms[c] += d; }
    g_ms[c] += d; g_n[c]++; g_bytes[c] += bytes; if (d > g_max[c]) g_max[c] = d;
}

typedef HRESULT(STDMETHODCALLTYPE* cbuf_t)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef HRESULT(STDMETHODCALLTYPE* ctex_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(STDMETHODCALLTYPE* csrv_t)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
typedef HRESULT(STDMETHODCALLTYPE* cil_t)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
typedef HRESULT(STDMETHODCALLTYPE* csh_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
typedef HRESULT(STDMETHODCALLTYPE* map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* upd_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* dri_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* dr_t)(ID3D11DeviceContext*, UINT, UINT);
static cbuf_t o_buf; static ctex_t o_tex; static csrv_t o_srv; static cil_t o_il; static csh_t o_vs, o_ps;
static map_t o_map; static upd_t o_upd; static dri_t o_dri; static dr_t o_dr;

static HRESULT STDMETHODCALLTYPE h_buf(ID3D11Device* d, const D3D11_BUFFER_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Buffer** c) {
    double t = tnow(); HRESULT r = o_buf(d, a, b, c); acc(C_BUF, t, a ? a->ByteWidth : 0); return r; }
static HRESULT STDMETHODCALLTYPE h_tex(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Texture2D** c) {
    double t = tnow(); HRESULT r = o_tex(d, a, b, c); acc(C_TEX, t, a ? (uint64_t)a->Width * a->Height * 4 : 0); return r; }
static HRESULT STDMETHODCALLTYPE h_srv(ID3D11Device* d, ID3D11Resource* a, const D3D11_SHADER_RESOURCE_VIEW_DESC* b, ID3D11ShaderResourceView** c) {
    double t = tnow(); HRESULT r = o_srv(d, a, b, c); acc(C_SRV, t, 0); return r; }
static HRESULT STDMETHODCALLTYPE h_il(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* a, UINT n, const void* b, SIZE_T l, ID3D11InputLayout** c) {
    double t = tnow(); HRESULT r = o_il(d, a, n, b, l, c); acc(C_IL, t, l); return r; }
static HRESULT STDMETHODCALLTYPE h_vs(ID3D11Device* d, const void* b, SIZE_T l, ID3D11ClassLinkage* k, void** c) {
    double t = tnow(); HRESULT r = o_vs(d, b, l, k, c); acc(C_VS, t, l); return r; }
static HRESULT STDMETHODCALLTYPE h_ps(ID3D11Device* d, const void* b, SIZE_T l, ID3D11ClassLinkage* k, void** c) {
    double t = tnow(); HRESULT r = o_ps(d, b, l, k, c); acc(C_PS, t, l); return r; }
static int g_mapstall_lines = 0;
static HRESULT STDMETHODCALLTYPE h_map(ID3D11DeviceContext* x, ID3D11Resource* a, UINT s, D3D11_MAP m, UINT f, D3D11_MAPPED_SUBRESOURCE* o) {
    double t = tnow(); HRESULT r = o_map(x, a, s, m, f, o); acc(C_MAP, t, 0);
    double d = tnow() - t;
    if (d > 5.0 && g_mapstall_lines < 200 && a) {
        g_mapstall_lines++;
        D3D11_RESOURCE_DIMENSION dim; a->GetType(&dim);
        uint32_t bytes = 0, usage = 0, cpu = 0, bind = 0;
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)a)->GetDesc(&bd); bytes = bd.ByteWidth; usage = bd.Usage; cpu = bd.CPUAccessFlags; bind = bd.BindFlags; }
        else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td; ((ID3D11Texture2D*)a)->GetDesc(&td); bytes = td.Width * td.Height; usage = td.Usage; cpu = td.CPUAccessFlags; bind = td.BindFlags; }
        log_line("DIAG-D3DHITCH: Map stall %.1f ms: map type %u (1 read, 2 write, 3 rw, 4 discard, 5 no-overwrite), flags %u, dim %u, size %u, usage %u, cpu %u, bind 0x%X, thread %s, result 0x%08lX",
                 d, (unsigned)m, f, (unsigned)dim, bytes, usage, cpu, bind, GetCurrentThreadId() == g_tid ? "game" : "other", (unsigned long)r);
    }
    return r; }
static void STDMETHODCALLTYPE h_upd(ID3D11DeviceContext* x, ID3D11Resource* a, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp) {
    double t = tnow(); o_upd(x, a, s, b, p, rp, dp); acc(C_UPD, t, dp ? dp : rp); }
static void STDMETHODCALLTYPE h_dri(ID3D11DeviceContext* x, UINT n, UINT s, INT b) {
    double t = tnow(); o_dri(x, n, s, b); acc(C_DRAW, t, 0); }
static void STDMETHODCALLTYPE h_dr(ID3D11DeviceContext* x, UINT n, UINT s) {
    double t = tnow(); o_dr(x, n, s); acc(C_DRAW, t, 0); }

static bool hook_all() {
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) || !dev || !ctx) { log_line("DIAG-D3DHITCH: throwaway device failed 0x%08lX", (unsigned long)hr); return false; }
    void** dv = *(void***)dev; void** cv = *(void***)ctx;
    bool ok = (row_FXVBRING.installed || hook_entry("D3DHITCH CreateBuffer", dv[3], (void*)&h_buf, (void**)&o_buf)) &&
              (row_FXPSOWARM.on || hook_entry("D3DHITCH CreateTexture2D", dv[5], (void*)&h_tex, (void**)&o_tex)) &&
              hook_entry("D3DHITCH CreateSRV", dv[7], (void*)&h_srv, (void**)&o_srv) &&
              (row_FXPSOWARM.on || (hook_entry("D3DHITCH CreateInputLayout", dv[11], (void*)&h_il, (void**)&o_il) &&
              hook_entry("D3DHITCH CreateVertexShader", dv[12], (void*)&h_vs, (void**)&o_vs) &&
              hook_entry("D3DHITCH CreatePixelShader", dv[15], (void*)&h_ps, (void**)&o_ps) &&
              hook_entry("D3DHITCH DrawIndexed", cv[12], (void*)&h_dri, (void**)&o_dri) &&
              hook_entry("D3DHITCH Draw", cv[13], (void*)&h_dr, (void**)&o_dr))) &&
              (row_FXVBRING.installed || hook_entry("D3DHITCH Map", cv[14], (void*)&h_map, (void**)&o_map)) &&
              hook_entry("D3DHITCH UpdateSubresource", cv[48], (void*)&h_upd, (void**)&o_upd);
    ctx->Release(); dev->Release();
    log_line("DIAG-D3DHITCH: D3D11 hooks %s", ok ? "on" : "FAILED (partial)");
    return ok;
}

// GPU time per frame: timestamp queries around each Present interval (render thread, from FX-PSOWARM's Present
// callback). Results are read 3 frames late without flushing; g_gpu[] keeps the last 8.
static ID3D11Query* g_qd[4]; static ID3D11Query* g_qt[4]; static int g_qi = 0; static bool g_qok = false;
static volatile double g_gpu_last[8]; static volatile LONG g_gpu_n = 0;
void d3dhitch_present(void* devv, void* ctxv) {
    if (!row_DIAGD3DHITCH.on) return;
    ID3D11Device* dev = (ID3D11Device*)devv; ID3D11DeviceContext* ctx = (ID3D11DeviceContext*)ctxv;
    static bool init = false;
    if (!init) {
        init = true;
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
        g_qok = true;
        for (int i = 0; i < 4; i++) g_qok = g_qok && SUCCEEDED(dev->CreateQuery(&qd, &g_qd[i])) && SUCCEEDED(dev->CreateQuery(&qt, &g_qt[i]));
        if (g_qok) { ctx->Begin(g_qd[0]); ctx->End(g_qt[0]); }
        log_line("DIAG-D3DHITCH: GPU timing %s", g_qok ? "on" : "unavailable");
        return;
    }
    if (!g_qok) return;
    // close frame g_qi: its end timestamp is the next frame's start
    int nx = (g_qi + 1) & 3;
    ctx->End(g_qt[nx]);
    ctx->End(g_qd[g_qi]);
    // read the frame 3 back (start qt[k], end qt[k+1], disjoint qd[k])
    int k = (g_qi + 2) & 3, k1 = (k + 1) & 3;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
    if (ctx->GetData(g_qd[k], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ctx->GetData(g_qt[k], &a, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ctx->GetData(g_qt[k1], &b, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency) {
        LONG n = InterlockedIncrement(&g_gpu_n);
        g_gpu_last[n & 7] = (double)(b - a) * 1000.0 / (double)dj.Frequency;
    }
    g_qi = nx;
    ctx->Begin(g_qd[g_qi]);
}
static double gpu_max_recent() { double m = 0; for (int i = 0; i < 8; i++) if (g_gpu_last[i] > m) m = g_gpu_last[i]; return m; }

static void pre() {
    static bool hooked = false;
    if (!hooked) { hooked = true; g_tid = GetCurrentThreadId(); hook_all(); vsync_hook_for_diag(); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    double pms; uint32_t pn, ps; unsigned pf; unsigned long ptid;
    vsync_present_stats(&pms, &pn, &ps, &pf, &ptid);
    if (g_prev.QuadPart) {
        double fr = (t.QuadPart - g_prev.QuadPart) * 1000.0 / g_freq.QuadPart;
        static int lines = 0;
        static uint32_t nfr = 0;
        bool base = ++nfr % 600 == 0;                    // a normal frame now and then, to compare
        if ((fr > 20.0 || base) && lines < 300) {
            lines++;
            char b[1100]; int k = _snprintf_s(b, sizeof(b), _TRUNCATE, "%sframe %.1f ms, Present %.1f ms, GPU frame max of last 8 %.1f ms;", base && fr <= 20.0 ? "BASELINE " : "", fr, pms, gpu_max_recent());
            for (int c = 0; c < C_N; c++)
                if (g_n[c] || g_other[c])
                    k += _snprintf_s(b + k, sizeof(b) - k, _TRUNCATE, " %s %.1f ms x%u (max %.1f, %llu KB; %.1f ms of it off the game thread)",
                                     NAMES[c], g_ms[c], g_n[c], g_max[c], (unsigned long long)(g_bytes[c] / 1024), g_oms[c]);
            log_line("DIAG-D3DHITCH: %s", b);
        }
    }
    g_prev = t;
    for (int c = 0; c < C_N; c++) { g_ms[c] = g_max[c] = g_oms[c] = 0; g_n[c] = g_other[c] = 0; g_bytes[c] = 0; }
}
static bool install() { QueryPerformanceFrequency(&g_freq); return true; }

Row row_DIAGD3DHITCH = {
    "DIAG-D3DHITCH",
    "diagnostic: D3D11 call times in slow frames (load hitches in render/Present)",
    0, install, pre, nullptr, 0
};
