// SPDX-License-Identifier: Apache-2.0
// FX-VBRING (default off; live switch): the game rewrites one 12 MB dynamic vertex buffer with Map(DISCARD) ~10
// times per frame. Each DISCARD makes the driver hand out a fresh 12 MB copy; at 120 fps (~1200 per second) a
// little extra work on a character swap empties the driver's pool and one Map waits 20-40 ms with the GPU idle
// (D 28/11/2026 DIAG-D3DHITCH: swap frame 38.5 ms, GPU 9.6 ms, Map(DISCARD) of the 12 MB VB 40.7 ms).
// While on, that buffer's writes go to a 64 MB ring the mod owns, mapped NO_OVERWRITE (never renamed): each DISCARD
// starts a new region right after the bytes the previous region's draws used; the game's vertex-buffer bindings are
// pointed at the region (offset added) and re-pointed when a new region starts. Before the ring wraps onto a
// quarter, an event query ended when that quarter was last left must be done (waits are counted and logged).
// Bytes used per region come from the draws: non-indexed from start+count, indexed from the highest index in the
// draw's range of an index buffer the mod kept a CPU copy of (created with initial data); with an index buffer it
// cannot read, the region is taken as the full 12 MB.
#include "../fps60.h"
#include <d3d11.h>
#include <stdlib.h>
#include <string.h>

extern Row row_FXVBRING;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_imm = nullptr;

// ---------------------------------------------------------------- index buffer copies
struct IbCopy { ID3D11Buffer* ib; uint8_t* data; uint32_t len; };
enum { NIB = 4096 };
static IbCopy g_ibs[NIB]; static int g_nib = 0;
static CRITICAL_SECTION g_cs;
static const IbCopy* ib_find(ID3D11Buffer* b) {
    for (int i = 0; i < g_nib; i++) if (g_ibs[i].ib == b) return &g_ibs[i];
    return nullptr;
}

// ---------------------------------------------------------------- the game buffer and the ring
static ID3D11Buffer* g_game = nullptr;           // the big dynamic VB
static uint32_t g_game_len = 0;
static ID3D11Buffer* g_ring = nullptr;
// U 28/11/2026: regions sized from the draws overlapped data still in use (artifacts). Every region is now a whole
// game-buffer slot (no estimate): 16 slots of 12 MB, each fenced, waited on only if the GPU still reads it.
enum { SLOTS = 16, QN = SLOTS };
static uint32_t RING = 0, QLEN = 0;
static ID3D11Query* g_qfence[QN];
static bool g_qpending[QN];
static bool g_active = false;                     // current epoch lives in the ring
static uint32_t g_base = 0, g_extent = 0;         // epoch start in the ring, bytes used so far (from draws)
static bool g_unknown = false;                    // a draw's extent could not be computed
static bool g_mapped = false;
static volatile LONG g_epochs = 0, g_unknowns = 0, g_waits = 0, g_wraps = 0;
static double g_wait_ms = 0;
static LARGE_INTEGER g_qpf;

// game bindings of g_game: slot -> (stride, offset)
static bool     g_bound[8];
static UINT     g_bstride[8], g_boff[8];
// current index buffer
static ID3D11Buffer* g_curib = nullptr; static DXGI_FORMAT g_ibfmt = DXGI_FORMAT_R16_UINT; static UINT g_iboff = 0;

