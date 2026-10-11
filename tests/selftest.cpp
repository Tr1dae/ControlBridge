// Offline self-test of the probe core (no game needed). Exercises: runtime RTTI lookup and heap
// scan, MinHook + the Wwise 6-argument pass-through detours on stand-in AK functions, transform
// decoding for all four layouts, the 'views' listener search, the command file and the CSV.
// Build: build.bat (bin\selftest.exe). Exit code 0 = all passed.
#include "../src/common/probe.h"
#include "../src/common/wwise.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace cb;

namespace m
{
	// Stand-in for Northlight's m::PerspectiveView: same RTTI name, Control 1 offsets.
	struct PerspectiveView
	{
		virtual ~PerspectiveView() = default;
		virtual float Fov() const { return fov; }
		uint8_t pad[0x68];      // vptr(8) + 0x68 = 0x70
		double  viewToWorld[12];
		uint8_t pad2[0x2E0 - 0x70 - 96];
		float   fov = 1.0f;
	};
	static_assert(offsetof(PerspectiveView, viewToWorld) == 0x70);
	static_assert(offsetof(PerspectiveView, fov) == 0x2E0);
}

namespace
{
	int g_fail = 0;

#define CHECK(c)                                                          \
	do {                                                                  \
		if (!(c)) {                                                       \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
			++g_fail;                                                     \
		}                                                                 \
	} while (0)

	// Stand-ins with Wwise's shapes. Bodies are big enough for MinHook's 5-byte patch.
	volatile uint64_t g_sink = 0;
	extern "C" __declspec(noinline) uintptr_t FakeSetPosition(uint64_t a_id, const void* a_xf)
	{
		g_sink = g_sink + a_id + reinterpret_cast<uintptr_t>(a_xf);
		return a_id == 0xDEAD ? 31 : 1;  // AK_Success = 1
	}
	extern "C" __declspec(noinline) uintptr_t FakeSetDefaultListeners(const uint64_t* a_ids, uint32_t a_n)
	{
		g_sink = g_sink + a_n + (a_ids ? a_ids[0] : 0);
		return 1;
	}
	extern "C" __declspec(noinline) uintptr_t FakeRenderAudio(bool a_b)
	{
		g_sink = g_sink + (a_b ? 7 : 3);
		return 1;
	}
	// 5 arguments: the fifth travels on the stack and must survive the pass-through.
	extern "C" __declspec(noinline) uintptr_t FakeMulti(uint64_t a_id, const void* a_xf, uint16_t a_n, uint8_t a_type, uint32_t a_flags)
	{
		g_sink = g_sink + a_id + a_n + a_type + reinterpret_cast<uintptr_t>(a_xf);
		return a_flags;
	}

	extern "C" __declspec(noinline) uintptr_t FakeSystem(uintptr_t a_world, uintptr_t a_b)
	{
		g_sink = g_sink + a_world * 3 + a_b;
		return a_world + a_b;
	}

	std::string ReadAll(const std::wstring& a_path)
	{
		FILE* f = _wfsopen(a_path.c_str(), L"rb", _SH_DENYNO);
		if (!f) return {};
		std::string s;
		char        b[4096];
		size_t      n;
		while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
		fclose(f);
		return s;
	}

	void TestDecode()
	{
		wwise::Xf x;
		float     ftp[9] = { 0, 0, 1, 0, 1, 0, 10, 20, 30 };  // front, top, pos
		CHECK(wwise::Decode(reinterpret_cast<uint8_t*>(ftp), false, x) == 0 && x.pos[2] == 30 && x.front[2] == 1 && x.top[1] == 1);
		float pft[9] = { 10, 20, 30, 0, 0, 1, 0, 1, 0 };  // pos, front, top
		CHECK(wwise::Decode(reinterpret_cast<uint8_t*>(pft), false, x) == 1 && x.pos[0] == 10 && x.front[2] == 1);
		uint8_t b[48];
		double  pos[3] = { 1000.5, -2.25, 77.0 };
		float   ft[6] = { 1, 0, 0, 0, 1, 0 };
		memcpy(b, pos, 24);
		memcpy(b + 24, ft, 24);
		CHECK(wwise::Decode(b, true, x) == 2 && x.pos[0] == 1000.5 && x.front[0] == 1);
		memcpy(b, ft, 24);
		memcpy(b + 24, pos, 24);
		CHECK(wwise::Decode(b, true, x) == 3 && x.pos[2] == 77.0 && x.top[1] == 1);
		float junk[9] = { 5, 5, 5, 5, 5, 5, 5, 5, 5 };
		CHECK(wwise::Decode(reinterpret_cast<uint8_t*>(junk), false, x) == -1);
	}
}

