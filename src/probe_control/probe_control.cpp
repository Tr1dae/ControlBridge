// Control Bridge Phase 0 probe for Control (2019), loaded as a winmm.dll proxy next to
// Control_DX12.exe / Control_DX11.exe (navigation_rmdwin*_f.dll imports winmm statically).
//
// Observes only. Everything is found by exported C++ name, so it doesn't depend on one build:
//   coregame  ClientPlayerController::getAvatarObject / ServerPlayerController::getAvatarObject
//             -> Jesse's EntityState; right after the game gets it, the probe calls the exported
//                EntityState::getTransform on the same thread (Matrix4x3<float>, raw into p0..p15)
//   coreshared g_pRenderView (const m::View*) -> the rendered camera: view-to-world at +0x70,
//                view-to-clip at +0xD0 (test 1 showed CameraComponentState is not the gameplay camera)
//   snd       AK::SoundEngine (see common/wwise.h): frame tick, listener, emitters
// Commands (logs\control_cmd.txt) in addition to the common ones: avatar | camera
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

	// ---- camera: coreshared::g_pRenderView (const m::View*) ----------------------------------------
	// m::View: view-to-world Matrix4x3<double> at +0x70 (rows right, up, forward, position: the
	// exported getViewDirection = +0xA0, getViewPoint = +0xB8), view-to-clip Matrix4<double> at +0xD0.
	const uintptr_t* g_renderView = nullptr;  // address of the global

	bool ReadCamera(double* a_m12, float& a_fovY)
	{
		uintptr_t v = 0;
		double    proj[16];
		if (!g_renderView || !Read(reinterpret_cast<uintptr_t>(g_renderView), v) || !SafeRead(a_m12, v + 0x70, 96) ||
			!SafeRead(proj, v + 0xD0, sizeof(proj)) || proj[5] == 0.0)
			return false;
		a_fovY = static_cast<float>(2.0 * std::atan(1.0 / std::fabs(proj[5])));
		return true;
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
		if (ReadCamera(a_r.cam, a_r.fov)) a_r.flags |= kRowCam;
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

	void LogCamera()
	{
		double m[12];
		float  fov = 0;
		uintptr_t v = 0;
		if (g_renderView) Read(reinterpret_cast<uintptr_t>(g_renderView), v);
		if (!ReadCamera(m, fov)) {
			Log("camera: g_pRenderView %p -> %p, unreadable", static_cast<const void*>(g_renderView), reinterpret_cast<void*>(v));
			return;
		}
		Log("camera (g_pRenderView -> %p): pos %.3f %.3f %.3f  fwd %.3f %.3f %.3f  up %.3f %.3f %.3f  fovY %.1f deg", reinterpret_cast<void*>(v),
			m[9], m[10], m[11], m[6], m[7], m[8], m[3], m[4], m[5], fov * 57.29578f);
	}

	void Summary()
	{
		LogAvatars();
		LogCamera();
	}

	bool Command(int argc, char** argv)
	{
		if (!_stricmp(argv[0], "avatar")) {
			LogAvatars();
		} else if (!_stricmp(argv[0], "camera")) {
			LogCamera();
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
		HMODULE shared = FirstModule({ L"coreshared_rmdwin10_f.dll", L"coreshared_rmdwin7_f.dll" });
		g_renderView = static_cast<const uintptr_t*>(Export(shared, "?g_pRenderView@coreshared@@3PEBVView@m@@EB"));
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
