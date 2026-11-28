# DXVK under ReShade + RenoDX (recommended)

AMD's D3D11 driver stalls (20-40 ms) on character swaps at 120 fps: the game rewrites a 12 MB dynamic vertex
buffer with Map(DISCARD) ~10x per frame and the driver's rename pool runs dry. DXVK handles these writes itself,
which removes the hitch. Its graphics pipeline libraries (GPL) also compile shaders in the background on GPUs
that support them (log line "Graphics pipeline libraries supported").

The mod's own attempt, FX-VBRING, caused geometry artifacts and stays off. It is only switched on by
`FX-VBRING=1` in fps60_live.txt.

## Setup (game folder)

ReShade must stay on D3D11 so RenoDX can replace shaders; DXVK sits below it as ReShade's proxy library.

| File | Source |
|---|---|
| `dxgi.dll` | ReShade (32-bit; previously installed as d3d11.dll) |
| `d3d11.dll` | DXVK `x32/d3d11.dll` |
| `dxgi_dxvk.dll` | DXVK `x32/dxgi.dll`, renamed |
| `renodx-ffx.addon32` | unchanged |

Append to `ReShade.ini`:

```ini
[PROXY]
EnableProxyLibrary=1
ProxyLibrary=dxgi_dxvk.dll
```

`dxvk.conf`:

```
dxgi.deferSurfaceCreation = True
d3d9.deferSurfaceCreation = True
```

Tested with DXVK 3.2 and ReShade 6.8.0 on an RX 7900 XTX (28/11/2026). RenoDX, fps60 FX-PSOWARM and VSYNC all
work in this setup.

## Undo

1. Delete `d3d11.dll`, `dxgi_dxvk.dll` and `dxvk.conf`.
2. Rename `dxgi.dll` back to `d3d11.dll`.
3. Remove the `[PROXY]` section from `ReShade.ini`.