int main()
{
	Init(GetModuleHandleW(nullptr), L"selftest");
	printf("log dir: %ls\n", g_dir.c_str());

	TestDecode();

	// ---- RTTI + heap scan ----
	auto* view = new m::PerspectiveView();
	auto  vts = FindVtables(GetModuleHandleW(nullptr), "PerspectiveView@m");
	CHECK(vts.size() == 1);
	CHECK(!vts.empty() && *reinterpret_cast<uintptr_t*>(view) == vts[0]);
	CHECK(ClassOf(reinterpret_cast<uintptr_t>(view)) == "PerspectiveView@m");
	if (!vts.empty()) {
		auto hits = ScanHeap(vts[0], 16);
		CHECK(std::find(hits.begin(), hits.end(), reinterpret_cast<uintptr_t>(view)) != hits.end());
	}
	CHECK(FindVtables(GetModuleHandleW(nullptr), "NoSuchClass@zz").empty());

	// ---- Wwise detours on stand-ins ----
	using namespace wwise;
	g_double = false;
	CHECK(Hook(reinterpret_cast<void*>(&FakeSetPosition), kDetours[kSetPos][0], reinterpret_cast<void**>(&g_orig[kSetPos][0]), "FakeSetPosition"));
	CHECK(Hook(reinterpret_cast<void*>(&FakeSetDefaultListeners), kDetours[kSetDef][0], reinterpret_cast<void**>(&g_orig[kSetDef][0]), "FakeSetDefaultListeners"));
	CHECK(Hook(reinterpret_cast<void*>(&FakeRenderAudio), kDetours[kRender][0], reinterpret_cast<void**>(&g_orig[kRender][0]), "FakeRenderAudio"));
	CHECK(Hook(reinterpret_cast<void*>(&FakeMulti), kDetours[kMulti][0], reinterpret_cast<void**>(&g_orig[kMulti][0]), "FakeMulti"));
	CHECK(!Hook(reinterpret_cast<void*>(&FakeMulti), kDetours[kMulti][1], reinterpret_cast<void**>(&g_orig[kMulti][1]), "FakeMulti alias"));
	g_game.fillCommon = FillCommon;
	g_game.summaryCommon = SummaryCommon;
	g_game.commandCommon = CommandCommon;

	float listener[9] = { 0, 0, 1, 0, 1, 0, 100, 5, -40 };  // front, top, pos (Control 1 style)
	float player[9] = { 0, 0, 1, 0, 1, 0, 101, 4, -37 };
	float far_[9] = { 0, 0, 1, 0, 1, 0, 500, 5, 500 };
	uint64_t lis = 77;
	CHECK(FakeSetDefaultListeners(&lis, 1) == 1);
	CHECK(FakeSetPosition(77, listener) == 1);
	CHECK(FakeSetPosition(1234, player) == 1);
	CHECK(FakeSetPosition(0xDEAD, far_) == 31);  // return values pass through
	CHECK(FakeMulti(555, far_, 1, 2, 0xABCDEF) == 0xABCDEF);  // 5th (stack) argument survives
	CHECK(ListenerId() == 77);
	Xf x;
	CHECK(Get(1234, x) && x.pos[0] == 101);
	CHECK(Get(555, x) && x.pos[0] == 500);
	double ref[3] = { 100, 5, -40 };
	auto   near_ = Nearest(ref, 3, 5.0);
	CHECK(!near_.empty() && near_[0].id == 1234 && std::abs(near_[0].dist - std::sqrt(11.0)) < 1e-6);

	// views: put the listener position into the stand-in view's translation and find it
	views::g_mod = GetModuleHandleW(nullptr);
	for (int i = 0; i < 12; ++i) view->viewToWorld[i] = 0;
	view->viewToWorld[9] = 100;
	view->viewToWorld[10] = 5;
	view->viewToWorld[11] = -40;
	view->fov = 1.2f;

	// ---- frames + CSV ----
	for (int i = 0; i < 5; ++i) CHECK(FakeRenderAudio(true) == 1);
	CHECK(g_frame.load() == 5);
	DrainRows();  // no CSV open yet: these rows are dropped
	OpenCsv();
	for (int i = 0; i < 3; ++i) FakeRenderAudio(false);
	DrainRows();

	// ---- commands ----
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "help\nrtti %p\nviews 8\nview %p\nemitters 5\ntrack 1234\nbogus\n", static_cast<void*>(view), static_cast<void*>(view));
	if (FILE* f = _wfopen(Path(L"selftest_cmd.txt").c_str(), L"w")) {
		fputs(cmd, f);
		fclose(f);
	}
	PollCommands();
	CHECK(GetFileAttributesW(Path(L"selftest_cmd.txt").c_str()) == INVALID_FILE_ATTRIBUTES);  // consumed
	Row r{};
	FillRow(r, 0);
	CHECK((r.flags & kRowListener) && r.lisPos[0] == 100);
	CHECK((r.flags & kRowEmitter) && r.emitterId == 1234);
	CHECK((r.flags & kRowCam) && r.cam[9] == 100 && std::abs(r.fov - 1.2f) < 1e-6);
	SummaryCommon();

	// ---- value search: find a heap position, move it, keep the mover, watch it ----
	auto*  posd = new double[3]{ 1234.5, -77.25, 9.0 };
	auto*  posf = new float[3]{ 1234.5f, -77.25f, 9.0f };
	double q[3] = { 1234.5, -77.25, 9.0 };
	auto   fd_ = FindTriple(q, 0.01, true, 4096);
	auto   ff_ = FindTriple(q, 0.01, false, 4096);
	CHECK(std::any_of(fd_.begin(), fd_.end(), [&](const Cand& c) { return c.addr == reinterpret_cast<uintptr_t>(posd); }));
	CHECK(std::any_of(ff_.begin(), ff_.end(), [&](const Cand& c) { return c.addr == reinterpret_cast<uintptr_t>(posf); }));
	if (FILE* f = _wfopen(Path(L"selftest_cmd.txt").c_str(), L"w")) {
		fputs("find 1234.5 -77.25 9 0.01\n", f);
		fclose(f);
	}
	PollCommands();
	posd[0] += 2.0;  // the "player" moves; copies of the old value don't
	if (FILE* f = _wfopen(Path(L"selftest_cmd.txt").c_str(), L"w")) {
		fputs("keep moved\nwatch 1 0\ncands\n", f);
		fclose(f);
	}
	PollCommands();
	CHECK(g_cands.size() == 1 && g_cands[0].addr == reinterpret_cast<uintptr_t>(posd));
	Row rw{};
	FillRow(rw, 0);
	CHECK(rw.watch[3] == 1236.5 && rw.watch[4] == -77.25 && rw.watch[0] == 0);

	// ---- obs: on-demand pass-through hook, then removed again ----
	snprintf(cmd, sizeof(cmd), "obs 2 %p fake_system\n", reinterpret_cast<void*>(&FakeSystem));
	if (FILE* f = _wfopen(Path(L"selftest_cmd.txt").c_str(), L"w")) {
		fputs(cmd, f);
		fclose(f);
	}
	PollCommands();
	CHECK(g_obs[2].target == reinterpret_cast<void*>(&FakeSystem));
	for (uintptr_t i = 0; i < 3; ++i) CHECK(FakeSystem(0x1000 + i, 5) == 0x1005 + i);
	CHECK(g_obs[2].calls.load() == 3 && g_obs[2].args[0].load() == 0x1002 && g_obs[2].args[1].load() == 5);
	if (FILE* f = _wfopen(Path(L"selftest_cmd.txt").c_str(), L"w")) {
		fputs("obs\nobs 2 off\n", f);
		fclose(f);
	}
	PollCommands();
	CHECK(g_obs[2].target == nullptr);
	CHECK(FakeSystem(1, 1) == 2 && g_obs[2].calls.load() == 3);  // unhooked: still works, no longer counted

	std::string log = ReadAll(Path(L"selftest_probe.log"));
	CHECK(log.find("obs 2 fake_system") != std::string::npos);
	CHECK(log.find("keep moved: 1 of") != std::string::npos);
	CHECK(log.find("rtti") != std::string::npos && log.find("'PerspectiveView@m'") != std::string::npos);
	CHECK(log.find("listener pos as doubles at +b8") != std::string::npos);  // 0x70 + 9*8
	CHECK(log.find("unknown command") != std::string::npos);
	CHECK(log.find("tracking view") != std::string::npos);

	fclose(g_csv);
	g_csv = nullptr;
	WIN32_FIND_DATAW fd;
	HANDLE           h = FindFirstFileW(Path(L"selftest_frames_*.csv").c_str(), &fd);
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h != INVALID_HANDLE_VALUE) {
		std::wstring newest;
		FILETIME     best{};
		do {
			if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) {
				best = fd.ftLastWriteTime;
				newest = fd.cFileName;
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		std::string csv = ReadAll(Path(newest));
		size_t      lines = std::count(csv.begin(), csv.end(), '\n');
		CHECK(lines == 4);  // header + 3 rows
		CHECK(csv.find("t,frame,tid,flags,p0") == 0);
	}

	MH_DisableHook(MH_ALL_HOOKS);
	delete view;
	printf(g_fail ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", g_fail);
	return g_fail ? 1 : 0;
}
