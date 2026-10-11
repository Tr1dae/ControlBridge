// Offline test of the shared-memory link (src/common/link.h). Uses a private mapping name per run,
// so it is safe to run while the games are up. Exit code 0 = all passed.
#include "../src/common/link.h"

#include <cstdio>
#include <random>
#include <thread>

using namespace cb::link;

namespace
{
	using namespace cb::proto;
	int g_fail = 0;

#define CHECK(c)                                                    \
	do {                                                            \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
			++g_fail;                                               \
		}                                                           \
	} while (0)

	std::wstring TestName(const wchar_t* a_tag)
	{
		return std::wstring(L"Local\\ControlBridge_test_") + a_tag + L"_" + std::to_wstring(GetCurrentProcessId());
	}

	void TestMapping()
	{
		std::wstring name = TestName(L"map");
		Mapping      a, b;
		CHECK(a.Open(name.c_str()) && a.created);
		CHECK(b.Open(name.c_str()) && !b.created);
		CHECK(a.shm && b.shm && a.shm != b.shm);  // two views of the same memory
		a.shm->header.hostPid = 1234;
		CHECK(b.shm->header.hostPid == 1234);
		CHECK(b.shm->toGuestBulk.capacity == kBulkRingBytes);
		printf("mapping: %.1f MB\n", sizeof(Shared) / 1048576.0);

		// A mapping made by an incompatible build is refused.
		std::wstring bad = TestName(L"bad");
		HANDLE       h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), bad.c_str());
		auto*        s = static_cast<Shared*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
		s->header.version = kVersion + 1;
		s->header.magic = kMagic;
		Mapping c;
		CHECK(!c.Open(bad.c_str()));
		UnmapViewOfFile(s);
		CloseHandle(h);
	}

	void TestSeqlock()
	{
		std::wstring name = TestName(L"seq");
		Mapping      w, r;
		w.Open(name.c_str());
		r.Open(name.c_str());
		GuestState probe{};
		CHECK(!Read(r.shm->guest, probe));  // never written

		std::atomic<bool> stop{ false };
		std::thread       writer([&] {
            GuestState g{};
            for (uint64_t i = 1; !stop; ++i) {
                g.frame = i;
                for (double& p : g.jesse.pos) p = static_cast<double>(i);
                for (float& f : g.camera.xf.rot) f = static_cast<float>(i & 0xFFFFF);
                g.qpc = static_cast<int64_t>(i);
                Write(w.shm->guest, g);
            }
        });
		uint64_t reads = 0, torn = 0, fails = 0, last = 0, backwards = 0;
		ULONGLONG until = GetTickCount64() + 1500;
		while (GetTickCount64() < until) {
			GuestState g;
			if (!Read(r.shm->guest, g)) {
				++fails;
				continue;
			}
			++reads;
			bool ok = g.jesse.pos[0] == static_cast<double>(g.frame) && g.jesse.pos[2] == static_cast<double>(g.frame) &&
			          g.qpc == static_cast<int64_t>(g.frame) && g.camera.xf.rot[8] == static_cast<float>(g.frame & 0xFFFFF);
			torn += !ok;
			backwards += g.frame < last;
			last = g.frame;
		}
		stop = true;
		writer.join();
		printf("seqlock: %llu reads, %llu torn, %llu gave up, %llu backwards\n", reads, torn, fails, backwards);
		CHECK(reads > 1000 && torn == 0 && backwards == 0);
	}

	// Test 5: two threads published at once (Resonant moved its audio thread) and left the counter
	// odd for good. Now: forced parity heals it, and TryWrite serializes publishers.
	void TestTwoWriters()
	{
		std::wstring name = TestName(L"two");
		Mapping      m;
		m.Open(name.c_str());
		GuestState g{}, out{};
		g.frame = 7;
		for (double& p : g.jesse.pos) p = 7.0;  // consistent, so reads before the writers start aren't 'torn'
		m.shm->guest.seq = 13;  // stuck odd, as after the overlap
		CHECK(!Read(m.shm->guest, out));
		Write(m.shm->guest, g);
		CHECK(Read(m.shm->guest, out) && out.frame == 7 && (m.shm->guest.seq & 1) == 0);

		std::atomic_flag  busy = ATOMIC_FLAG_INIT;
		std::atomic<bool> stop{ false };
		std::atomic<uint64_t> skipped{ 0 };
		auto writer = [&](uint64_t a_base) {
			GuestState w{};
			for (uint64_t i = 1; !stop; ++i) {
				w.frame = a_base + i;
				for (double& p : w.jesse.pos) p = static_cast<double>(w.frame);
				if (!TryWrite(m.shm->guest, w, busy)) ++skipped;
			}
		};
		std::thread a(writer, 0), b(writer, 1ull << 40);
		uint64_t reads = 0, torn = 0;
		ULONGLONG until = GetTickCount64() + 1000;
		while (GetTickCount64() < until) {
			if (!Read(m.shm->guest, out)) continue;
			++reads;
			torn += out.jesse.pos[0] != static_cast<double>(out.frame) || out.jesse.pos[2] != static_cast<double>(out.frame);
		}
		stop = true;
		a.join();
		b.join();
		printf("two writers: %llu reads, %llu torn, %llu writes skipped, final seq %u\n", reads, torn,
			static_cast<unsigned long long>(skipped.load()), m.shm->guest.seq);
		CHECK(reads > 1000 && torn == 0 && (m.shm->guest.seq & 1) == 0 && Read(m.shm->guest, out));
	}

	void TestRing()
	{
		std::wstring name = TestName(L"ring");
		Mapping      p, c;
		p.Open(name.c_str());
		c.Open(name.c_str());
		constexpr uint32_t kRecords = 200000;
		std::thread        producer([&] {
            std::mt19937 rng(7);
            std::vector<uint8_t> buf(3000);
            for (uint32_t i = 0; i < kRecords;) {
                uint32_t n = 4 + rng() % 2900;
                memcpy(buf.data(), &i, 4);
                for (uint32_t k = 4; k < n; ++k) buf[k] = static_cast<uint8_t>(i * 31 + k);
                if (Push(p.shm->toGuestCmd, p.shm->toGuestCmdData, kRecCommand, buf.data(), n)) ++i;
                else YieldProcessor();
            }
        });
		uint32_t next = 0, bad = 0;
		while (next < kRecords) {
			Drain(c.shm->toGuestCmd, c.shm->toGuestCmdData, [&](uint32_t a_type, const uint8_t* a_p, uint32_t a_n) {
				uint32_t id;
				memcpy(&id, a_p, 4);
				bool ok = a_type == kRecCommand && id == next;
				for (uint32_t k = 4; ok && k < a_n; ++k) ok = a_p[k] == static_cast<uint8_t>(id * 31 + k);
				bad += !ok;
				++next;
			});
		}
		producer.join();
		printf("ring: %u records through %llu KB, %u bad\n", next, kCmdRingBytes >> 10, bad);
		CHECK(bad == 0);
		CHECK(c.shm->toGuestCmd.head == c.shm->toGuestCmd.tail);
		CHECK(!Push(p.shm->toGuestCmd, p.shm->toGuestCmdData, kRecCommand, nullptr, static_cast<uint32_t>(kCmdRingBytes)));  // too big
		CHECK(PushCommand(p.shm->toHostCmd, p.shm->toHostCmdData, "tp 0 10 0"));
		std::string got;
		Drain(c.shm->toHostCmd, c.shm->toHostCmdData, [&](uint32_t, const uint8_t* a_p, uint32_t a_n) { got.assign(reinterpret_cast<const char*>(a_p), a_n); });
		CHECK(got == "tp 0 10 0");
	}
