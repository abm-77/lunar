#!/usr/bin/env python3
"""Graphics debugging tool for Umbral frame dumps and asset packs.

Subcommands:
  info   [ppm]                       — frame stats: size, non-black count, bbox, color buckets
  ascii  [x0 y0 x1 y1] [ppm]        — ASCII art of a pixel region (full frame if no rect)
  pixel  x,y [x,y ...] [ppm]        — sample exact RGB values at given coordinates
  scan   x0,y0,x1,y1 [ppm]          — scan a rect for unique colors (not just buckets)
  diff   ppm_a ppm_b [x0 y0 x1 y1]  — show pixels that differ between two frames
  hline  y [x0 x1] [ppm]            — horizontal scanline: print RGB for each pixel
  vline  x [y0 y1] [ppm]            — vertical scanline
  spv    umpack                      — extract + validate SPIR-V from .umpack

Default PPM path: /tmp/frame_dump.ppm
"""

import struct, sys, os, subprocess, shutil

DEFAULT_PPM = '/tmp/frame_dump.ppm'

def load_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos = 0
    lines = []
    while len(lines) < 3:
        end = data.index(b'\n', pos)
        lines.append(data[pos:end].decode())
        pos = end + 1
    W, H = map(int, lines[1].split())
    return data[pos:], W, H

def px(pixels, W, x, y):
    i = (y * W + x) * 3
    return pixels[i], pixels[i+1], pixels[i+2]

# ---- info ----
def cmd_info(args):
    path = args[0] if args else DEFAULT_PPM
    pixels, W, H = load_ppm(path)
    print(f'file: {path}  size: {W}x{H}  modified: {os.path.getmtime(path):.0f}')

    non_black = 0
    min_x, max_x, min_y, max_y = W, -1, H, -1
    buckets = {}
    for y in range(H):
        for x in range(W):
            r, g, b = px(pixels, W, x, y)
            if r > 10 or g > 10 or b > 10:
                non_black += 1
                min_x, max_x = min(min_x, x), max(max_x, x)
                min_y, max_y = min(min_y, y), max(max_y, y)
                key = (r >> 4, g >> 4, b >> 4)
                buckets[key] = buckets.get(key, 0) + 1

    total = W * H
    print(f'non-black: {non_black}/{total} ({100*non_black/total:.1f}%)')
    if non_black == 0:
        print('ALL BLACK'); return

    print(f'bbox: ({min_x},{min_y})-({max_x},{max_y})')
    top = sorted(buckets.items(), key=lambda x: -x[1])[:8]
    print('color buckets (approx RGB) → count:')
    for (rb, gb, bb), cnt in top:
        print(f'  ({rb*16:3d},{gb*16:3d},{bb*16:3d}) → {cnt}')

# ---- ascii ----
def cmd_ascii(args):
    coords = []
    path = DEFAULT_PPM
    for a in args:
        if a.endswith('.ppm'):
            path = a
        else:
            coords.append(int(a))
    pixels, W, H = load_ppm(path)
    if len(coords) >= 4:
        x0, y0, x1, y1 = coords[:4]
    else:
        x0, y0, x1, y1 = 0, 0, W, H
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W, x1), min(H, y1)

    chars = ' .,:;+*#@'
    print(f'region ({x0},{y0})-({x1},{y1}) of {W}x{H}:')
    for y in range(y0, y1):
        row = ''
        for x in range(x0, x1):
            r, g, b = px(pixels, W, x, y)
            lum = (r + g + b) // 3
            row += chars[lum * (len(chars) - 1) // 255]
        print(row[:200])

# ---- pixel ----
def cmd_pixel(args):
    path = DEFAULT_PPM
    coords = []
    for a in args:
        if a.endswith('.ppm'):
            path = a
        elif ',' in a:
            x, y = map(int, a.split(','))
            coords.append((x, y))
    pixels, W, H = load_ppm(path)
    for x, y in coords:
        if 0 <= x < W and 0 <= y < H:
            r, g, b = px(pixels, W, x, y)
            print(f'({x},{y}): R={r} G={g} B={b}  (#{r:02x}{g:02x}{b:02x})')
        else:
            print(f'({x},{y}): out of bounds ({W}x{H})')

# ---- scan ----
def cmd_scan(args):
    path = DEFAULT_PPM
    rect = None
    for a in args:
        if a.endswith('.ppm'):
            path = a
        elif ',' in a:
            rect = list(map(int, a.split(',')))
    pixels, W, H = load_ppm(path)
    if rect and len(rect) == 4:
        x0, y0, x1, y1 = rect
    else:
        print('usage: scan x0,y0,x1,y1 [ppm]'); return
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W, x1), min(H, y1)

    colors = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = px(pixels, W, x, y)
            colors[c] = colors.get(c, 0) + 1

    total = (x1 - x0) * (y1 - y0)
    print(f'region ({x0},{y0})-({x1},{y1}): {len(colors)} unique colors in {total} pixels')
    top = sorted(colors.items(), key=lambda x: -x[1])[:20]
    for (r, g, b), cnt in top:
        pct = 100 * cnt / total
        bar = '#' * int(pct / 2)
        print(f'  ({r:3d},{g:3d},{b:3d}) #{r:02x}{g:02x}{b:02x}  {cnt:6d} ({pct:5.1f}%) {bar}')

