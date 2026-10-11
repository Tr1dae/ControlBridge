"""Fast RIP-relative cross references for big x64 executables (numpy, no full disassembly).

  python -I tools/xref.py <exe> str <regex> [max]   strings matching regex -> code that references them
  python -I tools/xref.py <exe> rva <hex>...         code that references these RVAs (lea/mov/call/jmp)

Each hit shows the containing function (from .pdata), so the next step is
`python -I tools/disexp.py`-style disassembly at that function. Read-only.
"""
import re
import sys

import numpy as np
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.raw = open(path, "rb").read()
        self.text = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.tva = self.text.VirtualAddress
        self.tbytes = np.frombuffer(self.text.get_data(), dtype=np.uint8)
        self._pdata = None

    def rva_to_off(self, rva):
        return self.pe.get_offset_from_rva(rva)

    def strings(self, pattern):
        """(rva, text) of ASCII strings (>= 4 chars) in data sections matching pattern."""
        rx = re.compile(pattern.encode())
        out = []
        for s in self.pe.sections:
            if s.Characteristics & 0x20000000:
                continue
            data = s.get_data()
            for m in re.finditer(rb"[\x20-\x7e]{4,}", data):
                if rx.search(m.group()):
                    out.append((s.VirtualAddress + m.start(), m.group().decode()))
        return out

    def refs(self, targets):
        """RVAs of instructions whose RIP-relative disp32 lands on any target RVA."""
        b = self.tbytes
        n = len(b) - 8
        targets = np.asarray(sorted(set(targets)), dtype=np.int64)
        hits = []

        def s32(at):  # signed little-endian dword at byte positions `at`
            v = (b[at].astype(np.int64) | (b[at + 1].astype(np.int64) << 8) | (b[at + 2].astype(np.int64) << 16) |
                 (b[at + 3].astype(np.int64) << 24))
            return np.where(v >= 1 << 31, v - (1 << 32), v)

        # REX.W/REX.WR + (lea 8D | mov 8B | mov 89) + modrm mod=00 rm=101 + disp32: 7 bytes
        cand = np.nonzero(((b[:n] & 0xF8) == 0x48) & np.isin(b[1:n + 1], (0x8D, 0x8B, 0x89)) & ((b[2:n + 2] & 0xC7) == 0x05))[0]
        tgt = self.tva + cand + 7 + s32(cand + 3)
        sel = np.isin(tgt, targets)
        hits += [(int(self.tva + i), int(t)) for i, t in zip(cand[sel], tgt[sel])]
        # call/jmp rel32: 5 bytes
        cand = np.nonzero(np.isin(b[:n], (0xE8, 0xE9)))[0]
        tgt = self.tva + cand + 5 + s32(cand + 1)
        sel = np.isin(tgt, targets)
        hits += [(int(self.tva + i), int(t)) for i, t in zip(cand[sel], tgt[sel])]
        return sorted(hits)

    def func_of(self, rva):
        if self._pdata is None:
            d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
            raw = self.pe.get_data(d.VirtualAddress, d.Size)
            arr = np.frombuffer(raw[: len(raw) // 12 * 12], dtype=np.uint32).reshape(-1, 3)
            self._pdata = arr[np.argsort(arr[:, 0])]
        p = self._pdata
        i = np.searchsorted(p[:, 0], rva, side="right") - 1
        if i >= 0 and p[i, 0] <= rva < p[i, 1]:
            return int(p[i, 0]), int(p[i, 1])
        return None


def main():
    img = Image(sys.argv[1])
    mode = sys.argv[2]
    if mode == "str":
        mx = int(sys.argv[4]) if len(sys.argv) > 4 else 20
        found = img.strings(sys.argv[3])
        print(f"{len(found)} strings")
        found = found[:mx]
        refs = img.refs([r for r, _ in found])
        by = {}
        for at, tgt in refs:
            by.setdefault(tgt, []).append(at)
        for r, s in found:
            print(f"{r:#x} {s[:160]}")
            for at in by.get(r, []):
                f = img.func_of(at)
                print(f"    ref {at:#x} in func {f[0]:#x}-{f[1]:#x}" if f else f"    ref {at:#x}")
    else:
        targets = [int(x, 16) for x in sys.argv[3:]]
        for at, tgt in img.refs(targets):
            f = img.func_of(at)
            print(f"{tgt:#x} <- {at:#x}" + (f" in func {f[0]:#x}" if f else ""))


if __name__ == "__main__":
    main()
