// Control Bridge Phase 0 probe core, shared by both games' probe DLLs.
//
// Passive by design: hooks only observe (they call the original and record what they saw), every
// memory read is guarded, and nothing in either game is written. Pieces:
//   - log file <tag>_probe.log and a live command file <tag>_cmd.txt (polled every 250 ms)
//   - MinHook wrapper, export walking, runtime MSVC RTTI lookup (class name -> vtables -> instances)
//   - XInput + keyboard sampler on its own thread (calls the system xinput1_4.dll directly, so
//     other mods' XInput hooks don't matter)
//   - per-frame CSV <tag>_frames_<time>.csv, one row per game frame (Tick() from a frame hook)
//
// Header-only; each probe DLL includes it once.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <share.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "../../third_party/minhook/include/MinHook.h"

namespace cb
{
	// ---- process-wide state ---------------------------------------------------------------------
	inline HMODULE       g_self = nullptr;
	inline uintptr_t     g_exe = 0;
	inline std::wstring  g_dir;  // logs, dumps and the command file
	inline std::wstring  g_tag;  // "control" / "resonant": file name prefix
	inline FILE*         g_log = nullptr;
	inline SRWLOCK       g_logLock = SRWLOCK_INIT;
	inline LARGE_INTEGER g_qpf{}, g_qp0{};

	inline double Now()
	{
		LARGE_INTEGER q;
		QueryPerformanceCounter(&q);
		return double(q.QuadPart - g_qp0.QuadPart) / double(g_qpf.QuadPart);
	}

	inline std::wstring ModuleDir(HMODULE a_mod)
	{
		wchar_t p[MAX_PATH];
		GetModuleFileNameW(a_mod, p, MAX_PATH);
		if (wchar_t* s = wcsrchr(p, L'\\')) *s = 0;
		return p;
	}

	// controlbridge.ini next to the DLL, section [ControlBridge].
	inline std::wstring IniPath() { return ModuleDir(g_self) + L"\\controlbridge.ini"; }

	inline std::wstring IniString(const wchar_t* a_key, const std::wstring& a_default)
	{
		wchar_t b[1024];
		GetPrivateProfileStringW(L"ControlBridge", a_key, a_default.c_str(), b, 1024, IniPath().c_str());
		return b;
	}

	inline int IniInt(const wchar_t* a_key, int a_default)
	{
		return static_cast<int>(GetPrivateProfileIntW(L"ControlBridge", a_key, a_default, IniPath().c_str()));
	}

	inline std::wstring Path(const std::wstring& a_name) { return g_dir + L"\\" + a_name; }

	inline void Log(const char* a_fmt, ...)
	{
		char    buf[4096];
		va_list va;
		va_start(va, a_fmt);
		vsnprintf(buf, sizeof(buf), a_fmt, va);
		va_end(va);
		SYSTEMTIME t;
		GetLocalTime(&t);
		AcquireSRWLockExclusive(&g_logLock);
		if (g_log) {
			fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
			fflush(g_log);
		}
		ReleaseSRWLockExclusive(&g_logLock);
	}

