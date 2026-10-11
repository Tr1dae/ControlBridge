// Phase 1b: Dylan follows Jesse. When puppet mode is on, Dylan's character controller position is
// written from inside coregame::charactercontroller::applyControllerToTransform (the 30 Hz ECS
// system that copies the controller into the entity transform; run thunk from
// tools/ecs_systems.py), just before the original runs. The transform then takes the written
// position and the next controller step starts from it.
//
// Mapping, fixed when puppet mode starts (or on re-anchor): both games are left-handed, Y up,
// metres (test 1), so
//     dylan = dylan0 + Ry(theta) * (jesse - jesse0)
// with theta the yaw difference between the two cameras at that moment, so "forward" is the same
// direction in both views. Jesse falling more than 50 m below the anchor stops the follow (Resonant
// autosaves; never leave Dylan under the map).
//
// Hotkeys while Resonant is in front: F7 puppet on/off, F8 back to where puppet mode started,
// F9 re-anchor. Commands (resonant_cmd.txt): puppet on|off, home, anchor, pos, tp <dx> <dy> <dz>,
// tpabs <x> <y> <z>.
#pragma once

#include <cmath>

#include "../common/bridge.h"
#include "../common/probe.h"
#include "../common/wwise.h"
#include "player.h"

namespace cb::puppet
{
	using SysFn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	inline SysFn g_applyOrig = nullptr;

	inline std::atomic<bool> g_on{ false };
	inline SRWLOCK           g_lock = SRWLOCK_INIT;
	inline double            g_dylan0[3], g_jesse0[3], g_theta = 0;  // anchor
	inline bool              g_anchored = false;
	inline double            g_home[3];
	inline bool              g_haveHome = false;

	// One-shot teleport request (from commands / F8), applied on the next system run.
	inline bool   g_tpPending = false, g_tpRelative = false;
	inline double g_tp[3];

	inline std::atomic<uint64_t> g_calls{ 0 }, g_writes{ 0 }, g_mismatch{ 0 }, g_refused{ 0 };
	inline double                g_lastTarget[3] = {};

	inline double Yaw(const float* a_fwd) { return std::atan2(a_fwd[0], a_fwd[2]); }  // 0 = +Z

	inline bool Guest(proto::GuestState& a_g)
	{
		return bridge::g_ok && link::Alive(bridge::g_map.shm->header.guestHeartbeatMs) && link::Read(bridge::g_map.shm->guest, a_g) &&
		       (a_g.flags & proto::kGuestInWorld);
	}

	inline bool HostCameraYaw(double& a_yaw)
	{
		uintptr_t v = wwise::views::g_view.load();
		double    m[12];
		if (!v || !SafeRead(m, v + wwise::views::g_matOff, sizeof(m))) return false;
		float f[3] = { static_cast<float>(m[6]), static_cast<float>(m[7]), static_cast<float>(m[8]) };
		a_yaw = Yaw(f);
		return true;
	}

	// Takes the current positions (and camera yaws) as the anchor. Caller holds g_lock.
	inline bool AnchorLocked()
	{
		proto::GuestState g;
		double            d[3];
		if (!Guest(g) || !player::Pos(player::g_obj.load(), d)) {
			Log("puppet: cannot anchor (Jesse %s, Dylan %s)", Guest(g) ? "ok" : "missing", player::g_obj.load() ? "ok" : "missing");
			return false;
		}
		double hy = 0, gy = Yaw(g.camera.xf.rot + 6);
		bool   cams = HostCameraYaw(hy) && (g.camera.xf.rot[6] != 0 || g.camera.xf.rot[8] != 0);
		g_theta = cams ? hy - gy : 0.0;
		memcpy(g_dylan0, d, 24);
		memcpy(g_jesse0, g.jesse.pos, 24);
		g_anchored = true;
		if (!g_haveHome) {
			memcpy(g_home, d, 24);
			g_haveHome = true;
		}
		Log("puppet: anchored Dylan %.2f %.2f %.2f <- Jesse %.2f %.2f %.2f, yaw offset %.1f deg%s", d[0], d[1], d[2], g.jesse.pos[0], g.jesse.pos[1],
			g.jesse.pos[2], g_theta * 57.29578, cams ? "" : " (no camera: 0)");
		return true;
	}

	inline void Set(bool a_on)
	{
		AcquireSRWLockExclusive(&g_lock);
		bool ok = !a_on || AnchorLocked();
		g_on = a_on && ok;
		ReleaseSRWLockExclusive(&g_lock);
		Log("puppet: %s", g_on ? "ON (Dylan follows Jesse)" : "off");
		Beep(a_on && ok ? 1200 : 600, 120);
	}

	inline void RequestTp(const double* a_v, bool a_relative)
	{
		AcquireSRWLockExclusive(&g_lock);
		memcpy(g_tp, a_v, 24);
		g_tpRelative = a_relative;
		g_tpPending = true;
		ReleaseSRWLockExclusive(&g_lock);
	}

