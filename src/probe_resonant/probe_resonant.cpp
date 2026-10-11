// Control Bridge Phase 0 probe for CONTROL Resonant, loaded by the f2g crloader from
// crmods\ControlBridgeProbe\ (no game file is replaced).
//
// Observes only. Resonant is one monolithic exe without game exports, but it exports Wwise, so:
//   AK::SoundEngine (common/wwise.h): frame tick, listener (camera) and emitters (Dylan's
//     footsteps/voice emitters move with him), found by decorated export name
//   m::PerspectiveView (RTTI): 'views' finds live camera views and where the listener's position
//     sits inside them; 'view <addr> [matOff] [fovOff]' streams that view into the CSV
//   generic RTTI/memory commands for the next step (the player's transform in the ECS)
// Commands go to logs\resonant_cmd.txt (see README).
#include "../common/probe.h"
#include "../common/wwise.h"

using namespace cb;

namespace
{
	DWORD WINAPI Worker(void* a_self)
	{
		Init(static_cast<HMODULE>(a_self), L"resonant");
		HMODULE exe = GetModuleHandleW(nullptr);
		wwise::Install(exe);
		wwise::views::g_mod = exe;
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
