"""Dump undecorated exports (name, rva) of DLLs into one tab-separated file."""
import sys, ctypes, pefile
und = ctypes.windll.dbghelp.UnDecorateSymbolName
buf = ctypes.create_string_buffer(4096)
out = open(sys.argv[1], "w", encoding="utf-8")
for path in sys.argv[2:]:
    pe = pefile.PE(path, fast_load=True); pe.parse_data_directories(directories=[0])
    mod = path.replace("\\", "/").rsplit("/", 1)[-1]
    for s in (pe.DIRECTORY_ENTRY_EXPORT.symbols if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") else []):
        if not s.name: continue
        n = s.name.decode()
        d = buf.value.decode(errors="replace") if und(n.encode(), buf, 4096, 0x1000 | 0x2 | 0x8) else n  # name-only-ish
        out.write(f"{mod}\t{s.address:#x}\t{d}\t{n}\n")
