// Keeps Control's gamepad alive (test 9). input::InputManager owns 4 Gamepad slots at +0x00..+0x18;
// each slot points at an implementation whose update (input+0x12e0) only polls XInput while
// connected (+0x19) or while a re-check is requested (+0x18). One failed read (Steam Input moving
// the controller to the focused game, a wireless dropout) marks the pad disconnected and it is
// never polled again until refreshGamepadConnectivity sets +0x18, which happens on a Windows device
// change notice. Control then ignores the controller (keyboard+mouse keep working).
//
// From InputManager::update (game thread, once per frame), before the original runs:
//   - a disconnected pad gets its re-check flag set once a second (what refreshGamepadConnectivity does)
//   - with FakeFocus=1, a gamepad selection that was cleared (+0x68 = -1) is restored to the last
//     selected index once that pad reads as connected again
// set/clearSelectedGamepad are logged with their callers.
#pragma once

#include <intrin.h>

#include "../common/probe.h"
#include "focus.h"

namespace cb::gamepad
{
	using UpdateFn = void (*)(void*, float);
	using SetSelFn = void (*)(void*, int);
	using ClearSelFn = void (*)(void*);
	inline UpdateFn   g_updateOrig = nullptr;
	inline SetSelFn   g_setOrig = nullptr;
	inline ClearSelFn g_clearOrig = nullptr;

	inline std::atomic<uintptr_t> g_mgr{ 0 };
	inline std::atomic<int>       g_lastSelected{ -1 };
	inline double                 g_nextRefresh[4] = {};
	inline std::atomic<uint64_t>  g_refreshes{ 0 }, g_reselects{ 0 };
	inline bool                   g_lastConnected[4] = { true, true, true, true };

	inline uintptr_t Impl(uintptr_t a_mgr, int a_i)
	{
		uintptr_t pad = 0, impl = 0;
		return Read(a_mgr + a_i * 8, pad) && pad && Read(pad, impl) ? impl : 0;
	}

	inline void UpdateDetour(void* a_mgr, float a_dt)
	{
		auto   mgr = reinterpret_cast<uintptr_t>(a_mgr);
		double now = Now();
		g_mgr = mgr;
		for (int i = 0; i < 4; ++i) {
			uintptr_t impl = Impl(mgr, i);
			uint8_t   flags[2];
			if (!impl || !SafeRead(flags, impl + 0x18, 2)) continue;
			bool connected = flags[1] != 0;
			if (connected != g_lastConnected[i]) {
				Log("gamepad: slot %d %s", i, connected ? "connected" : "DISCONNECTED (Control stops polling it)");
				g_lastConnected[i] = connected;
			}
			if (!connected && !flags[0] && now >= g_nextRefresh[i]) {
				g_nextRefresh[i] = now + 1.0;
				*reinterpret_cast<volatile uint8_t*>(impl + 0x18) = 1;  // re-check on this update
				g_refreshes.fetch_add(1);
			}
		}
		int sel = -1;
		Read(mgr + 0x68, sel);
		int last = g_lastSelected.load();
		if (focus::Force() && sel < 0 && last >= 0 && last < 4 && g_setOrig) {
			uintptr_t impl = Impl(mgr, last);
			uint8_t   connected = 0;
			if (impl && Read(impl + 0x19, connected) && connected) {
				g_setOrig(a_mgr, last);
				g_reselects.fetch_add(1);
				Log("gamepad: selection was cleared, re-selected gamepad %d", last);
			}
		}
		g_updateOrig(a_mgr, a_dt);
	}

	inline void SetSelDetour(void* a_mgr, int a_i)
	{
		Log("gamepad: setSelectedGamepad(%d) from %s", a_i, Rva(reinterpret_cast<uintptr_t>(_ReturnAddress())).c_str());
		if (a_i >= 0 && a_i < 4) g_lastSelected = a_i;
		g_setOrig(a_mgr, a_i);
	}

	inline void ClearSelDetour(void* a_mgr)
	{
		Log("gamepad: clearSelectedGamepad() from %s", Rva(reinterpret_cast<uintptr_t>(_ReturnAddress())).c_str());
		g_clearOrig(a_mgr);
	}

	inline void Summary()
	{
		uintptr_t mgr = g_mgr.load();
		if (!mgr) return;
		int sel = -1, active = -1;
		Read(mgr + 0x68, sel);
		Read(mgr + 0x20, active);
		char line[160];
		int  n = snprintf(line, sizeof(line), "gamepad: selected %d, active id %d, connected", sel, active);
		for (int i = 0; i < 4; ++i) {
			uint8_t c = 0;
			uintptr_t impl = Impl(mgr, i);
			n += snprintf(line + n, sizeof(line) - n, " %d", impl && Read(impl + 0x19, c) ? c : -1);
		}
		Log("%s; re-checks %llu, re-selects %llu", line, static_cast<unsigned long long>(g_refreshes.load()),
			static_cast<unsigned long long>(g_reselects.load()));
	}

	inline void Install(HMODULE a_input)
	{
		auto ex = [&](const char* a_n) { return a_input ? reinterpret_cast<void*>(GetProcAddress(a_input, a_n)) : nullptr; };
		Hook(ex("?update@InputManager@input@@QEAAXM@Z"), reinterpret_cast<void*>(&UpdateDetour), reinterpret_cast<void**>(&g_updateOrig),
			"InputManager::update");
		Hook(ex("?setSelectedGamepad@InputManager@input@@QEAAXH@Z"), reinterpret_cast<void*>(&SetSelDetour), reinterpret_cast<void**>(&g_setOrig),
			"InputManager::setSelectedGamepad");
		Hook(ex("?clearSelectedGamepad@InputManager@input@@QEAAXXZ"), reinterpret_cast<void*>(&ClearSelDetour),
			reinterpret_cast<void**>(&g_clearOrig), "InputManager::clearSelectedGamepad");
	}
}