typedef HRESULT(STDMETHODCALLTYPE* cbuf_t)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef HRESULT(STDMETHODCALLTYPE* map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* unmap_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* setib_t)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
static cbuf_t o_cbuf; static map_t o_map; static unmap_t o_unmap; static setib_t o_setib;
typedef void(STDMETHODCALLTYPE* setvb_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
static setvb_t g_vb_orig = nullptr;               // FX-PSOWARM's trampoline (set by vbring_set_vb_orig)
void vbring_set_vb_orig(void* p) { g_vb_orig = (setvb_t)p; }

static HRESULT STDMETHODCALLTYPE h_cbuf(ID3D11Device* d, const D3D11_BUFFER_DESC* bd, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    HRESULT r = o_cbuf(d, bd, init, out);
    if (FAILED(r) || !out || !*out || !bd) return r;
    if ((bd->BindFlags & D3D11_BIND_INDEX_BUFFER) && init && init->pSysMem && bd->ByteWidth <= (8u << 20)) {
        EnterCriticalSection(&g_cs);
        if (g_nib < NIB) {
            uint8_t* c = (uint8_t*)malloc(bd->ByteWidth);
            if (c) { memcpy(c, init->pSysMem, bd->ByteWidth); (*out)->AddRef(); g_ibs[g_nib++] = { *out, c, bd->ByteWidth }; }
        }
        LeaveCriticalSection(&g_cs);
    }
    if (!g_game && bd->Usage == D3D11_USAGE_DYNAMIC && (bd->BindFlags & D3D11_BIND_VERTEX_BUFFER) &&
        (bd->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) && bd->ByteWidth >= (8u << 20) && bd->ByteWidth <= (32u << 20)) {
        g_game = *out; g_game->AddRef(); g_game_len = bd->ByteWidth;
        log_line("FX-VBRING: game dynamic vertex buffer %p, %u bytes", (void*)g_game, bd->ByteWidth);
    }
    return r;
}

static void setup() {
    if (g_ring || !g_dev) return;
    D3D11_BUFFER_DESC bd = {};
    QLEN = (g_game_len + 255) & ~255u; RING = QLEN * SLOTS;
    bd.ByteWidth = RING; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_ring))) { g_ring = nullptr; log_line("FX-VBRING: ring buffer not created"); return; }
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    for (int i = 0; i < QN; i++) g_dev->CreateQuery(&qd, &g_qfence[i]);
    log_line("FX-VBRING: %u slots of %u bytes (%u MB) ready", SLOTS, QLEN, RING >> 20);
}

static double tms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_qpf.QuadPart; }

// The GPU must be done with quarter q before writing into it again.
static void wait_quarter(ID3D11DeviceContext* c, int q) {
    if (!g_qpending[q]) return;
    BOOL done = FALSE;
    if (c->GetData(g_qfence[q], &done, sizeof(done), 0) == S_OK && done) { g_qpending[q] = false; return; }
    double t0 = tms();
    g_waits++;
    while (c->GetData(g_qfence[q], &done, sizeof(done), 0) != S_OK || !done) { if (tms() - t0 > 200) break; SwitchToThread(); }
    g_wait_ms += tms() - t0;
    g_qpending[q] = false;
}

static void rebind(ID3D11DeviceContext* c) {
    for (UINT s = 0; s < 8; s++) {
        if (!g_bound[s]) continue;
        ID3D11Buffer* b = g_active ? g_ring : g_game;
        UINT off = g_boff[s] + (g_active ? g_base : 0), st = g_bstride[s];
        g_vb_orig(c, s, 1, &b, &st, &off);
    }
}

// New epoch: place it after the previous one's bytes. Quarters written since their last fence and not part of the new
// region get a fence now; quarters the new region covers must have their last fence done (GPU finished with them).
static bool g_dirty[QN];
static bool new_epoch(ID3D11DeviceContext* c) {
    uint32_t next = g_active ? g_base + QLEN : 0;
    if (next + g_game_len > RING) { next = 0; g_wraps++; }
    int qa = (int)(next / QLEN), qb = qa;
    for (int q = 0; q < QN; q++)
        if (g_dirty[q] && (q < qa || q > qb)) { c->End(g_qfence[q]); g_qpending[q] = true; g_dirty[q] = false; }
    for (int q = qa; q <= qb; q++) { if (!g_dirty[q]) wait_quarter(c, q); g_dirty[q] = true; }
    g_base = next; g_extent = 0; g_unknown = false;
    g_epochs++;
    return true;
}

