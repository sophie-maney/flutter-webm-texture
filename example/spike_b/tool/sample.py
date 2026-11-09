#!/usr/bin/env python3
"""Spike B pixel sampler (stdlib only).

usage: sample.py <screenshot.png> <layout.json | log file containing SPIKE_B_LAYOUT> [label]

For every background row and every pattern block it samples 4 points (the
centres of the 16-px checker cells inside the 32-px block) in:
  T = texture tile, F = Flutter-drawn tile, C = control (bare background)
and reports:
  * max |T - F| over the valid premultiplied blocks (the correctness number);
  * for each block whose alpha is partial or which is a probe, whether T is
    closer to premultiplied source-over (s + C*(1-a)) or to a straight-alpha
    reading of the same bytes (s*a + C*(1-a)).
Also decodes the frame counter of the animated texture.
"""
import json
import re
import struct
import sys
import zlib

TOL = 3


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and interlace == 0, (depth, interlace)
            bpp = {2: 3, 6: 4}[ctype]
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, o = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[o]
        line = bytearray(raw[o + 1 : o + 1 + stride])
        o += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def load_layout(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    if text.lstrip().startswith("{"):
        return json.loads(text)
    # Chunks "SPIKE_B_LAYOUT i/n <text>"; use the last complete set.
    parts = {}
    for i, n, chunk in re.findall(r"SPIKE_B_LAYOUT (\d+)/(\d+) (.*)", text):
        i, n = int(i), int(n)
        if i == 1:
            parts = {}
        parts[i] = chunk.rstrip("\r")
        if len(parts) == n:
            done = "".join(parts[k] for k in range(1, n + 1))
    return json.loads(done)


def main():
    png, lay = sys.argv[1], sys.argv[2]
    label = sys.argv[3] if len(sys.argv) > 3 else png
    w, h, bpp, rows = read_png(png)
    L = load_layout(lay)
    pw, ph = L["physicalSize"]
    if (w, h) != (round(pw), round(ph)):
        print(f"WARNING screenshot {w}x{h} != view {pw}x{ph}; assuming top-left origin")

    def px(x, y):
        r = rows[y]
        return tuple(r[x * bpp : x * bpp + 3])

    worst, worst_name, n_ok, n_valid = 0, "", 0, 0
    behaviour = {"premultiplied": 0, "straight": 0, "neither": 0}
    probes, per_row = [], []
    for row in L["rows"]:
        tx, ty = row["texture"][:2]
        fx, fy = row["flutter"][:2]
        cx, cy = row["control"][:2]
        row_worst = 0
        for b in L["blocks"]:
            s = b["rgba"][:3]
            a = b["rgba"][3]
            probe = any(c > a for c in s)
            diffs, d_pm, d_st = [], 0, 0
            for oy in (8, 24):
                for ox in (8, 24):
                    X, Y = b["x"] + ox, b["y"] + oy
                    T = px(tx + X, ty + Y)
                    F = px(fx + X, fy + Y)
                    C = px(cx + X, cy + Y)
                    diffs.append(max(abs(t - f) for t, f in zip(T, F)))
                    pm = [min(255, sc + cc * (255 - a) / 255) for sc, cc in zip(s, C)]
                    st = [sc * a / 255 + cc * (255 - a) / 255 for sc, cc in zip(s, C)]
                    d_pm = max(d_pm, max(abs(t - e) for t, e in zip(T, pm)))
                    d_st = max(d_st, max(abs(t - e) for t, e in zip(T, st)))
            if not probe:
                n_valid += 1
                d = max(diffs)
                if d <= TOL:
                    n_ok += 1
                if d > worst:
                    worst, worst_name = d, f"{row['background']}/{b['name']}"
                row_worst = max(row_worst, d)
            if 0 < a < 255 or probe:
                # Only meaningful where the two readings differ visibly.
                if abs(d_pm - d_st) > 6:
                    kind = "premultiplied" if d_pm < d_st else "straight"
                    if min(d_pm, d_st) > 12:
                        kind = "neither"
                    behaviour[kind] += 1
                    if probe:
                        probes.append(
                            f"{row['background']}/{b['name']}: {kind} "
                            f"(err pm={d_pm:.0f} straight={d_st:.0f})"
                        )
        per_row.append(f"{row['background']}={row_worst}")

    ax, ay, aw, _ = L["anim"]
    bits = "".join(
        "1" if px(ax + i * (aw // 8) + 8, ay + 8)[0] > 128 else "0" for i in range(8)
    )
    frame = int(bits, 2)

    ok = n_ok == n_valid
    print(f"## {label}")
    print(f"valid blocks within ±{TOL}: {n_ok}/{n_valid}  -> {'PASS' if ok else 'FAIL'}")
    print(f"max |texture - flutter|: {worst} at {worst_name}; per background: {', '.join(per_row)}")
    print(f"blending of partial-alpha/probe blocks: {behaviour}")
    for p in probes:
        print(f"  probe {p}")
    print(f"anim frame counter (mod 256): {frame} ({bits})")
    print(
        "RESULT "
        + json.dumps(
            {
                "label": label,
                "pass": ok,
                "ok": n_ok,
                "valid": n_valid,
                "max_err": worst,
                "worst": worst_name,
                "behaviour": behaviour,
                "anim_frame": frame,
            }
        )
    )


if __name__ == "__main__":
    main()
