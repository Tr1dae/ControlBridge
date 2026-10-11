"""Offline check: do the exports the probes hook exist in the installed games? (read-only)

usage: python -I tools/check_probe_targets.py <Control folder> <Resonant folder>
"""
import re, sys, pefile

WWISE = ["?SetPosition@SoundEngine@AK@@", "?SetMultiplePositions@SoundEngine@AK@@", "?SetDefaultListeners@SoundEngine@AK@@",
         "?AddDefaultListener@SoundEngine@AK@@", "?RemoveDefaultListener@SoundEngine@AK@@", "?SetListeners@SoundEngine@AK@@",
         "?RenderAudio@SoundEngine@AK@@", "?UnregisterGameObj@SoundEngine@AK@@"]

def exports(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[0])
    return {s.name.decode(): s.address for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}

src = open("src/probe_control/probe_control.cpp", encoding="utf-8").read()
control_names = re.findall(r'Export\((core|input), "([^"]+)"\)', src)
ok = True
for suffix in ("rmdwin10_f", "rmdwin7_f"):
    mods = {"core": exports(f"{sys.argv[1]}/coregame_{suffix}.dll"), "input": exports(f"{sys.argv[1]}/input_{suffix}.dll")}
    for mod, n in control_names:
        found = n in mods[mod]
        ok &= found
        print(f"control {suffix} {mod:5} {'ok     ' if found else 'MISSING'} {n}")
    snd = exports(f"{sys.argv[1]}/snd_{suffix}.dll")
    for p in WWISE:
        hits = sorted({(a, n) for n, a in snd.items() if n.startswith(p)})
        print(f"control {suffix} snd {len(hits)} x {p} {[hex(a) for a, _ in hits]}")
exe = exports(f"{sys.argv[2]}/CONTROLResonant.exe")
for p in WWISE:
    hits = sorted({(a, n) for n, a in exe.items() if n.startswith(p)})
    print(f"resonant exe {len(hits)} x {p} {[hex(a) for a, _ in hits]}")
print("ALL CONTROL EXPORTS FOUND" if ok else "SOME CONTROL EXPORTS MISSING")
