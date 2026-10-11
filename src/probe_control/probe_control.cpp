// Control Bridge Phase 0 probe for Control (2019), loaded as a winmm.dll proxy next to
// Control_DX12.exe / Control_DX11.exe (navigation_rmdwin*_f.dll imports winmm statically).
//
// Observes only. Everything is found by exported C++ name, so it doesn't depend on one build:
//   coregame  ClientPlayerController::getAvatarObject / ServerPlayerController::getAvatarObject
//             -> Jesse's EntityState; right after the game gets it, the probe calls the exported
//                EntityState::getTransform on the same thread (Matrix4x3<float>, raw into p0..p15)
//             CameraComponentState::updateViewFromTransform -> every live camera; after the original
//                runs: PerspectiveView at +0xC0, so view-to-world (Matrix4x3<double>) at +0x130
//                and FOV at +0x3A0 (from View::getViewToWorld and PerspectiveView::getFov)
//   snd       AK::SoundEngine (see common/wwise.h): frame tick, listener, emitters
// Commands (logs\control_cmd.txt) in addition to the common ones: avatar | cams | cam <i>|auto
#include "fwd_winmm.h"

#include "../common/probe.h"
#include "../common/wwise.h"

using namespace cb;

namespace
{
	// ---- Jesse ---------------------------------------------------------------------------------
	using GetAvatarFn = void* (*)(void*);
	using GetTransformFn = void* (*)(const void*, void*);  // virtual, hidden return pointer in rdx

	struct Avatar
	{
		void*    ctrl;
		void*    ent;
		uint64_t calls, callsMark, xfCalls;
		uint32_t tid;
		double   t;  // last transform
		float    m[16];
		bool     ok;
	};

	const char*    kAvatarName[2] = { "client", "server" };
	GetAvatarFn    g_getAvatarOrig[2] = {};
	GetTransformFn g_getTransform = nullptr;
	Avatar         g_av[2] = {};
	SRWLOCK        g_avLock = SRWLOCK_INIT;
	thread_local bool t_inAvatar = false;

