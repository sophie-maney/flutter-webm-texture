#!/usr/bin/env python3
"""Spike C: turn one run's logs into a single JSON line.

usage: collect.py <label> <out-dir>

Reads <label>.log (logcat or the simulator's log stream), reassembles the
chunked SPIKE_C_RESULT JSON, and adds host-side samples from
<label>.hosttop.txt (macOS `top`: %CPU and POWER of the emulator or
simulator app process) and, on Android, the TOTAL/Graphics lines of
`dumpsys meminfo`. Stdlib only.
"""
import json
import re
import statistics
import sys
from pathlib import Path


def result(log: str):
    parts = {}
    total = None
    for m in re.finditer(r"SPIKE_C_RESULT (\d+)/(\d+) (.*)", log):
        i, n, body = int(m[1]), int(m[2]), m[3]
        # os_log escapes nothing we use, logcat appends nothing, but strip
        # trailing CR and the iOS "flutter: " artefacts defensively.
        parts[i] = body.rstrip("\r")
        total = n
    if total is None or len(parts) != total:
        raise SystemExit(f"incomplete SPIKE_C_RESULT ({len(parts)}/{total})")
    return json.loads("".join(parts[i] for i in range(1, total + 1)))


def host_top(path: Path, skip_s: float, run_s: float):
    """Mean %CPU and POWER over the measurement window (5-s samples)."""
    if not path.exists():
        return None
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        f = line.split()
        if len(f) == 3 and f[0].isdigit():
            try:
                rows.append((float(f[1]), float(f[2])))
            except ValueError:
                pass
    # top's first sample is a since-boot average; samples are 5 s apart.
    rows = rows[1:]
    lo, hi = int(skip_s // 5) + 1, int((skip_s + run_s) // 5)
    win = rows[lo:hi] or rows
    if not win:
        return None
    return {
        "samples": len(win),
        "cpuPct": round(statistics.mean(r[0] for r in win), 1),
        "power": round(statistics.mean(r[1] for r in win), 1),
    }


def meminfo(path: Path):
    if not path.exists():
        return None
    text = path.read_text(errors="replace")
    out = {}
    for key, pat in {
        "totalPssKb": r"^\s*TOTAL PSS:\s+(\d+)",
        "totalRssKb": r"TOTAL RSS:\s+(\d+)",
        "graphicsKb": r"^\s*Graphics:\s+(\d+)",
        "glMtrackKb": r"^\s*GL mtrack\s+(\d+)",
        "eglMtrackKb": r"^\s*EGL mtrack\s+(\d+)",
        "nativeHeapPssKb": r"^\s*Native Heap\s+(\d+)",
    }.items():
        m = re.search(pat, text, re.M)
        if m:
            out[key] = int(m[1])
    if "totalPssKb" not in out:
        m = re.search(r"^\s*TOTAL\s+(\d+)", text, re.M)
        if m:
            out["totalPssKb"] = int(m[1])
    return out


def main():
    label, out = sys.argv[1], Path(sys.argv[2])
    log = (out / f"{label}.log").read_text(errors="replace")
    r = result(log)
    warm = 10.5
    r["host"] = host_top(out / f"{label}.hosttop.txt", warm + 5, r["runSecs"])
    r["meminfo"] = meminfo(out / f"{label}.meminfo.txt")
    footprint = out / f"{label}.footprint.txt"
    if footprint.exists():
        text = footprint.read_text(errors="replace")
        fp = {}
        for key, pat in {
            "physFootprintMb": r"phys_footprint:\s+([\d.]+) MB",
            "peakMb": r"phys_footprint_peak:\s+([\d.]+) MB",
        }.items():
            m = re.search(pat, text)
            if m:
                fp[key] = float(m[1])
        m = re.search(r"^\s*(\S+ \S+)\s+\S+ \S+\s+\S+ \S+\s+\d+\s+Untagged", text, re.M)
        if m:
            fp["untagged"] = m[1]
        r["footprint"] = fp
    timeline = out / f"{label}.timeline.json"
    if timeline.exists() and timeline.stat().st_size:
        r["timeline"] = json.loads(timeline.read_text())
    print(json.dumps(r, separators=(",", ":")))


if __name__ == "__main__":
    main()
