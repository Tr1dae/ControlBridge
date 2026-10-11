"""Maps CONTROL Resonant's ECS systems to code (read-only static analysis).

Every system is registered with its name from `ecs::system::getName<&fn>` (a __FUNCSIG__ string),
followed by creating the system object and storing two code pointers in it:
    lea rax, [rip+run]   ; mov [reg+0x140], rax
    lea rax, [rip+X]     ; mov [reg+0x148], rax
This script finds every getName string, its reference in a registration function, and the next
two such stores, then writes {name: {"ref": rva, "run": rva, "aux": rva}} to JSON.

usage: python -I tools/ecs_systems.py <CONTROLResonant.exe> <out.json> [name-regex-to-print]
"""
import json
import re
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from xref import Image  # noqa: E402

NAME = re.compile(r"getName<(?:void|bool) __cdecl ([A-Za-z0-9_:]+)\(")


def main():
    img = Image(sys.argv[1])
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    cs.detail = True
    found = [(r, s) for r, s in img.strings(r"ecs::system::getName<") if NAME.search(s)]
    refs = {}
    for at, tgt in img.refs([r for r, _ in found]):
        refs.setdefault(tgt, at)
    names = {r for r, _ in found}
    out, missing = {}, 0
    for r, s in found:
        name = NAME.search(s).group(1)
        at = refs.get(r)
        if at is None:
            missing += 1
            continue
        stores, pending = {}, {}
        for ins in cs.disasm(img.pe.get_data(at, 0x300), at):
            rip_lea = (ins.mnemonic == "lea" and len(ins.operands) == 2 and ins.operands[1].type == X86_OP_MEM and
                       ins.operands[1].mem.base == X86_REG_RIP)
            if rip_lea:
                tgt = ins.address + ins.size + ins.operands[1].mem.disp
                if ins.address != at and tgt in names:
                    break  # the next system's registration starts: don't steal its pointers
                pending[ins.operands[0].reg] = tgt
            elif ins.mnemonic == "mov" and len(ins.operands) == 2 and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.disp in (0x140, 0x148):
                if ins.operands[1].reg in pending:
                    stores.setdefault(ins.operands[0].mem.disp, pending[ins.operands[1].reg])
            if len(stores) == 2:
                break
        entry = {"ref": at, "run": stores.get(0x140), "aux": stores.get(0x148)}
        out.setdefault(name, entry)
    json.dump({k: {kk: (hex(vv) if vv else None) for kk, vv in v.items()} for k, v in sorted(out.items())}, open(sys.argv[2], "w"), indent=1)
    resolved = sum(1 for v in out.values() if v["run"])
    print(f"{len(found)} getName strings, {len(out)} systems, {resolved} with a run thunk, {missing} unreferenced")
    if len(sys.argv) > 3:
        rx = re.compile(sys.argv[3])
        for k, v in sorted(out.items()):
            if rx.search(k):
                print(f"  {k:70} run {v['run'] and hex(v['run'])}  aux {v['aux'] and hex(v['aux'])}  ref {hex(v['ref'])}")


main()