	bool CallGetTransform(const void* a_ent, float* a_out)
	{
		__try {
			g_getTransform(a_ent, a_out);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void OnAvatar(int a_k, void* a_ctrl, void* a_ent)
	{
		if (!a_ent || t_inAvatar) return;
		double now = Now();
		bool   want;
		AcquireSRWLockExclusive(&g_avLock);
		Avatar& a = g_av[a_k];
		++a.calls;
		a.ctrl = a_ctrl;
		a.tid = GetCurrentThreadId();
		want = g_getTransform && (a_ent != a.ent || now - a.t > 0.002);  // at most ~500 transforms/s
		a.ent = a_ent;
		ReleaseSRWLockExclusive(&g_avLock);
		if (!want) return;
		alignas(16) float m[32] = {};  // generous: Matrix4x3<float> is 48 or 64 bytes
		t_inAvatar = true;
		bool ok = CallGetTransform(a_ent, m);
		t_inAvatar = false;
		AcquireSRWLockExclusive(&g_avLock);
		if (ok) memcpy(a.m, m, sizeof(a.m));
		a.ok = ok;
		a.t = now;
		++a.xfCalls;
		ReleaseSRWLockExclusive(&g_avLock);
	}

	template <int K>
	void* GetAvatarDetour(void* a_ctrl)
	{
		void* ent = g_getAvatarOrig[K](a_ctrl);
		OnAvatar(K, a_ctrl, ent);
		return ent;
	}

	// ---- cameras -------------------------------------------------------------------------------
	using UpdateViewFn = void (*)(void*);
	UpdateViewFn g_updateViewOrig = nullptr;
	constexpr uint32_t kCamMat = 0xC0 + 0x70, kCamFov = 0xC0 + 0x2E0;

	struct Cam
	{
		void*    p;
		uint64_t n, nMark;
		double   t;
		double   m[12];
		float    fov;
	};
	constexpr int kCams = 32;
	Cam     g_cams[kCams] = {};
	int     g_camSel = -1;  // -1: auto (most updated camera seen in the last 0.25 s)
	SRWLOCK g_camLock = SRWLOCK_INIT;

	void UpdateViewDetour(void* a_cam)
	{
		g_updateViewOrig(a_cam);
		Cam c{};
		auto b = static_cast<const uint8_t*>(a_cam);
		if (!SafeCopy(c.m, b + kCamMat, sizeof(c.m)) || !SafeCopy(&c.fov, b + kCamFov, 4)) return;
		c.t = Now();
		AcquireSRWLockExclusive(&g_camLock);
		int slot = -1, oldest = 0;
		for (int i = 0; i < kCams; ++i) {
			if (g_cams[i].p == a_cam) {
				slot = i;
				break;
			}
			if (g_cams[i].t < g_cams[oldest].t) oldest = i;
		}
		if (slot < 0) {
			slot = oldest;
			g_cams[slot] = {};
			g_cams[slot].p = a_cam;
		}
		Cam& d = g_cams[slot];
		memcpy(d.m, c.m, sizeof(c.m));
		d.fov = c.fov;
		d.t = c.t;
		++d.n;
		ReleaseSRWLockExclusive(&g_camLock);
	}

	int SelectedCam(double a_now)  // caller holds g_camLock
	{
		if (g_camSel >= 0) return g_cams[g_camSel].p ? g_camSel : -1;
		int best = -1;
		for (int i = 0; i < kCams; ++i) {
			if (g_cams[i].p && a_now - g_cams[i].t < 0.25 && (best < 0 || g_cams[i].n > g_cams[best].n)) best = i;
		}
		return best;
	}

	// ---- focus / pause (Phase 1: Control must keep playing while Resonant is in front) ----------
	// GameWindow::hasFocus = byte +0x7C, isMinimized = +0x7D (both imported by the exe);
	// InputManager::setFocus(bool) stores +0x39; GameHelper::setGamePaused(bool) is static.
	// Detours log every change of value; they never alter one.
	using GetBoolFn = bool (*)(const void*);
	using SetBoolFn = void (*)(void*, bool);
	using SetPausedFn = void (*)(bool);
	GetBoolFn   g_hasFocusOrig = nullptr, g_isMinOrig = nullptr;
	SetBoolFn   g_inputFocusOrig = nullptr;
	SetPausedFn g_setPausedOrig = nullptr;
	std::atomic<int> g_lastHasFocus{ -1 }, g_lastIsMin{ -1 }, g_lastInputFocus{ -1 };

	void NoteChange(std::atomic<int>& a_last, bool a_v, const char* a_what, const void* a_obj)
	{
		int prev = a_last.exchange(a_v ? 1 : 0);
		if (prev != (a_v ? 1 : 0)) Log("focus: %s %d -> %d (obj %p, thread %lu)", a_what, prev, a_v ? 1 : 0, a_obj, GetCurrentThreadId());
	}

	bool HasFocusDetour(const void* a_win)
	{
		bool v = g_hasFocusOrig(a_win);
		NoteChange(g_lastHasFocus, v, "GameWindow::hasFocus", a_win);
		return v;
	}

	bool IsMinimizedDetour(const void* a_win)
	{
		bool v = g_isMinOrig(a_win);
		NoteChange(g_lastIsMin, v, "GameWindow::isMinimized", a_win);
		return v;
	}

	void InputFocusDetour(void* a_mgr, bool a_v)
	{
		NoteChange(g_lastInputFocus, a_v, "InputManager::setFocus", a_mgr);
		g_inputFocusOrig(a_mgr, a_v);
	}

	void SetPausedDetour(bool a_v)
	{
		Log("pause: GameHelper::setGamePaused(%d) (thread %lu)", a_v, GetCurrentThreadId());
		g_setPausedOrig(a_v);
	}

	// ---- probe glue ------------------------------------------------------------------------------
	void Fill(Row& a_r)
	{
		AcquireSRWLockShared(&g_avLock);
		const Avatar& a = (g_av[0].ok && a_r.t - g_av[0].t < 0.5) ? g_av[0] : g_av[1];
		if (a.ok && a_r.t - a.t < 0.5) {
			memcpy(a_r.player, a.m, sizeof(a_r.player));
			a_r.flags |= kRowPlayer;
		}
		ReleaseSRWLockShared(&g_avLock);
		AcquireSRWLockShared(&g_camLock);
		int s = SelectedCam(a_r.t);
		if (s >= 0) {
			memcpy(a_r.cam, g_cams[s].m, sizeof(a_r.cam));
			a_r.fov = g_cams[s].fov;
			a_r.flags |= kRowCam;
		}
		ReleaseSRWLockShared(&g_camLock);
	}

	void LogAvatars()
	{
		AcquireSRWLockShared(&g_avLock);
		for (int k = 0; k < 2; ++k) {
			const Avatar& a = g_av[k];
			const float*  m = a.m;
			Log("avatar %s: ctrl %p ent %p ('%s') calls %llu transforms %llu thread %u ok %d", kAvatarName[k], a.ctrl, a.ent,
				a.ent ? ClassOf(reinterpret_cast<uintptr_t>(a.ent)).c_str() : "", static_cast<unsigned long long>(a.calls),
				static_cast<unsigned long long>(a.xfCalls), a.tid, a.ok);
			if (a.ok) {
				Log("  m: [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]", m[0], m[1], m[2], m[3], m[4],
					m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
			}
		}
		ReleaseSRWLockShared(&g_avLock);
	}

	void LogCams()
	{
		double now = Now();
		AcquireSRWLockExclusive(&g_camLock);
		int sel = SelectedCam(now);
		for (int i = 0; i < kCams; ++i) {
			Cam& c = g_cams[i];
			if (!c.p) continue;
			Log("  cam %2d%s %p ('%s') %llu updates (+%llu), %.2f s ago, fov %.4g, pos? [%.3f %.3f %.3f] / [%.3f %.3f %.3f]", i, i == sel ? "*" : " ",
				c.p, ClassOf(reinterpret_cast<uintptr_t>(c.p)).c_str(), static_cast<unsigned long long>(c.n),
				static_cast<unsigned long long>(c.n - c.nMark), now - c.t, c.fov, c.m[9], c.m[10], c.m[11], c.m[3], c.m[7], c.m[11]);
			c.nMark = c.n;
		}
		ReleaseSRWLockExclusive(&g_camLock);
	}

	void Summary()
	{
		LogAvatars();
		LogCams();
	}

	bool Command(int argc, char** argv)
	{
		if (!_stricmp(argv[0], "avatar")) {
			LogAvatars();
		} else if (!_stricmp(argv[0], "cams")) {
			LogCams();
		} else if (!_stricmp(argv[0], "cam") && argc > 1) {
			AcquireSRWLockExclusive(&g_camLock);
			g_camSel = !_stricmp(argv[1], "auto") ? -1 : std::clamp(atoi(argv[1]), 0, kCams - 1);
			ReleaseSRWLockExclusive(&g_camLock);
			Log("camera selection: %s", argv[1]);
		} else {
			return false;
		}
		return true;
	}

	void* Export(HMODULE a_mod, const char* a_name)
	{
		void* p = a_mod ? reinterpret_cast<void*>(GetProcAddress(a_mod, a_name)) : nullptr;
		if (!p) Log("export not found: %s", a_name);
		return p;
	}

	DWORD WINAPI Worker(void* a_self)
	{
		Init(static_cast<HMODULE>(a_self), L"control");
		// DX12 build uses the *_rmdwin10_f.dll set, DX11 the *_rmdwin7_f.dll set.
		HMODULE core = nullptr, snd = nullptr, rl = nullptr;
		for (int i = 0; i < 600 && !(core && snd && rl); ++i) {
			core = FirstModule({ L"coregame_rmdwin10_f.dll", L"coregame_rmdwin7_f.dll" });
			snd = FirstModule({ L"snd_rmdwin10_f.dll", L"snd_rmdwin7_f.dll" });
			rl = FirstModule({ L"rl_rmdwin10_f.dll", L"rl_rmdwin7_f.dll" });
			if (!(core && snd && rl)) Sleep(100);
		}
		Log("modules: coregame %s, snd %s, rl %s", Rva(reinterpret_cast<uintptr_t>(core)).c_str(), Rva(reinterpret_cast<uintptr_t>(snd)).c_str(),
			Rva(reinterpret_cast<uintptr_t>(rl)).c_str());

		// CallGetTransform=0 in controlbridge.ini keeps the avatar hook purely observing (no call into
		// the game), in case calling getTransform from inside getAvatarObject ever stalls a frame.
		if (IniInt(L"CallGetTransform", 1)) {
			g_getTransform = reinterpret_cast<GetTransformFn>(Export(core, "?getTransform@EntityState@coregame@@UEBA?AV?$Matrix4x3Template@M@m@@XZ"));
		} else {
			Log("CallGetTransform=0: Jesse's transform is not sampled");
		}
		Hook(Export(core, "?getAvatarObject@ClientPlayerController@coregame@@QEAAPEAVEntityState@2@XZ"), reinterpret_cast<void*>(&GetAvatarDetour<0>),
			reinterpret_cast<void**>(&g_getAvatarOrig[0]), "ClientPlayerController::getAvatarObject");
		Hook(Export(core, "?getAvatarObject@ServerPlayerController@coregame@@QEAAPEAVEntityState@2@XZ"), reinterpret_cast<void*>(&GetAvatarDetour<1>),
			reinterpret_cast<void**>(&g_getAvatarOrig[1]), "ServerPlayerController::getAvatarObject");
		Hook(Export(core, "?updateViewFromTransform@CameraComponentState@coregame@@AEAAXXZ"), reinterpret_cast<void*>(&UpdateViewDetour),
			reinterpret_cast<void**>(&g_updateViewOrig), "CameraComponentState::updateViewFromTransform");
		Hook(Export(core, "?hasFocus@GameWindow@coregame@@QEBA_NXZ"), reinterpret_cast<void*>(&HasFocusDetour),
			reinterpret_cast<void**>(&g_hasFocusOrig), "GameWindow::hasFocus");
		Hook(Export(core, "?isMinimized@GameWindow@coregame@@QEBA_NXZ"), reinterpret_cast<void*>(&IsMinimizedDetour),
			reinterpret_cast<void**>(&g_isMinOrig), "GameWindow::isMinimized");
		Hook(Export(core, "?setGamePaused@GameHelper@coregame@@SAX_N@Z"), reinterpret_cast<void*>(&SetPausedDetour),
			reinterpret_cast<void**>(&g_setPausedOrig), "GameHelper::setGamePaused");
		HMODULE input = FirstModule({ L"input_rmdwin10_f.dll", L"input_rmdwin7_f.dll" });
		Hook(Export(input, "?setFocus@InputManager@input@@QEAAX_N@Z"), reinterpret_cast<void*>(&InputFocusDetour),
			reinterpret_cast<void**>(&g_inputFocusOrig), "InputManager::setFocus");
		if (snd) wwise::Install(snd);
		wwise::views::g_mod = rl;  // m::PerspectiveView lives in rl_*.dll

		g_game.fill = Fill;
		g_game.summary = Summary;
		g_game.command = Command;
		Run();
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE a_self, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_self);
		// The proxy sits in the game folder; anything else that loads it just gets plain winmm.
		if (!ExeNameIs({ L"Control_DX12.exe", L"Control_DX11.exe" })) return TRUE;
		CreateThread(nullptr, 0, Worker, a_self, 0, nullptr);  // all setup happens there, outside the loader lock
	}
	return TRUE;
}
