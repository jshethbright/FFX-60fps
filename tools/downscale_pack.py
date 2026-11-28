# Downscale an FFX HD .dds.phyre texture pack by dropping top mip levels (no re-encode).
# usage: python downscale_pack.py SRC DST [--dry]
# Unchanged files are hard-linked into DST (same volume); changed textures are rewritten.
import os, sys, struct, time
from concurrent.futures import ThreadPoolExecutor

CAPS = {'map': 2048, 'btlmap': 2048, 'chr': 2048, 'event': 2048, 'magic': 1024}

def chain(w, h, n, bpb):
    t = 0
    for _ in range(n):
        t += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpb
        w = max(1, w // 2); h = max(1, h // 2)
    return t

def cap_for(rel):
    parts = rel.replace(chr(92), '/').split('/')
    if len(parts) > 4 and parts[0] == 'ffx_data' and parts[2] == 'ps3data':
        return CAPS.get(parts[3])
    return None

def convert(src, cap):
    """Returns new bytes or None (leave as is)."""
    with open(src, 'rb') as fh:
        d = fh.read()
    i = d.rfind(b'PTexture2D\x00', 0, 6000)
    if i < 0: return None
    fmt = d[i + 11:i + 15]
    if fmt == b'DXT5': bpb = 16
    elif fmt == b'DXT1': bpb = 8
    else: return None
    m0, m1, _, _, w, h = struct.unpack('<IIIIII', d[i - 0x68:i - 0x50])
    if max(w, h) <= cap: return None
    levels = m0 + 1
    top = struct.unpack('<I', d[80:84])[0]
    full = chain(w, h, levels, bpb)
    hdr = len(d) - full
    if hdr < 84 or hdr > 8000 or top != max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpb or m1 != m0:
        raise ValueError('layout mismatch')
    k, drop, ww, hh = 0, 0, w, h
    while max(ww, hh) > cap and levels - k > 1:
        drop += max(1, (ww + 3) // 4) * max(1, (hh + 3) // 4) * bpb
        ww = max(1, ww // 2); hh = max(1, hh // 2); k += 1
    if not k: return None
    out = bytearray(d[:hdr])
    struct.pack_into('<I', out, 80, max(1, (ww + 3) // 4) * max(1, (hh + 3) // 4) * bpb)
    struct.pack_into('<II', out, i - 0x68, m0 - k, m1 - k)
    struct.pack_into('<II', out, i - 0x58, ww, hh)
    out += d[hdr + drop:]
    assert len(out) == hdr + chain(ww, hh, levels - k, bpb)
    return bytes(out)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    dry = '--dry' in sys.argv
    jobs = []
    for root, _, fs in os.walk(src):
        rel_root = os.path.relpath(root, src)
        for f in fs:
            jobs.append(os.path.join(rel_root, f))
    stats = {'files': 0, 'conv': 0, 'linked': 0, 'err': 0, 'in': 0, 'out': 0}
    t0 = time.time()
    prog = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'downscale_progress.txt')

    def one(rel):
        s = os.path.join(src, rel); t = os.path.join(dst, rel)
        cap = cap_for(rel) if rel.endswith('.dds.phyre') else None
        data = None
        if cap:
            try: data = convert(s, cap)
            except Exception as e:
                stats['err'] += 1; data = None
                with open(prog + '.err', 'a') as e2: e2.write(f'{rel}: {e}\n')
        sz = os.path.getsize(s)
        if not dry:
            os.makedirs(os.path.dirname(t), exist_ok=True)
            if os.path.exists(t): os.remove(t)
            if data is not None:
                with open(t, 'wb') as fh: fh.write(data)
            else:
                os.link(s, t)
        stats['files'] += 1; stats['in'] += sz
        if data is not None: stats['conv'] += 1; stats['out'] += len(data)
        else: stats['linked'] += 1; stats['out'] += sz
        if stats['files'] % 500 == 0:
            with open(prog, 'w') as fh:
                fh.write(f"{stats['files']}/{len(jobs)} files, {stats['conv']} downscaled, {stats['err']} errors, "
                         f"{stats['in']/2**30:.1f} GB -> {stats['out']/2**30:.1f} GB, {time.time()-t0:.0f} s\n")

    with ThreadPoolExecutor(8) as ex:
        list(ex.map(one, jobs))
    with open(prog, 'w') as fh:
        fh.write(f"DONE {stats['files']}/{len(jobs)} files, {stats['conv']} downscaled, {stats['err']} errors, "
                 f"{stats['in']/2**30:.1f} GB -> {stats['out']/2**30:.1f} GB, {time.time()-t0:.0f} s\n")

if __name__ == '__main__':
    main()