#define OFF(T, F) static_cast<unsigned>(offsetof(T, F))
	void PrintLayout()
	{
		using namespace cb::proto;
		printf("{\"version\":%u,\"size\":%zu,\n", kVersion, sizeof(Shared));
		printf(" \"header\":{\"off\":%u,\"magic\":%u,\"version\":%u,\"hostPid\":%u,\"guestPid\":%u,\"hostHeartbeatMs\":%u,\"guestHeartbeatMs\":%u},\n",
			OFF(Shared, header), OFF(Header, magic), OFF(Header, version), OFF(Header, hostPid), OFF(Header, guestPid), OFF(Header, hostHeartbeatMs),
			OFF(Header, guestHeartbeatMs));
		printf(" \"slots\":{\"host\":%u,\"pad\":%u,\"guest\":%u,\"data\":%u},\n", OFF(Shared, host), OFF(Shared, pad), OFF(Shared, guest),
			OFF(Slot<HostState>, data));
		printf(" \"Xform\":{\"pos\":%u,\"rot\":%u}, \"View\":{\"xf\":%u,\"fovY\":%u,\"aspect\":%u},\n", OFF(Xform, pos), OFF(Xform, rot), OFF(View, xf),
			OFF(View, fovY), OFF(View, aspect));
		printf(" \"HostState\":{\"flags\":%u,\"worldId\":%u,\"frame\":%u,\"dt\":%u,\"teleportSeq\":%u,\"teleport\":%u,\"player\":%u,\"camera\":%u},\n",
			OFF(HostState, flags), OFF(HostState, worldId), OFF(HostState, frame), OFF(HostState, dt), OFF(HostState, teleportSeq),
			OFF(HostState, teleport), OFF(HostState, player), OFF(HostState, camera));
		printf(" \"PadState\":{\"flags\":%u,\"packet\":%u,\"buttons\":%u,\"lt\":%u,\"rt\":%u,\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u},\n",
			OFF(PadState, flags), OFF(PadState, packet), OFF(PadState, buttons), OFF(PadState, lt), OFF(PadState, rt), OFF(PadState, lx),
			OFF(PadState, ly), OFF(PadState, rx), OFF(PadState, ry));
		printf(" \"GuestState\":{\"flags\":%u,\"teleportAck\":%u,\"frame\":%u,\"qpc\":%u,\"jesse\":%u,\"vel\":%u,\"height\":%u,\"camera\":%u}}\n",
			OFF(GuestState, flags), OFF(GuestState, teleportAck), OFF(GuestState, frame), OFF(GuestState, qpc), OFF(GuestState, jesse),
			OFF(GuestState, vel), OFF(GuestState, height), OFF(GuestState, camera));
	}

	// Writes moving fake host/guest states into a mapping (never the live name) for a_seconds, so
	// tools/cb_monitor.py can be tested without the games.
	int Fake(const wchar_t* a_name, int a_seconds)
	{
		if (!wcscmp(a_name, kMappingName)) {
			printf("refusing to fake the live mapping\n");
			return 1;
		}
		Mapping m;
		if (!m.Open(a_name)) return 1;
		m.shm->header.hostPid = 111;
		m.shm->header.guestPid = 222;
		for (int i = 0; i < a_seconds * 60; ++i) {
			HostState h{};
			h.flags = kHostInGame;
			h.frame = i;
			h.player.pos[0] = 600 + i * 0.1;
			h.player.pos[1] = 0.13;
			h.player.pos[2] = -80;
			h.camera.xf.pos[0] = 600 + i * 0.1 + 4;
			h.camera.xf.pos[1] = 2.5;
			h.camera.xf.rot[6] = -1;  // forward -X
			h.camera.fovY = 0.97f;
			Write(m.shm->host, h);
			PadState p{ kPadActive, static_cast<uint32_t>(i), 0x1000, 0, 255, 0, 32767, 0, 0 };
			Write(m.shm->pad, p);
			GuestState g{};
			g.flags = kGuestInWorld;
			g.frame = i;
			g.jesse.pos[0] = -8;
			g.jesse.pos[1] = -152;
			g.jesse.pos[2] = -85 + i * 0.07;
			g.camera.xf.pos[1] = -150;
			g.camera.fovY = 0.75f;
			Write(m.shm->guest, g);
			m.shm->header.hostHeartbeatMs = m.shm->header.guestHeartbeatMs = GetTickCount64();
			Sleep(16);
		}
		return 0;
	}
}

int main(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "--layout")) {
		PrintLayout();
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "--fake")) {
		std::string  n = argv[2];
		std::wstring w(n.begin(), n.end());
		return Fake(w.c_str(), argc > 3 ? atoi(argv[3]) : 5);
	}
	TestMapping();
	TestSeqlock();
	TestTwoWriters();
	TestRing();
	printf(g_fail ? "LINK TEST FAILED (%d)\n" : "LINK TEST OK\n", g_fail);
	return g_fail ? 1 : 0;
}
