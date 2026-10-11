// Offline test of the shared-memory link (src/common/link.h). Uses a private mapping name per run,
// so it is safe to run while the games are up. Exit code 0 = all passed.
#include "../src/common/link.h"

#include <cstdio>
#include <random>
#include <thread>

using namespace cb::link;

namespace
{
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
}

int main()
{
	TestMapping();
	TestSeqlock();
	TestRing();
	printf(g_fail ? "LINK TEST FAILED (%d)\n" : "LINK TEST OK\n", g_fail);
	return g_fail ? 1 : 0;
}
