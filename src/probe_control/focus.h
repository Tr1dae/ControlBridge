// Fake focus for Control (FakeFocus=1). Test 6: alt-tabbing away paused Control (its pause menu
// opened) before Resonant was even up, so Jesse never moved. Control learns about focus three ways,
// and all three are covered while faking:
//   1. app::FocusEvent / app::MinimizeEvent objects through coregame::GameWindow::dispatchAppEvent
//      (exported): focus-lost and minimized events are swallowed (gained / restored pass), identified
//      by their type id. The first event of every type is logged.
//   2. GameWindow::hasFocus / isMinimized (bytes +0x7C / +0x7D, getters imported by the exe): the
//      detours in probe_control.cpp answer true / false, and the bytes are held there.
//   3. A game that thinks it has focus clips and re-centres the cursor: ClipCursor and SetCursorPos
//      are ignored while Control isn't really the foreground window, so it can't take the mouse
//      from Resonant.
//   4. Control reads mouse buttons with GetAsyncKeyState, which sees global state (test 8: desktop and
//      Resonant clicks fired Jesse's weapon and flipped Control to keyboard+mouse, after which it
//      ignored the gamepad). While it isn't really in front, GetAsyncKeyState / GetKeyState /
//      GetKeyboardState report nothing pressed, and raw-input registrations lose their background
//      (INPUTSINK) flags. The virtual pad is then its only input.
#pragma once

#include "../common/probe.h"

namespace cb::focus
{
	inline bool                   g_fake = false;
	inline std::atomic<uintptr_t> g_window{ 0 };  // the GameWindow (from the hasFocus detour's this)
	inline std::atomic<uint64_t>  g_swallowed{ 0 }, g_cursorBlocked{ 0 };

	inline bool Force() { return g_fake; }

	inline bool QueryForeground()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	// Refreshed every 10 ms by Tick (key-state calls come hundreds of times per frame).
	inline std::atomic<bool> g_fg{ true };
	inline bool              ReallyForeground() { return g_fg.load(std::memory_order_relaxed); }
	inline bool              Muted() { return g_fake && !ReallyForeground(); }

	// ---- 1. app events ------------------------------------------------------------------------
	// app::Event starts with an int type; app::EventHandler::dispatchAppEvent switches on it (24 types).
	// Mapped from its jump table against app::App's handleAppEvent overloads (RTTI vtable, slots
	// 25/24/23): 0 = CloseEvent, 1 = FocusEvent, 2 = MinimizeEvent. The event classes have no RTTI.
	inline constexpr int kFocusEvent = 1, kMinimizeEvent = 2;
	using DispatchFn = bool (*)(void*, void*);
	inline DispatchFn            g_dispatchOrig = nullptr;
	inline std::atomic<uint64_t> g_typeCount[24];

	inline bool DispatchDetour(void* a_win, void* a_ev)
	{
		g_window = reinterpret_cast<uintptr_t>(a_win);
		int32_t type = -1;
		Read(reinterpret_cast<uintptr_t>(a_ev), type);
		if (type >= 0 && type < 24 && g_typeCount[type].fetch_add(1) == 0) {
			Log("focus: first app event of type %d (thread %lu)", type, GetCurrentThreadId());
			HexDump(reinterpret_cast<uintptr_t>(a_ev), 0x20);
		}
		if (type == kFocusEvent || type == kMinimizeEvent) {
			// The int after the type is the new state: Focus 1 = gained / 0 = lost, Minimize 1 = minimized /
			// 0 = restored (test 7: the startup "gained" event, payload 01, was swallowed too, so Control
			// never thought it had focus and ignored the controller). Only losses are swallowed.
			int32_t state = -1;
			Read(reinterpret_cast<uintptr_t>(a_ev) + 4, state);
			bool loss = type == kFocusEvent ? state == 0 : state == 1;
			bool swallow = g_fake && loss;
			Log("focus: %s event, state %d (%s): %s", type == kFocusEvent ? "Focus" : "Minimize", state,
				type == kFocusEvent ? (state ? "gained" : "lost") : (state ? "minimized" : "restored"), swallow ? "SWALLOWED" : "passed");
			if (swallow) {
				g_swallowed.fetch_add(1);
				return false;
			}
		}
		return g_dispatchOrig(a_win, a_ev);
	}

	// ---- 3. cursor --------------------------------------------------------------------------------
	using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
	using SetCursorPosFn = BOOL(WINAPI*)(int, int);
	inline ClipCursorFn   g_clipOrig = nullptr;
	inline SetCursorPosFn g_setPosOrig = nullptr;

	inline BOOL WINAPI ClipCursorDetour(const RECT* a_r)
	{
		if (g_fake && a_r && !ReallyForeground()) {
			g_cursorBlocked.fetch_add(1);
			return TRUE;
		}
		return g_clipOrig(a_r);
	}

	inline BOOL WINAPI SetCursorPosDetour(int a_x, int a_y)
	{
		if (g_fake && !ReallyForeground()) {
			g_cursorBlocked.fetch_add(1);
			return TRUE;
		}
		return g_setPosOrig(a_x, a_y);
	}