# ---- diff ----
def cmd_diff(args):
    if len(args) < 2:
        print('usage: diff ppm_a ppm_b [x0 y0 x1 y1]'); return
    pa, pb = args[0], args[1]
    coords = list(map(int, args[2:6])) if len(args) >= 6 else None
    pxa, Wa, Ha = load_ppm(pa)
    pxb, Wb, Hb = load_ppm(pb)
    if Wa != Wb or Ha != Hb:
        print(f'size mismatch: {Wa}x{Ha} vs {Wb}x{Hb}'); return
    x0, y0 = (coords[0], coords[1]) if coords else (0, 0)
    x1, y1 = (coords[2], coords[3]) if coords else (Wa, Ha)

    diffs = 0
    max_delta = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            ra, ga, ba = px(pxa, Wa, x, y)
            rb, gb, bb = px(pxb, Wb, x, y)
            dr, dg, db = abs(ra-rb), abs(ga-gb), abs(ba-bb)
            d = max(dr, dg, db)
            if d > 0:
                diffs += 1
                max_delta = max(max_delta, d)
                if diffs <= 20:
                    print(f'  ({x},{y}): ({ra},{ga},{ba}) → ({rb},{gb},{bb})  Δ=({dr},{dg},{db})')
    total = (x1 - x0) * (y1 - y0)
    if diffs > 20:
        print(f'  ... and {diffs - 20} more')
    print(f'{diffs}/{total} pixels differ, max channel delta={max_delta}')

# ---- hline / vline ----
def cmd_hline(args):
    path = DEFAULT_PPM
    nums = []
    for a in args:
        if a.endswith('.ppm'): path = a
        else: nums.append(int(a))
    if not nums:
        print('usage: hline y [x0 x1] [ppm]'); return
    pixels, W, H = load_ppm(path)
    y = nums[0]
    x0 = nums[1] if len(nums) > 1 else 0
    x1 = nums[2] if len(nums) > 2 else W
    for x in range(max(0, x0), min(W, x1)):
        r, g, b = px(pixels, W, x, y)
        if r > 0 or g > 0 or b > 0:
            print(f'  x={x}: ({r},{g},{b})')

def cmd_vline(args):
    path = DEFAULT_PPM
    nums = []
    for a in args:
        if a.endswith('.ppm'): path = a
        else: nums.append(int(a))
    if not nums:
        print('usage: vline x [y0 y1] [ppm]'); return
    pixels, W, H = load_ppm(path)
    x = nums[0]
    y0 = nums[1] if len(nums) > 1 else 0
    y1 = nums[2] if len(nums) > 2 else H
    for y in range(max(0, y0), min(H, y1)):
        r, g, b = px(pixels, W, x, y)
        if r > 0 or g > 0 or b > 0:
            print(f'  y={y}: ({r},{g},{b})')

# ---- spv ----
def cmd_spv(args):
    if not args:
        print('usage: spv <assets.umpack>'); return
    path = args[0]
    with open(path, 'rb') as f:
        data = f.read()

    magic, version, endian, flags, n_entries = struct.unpack_from('<IHHII', data, 0)
    print(f'umpack: {n_entries} entries, v{version}')

    off = 16
    spirv_files = []
    for i in range(n_entries):
        name_len = struct.unpack_from('<I', data, off)[0]; off += 4
        name = data[off:off+name_len].decode(); off += name_len
        data_off, comp_sz, orig_sz, meta_type = struct.unpack_from('<QIII', data, off); off += 20
        meta = struct.unpack_from('<IIII', data, off); off += 16

        meta_names = {0: 'raw', 1: 'image', 2: 'audio', 3: 'font'}
        mt = meta_names.get(meta_type, f'?{meta_type}')
        print(f'  [{i}] {name} ({mt}, {orig_sz} bytes)')

        if not name.endswith('.umsh'):
            continue

        shader_name = os.path.splitext(name)[0]
        blob = data[data_off:data_off+orig_sz]

        # umsh header: 24 bytes
        sh_magic, sh_ver, sh_endian, sh_flags, sec_count, total, _pad = \
            struct.unpack_from('<IHHIIII', blob, 0)
        sec_off = 24
        for s in range(sec_count):
            sec_id, sec_flags, s_off, s_size = \
                struct.unpack_from('<IIQQ', blob, sec_off + s * 24)
            if sec_id != 1: continue  # STAGES
            stage_count = struct.unpack_from('<I', blob, s_off)[0]
            st_off = s_off + 4
            for st in range(stage_count):
                kind = blob[st_off]
                spv_off, spv_sz = struct.unpack_from('<II', blob, st_off + 4)
                st_off += 12
                stage = {0: 'vert', 1: 'frag'}.get(kind, f'kind{kind}')
                spv = blob[s_off + spv_off:s_off + spv_off + spv_sz]
                out = f'/tmp/{shader_name}_{stage}.spv'
                with open(out, 'wb') as sf:
                    sf.write(spv)
                spirv_files.append((out, f'{shader_name}.{stage}'))
                print(f'    {stage}: {spv_sz//4} words → {out}')

    # validate if spirv-val is available
    val = shutil.which('spirv-val')
    if val and spirv_files:
        print()
        all_ok = True
        for f, label in spirv_files:
            r = subprocess.run([val, f], capture_output=True, text=True)
            if r.returncode == 0:
                print(f'  ✓ {label}')
            else:
                print(f'  ✗ {label}:')
                for line in r.stderr.strip().split('\n'):
                    print(f'    {line}')
                all_ok = False
        if all_ok:
            print(f'all {len(spirv_files)} stages valid')
    elif spirv_files and not val:
        print('(spirv-val not found, skipping validation)')

# ---- dispatch ----
commands = {
    'info': cmd_info, 'ascii': cmd_ascii, 'pixel': cmd_pixel,
    'scan': cmd_scan, 'diff': cmd_diff, 'hline': cmd_hline,
    'vline': cmd_vline, 'spv': cmd_spv,
}

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__.strip())
        sys.exit(0)
    cmd = sys.argv[1]
    if cmd not in commands:
        print(f'unknown command: {cmd}')
        print(f'available: {", ".join(commands)}')
        sys.exit(1)
    commands[cmd](sys.argv[2:])
