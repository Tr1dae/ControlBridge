// Offline test of Control's virtual pad (src/probe_control/vpad.h) against the real system
// xinput1_4.dll and a private link mapping: served while the host is alive with an active pad,
// passed through otherwise; fake focus follows. Exit code 0 = passed.
#include "../src/probe_control/vpad.h"

#include <cstdio>
#include <string>

using namespace cb;

static int g_fail = 0;
#define CHECK(c)                                                    \
	do {                                                            \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
			++g_fail;                                               \
		}                                                           \
	} while (0)

int main()
{
	Init(GetModuleHandleW(nullptr), L"vpadtest");
	std::wstring name = L"Local\\ControlBridge_vpadtest_" + std::to_wstring(GetCurrentProcessId());
	bridge::Open(false, name.c_str());
	CHECK(bridge::g_ok);

	wchar_t sys[MAX_PATH];
	GetSystemDirectoryW(sys, MAX_PATH);
	HMODULE x = LoadLibraryW((std::wstring(sys) + L"\\xinput1_4.dll").c_str());
	auto    get = reinterpret_cast<vpad::GetStateFn>(GetProcAddress(x, "XInputGetState"));
	CHECK(x && get);

	vpad::g_enabled = vpad::g_fakeFocus = true;
	vpad::Tick();
	CHECK(vpad::g_hooked[0] && vpad::g_orig[0]);
	CHECK(g_xget.load() == reinterpret_cast<XGetFn>(vpad::g_orig[0]));  // the sampler bypasses the hook

	vpad::XStateRaw st{};
	DWORD           real = vpad::g_orig[0](0, &st);  // whatever is (or isn't) plugged in
	CHECK(!vpad::ForceFocus());

	// Host alive with an active pad: index 0 is the host's pad, through the game's own entry point.
	proto::PadState p{ proto::kPadActive, 4242, 0x1000 | 0x0100, 10, 250, -32768, 32767, 1234, -1234 };
	link::Write(bridge::g_map.shm->pad, p);
	bridge::g_map.shm->header.hostHeartbeatMs = GetTickCount64();
	memset(&st, 0, sizeof(st));
	CHECK(get(0, &st) == ERROR_SUCCESS);
	CHECK(st.packet == 4242 && st.pad.buttons == (0x1000 | 0x0100) && st.pad.lt == 10 && st.pad.rt == 250);
	CHECK(st.pad.lx == -32768 && st.pad.ly == 32767 && st.pad.rx == 1234 && st.pad.ry == -1234);
	CHECK(vpad::ForceFocus());
	CHECK(get(1, &st) == vpad::g_orig[0](1, &st));  // other indices pass through

	// Pad inactive (Resonant not in front): pass through, no fake focus.
	p.flags = 0;
	link::Write(bridge::g_map.shm->pad, p);
	CHECK(get(0, &st) == real);
	CHECK(!vpad::ForceFocus());

	// Host gone (heartbeat old): pass through even with an active pad left in the slot.
	p.flags = proto::kPadActive;
	link::Write(bridge::g_map.shm->pad, p);
	bridge::g_map.shm->header.hostHeartbeatMs = GetTickCount64() - 5000;
	CHECK(get(0, &st) == real);
	CHECK(!vpad::ForceFocus());

	vpad::g_enabled = false;  // switched off: pass through
	bridge::g_map.shm->header.hostHeartbeatMs = GetTickCount64();
	CHECK(get(0, &st) == real);

	MH_DisableHook(MH_ALL_HOOKS);
	printf("real pad on index 0: %s\n", real == ERROR_SUCCESS ? "connected" : "none");
	printf(g_fail ? "VPAD TEST FAILED (%d)\n" : "VPAD TEST OK\n", g_fail);
	return g_fail ? 1 : 0;
}
