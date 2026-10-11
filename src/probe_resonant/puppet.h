// Phase 1b: Dylan follows Jesse, moved with the engine's own teleport (puppet v3).
//
// Tests 11-13: writing positions into the character controller (the ControllerHitReport+0x38 copy,
// then what looked like the position behind applyControllerToTransform's per-entity body) moved
// nothing. The script binding nl_teleport (Luau table at exe+1a429d0 -> exe+1a38330) shows the
// real path:
//   transform = exe+1a480b0(&world, entity)       32-byte component, type hash 0x6cfbb2a9
//   exe+1838400(&world, entity, &transform32, 0)  sets the world transform (resolves parents)
//   Teleported[row] = 1                           component type hash 0xc55ae319, so
//                                                 movementmodel::teleportedPreMovement resyncs the
//                                                 controller; clearTeleported resets the tag
// world = [rcx] of any system run thunk. entity = 32-bit index into [world+0x58530] (entry: low 16
// bits archetype, 0xffff = free; high 32 bits row). Column lookup: exe+32ced90(cols, count, hash)
// on the archetype record at world + (archetype + 0xc22) * 32 (+0x10 cols, +0x24 count); column
// offsets at [world + archetype*32 + 0x18458]; chunk base at [world + archetype*8 + 0x50].
// The transform component (test 14) is a float quaternion (x y z w) + float position + 4 bytes.
//
// Everything runs inside applyControllerToTransform's run thunk (exe+1bc6500), after the original:
// on the game's thread, in the phase that writes controller transforms. Dylan's entity is found by
// matching transforms against the tracked controller (player.h); the candidates are logged.
//
// Mapping, fixed when puppet mode starts (or on re-anchor): both games are left-handed, Y up,
// metres (test 1), so
//     dylan = dylan0 + Ry(theta) * (jesse - jesse0)
// with theta the yaw difference between the two cameras at that moment. Jesse falling more than
// 50 m below the anchor stops the follow (Resonant autosaves; never leave Dylan under the map).
//
// Hotkeys while Resonant is in front: Insert puppet on/off, Home back to where puppet mode started,
// Page Up re-anchor. Commands (resonant_cmd.txt): puppet on|off, home, anchor, pos, tp <dx> <dy>
// <dz>, tpabs <x> <y> <z>, rescan.
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
	inline double            g_dylan0[3], g_jesse0[3], g_theta = 0;  // anchor (controller/feet space)
	inline bool              g_anchored = false;
	inline double            g_home[3];
	inline bool              g_haveHome = false;

	// One-shot teleport request (commands / Home), applied on the next system run.
	inline bool   g_tpPending = false, g_tpRelative = false;
	inline double g_tp[3];

	inline std::atomic<uint64_t> g_calls{ 0 }, g_writes{ 0 }, g_failed{ 0 }, g_refused{ 0 };
	inline double                g_lastTarget[3] = {};

	// ---- the link and the anchor ---------------------------------------------------------------
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

	// Target for this step in controller (feet) space, false if nothing to write. Caller holds g_lock.
	inline bool TargetLocked(const double* a_cur, double* a_target)
	{
		if (g_tpPending) {
			for (int i = 0; i < 3; ++i) a_target[i] = g_tpRelative ? a_cur[i] + g_tp[i] : g_tp[i];
			g_tpPending = false;
			if (g_on && g_anchored) {  // keep following from the new spot
				memcpy(g_dylan0, a_target, 24);
				proto::GuestState g;
				if (Guest(g)) memcpy(g_jesse0, g.jesse.pos, 24);
			}
			Log("puppet: teleport to %.2f %.2f %.2f", a_target[0], a_target[1], a_target[2]);
			return true;
		}
		if (!g_on || !g_anchored) return false;
		proto::GuestState g;
		if (!Guest(g)) return false;
		double dx = g.jesse.pos[0] - g_jesse0[0], dy = g.jesse.pos[1] - g_jesse0[1], dz = g.jesse.pos[2] - g_jesse0[2];
		double c = std::cos(g_theta), s = std::sin(g_theta);
		a_target[0] = g_dylan0[0] + dx * c + dz * s;
		a_target[1] = g_dylan0[1] + dy;
		a_target[2] = g_dylan0[2] - dx * s + dz * c;
		if (dy < -50.0) {
			g_refused.fetch_add(1);  // Jesse fell far: don't drag Dylan under the map
			return false;
		}
		return true;
	}

	// ---- the ECS teleport (build 25767184 addresses) -------------------------------------------------
	// Only the transform setter is called; the lookups are re-implemented with guarded reads, because
	// the game's own lookup (exe+1a480b0) fatal-errors on a free slot, and the scan runs on another
	// thread while entities come and go. exe+32ced90 is a plain linear search over u32 column hashes.
	inline constexpr uint32_t kSetWorldRva = 0x1838400;
	// Component type ids are FNV-1a 32 of the full name (checked: Teleported, WorldTransformReadOnly).
	constexpr uint32_t Fnv1a(const char* a_s)
	{
		uint32_t h = 0x811c9dc5;
		for (; *a_s; ++a_s) h = (h ^ static_cast<uint8_t>(*a_s)) * 0x01000193u;
		return h;
	}
	inline constexpr uint32_t kTransformHash = Fnv1a("coregame::component::WorldTransformReadOnly");
	inline constexpr uint32_t kTeleportedHash = Fnv1a("coregame::component::Teleported");
	inline constexpr uint32_t kCharacterControllerHash = Fnv1a("coregame::component::CharacterController");
	inline constexpr uint32_t kPlayerHash = Fnv1a("coregame::component::Player");
	inline constexpr uint32_t kActiveAvatarHash = Fnv1a("coregame::component::ActiveAvatar");
	static_assert(kTransformHash == 0x6cfbb2a9 && kTeleportedHash == 0xc55ae319);
	using SetWorldFn = void (*)(const uintptr_t*, uint64_t, const void*, uint8_t);
	inline SetWorldFn g_setWorld = nullptr;

	inline std::atomic<uintptr_t> g_world{ 0 };
	inline std::atomic<int64_t>   g_entity{ -1 };    // Dylan's entity index
	inline uintptr_t              g_entityFor = 0;   // the controller (hit report) it was matched to
	inline double                 g_nextScan = 0;
	inline double                 g_posOff[3] = {};  // transform position minus the controller copy
	inline std::atomic<uint32_t>  g_offWorld{ 0 };  // consecutive steps the entity sat far from the controller
	inline std::atomic<bool>      g_rescan{ false };

	inline bool Entry(uintptr_t a_world, uint32_t a_idx, uint16_t& a_arch, uint32_t& a_row)
	{
		uintptr_t table = 0;
		uint64_t  e = 0;
		if (!Read(a_world + 0x58530, table) || !table || !Read(table + uint64_t(a_idx) * 8, e)) return false;
		a_arch = static_cast<uint16_t>(e);
		a_row = static_cast<uint32_t>(e >> 32);
		return a_arch != 0xffff;
	}

	// Index of a component column in an archetype (as exe+32ced90), -1 if absent.
	inline int FindCol(uintptr_t a_world, uint16_t a_arch, uint32_t a_hash)
	{
		uintptr_t rec = a_world + (uintptr_t(a_arch) + 0xc22) * 32, cols = 0;
		uint32_t  n = 0;
		if (!Read(rec + 0x10, cols) || !Read(rec + 0x24, n) || !cols || n > 4096) return -1;
		uint32_t h[4096];
		if (!SafeCopy(h, reinterpret_cast<const void*>(cols), size_t(n) * 4)) return -1;
		for (uint32_t i = 0; i < n; ++i) {
			if (h[i] == a_hash) return static_cast<int>(i);
		}
		return -1;
	}

	// Address of a component's data for an entity (as exe+1a480b0 does: chunk base + column offset +
	// row * size), 0 if the entity or component isn't there. a_cache: per-archetype column (scan).
	inline uintptr_t ComponentPtr(uintptr_t a_world, uint32_t a_idx, uint32_t a_hash, uint32_t a_size, std::vector<int>* a_cache = nullptr)
	{
		uint16_t arch;
		uint32_t row, off = 0;
		if (!Entry(a_world, a_idx, arch, row)) return 0;
		int col;
		if (a_cache) {
			if ((*a_cache)[arch] == -2) (*a_cache)[arch] = FindCol(a_world, arch, a_hash);
			col = (*a_cache)[arch];
		} else {
			col = FindCol(a_world, arch, a_hash);
		}
		uintptr_t offs = 0, chunk = 0;
		if (col < 0 || !Read(a_world + uintptr_t(arch) * 32 + 0x18458, offs) || !Read(offs + uintptr_t(col) * 4, off) || !off ||
			!Read(a_world + uintptr_t(arch) * 8 + 0x50, chunk) || !chunk)
			return 0;
		return chunk + off + uintptr_t(row) * a_size;
	}

	inline bool CallSetWorld(uintptr_t a_world, uint32_t a_idx, const void* a_xf)
	{
		__try {
			g_setWorld(&a_world, a_idx, a_xf, 0);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	inline bool SetByte(uintptr_t a_at)
	{
		__try {
			*reinterpret_cast<volatile uint8_t*>(a_at) = 1;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Sets the entity's Teleported byte, as nl_teleport does.
	inline bool MarkTeleported(uintptr_t a_world, uint32_t a_idx)
	{
		uintptr_t p = ComponentPtr(a_world, a_idx, kTeleportedHash, 1);
		return p && SetByte(p);
	}

	// The 32-byte transform (test 14): float rotation quaternion x y z w at +0, float position at +16,
	// 4 bytes padding. Positions are world space (floats).
	inline void XfPos(const uint8_t* a_xf, double* a_out)
	{
		float f[3];
		memcpy(f, a_xf + 16, 12);
		for (int i = 0; i < 3; ++i) a_out[i] = f[i];
	}

	inline void XfSetPos(uint8_t* a_xf, const double* a_p)
	{
		float f[3] = { static_cast<float>(a_p[0]), static_cast<float>(a_p[1]), static_cast<float>(a_p[2]) };
		memcpy(a_xf + 16, f, 12);
	}

	inline bool CopyXf(uintptr_t a_t, uint8_t* a_out)
	{
		__try {
			memcpy(a_out, reinterpret_cast<const void*>(a_t), 32);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Finds Dylan's entity: the transform whose position sits on the tracked controller. Runs on the
	// probe's worker thread (test 14: done on the game thread every 2 s over 283k entities it froze
	// Resonant). The entity table is copied once; per entity only the game's own lookup and a
	// 32-byte read, no VirtualQuery.
	inline void Scan()
	{
		uintptr_t world = g_world.load(), hit = player::g_obj.load(), table = 0;
		double    want[3];
		if (!world || !hit || !player::Pos(hit, want) || !Read(world + 0x58530, table) || !table) return;
		double                   t0 = Now();
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(reinterpret_cast<void*>(table), &mbi, sizeof(mbi))) return;
		size_t n = std::min<size_t>((reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - table) / 8, 1u << 21);
		std::vector<uint64_t> entries(n);
		if (!SafeCopy(entries.data(), reinterpret_cast<const void*>(table), n * 8)) return;
		struct Cand
		{
			double   d;
			uint32_t idx;
			uint8_t  raw[32];
		};
		std::vector<Cand> c;
		std::vector<int>  cache(65536, -2);
		uint32_t          live = 0;
		for (uint32_t idx = 0; idx < n; ++idx) {
			if (static_cast<uint16_t>(entries[idx]) == 0xffff) continue;
			++live;
			uintptr_t t = ComponentPtr(world, idx, kTransformHash, 32, &cache);
			Cand      x{ 1e30, idx, {} };
			if (!t || !CopyXf(t, x.raw)) continue;
			double p[3];
			XfPos(x.raw, p);
			if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
				x.d = std::hypot(p[0] - want[0], p[2] - want[2]) + 0.1 * std::fabs(p[1] - want[1]);
			if (c.size() < 32 || x.d < c.back().d) {  // keep the 32 nearest (the character and its attached children)
				c.push_back(x);
				std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
				if (c.size() > 32) c.pop_back();
			}
		}
		Log("puppet: entity scan (%.0f ms): %zu table entries, %u live; Dylan's controller at %.2f %.2f %.2f; nearest transforms:", (Now() - t0) * 1000.0,
			n, live, want[0], want[1], want[2]);
		for (size_t i = 0; i < c.size() && i < 5; ++i) {
			float f[8];
			memcpy(f, c[i].raw, 32);
			Log("    entity %u: %.3f m | rot %.3f %.3f %.3f %.3f | pos %.3f %.3f %.3f", c[i].idx, c[i].d, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
		}
		// Test 15: several entities share Dylan's exact transform (his attached children); teleporting one
		// of those moved nothing. Pick the one that owns the character controller (then Player).
		const Cand* pick = nullptr;
		int         pickScore = -1;
		for (const Cand& x : c) {
			if (x.d >= 0.5) break;
			uint16_t arch;
			uint32_t row, cols = 0;
			if (!Entry(world, x.idx, arch, row)) continue;
			Read(world + (uintptr_t(arch) + 0xc22) * 32 + 0x24, cols);
			bool cc = FindCol(world, arch, kCharacterControllerHash) >= 0, pl = FindCol(world, arch, kPlayerHash) >= 0,
			     av = FindCol(world, arch, kActiveAvatarHash) >= 0;
			Log("    candidate %u: %u components%s%s%s", x.idx, cols, cc ? ", CharacterController" : "", pl ? ", Player" : "", av ? ", ActiveAvatar" : "");
			int score = (cc ? 4 : 0) + (pl ? 2 : 0) + (av ? 1 : 0);
			if (score > pickScore) pickScore = score, pick = &x;
		}
		if (pick && pickScore > 0) {
			double p[3];
			XfPos(pick->raw, p);
			AcquireSRWLockExclusive(&g_lock);
			for (int i = 0; i < 3; ++i) g_posOff[i] = p[i] - want[i];
			g_entity = pick->idx;
			g_entityFor = hit;
			g_offWorld = 0;
			ReleaseSRWLockExclusive(&g_lock);
			Log("puppet: Dylan = entity %u (transform %+.3f %+.3f %+.3f from the controller copy)", pick->idx, g_posOff[0], g_posOff[1], g_posOff[2]);
		} else if (pick) {
			Log("puppet: entities at Dylan's position, but none with CharacterController / Player / ActiveAvatar: not choosing");
		}
	}

	// applyControllerToTransform's run thunk (pointer arguments only; [rcx] = ECS world). Never scans.
	inline uintptr_t ThunkDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
	{
		g_calls.fetch_add(1, std::memory_order_relaxed);
		uintptr_t r = g_applyOrig(a, b, c, d, e, f);
		uintptr_t world = 0, hit = player::g_obj.load();
		if (!Read(a, world) || !world) return r;
		g_world = world;
		int64_t ent = g_entity.load();
		if (ent < 0 || !hit || g_entityFor != hit) return r;
		uintptr_t t = ComponentPtr(world, static_cast<uint32_t>(ent), kTransformHash, 32);
		uint8_t   xf[32];
		if (!t || !CopyXf(t, xf)) return r;
		double cur[3], ctl[3];
		XfPos(xf, cur);
		for (int i = 0; i < 3; ++i) cur[i] -= g_posOff[i];  // to controller (feet) space
		// Still Dylan? Outside puppet mode the transform follows the controller; if it hasn't for a
		// second, the entity was recycled: drop it and let the worker scan again.
		if (!g_on.load() && player::Pos(hit, ctl)) {
			if (wwise::Dist(cur, ctl) > 3.0) {
				if (g_offWorld.fetch_add(1) == 30) {
					Log("puppet: entity %lld no longer follows Dylan's controller, searching again", static_cast<long long>(ent));
					g_entity = -1;
					g_entityFor = 0;
				}
			} else {
				g_offWorld = 0;
			}
		}
		if (!(g_on.load() || g_tpPending)) return r;
		double target[3];
		AcquireSRWLockExclusive(&g_lock);
		bool write = TargetLocked(cur, target);
		ReleaseSRWLockExclusive(&g_lock);
		if (!write) return r;
		double at[3] = { target[0] + g_posOff[0], target[1] + g_posOff[1], target[2] + g_posOff[2] };
		XfSetPos(xf, at);  // position replaced, rotation kept
		bool ok = CallSetWorld(world, static_cast<uint32_t>(ent), xf) && MarkTeleported(world, static_cast<uint32_t>(ent));
		if (ok) {
			memcpy(g_lastTarget, target, 24);
			g_writes.fetch_add(1, std::memory_order_relaxed);
		} else if (g_failed.fetch_add(1) < 3) {
			Log("puppet: teleport failed for entity %lld", static_cast<long long>(ent));
		}
		return r;
	}

	// ---- hotkeys (worker tick) -------------------------------------------------------------------
	inline bool g_keyDown[3] = {};

	inline void Tick()
	{
		// Dylan's entity unknown (or a rescan asked for): scan here, on the probe's thread, at most every 10 s.
		uintptr_t hit = player::g_obj.load();
		if (hit && g_world.load() && (g_entityFor != hit || g_rescan.load()) && Now() >= g_nextScan) {
			g_nextScan = Now() + 10.0;
			g_rescan = false;
			Scan();
		}
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid != GetCurrentProcessId()) return;
		const int keys[3] = { VK_INSERT, VK_HOME, VK_PRIOR };
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
		Log("puppet: %s; system runs %llu, Dylan's entity %lld, teleports %llu, failed %llu, refused (Jesse fell) %llu; last target %.2f %.2f %.2f",
			g_on.load() ? "ON" : "off", static_cast<unsigned long long>(g_calls.load()), static_cast<long long>(g_entity.load()),
			static_cast<unsigned long long>(g_writes.load()), static_cast<unsigned long long>(g_failed.load()),
			static_cast<unsigned long long>(g_refused.load()), g_lastTarget[0], g_lastTarget[1], g_lastTarget[2]);
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
		} else if (!_stricmp(c, "rescan")) {
			g_rescan = true;
			g_nextScan = 0;
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

	inline void Install(uintptr_t a_exe, void* a_thunk)
	{
		g_setWorld = reinterpret_cast<SetWorldFn>(a_exe + kSetWorldRva);
		Hook(a_thunk, reinterpret_cast<void*>(&ThunkDetour), reinterpret_cast<void**>(&g_applyOrig), "applyControllerToTransform (puppet teleport)");
	}
}
