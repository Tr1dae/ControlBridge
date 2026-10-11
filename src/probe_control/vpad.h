// Phase 1: Jesse is played with the controller Resonant reads. Control loads XInput itself
// (rl_rmdwin*_f.dll: xinput1_4 / 1_3 / 9_1_0, XInputGetState by name), so the probe hooks
// XInputGetState in whichever of those modules is loaded. While the link's host is alive and its pad
// is active (Resonant in front, a controller connected), user index 0 returns that pad; otherwise
// every call passes through unchanged.
// Switched by controlbridge.ini (VirtualPad=1). Keeping Control running in the background is
// focus.h's job (FakeFocus=1).
#pragma once

#include "../common/bridge.h"
#include "../common/probe.h"

namespace cb::vpad
{
	struct XGamepad
	{
		uint16_t buttons;
		uint8_t  lt, rt;
		int16_t  lx, ly, rx, ry;
	};
	struct XStateRaw
	{
		DWORD    packet;
		XGamepad pad;
	};
	using GetStateFn = DWORD(WINAPI*)(DWORD, XStateRaw*);

	inline bool                  g_enabled = false;
	inline GetStateFn            g_orig[3] = {};
	inline const wchar_t* const  kDlls[3] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
	inline bool                  g_hooked[3] = {};
	inline std::atomic<uint64_t> g_calls{ 0 }, g_served{ 0 };
	inline std::atomic<bool>     g_live{ false };  // the last call served the virtual pad
	inline uint64_t              g_callsMark = 0, g_servedMark = 0;

	// The host's pad, if it should drive Jesse right now.
	inline bool HostPad(proto::PadState& a_out)
	{
		if (!bridge::g_ok || !link::Alive(bridge::g_map.shm->header.hostHeartbeatMs)) return false;
		return link::Read(bridge::g_map.shm->pad, a_out) && (a_out.flags & proto::kPadActive);
	}

	template <int N>
	DWORD WINAPI GetStateDetour(DWORD a_index, XStateRaw* a_state)
	{
		g_calls.fetch_add(1, std::memory_order_relaxed);
		proto::PadState p;
		if (g_enabled && a_index == 0 && a_state && HostPad(p)) {
			a_state->packet = p.packet;
			a_state->pad = { p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry };
			g_served.fetch_add(1, std::memory_order_relaxed);
			g_live = true;
			return ERROR_SUCCESS;
		}
		if (a_index == 0) g_live = false;
		return g_orig[N](a_index, a_state);
	}

	inline void* const kDetours[3] = { reinterpret_cast<void*>(&GetStateDetour<0>), reinterpret_cast<void*>(&GetStateDetour<1>),
		reinterpret_cast<void*>(&GetStateDetour<2>) };

	// Called from the worker tick: hooks XInput modules as Control loads them.
	inline void Tick()
	{
		if (!g_enabled) return;
		for (int i = 0; i < 3; ++i) {
			if (g_hooked[i]) continue;
			HMODULE m = GetModuleHandleW(kDlls[i]);
			if (!m) continue;
			g_hooked[i] = true;  // one attempt per module
			char name[64];
			snprintf(name, sizeof(name), "%ls!XInputGetState", kDlls[i]);
			void* fn = reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState"));
			if (Hook(fn, kDetours[i], reinterpret_cast<void**>(&g_orig[i]), name) && i == 0) {
				g_xget = reinterpret_cast<XGetFn>(g_orig[0]);  // the sampler keeps seeing the real pad
			}
		}
	}

	inline void Summary()
	{
		if (!g_enabled) return;
		uint64_t c = g_calls.load(), s = g_served.load();
		proto::PadState p{};
		bool            host = HostPad(p);
		Log("vpad: XInputGetState %llu calls (+%llu), virtual pad served %llu (+%llu); host pad %s",
			static_cast<unsigned long long>(c), static_cast<unsigned long long>(c - g_callsMark), static_cast<unsigned long long>(s),
			static_cast<unsigned long long>(s - g_servedMark), host ? "active" : "inactive");
		g_callsMark = c;
		g_servedMark = s;
	}

	inline void Init()
	{
		g_enabled = IniInt(L"VirtualPad", 0) != 0;
		Log("vpad: virtual pad %s", g_enabled ? "on" : "off");
	}
}