	inline void Init(HMODULE a_self, const wchar_t* a_tag)
	{
		g_self = a_self;
		g_tag = a_tag;
		g_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		QueryPerformanceFrequency(&g_qpf);
		QueryPerformanceCounter(&g_qp0);
		g_dir = IniString(L"LogDir", ModuleDir(a_self) + L"\\ControlBridge-logs");
		CreateDirectoryW(g_dir.c_str(), nullptr);
		g_log = _wfsopen(Path(g_tag + L"_probe.log").c_str(), L"w", _SH_DENYWR);
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe)->e_lfanew);
		Log("Control Bridge probe '%ls' (built %s %s), pid %lu", a_tag, __DATE__, __TIME__, GetCurrentProcessId());
		Log("exe %ls base %p, PE timestamp %08lx, image size %lx", exe, reinterpret_cast<void*>(g_exe),
			nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
	}

	// ---- guarded memory access --------------------------------------------------------------------
	inline bool Readable(uintptr_t a_addr, size_t a_n)
	{
		if (a_addr < 0x10000 || a_addr + a_n < a_addr) return false;
		uintptr_t end = a_addr + a_n;
		while (a_addr < end) {
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(reinterpret_cast<void*>(a_addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
			DWORD p = mbi.Protect & 0xFF;
			if ((mbi.Protect & PAGE_GUARD) || p == PAGE_NOACCESS || p == PAGE_EXECUTE) return false;
			a_addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}
		return true;
	}

	// memcpy under SEH. Only for pointers the game just handed us; use SafeRead for anything else.
	inline bool SafeCopy(void* a_dst, const void* a_src, size_t a_n)
	{
		__try {
			memcpy(a_dst, a_src, a_n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Checks the pages first, so a stale pointer doesn't even raise a first-chance exception.
	inline bool SafeRead(void* a_dst, uintptr_t a_src, size_t a_n)
	{
		return Readable(a_src, a_n) && SafeCopy(a_dst, reinterpret_cast<const void*>(a_src), a_n);
	}

	template <class T>
	inline bool Read(uintptr_t a_addr, T& a_out)
	{
		return SafeRead(&a_out, a_addr, sizeof(T));
	}

	// ---- modules ------------------------------------------------------------------------------------
	inline IMAGE_NT_HEADERS64* Nt(HMODULE a_mod)
	{
		auto b = reinterpret_cast<uintptr_t>(a_mod);
		return reinterpret_cast<IMAGE_NT_HEADERS64*>(b + reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_lfanew);
	}

	inline bool InModule(uintptr_t a_addr, HMODULE a_mod)
	{
		auto b = reinterpret_cast<uintptr_t>(a_mod);
		return a_addr >= b && a_addr < b + Nt(a_mod)->OptionalHeader.SizeOfImage;
	}

	// "module+rva" for code and data addresses, plain hex otherwise.
	inline std::string Rva(uintptr_t a_addr)
	{
		char    buf[MAX_PATH + 32];
		HMODULE m = nullptr;
		if (a_addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						  reinterpret_cast<LPCSTR>(a_addr), &m)) {
			char name[MAX_PATH];
			GetModuleFileNameA(m, name, MAX_PATH);
			const char* b = strrchr(name, '\\');
			snprintf(buf, sizeof(buf), "%s+%llx", reinterpret_cast<uintptr_t>(m) == g_exe ? "exe" : (b ? b + 1 : name),
				static_cast<unsigned long long>(a_addr - reinterpret_cast<uintptr_t>(m)));
		} else {
			snprintf(buf, sizeof(buf), "%p", reinterpret_cast<void*>(a_addr));
		}
		return buf;
	}

	inline HMODULE FirstModule(std::initializer_list<const wchar_t*> a_names)
	{
		for (auto* n : a_names) {
			if (HMODULE m = GetModuleHandleW(n)) return m;
		}
		return nullptr;
	}

	inline bool Section(HMODULE a_mod, const char* a_name, uintptr_t& a_begin, size_t& a_size)
	{
		auto* nt = Nt(a_mod);
		auto* s = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
			char n[9] = {};
			memcpy(n, s->Name, 8);
			if (!strcmp(n, a_name)) {
				a_begin = reinterpret_cast<uintptr_t>(a_mod) + s->VirtualAddress;
				a_size = s->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	// Calls a_fn(decoratedName, address) for every named, non-forwarded export.
	template <class F>
	inline void ForEachExport(HMODULE a_mod, F&& a_fn)
	{
		auto  b = reinterpret_cast<uintptr_t>(a_mod);
		auto& dd = Nt(a_mod)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (!dd.VirtualAddress) return;
		auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(b + dd.VirtualAddress);
		auto* names = reinterpret_cast<DWORD*>(b + ed->AddressOfNames);
		auto* ords = reinterpret_cast<WORD*>(b + ed->AddressOfNameOrdinals);
		auto* funcs = reinterpret_cast<DWORD*>(b + ed->AddressOfFunctions);
		for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
			DWORD rva = funcs[ords[i]];
			if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) continue;  // forwarder
			a_fn(reinterpret_cast<const char*>(b + names[i]), reinterpret_cast<void*>(b + rva));
		}
	}

	// ---- hooks (MinHook) -----------------------------------------------------------------------------
	inline std::vector<void*> g_hooked;

	// Hooks a_target once (aliased exports share an address; the second request is skipped).
	inline bool Hook(void* a_target, void* a_detour, void** a_orig, const char* a_name)
	{
		static bool s_init = false;
		if (!a_target) {
			Log("hook %s: target not found", a_name);
			return false;
		}
		for (void* t : g_hooked) {
			if (t == a_target) {
				Log("hook %s @ %s: same address as an earlier hook, skipped", a_name, Rva(reinterpret_cast<uintptr_t>(a_target)).c_str());
				return false;
			}
		}
		if (!s_init) {
			MH_STATUS s = MH_Initialize();
			if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
				Log("MH_Initialize failed: %s", MH_StatusToString(s));
				return false;
			}
			s_init = true;
		}
		MH_STATUS s = MH_CreateHook(a_target, a_detour, a_orig);
		if (s == MH_OK) s = MH_EnableHook(a_target);
		Log("hook %-40s @ %s: %s", a_name, Rva(reinterpret_cast<uintptr_t>(a_target)).c_str(), MH_StatusToString(s));
		if (s != MH_OK) return false;
		g_hooked.push_back(a_target);
		return true;
	}

	// ---- RTTI ------------------------------------------------------------------------------------------
	// MSVC x64: vtable[-1] -> CompleteObjectLocator {u32 sig=1, u32 offset, u32 cdOffset,
	// i32 typeDescRva, i32 hierarchyRva, i32 selfRva}; TypeDescriptor {vtable*, spare, char name[]}.

	// All vtables (one per base offset) of class a_cls in a_mod. a_cls is the mangled tail without
	// prefix and "@@": "PerspectiveView@m" for m::PerspectiveView.
	inline std::vector<uintptr_t> FindVtables(HMODULE a_mod, const char* a_cls)
	{
		std::vector<uintptr_t> out;
		auto      base = reinterpret_cast<uintptr_t>(a_mod);
		uintptr_t db, rb;
		size_t    dn, rn;
		if (!Section(a_mod, ".data", db, dn) || !Section(a_mod, ".rdata", rb, rn)) return out;
		std::string_view data(reinterpret_cast<const char*>(db), dn);
		for (const char* pre : { ".?AV", ".?AU" }) {
			std::string needle = std::string(pre) + a_cls + "@@";
			for (size_t pos = data.find(needle); pos != std::string_view::npos; pos = data.find(needle, pos + 1)) {
				if (pos < 0x10 || pos + needle.size() >= dn || data[pos + needle.size()] != 0) continue;
				auto tdRva = static_cast<uint32_t>(db + pos - 0x10 - base);
				for (uintptr_t c = rb; c + 24 <= rb + rn; c += 4) {
					auto* col = reinterpret_cast<const uint32_t*>(c);
					if (col[0] != 1 || col[3] != tdRva || col[5] != static_cast<uint32_t>(c - base)) continue;
					for (uintptr_t v = rb; v + 8 <= rb + rn; v += 8) {
						if (*reinterpret_cast<const uintptr_t*>(v) == c) out.push_back(v + 8);
					}
				}
			}
		}
		return out;
	}

	// Mangled class tail of the object at a_obj ("PerspectiveView@m"), or "" if it has no RTTI.
	inline std::string ClassOf(uintptr_t a_obj)
	{
		uintptr_t vt = 0, colp = 0;
		uint32_t  col[6];
		if (!Read(a_obj, vt) || !Read(vt - 8, colp) || !SafeRead(col, colp, sizeof(col)) || col[0] != 1) return {};
		uintptr_t base = colp - col[5];
		char      name[256] = {};
		if (!SafeRead(name, base + col[3] + 0x10, sizeof(name) - 1)) return {};
		std::string s = name;
		if (s.size() > 4 && (s.compare(0, 4, ".?AV") == 0 || s.compare(0, 4, ".?AU") == 0)) s = s.substr(4);
		if (s.size() > 2 && s.compare(s.size() - 2, 2, "@@") == 0) s.resize(s.size() - 2);
		return s;
	}

	// ---- heap scans --------------------------------------------------------------------------------------
	template <class F>
	inline void ForEachHeapRegion(F&& a_fn)
	{
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t a = 0x10000;
		while (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
			uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			DWORD     p = mbi.Protect & 0xFF;
			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && !(mbi.Protect & PAGE_GUARD) &&
				(p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE)) {
				a_fn(b, mbi.RegionSize);
			}
			a = b + mbi.RegionSize;
			if (a < b) break;
		}
	}

	// 8-aligned heap addresses holding the qword a_value (a vtable address finds live objects).
	// Copies 1 MB chunks under SEH, so memory freed mid-scan is skipped, not crashed on.
	inline std::vector<uintptr_t> ScanHeap(uint64_t a_value, size_t a_max)
	{
		std::vector<uintptr_t> out;
		std::vector<uint64_t>  chunk(1 << 17);
		ULONG_PTR              stackLo = 0, stackHi = 0;  // our own stack holds copies of a_value
		GetCurrentThreadStackLimits(&stackLo, &stackHi);
		ForEachHeapRegion([&](uintptr_t a_base, size_t a_size) {
			if (a_base >= stackLo && a_base < stackHi) return;
			for (size_t off = 0; off < a_size && out.size() < a_max; off += chunk.size() * 8) {
				size_t n = std::min(chunk.size() * 8, a_size - off);
				if (!SafeCopy(chunk.data(), reinterpret_cast<const void*>(a_base + off), n)) continue;
				for (size_t i = 0; i < n / 8 && out.size() < a_max; ++i) {
					if (chunk[i] == a_value && a_base + off + i * 8 != reinterpret_cast<uintptr_t>(&chunk[i])) out.push_back(a_base + off + i * 8);
				}
			}
			Sleep(0);
		});
		return out;
	}

	inline void HexDump(uintptr_t a_addr, size_t a_n)
	{
		std::vector<uint8_t> b(a_n);
		if (!SafeRead(b.data(), a_addr, a_n)) {
			Log("  %p: unreadable", reinterpret_cast<void*>(a_addr));
			return;
		}
		for (size_t o = 0; o < a_n; o += 16) {
			char line[256];
			int  p = snprintf(line, sizeof(line), "  +%04zx:", o);
			for (size_t i = 0; i < 16 && o + i < a_n; ++i) p += snprintf(line + p, sizeof(line) - p, " %02x", b[o + i]);
			p += snprintf(line + p, sizeof(line) - p, "  |f");
			for (size_t i = 0; i + 4 <= 16 && o + i + 4 <= a_n; i += 4) {
				float f;
				memcpy(&f, &b[o + i], 4);
				p += snprintf(line + p, sizeof(line) - p, " %.5g", f);
			}
			p += snprintf(line + p, sizeof(line) - p, " |d");
			for (size_t i = 0; i + 8 <= 16 && o + i + 8 <= a_n; i += 8) {
				double d;
				memcpy(&d, &b[o + i], 8);
				p += snprintf(line + p, sizeof(line) - p, " %.6g", d);
			}
			Log("%s", line);
		}
	}

	// ---- value search + watches ---------------------------------------------------------------------------
	// Finds where a known position lives in memory: heap scan for three consecutive doubles (or
	// floats) within a_tol of a_xyz. Re-reading the candidates after the player has moved ('cands',
	// 'keep moved') separates the live transform from stale copies; 'watch' then streams an address
	// into the CSV (w0..w11).
	struct Cand
	{
		uintptr_t addr;
		bool      dbl;
		double    first[3];
	};
	inline std::vector<Cand> g_cands;

	inline bool ReadTriple(uintptr_t a_addr, bool a_dbl, double* a_out)
	{
		if (a_dbl) return SafeRead(a_out, a_addr, 24);
		float f[3];
		if (!SafeRead(f, a_addr, 12)) return false;
		for (int i = 0; i < 3; ++i) a_out[i] = f[i];
		return true;
	}

	inline std::vector<Cand> FindTriple(const double* a_xyz, double a_tol, bool a_dbl, size_t a_max)
	{
		std::vector<Cand>    out;
		std::vector<uint8_t> chunk(1 << 20);
		const size_t         stride = a_dbl ? 8 : 4, span = 3 * stride;
		ULONG_PTR            stackLo = 0, stackHi = 0;
		GetCurrentThreadStackLimits(&stackLo, &stackHi);
		ForEachHeapRegion([&](uintptr_t a_base, size_t a_size) {
			if (a_base >= stackLo && a_base < stackHi) return;
			for (size_t off = 0; off < a_size && out.size() < a_max; off += chunk.size() - span) {
				size_t n = std::min(chunk.size(), a_size - off);
				if (n < span || !SafeCopy(chunk.data(), reinterpret_cast<const void*>(a_base + off), n)) continue;
				const uint8_t* b = chunk.data();
				for (size_t i = 0; i + span <= n && out.size() < a_max; i += stride) {
					double v[3];
					if (a_dbl) {
						memcpy(v, b + i, 24);
					} else {
						float f[3];
						memcpy(f, b + i, 12);
						v[0] = f[0], v[1] = f[1], v[2] = f[2];
					}
					if (std::fabs(v[0] - a_xyz[0]) > a_tol || std::fabs(v[1] - a_xyz[1]) > a_tol || std::fabs(v[2] - a_xyz[2]) > a_tol) continue;
					uintptr_t at = a_base + off + i;
					if (at >= reinterpret_cast<uintptr_t>(chunk.data()) && at < reinterpret_cast<uintptr_t>(chunk.data()) + chunk.size()) continue;
					out.push_back({ at, a_dbl, { v[0], v[1], v[2] } });
				}
				if (n < chunk.size()) break;
			}
			Sleep(0);
		});
		return out;
	}

	inline void FindAndLog(const double* a_xyz, double a_tol, size_t a_max)
	{
		double t0 = Now();
		g_cands = FindTriple(a_xyz, a_tol, true, a_max);
		size_t nd = g_cands.size();
		auto   f = FindTriple(a_xyz, a_tol, false, a_max);
		g_cands.insert(g_cands.end(), f.begin(), f.end());
		Log("find %.3f %.3f %.3f tol %g: %zu as doubles, %zu as floats (%.1f s)", a_xyz[0], a_xyz[1], a_xyz[2], a_tol, nd, f.size(), Now() - t0);
		for (size_t i = 0; i < g_cands.size() && i < 64; ++i) {
			auto& c = g_cands[i];
			Log("  [%zu] %p %s %.4f %.4f %.4f", i, reinterpret_cast<void*>(c.addr), c.dbl ? "d" : "f", c.first[0], c.first[1], c.first[2]);
		}
	}

	struct Watch
	{
		uintptr_t addr;
		bool      dbl;
	};
	inline Watch   g_watch[4] = {};
	inline SRWLOCK g_watchLock = SRWLOCK_INIT;

	// ---- pad + keyboard sampler -------------------------------------------------------------------------
	struct PadSnap
	{
		uint32_t packet = 0;
		uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
		uint8_t  lt = 0, rt = 0;
		int16_t  lx = 0, ly = 0, rx = 0, ry = 0;
		int8_t   index = -1;  // XInput user index, -1 = no pad
		uint8_t  fg = 0;      // this game's window is in front
		uint32_t keys = 0;    // kKeys bits, only while fg
	};

	// W A S D Space LShift LCtrl E Q F R Tab LMB RMB MMB C V G
	inline constexpr int kKeys[] = { 'W', 'A', 'S', 'D', VK_SPACE, VK_LSHIFT, VK_LCONTROL, 'E', 'Q', 'F', 'R', VK_TAB,
		VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, 'C', 'V', 'G' };

	inline SRWLOCK g_padLock = SRWLOCK_INIT;
	inline PadSnap g_pad;

	inline PadSnap Pad()
	{
		AcquireSRWLockShared(&g_padLock);
		PadSnap p = g_pad;
		ReleaseSRWLockShared(&g_padLock);
		return p;
	}

	struct XState
	{
		DWORD    packet;
		uint16_t buttons;
		uint8_t  lt, rt;
		int16_t  lx, ly, rx, ry;
	};

	inline DWORD WINAPI PadThread(void*)
	{
		wchar_t sys[MAX_PATH];
		GetSystemDirectoryW(sys, MAX_PATH);
		HMODULE x = LoadLibraryW((std::wstring(sys) + L"\\xinput1_4.dll").c_str());
		auto    get = x ? reinterpret_cast<DWORD(WINAPI*)(DWORD, XState*)>(GetProcAddress(x, "XInputGetState")) : nullptr;
		Log("pad sampler: system xinput1_4 %s", get ? "ready" : "MISSING (keyboard only)");
		int       index = -1;
		ULONGLONG nextProbe = 0;
		for (;;) {
			Sleep(4);
			PadSnap s;
			if (get) {
				XState st{};
				if (index >= 0 && get(index, &st) != ERROR_SUCCESS) index = -1;
				if (index < 0 && GetTickCount64() >= nextProbe) {  // polling absent pads is slow: once a second
					nextProbe = GetTickCount64() + 1000;
					for (int i = 0; i < 4 && index < 0; ++i) {
						if (get(i, &st) == ERROR_SUCCESS) index = i;
					}
				}
				if (index >= 0) {
					s = { st.packet, st.buttons, st.lt, st.rt, st.lx, st.ly, st.rx, st.ry, static_cast<int8_t>(index) };
				}
			}
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			s.fg = pid == GetCurrentProcessId();
			if (s.fg) {
				for (size_t i = 0; i < std::size(kKeys); ++i) {
					if (GetAsyncKeyState(kKeys[i]) & 0x8000) s.keys |= 1u << i;
				}
			}
			AcquireSRWLockExclusive(&g_padLock);
			g_pad = s;
			ReleaseSRWLockExclusive(&g_padLock);
		}
	}

	// ---- per-frame rows ------------------------------------------------------------------------------------
	enum RowFlags : uint32_t
	{
		kRowPlayer = 1u << 0,    // player[] holds the game's player transform (raw, see the game's probe)
		kRowCam = 1u << 1,       // cam[] / fov hold a camera view-to-world (raw doubles) and FOV
		kRowListener = 1u << 2,  // Wwise default listener transform (decoded)
		kRowEmitter = 1u << 3,   // tracked Wwise emitter position
		kRowNoTick = 1u << 4,    // written by the fallback sampler: no frame hook fired for a second
	};

	struct Row
	{
		double   t;
		uint64_t frame;
		uint32_t tid;
		uint32_t flags;
		float    player[16];
		double   cam[12];
		float    fov;
		double   lisPos[3];
		float    lisFront[3], lisTop[3];
		uint64_t emitterId;
		double   emitterPos[3];
		double   watch[12];  // 4 watched triples ('watch' command), zero when unset/unreadable
		PadSnap  pad;
	};

	struct GameHooks
	{
		void (*fill)(Row&) = nullptr;                // add game-specific columns; reads cached snapshots only
		bool (*command)(int, char**) = nullptr;      // game-specific commands; false = unknown
		void (*summary)() = nullptr;                 // every 5 s
		void (*fillCommon)(Row&) = nullptr;          // Wwise listener/emitter (set by wwise.h)
		void (*summaryCommon)() = nullptr;
		bool (*commandCommon)(int, char**) = nullptr;
	};
	inline GameHooks g_game;

	inline std::atomic<uint64_t> g_frame{ 0 };
	inline std::atomic<double>   g_lastTick{ -1.0 };
	inline SRWLOCK               g_rowLock = SRWLOCK_INIT;
	inline std::vector<Row>      g_rows;
	inline std::atomic<bool>     g_csvOn{ true };
	inline int                   g_csvEvery = 1;

	inline void FillRow(Row& a_r, uint32_t a_extraFlags)
	{
		a_r.t = Now();
		a_r.frame = g_frame.load();
		a_r.tid = GetCurrentThreadId();
		a_r.flags = a_extraFlags;
		a_r.pad = Pad();
		Watch w[4];
		AcquireSRWLockShared(&g_watchLock);
		memcpy(w, g_watch, sizeof(w));
		ReleaseSRWLockShared(&g_watchLock);
		for (int i = 0; i < 4; ++i) {
			if (w[i].addr) ReadTriple(w[i].addr, w[i].dbl, a_r.watch + 3 * i);
		}
		if (g_game.fillCommon) g_game.fillCommon(a_r);
		if (g_game.fill) g_game.fill(a_r);
	}

	// Call once per game frame from a frame hook. Cheap: one row into a queue.
	inline void Tick()
	{
		uint64_t f = g_frame.fetch_add(1) + 1;
		g_lastTick.store(Now());
		if (!g_csvOn.load(std::memory_order_relaxed) || f % g_csvEvery) return;
		Row r{};
		FillRow(r, 0);
		AcquireSRWLockExclusive(&g_rowLock);
		if (g_rows.size() < 100000) g_rows.push_back(r);
		ReleaseSRWLockExclusive(&g_rowLock);
	}

	inline FILE* g_csv = nullptr;
	inline double g_csvStart = 0.0;
	inline double g_csvLimit = 0.0;  // seconds; 0 = no limit
	inline uint64_t g_csvRows = 0;

	inline void OpenCsv()
	{
		if (g_csv) fclose(g_csv);
		time_t    now = time(nullptr);
		tm        lt;
		localtime_s(&lt, &now);
		wchar_t name[128];
		swprintf(name, 128, L"%ls_frames_%04d%02d%02d_%02d%02d%02d.csv", g_tag.c_str(), lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
			lt.tm_hour, lt.tm_min, lt.tm_sec);
		g_csv = _wfsopen(Path(name).c_str(), L"w", _SH_DENYWR);
		if (!g_csv) {
			Log("csv: cannot open %ls", name);
			return;
		}
		setvbuf(g_csv, nullptr, _IOFBF, 1 << 20);
		fprintf(g_csv, "t,frame,tid,flags");
		for (int i = 0; i < 16; ++i) fprintf(g_csv, ",p%d", i);
		for (int i = 0; i < 12; ++i) fprintf(g_csv, ",c%d", i);
		fprintf(g_csv, ",fov,lis_x,lis_y,lis_z,lis_fx,lis_fy,lis_fz,lis_tx,lis_ty,lis_tz,emi_id,emi_x,emi_y,emi_z");
		for (int i = 0; i < 12; ++i) fprintf(g_csv, ",w%d", i);
		fprintf(g_csv, ",pad,buttons,lt,rt,lx,ly,rx,ry,keys,fg\n");
		g_csvStart = Now();
		g_csvRows = 0;
		Log("csv: %ls (every %d frame(s), limit %.0f min)", name, g_csvEvery, g_csvLimit / 60.0);
	}

	inline void WriteRow(const Row& r)
	{
		FILE* f = g_csv;
		fprintf(f, "%.4f,%llu,%u,%u", r.t, static_cast<unsigned long long>(r.frame), r.tid, r.flags);
		for (float v : r.player) fprintf(f, ",%.6g", v);
		for (double v : r.cam) fprintf(f, ",%.6f", v);
		fprintf(f, ",%.6g,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%.4f,%.4f,%.4f", r.fov, r.lisPos[0], r.lisPos[1], r.lisPos[2],
			r.lisFront[0], r.lisFront[1], r.lisFront[2], r.lisTop[0], r.lisTop[1], r.lisTop[2],
			static_cast<unsigned long long>(r.emitterId), r.emitterPos[0], r.emitterPos[1], r.emitterPos[2]);
		for (double v : r.watch) fprintf(f, ",%.4f", v);
		fprintf(f, ",%d,%04x,%u,%u,%d,%d,%d,%d,%05x,%u\n", r.pad.index, r.pad.buttons, r.pad.lt, r.pad.rt, r.pad.lx, r.pad.ly, r.pad.rx,
			r.pad.ry, r.pad.keys, r.pad.fg);
	}

	inline void DrainRows()
	{
		std::vector<Row> rows;
		AcquireSRWLockExclusive(&g_rowLock);
		rows.swap(g_rows);
		ReleaseSRWLockExclusive(&g_rowLock);
		if (!g_csv || rows.empty()) return;
		if (g_csvLimit > 0 && Now() - g_csvStart > g_csvLimit) {
			if (g_csvOn.exchange(false)) Log("csv: time limit reached after %llu rows; 'csv new' starts another file", g_csvRows);
			return;
		}
		for (auto& r : rows) WriteRow(r);
		g_csvRows += rows.size();
		fflush(g_csv);
	}

	// ---- commands ---------------------------------------------------------------------------------------------
	// Write lines into <LogDir>\<tag>_cmd.txt; the probe runs them within 250 ms, logs the results and
	// deletes the file. Addresses: hex, "exe+rva" or "<module.dll>+rva".
	inline uintptr_t ParseAddr(const char* a_s)
	{
		if (!a_s) return 0;
		if (const char* plus = strchr(a_s, '+')) {
			std::string mod(a_s, plus);
			uintptr_t   base = _stricmp(mod.c_str(), "exe") == 0 ? g_exe : reinterpret_cast<uintptr_t>(GetModuleHandleA(mod.c_str()));
			return base ? base + strtoull(plus + 1, nullptr, 16) : 0;
		}
		return strtoull(a_s, nullptr, 16);
	}

	inline bool CommonCommand(int argc, char** argv)
	{
		const char* c = argv[0];
		if (!_stricmp(c, "help")) {
			Log("commands: mem <addr> [size] | ptr <addr> | bin <addr> <size> <name> | mod <dll> | rtti <addr> | "
				"vt <Class@ns> [max] | csv on|off|new | frames | + game/wwise commands (see README)");
		} else if (!_stricmp(c, "mem") && argc > 1) {
			uintptr_t a = ParseAddr(argv[1]);
			size_t    n = argc > 2 ? strtoull(argv[2], nullptr, 16) : 0x100;
			Log("mem %s +%zx", Rva(a).c_str(), n);
			HexDump(a, std::min<size_t>(n, 0x4000));
		} else if (!_stricmp(c, "ptr") && argc > 1) {
			uintptr_t a = ParseAddr(argv[1]), v = 0;
			bool      ok = Read(a, v);
			Log("ptr [%s] = %s%s  class '%s'", Rva(a).c_str(), Rva(v).c_str(), ok ? "" : " (unreadable)", ok ? ClassOf(v).c_str() : "");
		} else if (!_stricmp(c, "bin") && argc > 3) {
			uintptr_t            a = ParseAddr(argv[1]);
			size_t               n = std::min<size_t>(strtoull(argv[2], nullptr, 16), 64u << 20);
			std::vector<uint8_t> b(n);
			bool                 ok = SafeRead(b.data(), a, n);
			std::string          name = argv[3];
			if (FILE* f = _wfopen(Path(std::wstring(name.begin(), name.end()) + L".bin").c_str(), L"wb")) {
				fwrite(b.data(), 1, n, f);
				fclose(f);
			}
			Log("bin %s +%zx -> %s.bin %s", Rva(a).c_str(), n, argv[3], ok ? "ok" : "(unreadable: zeros)");
		} else if (!_stricmp(c, "mod") && argc > 1) {
			HMODULE    m = GetModuleHandleA(argv[1]);
			MODULEINFO mi{};
			if (m) GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
			Log("mod %s: base %p size %lx", argv[1], m, mi.SizeOfImage);
		} else if (!_stricmp(c, "rtti") && argc > 1) {
			uintptr_t a = ParseAddr(argv[1]);
			Log("rtti %p: '%s'", reinterpret_cast<void*>(a), ClassOf(a).c_str());
		} else if (!_stricmp(c, "vt") && argc > 1) {  // vt <Class@ns> [max] [module]
			HMODULE m = argc > 3 ? GetModuleHandleA(argv[3]) : reinterpret_cast<HMODULE>(g_exe);
			size_t  mx = argc > 2 ? strtoull(argv[2], nullptr, 10) : 32;
			auto    vts = m ? FindVtables(m, argv[1]) : std::vector<uintptr_t>{};
			Log("vt %s: %zu vtable(s)", argv[1], vts.size());
			for (auto v : vts) {
				auto hits = ScanHeap(v, mx);
				Log("  vtable %s: %zu instance(s)", Rva(v).c_str(), hits.size());
				for (auto h : hits) Log("    %p", reinterpret_cast<void*>(h));
			}
		} else if (!_stricmp(c, "csv") && argc > 1) {
			if (!_stricmp(argv[1], "new")) OpenCsv();
			g_csvOn = _stricmp(argv[1], "off") != 0;
			Log("csv %s", g_csvOn.load() ? "on" : "off");
		} else if (!_stricmp(c, "find") && argc > 3) {  // find <x> <y> <z> [tol] [max]
			double xyz[3] = { atof(argv[1]), atof(argv[2]), atof(argv[3]) };
			FindAndLog(xyz, argc > 4 ? atof(argv[4]) : 0.05, argc > 5 ? strtoull(argv[5], nullptr, 10) : 4096);
		} else if (!_stricmp(c, "cands") || !_stricmp(c, "keep")) {
			// cands: re-read every candidate. keep moved|static: drop the others (moved = changed by > 1 cm
			// since it was found; do it after the player has moved).
			std::vector<Cand> kept;
			for (size_t i = 0; i < g_cands.size(); ++i) {
				auto&  k = g_cands[i];
				double v[3];
				bool   ok = ReadTriple(k.addr, k.dbl, v);
				double d = ok ? std::sqrt((v[0] - k.first[0]) * (v[0] - k.first[0]) + (v[1] - k.first[1]) * (v[1] - k.first[1]) +
									  (v[2] - k.first[2]) * (v[2] - k.first[2]))
				              : -1;
				bool   moved = ok && d > 0.01;
				if (!_stricmp(c, "cands") && i < 64) {
					Log("  [%zu] %p %s %s now %.4f %.4f %.4f  moved %.3f", i, reinterpret_cast<void*>(k.addr), k.dbl ? "d" : "f",
						ok ? "" : "UNREADABLE", ok ? v[0] : 0, ok ? v[1] : 0, ok ? v[2] : 0, d);
				}
				if (argc > 1 && ok && ((!_stricmp(argv[1], "moved") && moved) || (!_stricmp(argv[1], "static") && !moved))) kept.push_back(k);
			}
			if (!_stricmp(c, "keep") && argc > 1) {
				Log("keep %s: %zu of %zu candidates", argv[1], kept.size(), g_cands.size());
				g_cands = kept;
			}
		} else if (!_stricmp(c, "watch") && argc > 2) {  // watch <slot 0-3> <cand index | addr> [d|f]
			int       slot = std::clamp(atoi(argv[1]), 0, 3);
			size_t    idx = strtoull(argv[2], nullptr, 10);
			bool      isIdx = strlen(argv[2]) < 6 && idx < g_cands.size();
			Watch     w{ isIdx ? g_cands[idx].addr : ParseAddr(argv[2]), isIdx ? g_cands[idx].dbl : !(argc > 3 && !_stricmp(argv[3], "f")) };
			AcquireSRWLockExclusive(&g_watchLock);
			g_watch[slot] = w;
			ReleaseSRWLockExclusive(&g_watchLock);
			Log("watch %d = %p (%s) -> csv w%d..w%d", slot, reinterpret_cast<void*>(w.addr), w.dbl ? "doubles" : "floats", slot * 3, slot * 3 + 2);
		} else if (!_stricmp(c, "unwatch")) {
			AcquireSRWLockExclusive(&g_watchLock);
			if (argc > 1) g_watch[std::clamp(atoi(argv[1]), 0, 3)] = {};
			else memset(g_watch, 0, sizeof(g_watch));
			ReleaseSRWLockExclusive(&g_watchLock);
			Log("unwatch %s", argc > 1 ? argv[1] : "all");
		} else if (!_stricmp(c, "frames")) {
			Log("frames: %llu ticks, last %.2f s ago, csv %s, %llu rows written", static_cast<unsigned long long>(g_frame.load()),
				g_lastTick.load() < 0 ? -1.0 : Now() - g_lastTick.load(), g_csvOn.load() ? "on" : "off", g_csvRows);
		} else {
			return false;
		}
		return true;
	}

	inline void PollCommands()
	{
		std::wstring file = Path(g_tag + L"_cmd.txt");
		FILE*        f = _wfopen(file.c_str(), L"r");
		if (!f) return;
		std::vector<std::string> lines;
		char                     buf[1024];
		while (fgets(buf, sizeof(buf), f)) lines.emplace_back(buf);
		fclose(f);
		DeleteFileW(file.c_str());
		for (auto& l : lines) {
			std::vector<char> w(l.begin(), l.end());
			w.push_back(0);
			char* argv[12] = {};
			int   argc = 0;
			char* ctx = nullptr;
			for (char* tok = strtok_s(w.data(), " \t\r\n", &ctx); tok && argc < 12; tok = strtok_s(nullptr, " \t\r\n", &ctx)) argv[argc++] = tok;
			if (!argc || argv[0][0] == '#') continue;
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
			Log("> %s", l.c_str());
			bool known = CommonCommand(argc, argv) || (g_game.commandCommon && g_game.commandCommon(argc, argv)) ||
			             (g_game.command && g_game.command(argc, argv));
			if (!known) Log("  unknown command (try 'help')");
		}
		Log("< done");
	}

	// ---- main loop ------------------------------------------------------------------------------------------------
	// Runs on the probe's own thread for the life of the process.
	inline void Run()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		g_csvEvery = std::max(1, IniInt(L"FrameCsvEvery", 1));
		g_csvLimit = 60.0 * IniInt(L"FrameCsvMinutes", 20);
		g_csvOn = IniInt(L"FrameCsv", 1) != 0;
		OpenCsv();
		CreateThread(nullptr, 0, PadThread, nullptr, 0, nullptr);
		double nextCmd = 0, nextSummary = 5, nextFallback = 0;
		for (;;) {
			Sleep(10);
			double now = Now();
			// No frame hook for a second (menus, loading, or the hook never fired): sample at 20 Hz anyway.
			if (g_csvOn && now - g_lastTick.load() > 1.0 && now >= nextFallback) {
				nextFallback = now + 0.05;
				Row r{};
				FillRow(r, kRowNoTick);
				AcquireSRWLockExclusive(&g_rowLock);
				g_rows.push_back(r);
				ReleaseSRWLockExclusive(&g_rowLock);
			}
			if (now >= nextCmd) {
				nextCmd = now + 0.25;
				DrainRows();
				PollCommands();
			}
			if (now >= nextSummary) {
				nextSummary = now + 5.0;
				PadSnap p = Pad();
				Log("--- %llu frames, last tick %.2f s ago; pad %d btn %04x LT %u RT %u L(%d,%d) R(%d,%d) keys %05x fg %u",
					static_cast<unsigned long long>(g_frame.load()), g_lastTick.load() < 0 ? -1.0 : now - g_lastTick.load(), p.index,
					p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry, p.keys, p.fg);
				if (g_game.summaryCommon) g_game.summaryCommon();
				if (g_game.summary) g_game.summary();
			}
		}
	}

	inline bool ExeNameIs(std::initializer_list<const wchar_t*> a_names)
	{
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const wchar_t* b = wcsrchr(exe, L'\\');
		b = b ? b + 1 : exe;
		for (auto* n : a_names) {
			if (!_wcsicmp(b, n)) return true;
		}
		return false;
	}
}