	// ---- 4. keyboard / mouse state while in the background ---------------------------------------------
	using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
	using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
	using RegisterRawFn = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
	inline GetAsyncKeyStateFn    g_asyncOrig = nullptr, g_keyStateOrig = nullptr;
	inline GetKeyboardStateFn    g_kbStateOrig = nullptr;
	inline RegisterRawFn         g_rawRegOrig = nullptr;
	inline std::atomic<uint64_t> g_keysMuted{ 0 };

	inline SHORT WINAPI GetAsyncKeyStateDetour(int a_vk)
	{
		if (Muted()) {
			g_keysMuted.fetch_add(1, std::memory_order_relaxed);
			return 0;
		}
		return g_asyncOrig(a_vk);
	}

	inline SHORT WINAPI GetKeyStateDetour(int a_vk)
	{
		if (Muted()) return 0;
		return g_keyStateOrig(a_vk);
	}

	inline BOOL WINAPI GetKeyboardStateDetour(PBYTE a_keys)
	{
		if (Muted() && a_keys) {
			memset(a_keys, 0, 256);
			return TRUE;
		}
		return g_kbStateOrig(a_keys);
	}

	inline BOOL WINAPI RegisterRawDetour(PCRAWINPUTDEVICE a_devs, UINT a_n, UINT a_size)
	{
		std::vector<RAWINPUTDEVICE> d(a_devs, a_devs + (a_devs ? a_n : 0));
		for (auto& r : d) {
			Log("focus: RegisterRawInputDevices usage %02x/%02x flags %08lx hwnd %p%s", r.usUsagePage, r.usUsage, r.dwFlags, r.hwndTarget,
				g_fake && (r.dwFlags & (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK)) ? " -> background flags removed" : "");
			if (g_fake) r.dwFlags &= ~(RIDEV_INPUTSINK | RIDEV_EXINPUTSINK);
		}
		return g_rawRegOrig(d.empty() ? a_devs : d.data(), a_n, a_size);
	}

	// ---- 2. hold the window's flags (worker tick) ---------------------------------------------------
	inline void Tick()
	{
		g_fg = QueryForeground();
		uintptr_t w = g_window.load();
		if (!g_fake || !w) return;
		uint8_t f[2];
		if (!SafeRead(f, w + 0x7C, 2) || (f[0] == 1 && f[1] == 0)) return;
		__try {
			*reinterpret_cast<volatile uint8_t*>(w + 0x7C) = 1;
			*reinterpret_cast<volatile uint8_t*>(w + 0x7D) = 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	inline void Summary()
	{
		if (!g_fake) return;
		Log("focus: faking (really foreground: %s); focus events swallowed %llu, cursor calls blocked %llu, key reads muted %llu",
			ReallyForeground() ? "yes" : "no", static_cast<unsigned long long>(g_swallowed.load()),
			static_cast<unsigned long long>(g_cursorBlocked.load()), static_cast<unsigned long long>(g_keysMuted.load()));
	}

	inline void Install(HMODULE a_core)
	{
		g_fake = IniInt(L"FakeFocus", 0) != 0;
		Log("focus: fake focus %s", g_fake ? "ON (Control won't pause or take the mouse when it loses focus)" : "off");
		// Hooked even when off, to log the event classes and focus events.
		Hook(a_core ? reinterpret_cast<void*>(GetProcAddress(a_core, "?dispatchAppEvent@GameWindow@coregame@@UEAA_NPEAUEvent@app@@@Z")) : nullptr,
			reinterpret_cast<void*>(&DispatchDetour), reinterpret_cast<void**>(&g_dispatchOrig), "GameWindow::dispatchAppEvent");
		if (!g_fake) return;
		HMODULE u = GetModuleHandleW(L"user32.dll");
		Hook(reinterpret_cast<void*>(GetProcAddress(u, "ClipCursor")), reinterpret_cast<void*>(&ClipCursorDetour),
			reinterpret_cast<void**>(&g_clipOrig), "user32!ClipCursor");
		Hook(reinterpret_cast<void*>(GetProcAddress(u, "SetCursorPos")), reinterpret_cast<void*>(&SetCursorPosDetour),
			reinterpret_cast<void**>(&g_setPosOrig), "user32!SetCursorPos");
		Hook(reinterpret_cast<void*>(GetProcAddress(u, "GetAsyncKeyState")), reinterpret_cast<void*>(&GetAsyncKeyStateDetour),
			reinterpret_cast<void**>(&g_asyncOrig), "user32!GetAsyncKeyState");
		Hook(reinterpret_cast<void*>(GetProcAddress(u, "GetKeyState")), reinterpret_cast<void*>(&GetKeyStateDetour),
			reinterpret_cast<void**>(&g_keyStateOrig), "user32!GetKeyState");
		Hook(reinterpret_cast<void*>(GetProcAddress(u, "GetKeyboardState")), reinterpret_cast<void*>(&GetKeyboardStateDetour),
			reinterpret_cast<void**>(&g_kbStateOrig), "user32!GetKeyboardState");
		Hook(reinterpret_cast<void*>(GetProcAddress(u, "RegisterRawInputDevices")), reinterpret_cast<void*>(&RegisterRawDetour),
			reinterpret_cast<void**>(&g_rawRegOrig), "user32!RegisterRawInputDevices");
	}
}
