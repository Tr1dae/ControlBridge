"""Disassemble functions by export name substring (first N instructions). usage: disexp.py <pe> <n> <substr>..."""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True); pe.parse_data_directories(directories=[0])
n = int(sys.argv[2]); cs = Cs(CS_ARCH_X86, CS_MODE_64)
import ctypes
und = ctypes.windll.dbghelp.UnDecorateSymbolName; buf = ctypes.create_string_buffer(4096)
for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if not s.name: continue
    nm = s.name.decode(); und(nm.encode(), buf, 4096, 0x1000|0x2|0x8); d = buf.value.decode(errors="replace")
    if not any(x == d or (x.endswith("*") and d.startswith(x[:-1])) for x in sys.argv[3:]): continue
    code = pe.get_data(s.address, 160)
    print(f"--- {d}  @{s.address:#x}  [{nm[:90]}]")
    for i, ins in enumerate(cs.disasm(code, s.address)):
        if i >= n: break
        print(f"   {ins.address:#x}: {ins.mnemonic} {ins.op_str}")
        if ins.mnemonic in ("ret", "jmp") and i > 0 and n > 8: break
