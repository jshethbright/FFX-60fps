// SPDX-License-Identifier: Apache-2.0
// FX-PSOWARM (default on): the driver compiles each shader + state combination the first time it is drawn, so a
// model, enemy or effect that appears for the first time in a session stalls one frame 20-40 ms in Map or Present
// (U 28/11/2026, AMD RX 7900 XTX, shader cache on: battle swaps hitch the first time only; D DIAG-D3DHITCH: normal
// 8.5 ms frames, hitch frames the same uploads and draws but one Map/Present waiting 20-40 ms, nothing created).
// Every combination the game draws on the immediate context is recorded (shader bytecode hashes, input layout,
// blend/raster/depth state, render-target formats, topology, strides) into fps60_pso.bin beside the exe. At the next
// launch each recorded combination is drawn once off-screen (1 triangle into a 4x4 target, inside its own
// ID3DDeviceContextState so the game's bindings are untouched) as soon as its shaders exist, at most ~3 ms per
// frame, so the driver compiles it during loading instead of in battle. Combinations never seen before still
// compile once, then are known for every later session.
#include "../fps60.h"
#include <d3d11_1.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "d3d11.lib")
bool vbring_setvb(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
void vbring_draw(ID3D11DeviceContext*, int, uint32_t, uint32_t, int32_t, uint32_t, uint32_t);
void vbring_present(void* dev, void* ctx);
void vbring_set_vb_orig(void* p);


// ---------------------------------------------------------------- record
#pragma pack(push, 4)
struct Pso {
    uint64_t vs, ps, il;                 // bytecode hashes; il = layout key (elements + signature)
    D3D11_BLEND_DESC blend;
    D3D11_RASTERIZER_DESC rast;
    D3D11_DEPTH_STENCIL_DESC ds;
    uint32_t has;                        // 1 blend, 2 rast, 4 ds
    uint32_t rtf[8], nrt, dsf, samples, topo, stride[8];
};
#pragma pack(pop)
static const uint32_t MAGIC = 0x4F535046, VERSION = 1;   // "FPSO"

static uint64_t fnv(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// ---------------------------------------------------------------- small hash tables (open addressing)
struct Map { uint64_t* k; uint64_t* v; uint32_t cap, n; };
static bool map_init(Map& m, uint32_t cap) {
    m.k = (uint64_t*)calloc(cap, 8); m.v = (uint64_t*)calloc(cap, 8); m.cap = cap; m.n = 0;
    return m.k && m.v;
}
static uint64_t* map_slot(Map& m, uint64_t key, bool make) {   // key 0 is never stored
    if (!key) key = 1;
    uint32_t i = (uint32_t)(key ^ (key >> 29)) & (m.cap - 1);
    for (uint32_t n = 0; n < m.cap; n++, i = (i + 1) & (m.cap - 1)) {
        if (m.k[i] == key) return &m.v[i];
        if (!m.k[i]) {
            if (!make || m.n * 4 > m.cap * 3) return nullptr;
            m.k[i] = key; m.n++; return &m.v[i];
        }
    }
    return nullptr;
}
static bool map_get(Map& m, uint64_t key, uint64_t* out) { uint64_t* s = map_slot(m, key, false); if (s) *out = *s; return s != nullptr; }
static void map_put(Map& m, uint64_t key, uint64_t v) { uint64_t* s = map_slot(m, key, true); if (s) *s = v; }

static CRITICAL_SECTION g_cs;
static Map g_obj2hash, g_hash2obj;       // shaders and input layouts (objects AddRef'd: never freed, never reused)
static Map g_seen;                       // per session: bound-pointer tuples already looked at (render thread)
static Map g_known;                      // record content hashes (file + new)
static volatile LONG g_shaders = 0;

enum { MAX_RECS = 16384 };
static Pso*    g_recs = nullptr;
static uint8_t* g_warmed = nullptr;
static uint32_t g_nrec = 0, g_saved = 0, g_cursor = 0;
static LONG    g_scan_gen = -1;
static uint32_t g_nwarm = 0, g_nloaded = 0, g_nnew = 0;
static double  g_warm_ms = 0;
static char    g_path[MAX_PATH];
static ID3D11DeviceContext* g_imm = nullptr;
static ID3D11Device* g_dev = nullptr;

// ---------------------------------------------------------------- device hooks (any thread)
typedef HRESULT(STDMETHODCALLTYPE* csh_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
typedef HRESULT(STDMETHODCALLTYPE* cil_t)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
static csh_t o_cvs, o_cps; static cil_t o_cil;
static void note_obj(IUnknown* o, uint64_t h) {
    if (!o) return;
    o->AddRef();
    EnterCriticalSection(&g_cs);
    map_put(g_obj2hash, (uint64_t)(uintptr_t)o, h);
    uint64_t old;
    if (!map_get(g_hash2obj, h, &old)) map_put(g_hash2obj, h, (uint64_t)(uintptr_t)o);
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_shaders);
}
// FX-PSOWARM-REUSE (default 1): the game creates the same shaders again for every model it loads (D 28/11/2026:
// 6459 shader/layout creations of a few hundred distinct ones in a session). The driver compiles at creation under
// its device lock, so the render thread's Map/Present waited 20-40 ms behind the loader thread. A create with the
// bytecode of a live object returns that object (shaders and layouts are immutable).
static int g_reuse = 1;
static volatile LONG g_reused = 0;
static bool reuse(uint64_t h, void** c) {
    if (!g_reuse || !c) return false;
    uint64_t v = 0;
    EnterCriticalSection(&g_cs);
    bool ok = map_get(g_hash2obj, h, &v) && v;
    if (ok) ((IUnknown*)(uintptr_t)v)->AddRef();
    LeaveCriticalSection(&g_cs);
    if (ok) { *c = (void*)(uintptr_t)v; InterlockedIncrement(&g_reused); }
    return ok;
}
static HRESULT STDMETHODCALLTYPE h_cvs(ID3D11Device* d, const void* b, SIZE_T l, ID3D11ClassLinkage* k, void** c) {
    uint64_t h = b ? fnv(b, l, 0x5653) : 0;
    if (b && !k && reuse(h, c)) return S_OK;
    HRESULT r = o_cvs(d, b, l, k, c);
    if (SUCCEEDED(r) && c && *c && b) note_obj((IUnknown*)*c, h);
    return r;
}
static HRESULT STDMETHODCALLTYPE h_cps(ID3D11Device* d, const void* b, SIZE_T l, ID3D11ClassLinkage* k, void** c) {
    uint64_t h = b ? fnv(b, l, 0x5053) : 0;
    if (b && !k && reuse(h, c)) return S_OK;
    HRESULT r = o_cps(d, b, l, k, c);
    if (SUCCEEDED(r) && c && *c && b) note_obj((IUnknown*)*c, h);
    return r;
}
static HRESULT STDMETHODCALLTYPE h_cil(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* e, UINT n, const void* b, SIZE_T l, ID3D11InputLayout** c) {
    uint64_t h = fnv(b, l, 0x494C);
    for (UINT i = 0; e && i < n; i++) {
        if (e[i].SemanticName) h = fnv(e[i].SemanticName, strlen(e[i].SemanticName), h);
        uint32_t w[6] = { e[i].SemanticIndex, (uint32_t)e[i].Format, e[i].InputSlot, e[i].AlignedByteOffset,
                          (uint32_t)e[i].InputSlotClass, e[i].InstanceDataStepRate };
        h = fnv(w, sizeof(w), h);
    }
    if (e && b && reuse(h, (void**)c)) return S_OK;
    HRESULT r = o_cil(d, e, n, b, l, c);
    if (SUCCEEDED(r) && c && *c && e) note_obj(*c, h);
    return r;
}

// ---------------------------------------------------------------- texture touch (FX-PSOWARM-TOUCH, default 1)
// D 28/11/2026 (DIAG-D3DHITCH): hitch frames have the same uploads and draws as normal ones but GPU frames of 30-50 ms,
// and the game's 12 MB dynamic vertex buffer Map(DISCARD) waits 5-40 ms behind them; nothing is created in those
// frames. Textures are created on the loader thread earlier; the driver finishes their placement the first time a
// frame uses them. Each new texture is queued and a 4x4 corner of it copied into a scratch texture on the render
// thread before Present, a few per frame, so that work happens before the model is drawn.
typedef HRESULT(STDMETHODCALLTYPE* ctex_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static ctex_t o_ctex;
static int g_touch = 0;   // default 0: made battle-start hitches worse (D 28/11/2026: 150-214 ms frames)
enum { TQ = 4096 };
static ID3D11Texture2D* g_tq[TQ];
static volatile LONG g_tq_w = 0;
static LONG g_tq_r = 0;
static volatile LONG g_touched = 0, g_tq_drop = 0;
static HRESULT STDMETHODCALLTYPE h_ctex(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* td, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT r = o_ctex(d, td, init, out);
    if (g_touch && SUCCEEDED(r) && out && *out && td && init && td->SampleDesc.Count <= 1 &&
        (td->Usage == D3D11_USAGE_DEFAULT || td->Usage == D3D11_USAGE_IMMUTABLE) && (td->BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        EnterCriticalSection(&g_cs);
        if (g_tq_w - g_tq_r < TQ) { (*out)->AddRef(); g_tq[g_tq_w % TQ] = *out; g_tq_w++; }
        else g_tq_drop++;
        LeaveCriticalSection(&g_cs);
    }
    return r;
}
struct Scratch { DXGI_FORMAT f; ID3D11Texture2D* t; };
static Scratch g_scr[64]; static int g_nscr = 0;
static ID3D11Texture2D* scratch_for(DXGI_FORMAT f);
static void touch_some(double budget_ms, double (*now)()) {
    double t0 = now();
    for (int n = 0; n < 64 && now() - t0 < budget_ms; n++) {
        ID3D11Texture2D* t = nullptr;
        EnterCriticalSection(&g_cs);
        if (g_tq_r < g_tq_w) { t = g_tq[g_tq_r % TQ]; g_tq_r++; }
        LeaveCriticalSection(&g_cs);
        if (!t) return;
        D3D11_TEXTURE2D_DESC td; t->GetDesc(&td);
        ID3D11Texture2D* s = scratch_for(td.Format);
        if (s && td.Width >= 4 && td.Height >= 4) {
            D3D11_BOX b = { 0, 0, 0, 4, 4, 1 };
            g_imm->CopySubresourceRegion(s, 0, 0, 0, 0, t, 0, &b);
            g_touched++;
        }
        t->Release();
    }
}

// ---------------------------------------------------------------- context state tracking (render thread)
static bool g_warming = false;
struct Cur {
    void *vs, *ps, *gs, *il, *blend, *ds, *rs;
    uint32_t topo, stride[8], rtf[8], nrt, dsf, samples;
};
static Cur g_cur = {};
static bool track(ID3D11DeviceContext* c) { return c == g_imm && !g_warming; }

typedef void(STDMETHODCALLTYPE* setsh_t)(ID3D11DeviceContext*, void*, void* const*, UINT);
typedef void(STDMETHODCALLTYPE* setil_t)(ID3D11DeviceContext*, ID3D11InputLayout*);
typedef void(STDMETHODCALLTYPE* setvb_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void(STDMETHODCALLTYPE* settopo_t)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE* setrt_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* setrtu_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* setbl_t)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
typedef void(STDMETHODCALLTYPE* setds_t)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
typedef void(STDMETHODCALLTYPE* setrs_t)(ID3D11DeviceContext*, ID3D11RasterizerState*);
typedef void(STDMETHODCALLTYPE* dri_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* dr_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* drii_t)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* dri2_t)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
static setsh_t o_vss, o_pss, o_gss; static setil_t o_il; static setvb_t o_vb; static settopo_t o_topo;
static setrt_t o_rt; static setrtu_t o_rtu; static setbl_t o_bl; static setds_t o_ds; static setrs_t o_rs;
static dri_t o_dri; static dr_t o_dr; static drii_t o_drii; static dri2_t o_dri2;

static void STDMETHODCALLTYPE h_vss(ID3D11DeviceContext* c, void* s, void* const* k, UINT n) { if (track(c)) g_cur.vs = s; o_vss(c, s, k, n); }
static void STDMETHODCALLTYPE h_pss(ID3D11DeviceContext* c, void* s, void* const* k, UINT n) { if (track(c)) g_cur.ps = s; o_pss(c, s, k, n); }
static void STDMETHODCALLTYPE h_gss(ID3D11DeviceContext* c, void* s, void* const* k, UINT n) { if (track(c)) g_cur.gs = s; o_gss(c, s, k, n); }
static void STDMETHODCALLTYPE h_il(ID3D11DeviceContext* c, ID3D11InputLayout* l) { if (track(c)) g_cur.il = l; o_il(c, l); }
static void STDMETHODCALLTYPE h_vb(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* off) {
    if (track(c) && st) for (UINT i = 0; i < n && s + i < 8; i++) g_cur.stride[s + i] = b && b[i] ? st[i] : 0;
    if (!g_warming && vbring_setvb(c, s, n, b, st, off)) return;
    o_vb(c, s, n, b, st, off);
}
static void STDMETHODCALLTYPE h_topo(ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY t) { if (track(c)) g_cur.topo = (uint32_t)t; o_topo(c, t); }
static void note_rt(UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d) {
    g_cur.nrt = 0; g_cur.samples = 1; g_cur.dsf = 0;
    for (UINT i = 0; i < n && i < 8; i++) {
        g_cur.rtf[i] = 0;
        if (!v || !v[i]) continue;
        D3D11_RENDER_TARGET_VIEW_DESC rd; v[i]->GetDesc(&rd);
        g_cur.rtf[i] = (uint32_t)rd.Format;
        if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS) {
            ID3D11Resource* r = nullptr; v[i]->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
                D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); g_cur.samples = td.SampleDesc.Count; t->Release();
            }
            if (r) r->Release();
        }
        g_cur.nrt = i + 1;
    }
    for (UINT i = g_cur.nrt; i < 8; i++) g_cur.rtf[i] = 0;
    if (d) { D3D11_DEPTH_STENCIL_VIEW_DESC dd; d->GetDesc(&dd); g_cur.dsf = (uint32_t)dd.Format; }
}
static void STDMETHODCALLTYPE h_rt(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d) {
    if (track(c)) note_rt(n, v, d);
    o_rt(c, n, v, d);
}
static void STDMETHODCALLTYPE h_rtu(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d,
                                    UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* ui) {
    if (track(c) && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) note_rt(n, v, d);
    o_rtu(c, n, v, d, us, un, u, ui);
}
static void STDMETHODCALLTYPE h_bl(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) { if (track(c)) g_cur.blend = b; o_bl(c, b, f, m); }
static void STDMETHODCALLTYPE h_ds(ID3D11DeviceContext* c, ID3D11DepthStencilState* d, UINT r) { if (track(c)) g_cur.ds = d; o_ds(c, d, r); }
static void STDMETHODCALLTYPE h_rs(ID3D11DeviceContext* c, ID3D11RasterizerState* r) { if (track(c)) g_cur.rs = r; o_rs(c, r); }

