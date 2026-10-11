"""Survey PE files: version, timestamp, sections, interesting imports, export count, RTTI hint."""
import sys, re, datetime, pefile
INTEREST = re.compile(r"d3d1[12]|dxgi|xinput|winmm|version|dinput|dwmapi|steam|physx|ws2_|wininet|bcrypt|crypt|ntdll|amd_ags|nvapi|sl\.interposer|gameinput|hid", re.I)
for path in sys.argv[1:]:
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY[k] for k in ("IMAGE_DIRECTORY_ENTRY_IMPORT","IMAGE_DIRECTORY_ENTRY_EXPORT","IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT","IMAGE_DIRECTORY_ENTRY_RESOURCE","IMAGE_DIRECTORY_ENTRY_TLS","IMAGE_DIRECTORY_ENTRY_SECURITY")])
    ver = "?"
    for fi in getattr(pe, "FileInfo", []) or []:
        for e in fi:
            for st in getattr(e, "StringTable", []):
                d = {k.decode(): v.decode(errors="replace") for k, v in st.entries.items()}
                ver = f"{d.get('FileVersion','?')} / {d.get('ProductName','?')}"
    ts = datetime.datetime.utcfromtimestamp(pe.FILE_HEADER.TimeDateStamp)
    print(f"== {path}\n  version {ver}  linked {ts:%Y-%m-%d}  size {pe.OPTIONAL_HEADER.SizeOfImage:#x}  imagebase {pe.OPTIONAL_HEADER.ImageBase:#x}")
    print("  sections:", " ".join(f"{s.Name.rstrip(b'\0').decode(errors='replace')}({s.Misc_VirtualSize>>10}K,{'X' if s.Characteristics & 0x20000000 else ''}{'W' if s.Characteristics & 0x80000000 else ''})" for s in pe.sections))
    sec = pe.OPTIONAL_HEADER.DATA_DIRECTORY[4]
    print(f"  authenticode: {'yes' if sec.Size else 'no'}  TLS callbacks dir: {'yes' if hasattr(pe,'DIRECTORY_ENTRY_TLS') else 'no'}")
    for attr, label in (("DIRECTORY_ENTRY_IMPORT", "import"), ("DIRECTORY_ENTRY_DELAY_IMPORT", "delay")):
        for imp in getattr(pe, attr, []):
            dll = imp.dll.decode()
            if INTEREST.search(dll):
                names = [i.name.decode() if i.name else f"#{i.ordinal}" for i in imp.imports]
                print(f"  {label} {dll}: {len(names)} {', '.join(names[:12])}{' ...' if len(names) > 12 else ''}")
    alldlls = sorted({i.dll.decode().lower() for i in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])})
    print("  all imports:", ", ".join(alldlls))
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        syms = pe.DIRECTORY_ENTRY_EXPORT.symbols
        print(f"  exports: {len(syms)}  e.g. {[s.name.decode() for s in syms[:3] if s.name]}")
    raw = open(path, "rb").read()
    print(f"  RTTI type descriptors (.?AV/.?AU): {len(re.findall(rb'\.\?A[VU][A-Za-z_]', raw))}")
