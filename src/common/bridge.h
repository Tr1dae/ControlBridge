// Phase 1 link glue for the probes: opens the shared mapping and publishes this game's side once per
// frame (from cb::Tick). Still passive: nothing read from the other side changes either game yet.
//   host  (Resonant): HostState (player, camera) + PadState (the real controller, read while Resonant
//                     is in front)
//   guest (Control):  GuestState (Jesse, camera)
#pragma once

#include "link.h"
#include "probe.h"

namespace cb::bridge
{
	inline link::Mapping g_map;
	inline bool          g_ok = false, g_host = false;
	inline uint64_t      g_published = 0, g_skipped = 0;
	inline std::atomic_flag g_busy = ATOMIC_FLAG_INIT;  // one publisher at a time (frame thread can change)

	inline void Open(bool a_host, const wchar_t* a_name = proto::kMappingName)
	{
		g_host = a_host;
		g_ok = g_map.Open(a_name);
		if (!g_ok) {
			Log("link: cannot open %ls (another build of the other side running?)", a_name);
			return;
		}
		(a_host ? g_map.shm->header.hostPid : g_map.shm->header.guestPid) = GetCurrentProcessId();
		Log("link: %ls %s as %s (%.1f MB)", a_name, g_map.created ? "created" : "opened", a_host ? "host" : "guest",
			sizeof(proto::Shared) / 1048576.0);
	}

	// Rows (right, up, forward) as doubles -> Xform rotation floats.
	inline void SetXform(proto::Xform& a_x, const double* a_pos, const double* a_rows9)
	{
		memcpy(a_x.pos, a_pos, 24);
		for (int i = 0; i < 9; ++i) a_x.rot[i] = static_cast<float>(a_rows9[i]);
	}

	inline void PublishHost(const proto::HostState& a_s, const PadSnap& a_pad)
	{
		if (!g_ok) return;
		if (g_busy.test_and_set(std::memory_order_acquire)) {
			++g_skipped;
			return;
		}
		link::Write(g_map.shm->host, a_s);
		proto::PadState p{};
		if (a_pad.index >= 0 && a_pad.fg) {
			p = { proto::kPadActive, a_pad.packet, a_pad.buttons, a_pad.lt, a_pad.rt, a_pad.lx, a_pad.ly, a_pad.rx, a_pad.ry };
		}
		link::Write(g_map.shm->pad, p);
		g_map.shm->header.hostHeartbeatMs = GetTickCount64();
		++g_published;
		g_busy.clear(std::memory_order_release);
	}

	inline void PublishGuest(const proto::GuestState& a_s)
	{
		if (!g_ok) return;
		if (!link::TryWrite(g_map.shm->guest, a_s, g_busy)) {
			++g_skipped;
			return;
		}
		g_map.shm->header.guestHeartbeatMs = GetTickCount64();
		++g_published;
	}

	inline void Summary()
	{
		if (!g_ok) return;
		bool other = link::Alive(g_host ? g_map.shm->header.guestHeartbeatMs : g_map.shm->header.hostHeartbeatMs);
		if (g_host) {
			proto::GuestState gs{};
			bool              ok = link::Read(g_map.shm->guest, gs);
			Log("link: %llu published (%llu skipped); guest %s%s jesse %.2f %.2f %.2f", static_cast<unsigned long long>(g_published), static_cast<unsigned long long>(g_skipped), other ? "alive" : "absent",
				ok ? "," : " (no state)", gs.jesse.pos[0], gs.jesse.pos[1], gs.jesse.pos[2]);
		} else {
			proto::HostState hs{};
			bool             ok = link::Read(g_map.shm->host, hs);
			Log("link: %llu published (%llu skipped); host %s%s player %.2f %.2f %.2f", static_cast<unsigned long long>(g_published), static_cast<unsigned long long>(g_skipped), other ? "alive" : "absent",
				ok ? "," : " (no state)", hs.player.pos[0], hs.player.pos[1], hs.player.pos[2]);
		}
	}
}