static void on_draw(ID3D11DeviceContext* c) {
    if (!track(c) || !g_recs || g_cur.gs || !g_cur.vs) return;
    uint64_t pk = fnv(&g_cur, sizeof(g_cur));
    uint64_t dummy;
    if (map_get(g_seen, pk, &dummy)) return;
    map_put(g_seen, pk, 1);
    Pso p; memset(&p, 0, sizeof(p));
    EnterCriticalSection(&g_cs);
    bool ok = map_get(g_obj2hash, (uint64_t)(uintptr_t)g_cur.vs, &p.vs) &&
              (!g_cur.ps || map_get(g_obj2hash, (uint64_t)(uintptr_t)g_cur.ps, &p.ps)) &&
              (!g_cur.il || map_get(g_obj2hash, (uint64_t)(uintptr_t)g_cur.il, &p.il));
    LeaveCriticalSection(&g_cs);
    if (!ok) return;                                   // created before the hooks: cannot be named
    if (g_cur.blend) { ((ID3D11BlendState*)g_cur.blend)->GetDesc(&p.blend); p.has |= 1; }
    if (g_cur.rs) { ((ID3D11RasterizerState*)g_cur.rs)->GetDesc(&p.rast); p.has |= 2; }
    if (g_cur.ds) { ((ID3D11DepthStencilState*)g_cur.ds)->GetDesc(&p.ds); p.has |= 4; }
    memcpy(p.rtf, g_cur.rtf, sizeof(p.rtf));
    p.nrt = g_cur.nrt; p.dsf = g_cur.dsf; p.samples = g_cur.samples; p.topo = g_cur.topo;
    memcpy(p.stride, g_cur.stride, sizeof(p.stride));
    uint64_t ck = fnv(&p, sizeof(p));
    if (map_get(g_known, ck, &dummy) || g_nrec >= MAX_RECS) return;
    map_put(g_known, ck, 1);
    g_recs[g_nrec] = p; g_warmed[g_nrec] = 1;          // drawn by the game already this session
    g_nrec++; g_nnew++;
}
static void STDMETHODCALLTYPE h_dri(ID3D11DeviceContext* c, UINT a, UINT b, INT d) { on_draw(c); if (!g_warming) vbring_draw(c, 1, a, b, d, 1, 0); o_dri(c, a, b, d); }
static void STDMETHODCALLTYPE h_dr(ID3D11DeviceContext* c, UINT a, UINT b) { on_draw(c); if (!g_warming) vbring_draw(c, 0, a, b, 0, 1, 0); o_dr(c, a, b); }
static void STDMETHODCALLTYPE h_drii(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) { on_draw(c); if (!g_warming) vbring_draw(c, 3, a, d, e, b, f); o_drii(c, a, b, d, e, f); }
static void STDMETHODCALLTYPE h_dri2(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) { on_draw(c); if (!g_warming) vbring_draw(c, 2, a, d, 0, b, e); o_dri2(c, a, b, d, e); }