static HRESULT STDMETHODCALLTYPE h_map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP mt, UINT fl, D3D11_MAPPED_SUBRESOURCE* out) {
    if (r == (ID3D11Resource*)g_game && c == g_imm && out && g_ring) {
        bool want = row_FXVBRING.on && !g_disabled;
        if (mt == D3D11_MAP_WRITE_DISCARD) {
            bool was = g_active;
            if (want) { new_epoch(c); g_active = true; }
            else g_active = false;
            if (was != g_active || g_active) rebind(c);
        }
        if (g_active && (mt == D3D11_MAP_WRITE_DISCARD || mt == D3D11_MAP_WRITE_NO_OVERWRITE)) {
            D3D11_MAPPED_SUBRESOURCE m;
            HRESULT hr = o_map(c, g_ring, 0, D3D11_MAP_WRITE_NO_OVERWRITE, fl, &m);
            if (SUCCEEDED(hr)) { out->pData = (uint8_t*)m.pData + g_base; out->RowPitch = g_game_len; out->DepthPitch = g_game_len; g_mapped = true; return S_OK; }
            g_active = false; rebind(c);                  // could not map the ring: back to the game buffer
        }
    }
    double t0 = tms();
    HRESULT hr = o_map(c, r, sub, mt, fl, out);
    double d = tms() - t0;
    static int lines = 0;
    if (d > 5.0 && lines < 100 && row_DIAGD3DHITCH.on) { lines++; log_line("FX-VBRING: Map stall %.1f ms (type %u, %s)", d, (unsigned)mt, r == (ID3D11Resource*)g_game ? "game dynamic VB" : "other"); }
    return hr;
}
static void STDMETHODCALLTYPE h_unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (r == (ID3D11Resource*)g_game && g_mapped && c == g_imm) { g_mapped = false; o_unmap(c, g_ring, 0); return; }
    o_unmap(c, r, sub);
}
static void STDMETHODCALLTYPE h_setib(ID3D11DeviceContext* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT off) {
    if (c == g_imm) { g_curib = b; g_ibfmt = f; g_iboff = off; }
    o_setib(c, b, f, off);
}

// ---------------------------------------------------------------- called from FX-PSOWARM's hooks (immediate context)
// IASetVertexBuffers: note bindings of the game buffer, substitute the ring. Returns true when it issued the call.
bool vbring_setvb(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* off) {
    if (c != g_imm || !g_game) return false;
    bool any = false;
    for (UINT i = 0; i < n && s + i < 8; i++) {
        bool isg = b && b[i] == g_game;
        g_bound[s + i] = isg;
        if (isg) { g_bstride[s + i] = st ? st[i] : 0; g_boff[s + i] = off ? off[i] : 0; any = true; }
    }
    if (!any || !g_active || n > 8) return false;
    ID3D11Buffer* nb[8]; UINT no[8];
    for (UINT i = 0; i < n; i++) {
        nb[i] = b ? b[i] : nullptr; no[i] = off ? off[i] : 0;
        if (nb[i] == g_game) { nb[i] = g_ring; no[i] += g_base; }
    }
    g_vb_orig(c, s, n, nb, st, no);
    return true;
}

