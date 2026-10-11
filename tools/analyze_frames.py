"""Reads a probe frame CSV (logs/<game>_frames_*.csv) and works out, from the test moves
(stand, stick forward, stick right, jump, camera turn), which columns hold the player and camera
positions, the world's up axis, units (speed) and handedness.

usage: python -I tools/analyze_frames.py logs/control_frames_XXXX.csv [more.csv ...]
"""
import sys

import numpy as np

STICK = 20000  # "pushed" threshold (of 32767)
CALM = 8000


def load(path):
    with open(path, encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        hexcols = {head.index("buttons"), head.index("keys")}
        rows = []
        for line in f:
            parts = line.strip().split(",")
            if len(parts) != len(head):
                continue  # a row cut short by a crash or by reading mid-write
            rows.append([int(v, 16) if i in hexcols else float(v) for i, v in enumerate(parts)])
    return head, np.array(rows, dtype=np.float64).reshape(-1, len(head))


def col(head, data, name):
    return data[:, head.index(name)]


def triples(head, data):
    """Every position-like triple worth testing: name -> (n,3) array (NaN where invalid)."""
    flags = col(head, data, "flags").astype(int)
    out = {}
    p = np.stack([col(head, data, f"p{i}") for i in range(16)], 1)
    c = np.stack([col(head, data, f"c{i}") for i in range(12)], 1)
    ok_p, ok_c = (flags & 1) > 0, (flags & 2) > 0
    # Matrix4x3 layouts: translation in the last row (9,10,11 / 12,13,14) or the last column (3,7,11)
    for name, idx in (("player[12,13,14]", (12, 13, 14)), ("player[9,10,11]", (9, 10, 11)), ("player[3,7,11]", (3, 7, 11))):
        out[name] = np.where(ok_p[:, None], p[:, idx], np.nan)
    for name, idx in (("cam[9,10,11]", (9, 10, 11)), ("cam[3,7,11]", (3, 7, 11))):
        out[name] = np.where(ok_c[:, None], c[:, idx], np.nan)
    lis = np.stack([col(head, data, k) for k in ("lis_x", "lis_y", "lis_z")], 1)
    out["listener"] = np.where(((flags & 4) > 0)[:, None], lis, np.nan)
    emi = np.stack([col(head, data, k) for k in ("emi_x", "emi_y", "emi_z")], 1)
    out["emitter"] = np.where(((flags & 8) > 0)[:, None], emi, np.nan)
    for w in range(4):
        tri = np.stack([col(head, data, f"w{3 * w + i}") for i in range(3)], 1)
        if np.any(tri != 0):
            out[f"watch{w}"] = np.where((tri != 0).any(1)[:, None], tri, np.nan)
    return {k: v for k, v in out.items() if np.isfinite(v).any() and np.nanstd(v, axis=0).max() > 1e-4}


def segments(mask, t, min_len=0.5):
    """[(t0, t1)] runs where mask holds for at least min_len seconds."""
    runs, start = [], None
    for i, m in enumerate(mask):
        if m and start is None:
            start = i
        if (not m or i == len(mask) - 1) and start is not None:
            end = i if not m else i
            if t[end] - t[start] >= min_len:
                runs.append((start, end))
            start = None
    return runs


def analyze(path):
    head, d = load(path)
    t = col(head, d, "t")
    print(f"\n=== {path}: {len(t)} rows, {t[-1] - t[0]:.1f} s, "
          f"{np.mean(1 / np.diff(t)[np.diff(t) > 0]):.0f} rows/s, no-tick rows {np.sum(col(head, d, 'flags').astype(int) & 16)//16}")
    lx, ly = col(head, d, "lx"), col(head, d, "ly")
    rx = col(head, d, "rx")
    btn = col(head, d, "buttons").astype(int)
    tri = triples(head, d)
    print("moving triples:", ", ".join(tri) or "none")

    fwd = segments((ly > STICK) & (np.abs(lx) < CALM), t)
    right = segments((lx > STICK) & (np.abs(ly) < CALM), t)
    turn = segments((rx > STICK) & (np.abs(lx) < CALM) & (np.abs(ly) < CALM), t)
    jumps = [i for i in range(1, len(btn)) if (btn[i] & 0x1000) and not (btn[i - 1] & 0x1000)]
    print(f"moves: {len(fwd)} forward, {len(right)} right, {len(jumps)} jump presses (A), {len(turn)} camera turns")

    def disp(arr, s):
        a, b = arr[s[0]], arr[s[1]]
        return (b - a), t[s[1]] - t[s[0]]

    for name, arr in tri.items():
        print(f"  {name}:")
        for label, segs in (("forward", fwd), ("right", right)):
            for s in segs[:2]:
                v, dt = disp(arr, s)
                if np.all(np.isfinite(v)):
                    print(f"    {label:8} {dt:4.1f}s  delta {v.round(3)}  speed {np.linalg.norm(v) / dt:6.2f}/s  dir {(v / (np.linalg.norm(v) + 1e-9)).round(2)}")
        for j in jumps[:2]:
            w = arr[j:min(len(arr), j + int(1.5 * len(t) / (t[-1] - t[0] + 1e-9)))]
            if np.isfinite(w).all() and len(w) > 2:
                rise = w.max(0) - w[0]
                print(f"    jump      rise per axis {rise.round(3)} -> up axis {'xyz'[int(np.argmax(rise))]} ({rise.max():.2f})")
    # handedness: forward x up should point along +right in a right-handed world
    for name, arr in tri.items():
        if not (fwd and right and jumps):
            break
        f, _ = disp(arr, fwd[0])
        r, _ = disp(arr, right[0])
        j = jumps[0]
        w = arr[j:j + 60]
        if not (np.isfinite(f).all() and np.isfinite(r).all() and np.isfinite(w).all()):
            continue
        up = np.zeros(3)
        up[int(np.argmax(w.max(0) - w[0]))] = 1
        f, r = f - up * f.dot(up), r - up * r.dot(up)
        if np.linalg.norm(f) < 0.1 or np.linalg.norm(r) < 0.1:
            continue
        s = np.cross(f / np.linalg.norm(f), up).dot(r / np.linalg.norm(r))
        print(f"  handedness from {name}: (forward x up) . right = {s:+.2f} -> {'right' if s > 0 else 'left'}-handed")
    if "listener" in tri:
        lis = tri["listener"]
        for name in [k for k in tri if k.startswith("cam")]:
            diff = np.nanmax(np.linalg.norm(tri[name] - lis, axis=1))
            print(f"  {name} vs listener: max distance {diff:.3f}")


for p in sys.argv[1:]:
    analyze(p)