static ID3D11Texture2D* scratch_for(DXGI_FORMAT f) {
    for (int i = 0; i < g_nscr; i++) if (g_scr[i].f == f) return g_scr[i].t;
    if (g_nscr >= 64) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture2D* t = nullptr;
    g_dev->CreateTexture2D(&td, nullptr, &t);       // through o_ctex? no: g_dev->CreateTexture2D is hooked, no init data -> not queued
    g_scr[g_nscr++] = { f, t };
    return t;
}

// ---------------------------------------------------------------- warm-up (render thread, before Present)
static ID3DDeviceContextState* g_state = nullptr;
static ID3D11DeviceContext1* g_ctx1 = nullptr;
static ID3D11Buffer* g_vbuf = nullptr;
struct Target { uint32_t fmt, samples; bool depth; ID3D11View* view; };
static Target g_tg[48]; static int g_ntg = 0;
static ID3D11View* target(uint32_t fmt, uint32_t samples, bool depth) {
    for (int i = 0; i < g_ntg; i++) if (g_tg[i].fmt == fmt && g_tg[i].samples == samples && g_tg[i].depth == depth) return g_tg[i].view;
    if (g_ntg >= 48) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = (DXGI_FORMAT)fmt;
    td.SampleDesc.Count = samples ? samples : 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* t = nullptr; ID3D11View* v = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &t)) && t) {
        if (depth) g_dev->CreateDepthStencilView(t, nullptr, (ID3D11DepthStencilView**)&v);
        else g_dev->CreateRenderTargetView(t, nullptr, (ID3D11RenderTargetView**)&v);
        t->Release();
    }
    g_tg[g_ntg++] = { fmt, samples, depth, v };     // null view: format refused, remembered
    return v;
}
static bool obj(uint64_t h, void** out) {
    uint64_t v = 0;
    EnterCriticalSection(&g_cs);
    bool ok = map_get(g_hash2obj, h, &v);
    LeaveCriticalSection(&g_cs);
    *out = (void*)(uintptr_t)v;
    return ok;
}
static bool warm_one(const Pso& p) {
    void *vs, *ps = nullptr, *il = nullptr;
    if (!obj(p.vs, &vs) || (p.ps && !obj(p.ps, &ps)) || (p.il && !obj(p.il, &il))) return false;
    ID3D11RenderTargetView* rtv[8] = {};
    for (uint32_t i = 0; i < p.nrt && i < 8; i++) if (p.rtf[i]) rtv[i] = (ID3D11RenderTargetView*)target(p.rtf[i], p.samples, false);
    ID3D11DepthStencilView* dsv = p.dsf ? (ID3D11DepthStencilView*)target(p.dsf, p.samples, true) : nullptr;
    ID3D11BlendState* bs = nullptr; ID3D11RasterizerState* rs = nullptr; ID3D11DepthStencilState* ds = nullptr;
    if (p.has & 1) g_dev->CreateBlendState(&p.blend, &bs);
    if (p.has & 2) g_dev->CreateRasterizerState(&p.rast, &rs);
    if (p.has & 4) g_dev->CreateDepthStencilState(&p.ds, &ds);
    ID3D11Buffer* vb[8]; UINT off[8] = {}, st[8];
    for (int i = 0; i < 8; i++) { vb[i] = g_vbuf; st[i] = p.stride[i]; }
    g_imm->IASetInputLayout((ID3D11InputLayout*)il);
    g_imm->IASetVertexBuffers(0, 8, vb, st, off);
    g_imm->IASetPrimitiveTopology(p.topo ? (D3D11_PRIMITIVE_TOPOLOGY)p.topo : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_imm->VSSetShader((ID3D11VertexShader*)vs, nullptr, 0);
    g_imm->PSSetShader((ID3D11PixelShader*)ps, nullptr, 0);
    g_imm->OMSetRenderTargets(p.nrt, rtv, dsv);
    g_imm->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    g_imm->OMSetDepthStencilState(ds, 0);
    g_imm->RSSetState(rs);
    D3D11_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
    g_imm->RSSetViewports(1, &vp);
    g_imm->Draw(3, 0);
    if (bs) bs->Release(); if (rs) rs->Release(); if (ds) ds->Release();
    return true;
}

static LARGE_INTEGER g_qpf;
static double now_ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_qpf.QuadPart; }

