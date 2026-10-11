// Dylan's position in CONTROL Resonant (test 4): Remedy's physics::CharacterController holds the
// controller position as doubles (float precision) right after its physics::ControllerHitReport
// member: hitReport+0x38. A second copy (previous step) follows at +0x58 after the first.
//
// The ControllerHitReport vtable is found by RTTI name, its instances by heap scan (a fraction of a
// second). Re-searched when the
// tracked object dies (vtable changes) or every 10 s while untracked. Read-only.
// Selection: nearest to the Wwise listener (see Search).
#pragma once

#include "../common/probe.h"
#include "../common/wwise.h"

namespace cb::player
{
	inline constexpr uint32_t kPosOff = 0x38;
	inline constexpr uint32_t kPrevOff = 0x38 + 0x58;

	inline uintptr_t              g_vt = 0;
	inline std::atomic<uintptr_t> g_obj{ 0 };
	inline double                 g_nextSearch = 0;
	inline size_t                 g_instances = 0;

	inline bool Pos(uintptr_t a_obj, double* a_out)
	{
		uintptr_t vt = 0;
		return a_obj && Read(a_obj, vt) && vt == g_vt && SafeRead(a_out, a_obj + kPosOff, 24) && std::isfinite(a_out[0]);
	}

	// Picks the controller nearest the Wwise listener: in Resonant the listener sits between the camera
	// and Dylan (test 4: 1.5 m from the camera), so Dylan's controller is the closest one. (Test 5: the
	// nearest-emitter rule found none of the 62 controllers within 3 m.)
	inline void Search()
	{
		using namespace wwise;
		Xf lis;
		if (!g_vt || !Get(ListenerId(), lis)) return;
		auto inst = ScanHeap(g_vt, 512);
		g_instances = inst.size();
		std::vector<std::pair<double, uintptr_t>> by;
		for (uintptr_t p : inst) {
			double pos[3];
			if (Pos(p, pos)) by.push_back({ Dist(pos, lis.pos), p });
		}
		std::sort(by.begin(), by.end());
		char line[256];
		int  n = snprintf(line, sizeof(line), "player: %zu controllers, nearest to the listener:", inst.size());
		for (size_t i = 0; i < by.size() && i < 3; ++i) n += snprintf(line + n, sizeof(line) - n, " %p %.2f m", reinterpret_cast<void*>(by[i].second), by[i].first);
		Log("%s", line);
		uintptr_t best = !by.empty() && by[0].first < 10.0 ? by[0].second : 0;
		if (best != g_obj.load()) Log("player: tracking %p", reinterpret_cast<void*>(best));
		g_obj = best;
	}

	inline void Tick()
	{
		double now = Now(), pos[3];
		if (Pos(g_obj.load(), pos)) return;
		if (g_obj.load()) {
			Log("player: controller %p gone", reinterpret_cast<void*>(g_obj.load()));
			g_obj = 0;
			g_nextSearch = now + 1.0;
		}
		if (now < g_nextSearch) return;
		g_nextSearch = now + 10.0;
		Search();
	}

	// CSV: p0..p2 = position, p3..p5 = previous copy (floats).
	inline void Fill(Row& a_r)
	{
		uintptr_t o = g_obj.load();
		double    p[3], q[3];
		if (!Pos(o, p)) return;
		if (!SafeRead(q, o + kPrevOff, 24)) memcpy(q, p, 24);
		for (int i = 0; i < 3; ++i) {
			a_r.player[i] = static_cast<float>(p[i]);
			a_r.player[3 + i] = static_cast<float>(q[i]);
		}
		a_r.flags |= kRowPlayer;
	}

	inline void Summary()
	{
		double p[3];
		uintptr_t o = g_obj.load();
		if (Pos(o, p)) Log("player: controller %p pos %.3f %.3f %.3f (%zu controllers)", reinterpret_cast<void*>(o), p[0], p[1], p[2], g_instances);
		else Log("player: no controller tracked (%zu controllers seen)", g_instances);
	}

	inline void Init(HMODULE a_exe)
	{
		auto vts = FindVtables(a_exe, "ControllerHitReport@physics");
		g_vt = vts.empty() ? 0 : vts[0];
		Log("player: physics::ControllerHitReport vtable %s (%zu found)", g_vt ? Rva(g_vt).c_str() : "MISSING", vts.size());
	}
}