	inline void WritePos(uintptr_t a_obj, const double* a_p)
	{
		__try {
			memcpy(reinterpret_cast<void*>(a_obj + player::kPosOff), a_p, 24);
			memcpy(reinterpret_cast<void*>(a_obj + player::kPrevOff), a_p, 24);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// The system's run thunk: integer/pointer arguments only (see tools/ecs_systems.py).
	inline uintptr_t ApplyDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
	{
		g_calls.fetch_add(1, std::memory_order_relaxed);
		uintptr_t obj = player::g_obj.load();
		double    cur[3], target[3];
		bool      write = false;
		if (obj && player::Pos(obj, cur) && (g_on.load() || g_tpPending)) {
			AcquireSRWLockExclusive(&g_lock);
			if (g_tpPending) {
				for (int i = 0; i < 3; ++i) target[i] = g_tpRelative ? cur[i] + g_tp[i] : g_tp[i];
				g_tpPending = false;
				write = true;
				if (g_on && g_anchored) {  // keep following from the new spot
					memcpy(g_dylan0, target, 24);
					proto::GuestState g;
					if (Guest(g)) memcpy(g_jesse0, g.jesse.pos, 24);
				}
				Log("puppet: teleport to %.2f %.2f %.2f", target[0], target[1], target[2]);
			} else if (g_on && g_anchored) {
				proto::GuestState g;
				if (Guest(g)) {
					double dx = g.jesse.pos[0] - g_jesse0[0], dy = g.jesse.pos[1] - g_jesse0[1], dz = g.jesse.pos[2] - g_jesse0[2];
					double c = std::cos(g_theta), s = std::sin(g_theta);
					target[0] = g_dylan0[0] + dx * c + dz * s;
					target[1] = g_dylan0[1] + dy;
					target[2] = g_dylan0[2] - dx * s + dz * c;
					if (dy < -50.0) {
						g_refused.fetch_add(1);  // Jesse fell far: don't drag Dylan under the map
					} else {
						write = true;
					}
				}
			}
			ReleaseSRWLockExclusive(&g_lock);
		}
		if (write) {
			WritePos(obj, target);
			memcpy(g_lastTarget, target, 24);
			g_writes.fetch_add(1, std::memory_order_relaxed);
		}
		uintptr_t r = g_applyOrig(a, b, c, d, e, f);
		if (write) {  // does the game keep what we wrote?
			double after[3];
			if (player::Pos(obj, after) && wwise::Dist(after, target) > 0.01 && g_mismatch.fetch_add(1) < 5) {
				Log("puppet: after the system ran, controller at %.3f %.3f %.3f (wrote %.3f %.3f %.3f)", after[0], after[1], after[2], target[0], target[1],
					target[2]);
			}
		}
		return r;
	}

	// ---- hotkeys (worker tick) -------------------------------------------------------------------
	inline bool g_keyDown[3] = {};

	inline void Tick()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid != GetCurrentProcessId()) return;
		const int keys[3] = { VK_F7, VK_F8, VK_F9 };
		for (int i = 0; i < 3; ++i) {
			bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
			if (down && !g_keyDown[i]) {
				if (i == 0) {
					Set(!g_on.load());
				} else if (i == 1) {
					if (g_haveHome) {
						RequestTp(g_home, false);
						Beep(900, 120);
					}
				} else {
					AcquireSRWLockExclusive(&g_lock);
					bool ok = AnchorLocked();
					ReleaseSRWLockExclusive(&g_lock);
					Beep(ok ? 1500 : 600, 80);
				}
			}
			g_keyDown[i] = down;
		}
	}

	inline void Summary()
	{
		if (!g_applyOrig) return;
		Log("puppet: %s; system runs %llu, writes %llu, kept-mismatch %llu, refused (Jesse fell) %llu; last target %.2f %.2f %.2f",
			g_on.load() ? "ON" : "off", static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_writes.load()),
			static_cast<unsigned long long>(g_mismatch.load()), static_cast<unsigned long long>(g_refused.load()), g_lastTarget[0], g_lastTarget[1],
			g_lastTarget[2]);
	}

	inline bool Command(int argc, char** argv)
	{
		const char* c = argv[0];
		if (!_stricmp(c, "puppet") && argc > 1) {
			Set(!_stricmp(argv[1], "on"));
		} else if (!_stricmp(c, "home")) {
			if (g_haveHome) RequestTp(g_home, false);
			Log("home: %s", g_haveHome ? "requested" : "no home yet (set when puppet mode first anchors)");
		} else if (!_stricmp(c, "anchor")) {
			AcquireSRWLockExclusive(&g_lock);
			AnchorLocked();
			ReleaseSRWLockExclusive(&g_lock);
		} else if (!_stricmp(c, "pos")) {
			double p[3];
			if (player::Pos(player::g_obj.load(), p)) Log("pos: Dylan %.3f %.3f %.3f", p[0], p[1], p[2]);
		} else if ((!_stricmp(c, "tp") || !_stricmp(c, "tpabs")) && argc > 3) {
			double v[3] = { atof(argv[1]), atof(argv[2]), atof(argv[3]) };
			RequestTp(v, !_stricmp(c, "tp"));
			Log("%s %.2f %.2f %.2f requested", c, v[0], v[1], v[2]);
		} else {
			return false;
		}
		return true;
	}

	inline void Install(void* a_applyThunk)
	{
		Hook(a_applyThunk, reinterpret_cast<void*>(&ApplyDetour), reinterpret_cast<void**>(&g_applyOrig), "applyControllerToTransform (puppet)");
	}
}