static void save() {
    if (g_saved >= g_nrec) return;
    FILE* f = nullptr;
    bool fresh = GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES;
    if (fopen_s(&f, g_path, "ab") || !f) return;
    if (fresh) { uint32_t h[3] = { MAGIC, VERSION, (uint32_t)sizeof(Pso) }; fwrite(h, sizeof(h), 1, f); }
    fwrite(g_recs + g_saved, sizeof(Pso), g_nrec - g_saved, f);
    fclose(f);
    g_saved = g_nrec;
}

static bool setup(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) || !g_dev) return false;
    g_dev->GetImmediateContext(&g_imm);
    ID3D11Device1* d1 = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) || !d1 ||
        FAILED(g_imm->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx1)) || !g_ctx1) {
        log_line("FX-PSOWARM: no D3D11.1 device interface - recording only");
        if (d1) d1->Release();
        return true;
    }
    D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel();
    HRESULT hr = d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_state);
    d1->Release();
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 65536; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    static uint8_t zero[65536];
    D3D11_SUBRESOURCE_DATA sd = { zero, 0, 0 };
    g_dev->CreateBuffer(&bd, &sd, &g_vbuf);
    if (FAILED(hr) || !g_state || !g_vbuf) log_line("FX-PSOWARM: warm-up state not created (0x%08lX) - recording only", (unsigned long)hr);
    return true;
}

