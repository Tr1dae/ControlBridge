// Control Bridge Phase 0 probe for CONTROL Resonant, loaded by the f2g crloader from
// crmods\ControlBridgeProbe\ (no game file is replaced).
//
// Observes only. Resonant is one monolithic exe without game exports, but it exports Wwise, so:
//   AK::SoundEngine (common/wwise.h): frame tick, listener (camera) and emitters (Dylan's
//     footsteps/voice emitters move with him), found by decorated export name
//   m::PerspectiveView (RTTI): 'views' finds live camera views and where the listener's position
//     sits inside them; 'view <addr> [matOff] [fovOff]' streams that view into the CSV
//   physics::CharacterController (player.h): Dylan's controller position, found by RTTI + heap scan
//   generic RTTI/memory commands for the next step (the player's transform in the ECS)
// Commands go to logs\resonant_cmd.txt (see README).
#include "../common/bridge.h"
#include "../common/probe.h"
#include "../common/wwise.h"
#include "player.h"
#include "puppet.h"

using namespace cb;

namespace
{
	// Phase 1 (passive): Dylan's controller position, the tracked camera view and the real controller
	// go to the link every frame.
	void Publish(const Row& a_r)
	{
		static double s_last = 0;
		proto::HostState h{};
		h.frame = a_r.frame;
		h.dt = s_last ? static_cast<float>(a_r.t - s_last) : 0.0f;
		s_last = a_r.t;
		double p[3];
		if (player::Pos(player::g_obj.load(), p)) {
			const double ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };  // controller has no facing; Phase 1 adds it
			bridge::SetXform(h.player, p, ident);
			h.flags |= proto::kHostInGame;
		}
		if (a_r.flags & kRowCam) {
			bridge::SetXform(h.camera.xf, a_r.cam + 9, a_r.cam);
			h.camera.fovY = a_r.fov;
		}
		bridge::PublishHost(h, a_r.pad);
	}

	DWORD WINAPI Worker(void* a_self)
	{
		Init(static_cast<HMODULE>(a_self), L"resonant");
		HMODULE exe = GetModuleHandleW(nullptr);
		wwise::Install(exe);
		wwise::views::g_mod = exe;
		wwise::views::g_matOff = 0x68;  // test 2: Resonant's m::View keeps the camera position at +0xB0, 8 bytes earlier than Control's

		// ECS systems from tools/ecs_systems.py, observed pass-through (call rate, ECS world in rcx).
		// Their RVAs belong to one build: only hooked when the exe's PE timestamp matches.
		struct Sys
		{
			uint32_t    rva;
			const char* name;
		};
		static const Sys kSys25767184[] = {
			{ 0x1bc6500, "applyControllerToTransform" },
			{ 0x22bb5e0, "player_locomotion::update_locomotion_state" },
			{ 0x2825130, "cameramixer::camera_update" },
		};
		if (Nt(exe)->FileHeader.TimeDateStamp == 0x6ac4318a) {
			puppet::Install(reinterpret_cast<void*>(g_exe + kSys25767184[0].rva));  // applyControllerToTransform drives Dylan
			for (int i = 1; i < 3; ++i) {
				void* t = reinterpret_cast<void*>(g_exe + kSys25767184[i].rva);
				ObsSlot& o = g_obs[i];
				strncpy_s(o.name, kSys25767184[i].name, _TRUNCATE);
				if (Hook(t, kObsDetours[i], reinterpret_cast<void**>(&g_obsOrig[i]), o.name)) o.target = t;
			}
		} else {
			Log("exe build differs from the ECS map (PE timestamp %08lx): system observers and puppet mode off", Nt(exe)->FileHeader.TimeDateStamp);
		}
		player::Init(exe);
		bridge::Open(true);
		g_game.publish = Publish;
		g_game.fill = player::Fill;
		g_game.summary = [] {
			player::Summary();
			bridge::Summary();
			puppet::Summary();
		};
		g_game.workerTick = [] {
			wwise::autorecon::Tick();
			player::Tick();
			puppet::Tick();
		};
		g_game.command = puppet::Command;
		Run();
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE a_self, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_self);
		if (!ExeNameIs({ L"CONTROLResonant.exe" })) return TRUE;
		CreateThread(nullptr, 0, Worker, a_self, 0, nullptr);  // crloader calls us on its own thread; setup runs on ours
	}
	return TRUE;
}
