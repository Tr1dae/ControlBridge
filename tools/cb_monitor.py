"""Live view of the Control Bridge link (read-only).

  python -I tools/cb_monitor.py [--name Local\\ControlBridge_v1] [--hz 2] [--once] [--seconds N]

Opens the existing mapping read-only (never creates it: a mapping made here first would be refused
by the games) and prints both sides: heartbeats, Dylan / Jesse, both cameras, the pad. The field
layout comes from `bin/link_test.exe --layout`, so this never drifts from the C++ header.
"""
import argparse
import ctypes
import json
import math
import os
import struct
import subprocess
import sys
import time
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = wintypes.HANDLE
k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
k32.GetTickCount64.restype = ctypes.c_uint64
FILE_MAP_READ = 4


def layout():
    out = subprocess.run([os.path.join(ROOT, "bin", "link_test.exe"), "--layout"], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


class Link:
    def __init__(self, name, lay, wait=False):
        self.L = lay
        h = k32.OpenFileMappingW(FILE_MAP_READ, False, name)
        while not h and wait:
            time.sleep(1)
            h = k32.OpenFileMappingW(FILE_MAP_READ, False, name)
        if not h:
            raise SystemExit(f"{name}: not open (start a game with the probe first)")
        self.base = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0x1000)
        if not self.base:
            raise SystemExit("MapViewOfFile failed")

    def raw(self, off, n):
        return ctypes.string_at(self.base + off, n)

    def u32(self, off):
        return struct.unpack("<I", self.raw(off, 4))[0]

    def u64(self, off):
        return struct.unpack("<Q", self.raw(off, 8))[0]

    def slot(self, name, size=240):
        """Seqlock read: bytes of the slot's data, or None if no consistent copy."""
        off = self.L["slots"][name]
        for _ in range(100):
            s1 = self.u32(off)
            if s1 & 1:
                continue
            data = self.raw(off + self.L["slots"]["data"], size)
            if self.u32(off) == s1:
                return data if s1 else None
        return None

    def xform(self, b, off):
        pos = struct.unpack_from("<3d", b, off + self.L["Xform"]["pos"])
        rot = struct.unpack_from("<9f", b, off + self.L["Xform"]["rot"])
        return pos, rot

    def view(self, b, off):
        pos, rot = self.xform(b, off + self.L["View"]["xf"])
        fov = struct.unpack_from("<f", b, off + self.L["View"]["fovY"])[0]
        return pos, rot, fov


def fmt_v(v):
    return "(" + ", ".join(f"{x:9.3f}" for x in v) + ")"


def snapshot(link):
    L, H = link.L, link.L["header"]
    now = k32.GetTickCount64()
    out = {}
    magic, version = link.u32(H["magic"]), link.u32(H["version"])
    out["header"] = dict(magic=hex(magic), version=version, hostPid=link.u32(H["hostPid"]), guestPid=link.u32(H["guestPid"]),
                         hostAge=now - link.u64(H["hostHeartbeatMs"]) if link.u64(H["hostHeartbeatMs"]) else None,
                         guestAge=now - link.u64(H["guestHeartbeatMs"]) if link.u64(H["guestHeartbeatMs"]) else None)
    h = link.slot("host")
    if h:
        HS = L["HostState"]
        flags, = struct.unpack_from("<I", h, HS["flags"])
        frame, = struct.unpack_from("<Q", h, HS["frame"])
        ppos, _ = link.xform(h, HS["player"])
        cpos, crot, cfov = link.view(h, HS["camera"])
        out["host"] = dict(flags=flags, frame=frame, player=ppos, cam=cpos, camFwd=crot[6:9], fovY=math.degrees(cfov))
    p = link.slot("pad", 32)
    if p:
        P = L["PadState"]
        flags, buttons = struct.unpack_from("<I", p, P["flags"])[0], struct.unpack_from("<H", p, P["buttons"])[0]
        lx, ly, rx, ry = struct.unpack_from("<4h", p, P["lx"])
        lt, rt = struct.unpack_from("<2B", p, P["lt"])
        out["pad"] = dict(active=bool(flags & 1), buttons=buttons, lt=lt, rt=rt, lx=lx, ly=ly, rx=rx, ry=ry)
    g = link.slot("guest")
    if g:
        GS = L["GuestState"]
        flags, = struct.unpack_from("<I", g, GS["flags"])
        frame, = struct.unpack_from("<Q", g, GS["frame"])
        jpos, jrot = link.xform(g, GS["jesse"])
        cpos, crot, cfov = link.view(g, GS["camera"])
        out["guest"] = dict(flags=flags, frame=frame, jesse=jpos, jesseFwd=jrot[6:9], cam=cpos, camFwd=crot[6:9], fovY=math.degrees(cfov))
    return out


def show(s, prev, dt):
    hd = s["header"]
    age = lambda a: "never" if a is None else ("alive" if a < 1500 else f"gone {a / 1000:.0f}s")
    lines = [f"link {hd['magic']} v{hd['version']}  host pid {hd['hostPid']} {age(hd['hostAge'])}  guest pid {hd['guestPid']} {age(hd['guestAge'])}"]
    if "host" in s:
        h = s["host"]
        fps = (h["frame"] - prev["host"]["frame"]) / dt if prev and "host" in prev and dt else 0
        lines.append(f"  Resonant  frame {h['frame']:8d} ({fps:5.0f}/s)  Dylan {fmt_v(h['player'])}  cam {fmt_v(h['cam'])} fwd {fmt_v(h['camFwd'])} fovY {h['fovY']:.1f}")
    if "pad" in s:
        p = s["pad"]
        lines.append(f"  pad       {'active' if p['active'] else 'idle  '} buttons {p['buttons']:04x} LT {p['lt']:3d} RT {p['rt']:3d} L({p['lx']:6d},{p['ly']:6d}) R({p['rx']:6d},{p['ry']:6d})")
    if "guest" in s:
        g = s["guest"]
        fps = (g["frame"] - prev["guest"]["frame"]) / dt if prev and "guest" in prev and dt else 0
        lines.append(f"  Control   frame {g['frame']:8d} ({fps:5.0f}/s)  Jesse {fmt_v(g['jesse'])}  cam {fmt_v(g['cam'])} fwd {fmt_v(g['camFwd'])} fovY {g['fovY']:.1f}")
    if "host" in s and "guest" in s:
        off = [a - b for a, b in zip(s["host"]["player"], s["guest"]["jesse"])]
        lines.append(f"  anchor    Dylan - Jesse = {fmt_v(off)}")
    print("\n".join(lines), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="Local\\ControlBridge_v1")
    ap.add_argument("--hz", type=float, default=2.0)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--wait", action="store_true", help="wait for a game to create the mapping")
    a = ap.parse_args()
    link = Link(a.name, layout(), a.wait)
    print(time.strftime("%H:%M:%S"), "link open", flush=True)
    prev, t_prev, t0 = None, time.time(), time.time()
    while True:
        s = snapshot(link)
        t = time.time()
        show(s, prev, t - t_prev)
        prev, t_prev = s, t
        if a.once or (a.seconds and t - t0 >= a.seconds):
            break
        time.sleep(1 / a.hz)


if __name__ == "__main__":
    sys.exit(main())