void d3dhitch_present(void* dev, void* ctx);   // DIAG-D3DHITCH GPU timing
static void on_present(void* scv) {
    if (!row_FXPSOWARM.on || g_disabled) return;
    static bool tried = false;
    if (!tried) { tried = true; if (!setup((IDXGISwapChain*)scv)) return; }
    if (!g_imm) return;
    d3dhitch_present(g_dev, g_imm);
    vbring_present(g_dev, g_imm);
    static uint32_t frames = 0;
    if (++frames % 600 == 0) {
        save();
        if (g_nwarm || g_nnew)
            log_line("FX-PSOWARM: %u combinations known (%u from the file, %u new this session), %u warmed in %.0f ms, %ld shaders/layouts created, %ld creates reused; textures touched %ld, queued %ld, dropped %ld",
                     g_nrec, g_nloaded, g_nnew, g_nwarm, g_warm_ms, g_shaders, g_reused, g_touched, g_tq_w - g_tq_r, g_tq_drop);
    }
    if (g_touch) __try { touch_some(1.5, now_ms); } __except (EXCEPTION_EXECUTE_HANDLER) { g_touch = 0; log_line("FX-PSOWARM: fault touching textures - touch off"); }
    if (!g_state || !g_ctx1 || !g_vbuf) return;
    LONG gen = g_shaders;
    if (gen == g_scan_gen && g_cursor >= g_nrec) return;   // nothing new to try
    if (g_cursor >= g_nrec) { g_cursor = 0; g_scan_gen = gen; }
    double t0 = now_ms();
    ID3DDeviceContextState* prev = nullptr;
    bool swapped = false;
    __try {
        while (g_cursor < g_nrec && now_ms() - t0 < 3.0) {
            uint32_t i = g_cursor++;
            if (g_warmed[i]) continue;
            if (!swapped) { g_warming = true; g_ctx1->SwapDeviceContextState(g_state, &prev); swapped = true; }
            if (warm_one(g_recs[i])) { g_warmed[i] = 1; g_nwarm++; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("FX-PSOWARM: fault 0x%08lX warming - warm-up off", GetExceptionCode());
        g_state = nullptr;
    }
    if (swapped) { g_ctx1->SwapDeviceContextState(prev, nullptr); if (prev) prev->Release(); g_warming = false; }
    g_warm_ms += now_ms() - t0;
}

// ---------------------------------------------------------------- install
static void load() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "rb") || !f) return;
    uint32_t h[3] = {};
    if (fread(h, sizeof(h), 1, f) != 1 || h[0] != MAGIC || h[1] != VERSION || h[2] != sizeof(Pso)) {
        fclose(f);
        log_line("FX-PSOWARM: %s has another format - started over", g_path);
        DeleteFileA(g_path);
        return;
    }
    Pso p;
    while (g_nrec < MAX_RECS && fread(&p, sizeof(p), 1, f) == 1) {
        uint64_t ck = fnv(&p, sizeof(p)), d;
        if (map_get(g_known, ck, &d)) continue;
        map_put(g_known, ck, 1);
        g_recs[g_nrec++] = p;
    }
    fclose(f);
    g_saved = g_nloaded = g_nrec;
}

