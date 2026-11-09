#!/usr/bin/env python3
"""Spike C: print markdown tables from one or more results.jsonl files.

usage: table.py results.jsonl [more.jsonl ...]
"""
import json
import sys


def mb(kb):
    return "–" if kb is None or kb < 0 else f"{kb / 1024:.0f}"


def main():
    rows = []
    for path in sys.argv[1:]:
        with open(path) as f:
            rows += [json.loads(l) for l in f if l.strip()]
    print("| Run | Mode | N | Flutter fps | Raster p50 / p90 / p99 (ms) | UI build p90 (ms) "
          "| Native tick p50 / p90 / p99 (ms) | Ticks | Buffers (max/win × size) "
          "| App CPU (% of 1 core) | RSS max (MB) | PSS / footprint end (MB) | Host CPU % / energy |")
    print("|" + "---|" * 13)
    for r in rows:
        fr, nat, ex = r["frames"], r["native"], r["native"]["extra"]
        mode = r["mode"] if r["count"] else "baseline"
        if r["mode"] == "atlas":
            mode += f" {r['atlas']}" + (f" g{r['gutter']}" if r.get("gutter") else "")
        mem = r["memory"]
        last = mem["last"]
        pss = last.get("pssKb", last.get("footprintKb"))
        if "footprint" in r and r["footprint"].get("physFootprintMb"):
            pss = r["footprint"]["physFootprintMb"] * 1024
        bufs = ex.get("distinctBuffers", 0)
        per = ex.get("maxBuffersPerWindow", 0)
        size = ex.get("bufferBytes", 0) / 1048576
        buf = "–" if not bufs else f"{bufs} ({per}/win × {size:.2f} MB) = {bufs * size:.0f} MB"
        host = r.get("host") or {}
        # iOS: texture-only frames report no FrameTiming; use the engine
        # timeline's Rasterizer::DrawToSurfaces instead (marked †).
        tl = (r.get("timeline") or {}).get("events", {}).get("Rasterizer::DrawToSurfaces")
        if not fr["raster"]["n"] and tl:
            fr = dict(fr, fps=f"{tl['perSec']}†", raster=tl,
                      build={"p90": "n/a"})
        print(
            f"| `{r['label']}` | {mode} | {r['count']} | {fr['fps']} "
            f"| {fr['raster']['p50']} / {fr['raster']['p90']} / {fr['raster']['p99']} "
            f"| {fr['build']['p90']} "
            f"| {nat['work']['p50']} / {nat['work']['p90']} / {nat['work']['p99']} "
            f"| {nat['ticks']}/{nat['expectedTicks']} | {buf} "
            f"| {r['cpuPctOfOneCore']} | {mb(mem['max'].get('rssKb'))} | {mb(pss)} "
            f"| {host.get('cpuPct', '–')} / {host.get('power', '–')} |"
        )
    print()
    print("Drift (raster p90 ms / app CPU % per 30-s window):")
    print()
    print("| Run | " + " | ".join(f"{i * 30}–{(i + 1) * 30} s" for i in range(4)) + " |")
    print("|---|---|---|---|---|")
    for r in rows:
        if r["runSecs"] != 120:
            continue
        cells = [f"{w['rasterP90']} / {w['cpuPct']}" for w in r["windows"]]
        print(f"| `{r['label']}` | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