static uint32_t max_index(uint32_t start, uint32_t count, bool* ok) {
    const IbCopy* ib = nullptr;
    EnterCriticalSection(&g_cs);
    ib = ib_find(g_curib);
    LeaveCriticalSection(&g_cs);
    if (!ib) { *ok = false; return 0; }
    uint32_t isz = g_ibfmt == DXGI_FORMAT_R32_UINT ? 4 : 2;
    uint64_t a = (uint64_t)g_iboff + (uint64_t)start * isz, e = a + (uint64_t)count * isz;
    if (e > ib->len) { *ok = false; return 0; }
    uint32_t m = 0;
    if (isz == 2) { const uint16_t* p = (const uint16_t*)(ib->data + a); for (uint32_t i = 0; i < count; i++) if (p[i] > m && p[i] != 0xFFFF) m = p[i]; }
    else { const uint32_t* p = (const uint32_t*)(ib->data + a); for (uint32_t i = 0; i < count; i++) if (p[i] > m && p[i] != 0xFFFFFFFF) m = p[i]; }
    *ok = true;
    return m;
}
// kind 0 Draw(count, start), 1 DrawIndexed(count, startIndex, baseVertex), 2 DrawInstanced, 3 DrawIndexedInstanced
void vbring_draw(ID3D11DeviceContext* c, int kind, uint32_t count, uint32_t start, int32_t basev, uint32_t inst, uint32_t sinst) {
    if (c != g_imm || !g_active) return;
    for (UINT s = 0; s < 8; s++) {
        if (!g_bound[s]) continue;
        uint64_t last;                                    // one past the last vertex read
        if (kind >= 2) { g_unknown = true; return; }   // instancing: per-instance extent not tracked, full 12 MB
        if (kind == 0) last = (uint64_t)start + count;
        else {
            bool ok; uint32_t m = max_index(start, count, &ok);
            if (!ok) { g_unknown = true; return; }
            int64_t v = (int64_t)m + basev + 1; last = v > 0 ? (uint64_t)v : 0;
        }
        (void)inst; (void)sinst;                          // per-instance data in this buffer: covered by the 12 MB fallback below
        uint64_t bytes = (uint64_t)g_boff[s] + last * g_bstride[s];
        if (!g_bstride[s]) { g_unknown = true; return; }
        if (bytes > g_game_len) bytes = g_game_len;
        if (bytes > g_extent) g_extent = (uint32_t)bytes;
    }
}

void vbring_present(void* dev, void* ctx) {
    if (!g_dev) { g_dev = (ID3D11Device*)dev; g_imm = (ID3D11DeviceContext*)ctx; QueryPerformanceFrequency(&g_qpf); }
    static bool first = true;
    if (first) { first = false; if (!config_row("FX-VBRING-START", 0)) row_FXVBRING.on = 0; }   // off until fps60_live.txt FX-VBRING=1
    if (!g_ring && g_game) setup();
    static uint32_t n = 0;
    if (++n % 600 == 0 && g_epochs)
        log_line("FX-VBRING: %s; %ld regions, %ld full-size (index buffer unreadable), %ld wraps, %ld fence waits (%.1f ms)",
                 g_active ? "active" : "idle", g_epochs, g_unknowns, g_wraps, g_waits, g_wait_ms);
}

static bool install() {
    InitializeCriticalSection(&g_cs);
    QueryPerformanceFrequency(&g_qpf);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) || !dev || !ctx) return false;
    void** dv = *(void***)dev; void** cv = *(void***)ctx;
    bool ok = hook_entry("FX-VBRING CreateBuffer", dv[3], (void*)&h_cbuf, (void**)&o_cbuf) &&
              hook_entry("FX-VBRING Map", cv[14], (void*)&h_map, (void**)&o_map) &&
              hook_entry("FX-VBRING Unmap", cv[15], (void*)&h_unmap, (void**)&o_unmap) &&
              hook_entry("FX-VBRING IASetIndexBuffer", cv[19], (void*)&h_setib, (void**)&o_setib);
    ctx->Release(); dev->Release();
    log_line("  FX-VBRING: hooks %s (ring used only while the row is on; fps60_live.txt FX-VBRING=1)", ok ? "on" : "FAILED");
    return ok;
}

// Hooks stay installed with the row off (it switches at the next DISCARD), so install always runs.
Row row_FXVBRING = {
    "FX-VBRING",
    "game's 12 MB dynamic vertex buffer written through a mod-owned ring (no driver renames: swap Map stalls)",
    1, install, nullptr, nullptr, 0, nullptr
};