static bool install() {
    QueryPerformanceFrequency(&g_qpf);
    g_reuse = config_row("FX-PSOWARM-REUSE", 1);
    g_touch = config_row("FX-PSOWARM-TOUCH", 0);
    InitializeCriticalSection(&g_cs);
    g_recs = (Pso*)calloc(MAX_RECS, sizeof(Pso));
    g_warmed = (uint8_t*)calloc(MAX_RECS, 1);
    if (!g_recs || !g_warmed || !map_init(g_obj2hash, 1 << 15) || !map_init(g_hash2obj, 1 << 15) ||
        !map_init(g_seen, 1 << 16) || !map_init(g_known, 1 << 15)) return false;
    DWORD n = GetModuleFileNameA(nullptr, g_path, MAX_PATH);
    while (n && g_path[n - 1] != 0x5C) n--;
    if (!n || n + 16 > MAX_PATH) return false;
    strcpy_s(g_path + n, MAX_PATH - n, "fps60_pso.bin");
    load();

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) || !dev || !ctx) { log_line("  FX-PSOWARM: throwaway device failed 0x%08lX", (unsigned long)hr); return false; }
    void** dv = *(void***)dev; void** cv = *(void***)ctx;
    bool ok = hook_entry("FX-PSOWARM CreateTexture2D", dv[5], (void*)&h_ctex, (void**)&o_ctex) &&
              hook_entry("FX-PSOWARM CreateVertexShader", dv[12], (void*)&h_cvs, (void**)&o_cvs) &&
              hook_entry("FX-PSOWARM CreatePixelShader", dv[15], (void*)&h_cps, (void**)&o_cps) &&
              hook_entry("FX-PSOWARM CreateInputLayout", dv[11], (void*)&h_cil, (void**)&o_cil) &&
              hook_entry("FX-PSOWARM PSSetShader", cv[9], (void*)&h_pss, (void**)&o_pss) &&
              hook_entry("FX-PSOWARM VSSetShader", cv[11], (void*)&h_vss, (void**)&o_vss) &&
              hook_entry("FX-PSOWARM DrawIndexed", cv[12], (void*)&h_dri, (void**)&o_dri) &&
              hook_entry("FX-PSOWARM Draw", cv[13], (void*)&h_dr, (void**)&o_dr) &&
              hook_entry("FX-PSOWARM IASetInputLayout", cv[17], (void*)&h_il, (void**)&o_il) &&
              hook_entry("FX-PSOWARM IASetVertexBuffers", cv[18], (void*)&h_vb, (void**)&o_vb) &&
              hook_entry("FX-PSOWARM DrawIndexedInstanced", cv[20], (void*)&h_drii, (void**)&o_drii) &&
              hook_entry("FX-PSOWARM DrawInstanced", cv[21], (void*)&h_dri2, (void**)&o_dri2) &&
              hook_entry("FX-PSOWARM GSSetShader", cv[23], (void*)&h_gss, (void**)&o_gss) &&
              hook_entry("FX-PSOWARM IASetPrimitiveTopology", cv[24], (void*)&h_topo, (void**)&o_topo) &&
              hook_entry("FX-PSOWARM OMSetRenderTargets", cv[33], (void*)&h_rt, (void**)&o_rt) &&
              hook_entry("FX-PSOWARM OMSetRenderTargetsAndUAVs", cv[34], (void*)&h_rtu, (void**)&o_rtu) &&
              hook_entry("FX-PSOWARM OMSetBlendState", cv[35], (void*)&h_bl, (void**)&o_bl) &&
              hook_entry("FX-PSOWARM OMSetDepthStencilState", cv[36], (void*)&h_ds, (void**)&o_ds) &&
              hook_entry("FX-PSOWARM RSSetState", cv[43], (void*)&h_rs, (void**)&o_rs);
    ctx->Release(); dev->Release();
    if (!ok) { log_line("  FX-PSOWARM: D3D11 hooks failed"); return false; }
    vbring_set_vb_orig((void*)o_vb);
    vsync_set_present_cb(on_present);
    if (!vsync_hook_for_diag()) { log_line("  FX-PSOWARM: Present hook failed"); return false; }
    log_line("  FX-PSOWARM: %u shader/state combinations loaded from %s", g_nrec, g_path);
    return true;
}

static void stop() { save(); }

Row row_FXPSOWARM = {
    "FX-PSOWARM",
    "shader/state combinations the game drew before are drawn once off-screen at startup (first-draw driver compile hitches)",
    1, install, nullptr, nullptr, 0, stop
};
