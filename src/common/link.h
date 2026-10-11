// Control Bridge link: opens the shared mapping and implements the seqlock slots and SPSC rings of
// cb_protocol.h. Header-only, no dependencies beyond Windows; used by both DLLs and the tests.
//
// x64 is TSO (stores aren't reordered with stores, loads not with loads), so compiler barriers
// around the seqlock and ring index updates are enough; the counters are read and written whole
// (aligned 32/64-bit).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "cb_protocol.h"

namespace cb::link
{
	using namespace cb::proto;

	inline void Barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }

	// ---- mapping ---------------------------------------------------------------------------------
	struct Mapping
	{
		HANDLE  handle = nullptr;
		Shared* shm = nullptr;
		bool    created = false;  // this side made it (and initialized it)

		// Opens or creates the mapping. False if it exists with another magic/version: an old build
		// of the other side is still running.
		bool Open(const wchar_t* a_name = kMappingName)
		{
			ULARGE_INTEGER sz;
			sz.QuadPart = sizeof(Shared);
			handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, sz.HighPart, sz.LowPart, a_name);
			if (!handle) return false;
			created = GetLastError() != ERROR_ALREADY_EXISTS;
			shm = static_cast<Shared*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
			if (!shm) return Close(), false;
			// The first side initializes. A side that lost the creation race may briefly see magic 0:
			// wait for the creator.
			if (created) {
				shm->toGuestCmd.capacity = kCmdRingBytes;
				shm->toHostCmd.capacity = kCmdRingBytes;
				shm->toGuestBulk.capacity = kBulkRingBytes;
				shm->header.version = kVersion;
				Barrier();
				shm->header.magic = kMagic;
			} else {
				for (int i = 0; i < 100 && shm->header.magic == 0; ++i) Sleep(10);
			}
			if (shm->header.magic != kMagic || shm->header.version != kVersion) return Close(), false;
			return true;
		}

		void Close()
		{
			if (shm) UnmapViewOfFile(shm);
			if (handle) CloseHandle(handle);
			shm = nullptr;
			handle = nullptr;
		}

		~Mapping() { Close(); }
	};

	inline bool Alive(volatile uint64_t& a_heartbeat)
	{
		uint64_t hb = a_heartbeat;
		return hb && GetTickCount64() - hb < kHeartbeatTimeoutMs;
	}

	// ---- seqlock -----------------------------------------------------------------------------------
	template <class T>
	inline void Write(Slot<T>& a_slot, const T& a_value)
	{
		uint32_t s = a_slot.seq;
		a_slot.seq = s + 1;  // odd: writing
		Barrier();
		memcpy(const_cast<T*>(&a_slot.data), &a_value, sizeof(T));
		Barrier();
		a_slot.seq = s + 2;
	}

	// Returns false if no consistent copy was obtained (writer kept interrupting) or nothing was
	// ever written. Callers keep their last good value in that case.
	template <class T>
	inline bool Read(const Slot<T>& a_slot, T& a_out, int a_tries = 64)
	{
		for (int i = 0; i < a_tries; ++i) {
			uint32_t s1 = a_slot.seq;
			if (s1 & 1) {
				YieldProcessor();
				continue;
			}
			Barrier();
			memcpy(&a_out, const_cast<const T*>(&a_slot.data), sizeof(T));
			Barrier();
			if (a_slot.seq == s1) return s1 != 0;
		}
		return false;
	}

	// ---- SPSC ring ------------------------------------------------------------------------------------
	inline constexpr uint32_t kRecHeader = 8;
	inline uint64_t           Align8(uint64_t a_n) { return (a_n + 7) & ~7ull; }

	// Producer side. False if the record doesn't fit right now (the consumer is behind).
	inline bool Push(RingHeader& a_ring, uint8_t* a_data, uint32_t a_type, const void* a_payload, uint32_t a_size)
	{
		uint64_t cap = a_ring.capacity, need = Align8(kRecHeader + a_size);
		if (need > cap / 2) return false;
		uint64_t head = a_ring.head, tail = a_ring.tail;
		uint64_t pos = head % cap, toEnd = cap - pos;
		uint64_t total = need + (toEnd < need ? toEnd : 0);  // a skip record fills the end if needed
		if (cap - (head - tail) < total) return false;
		if (toEnd < need) {
			uint32_t skip[2] = { static_cast<uint32_t>(toEnd - kRecHeader), kRecSkip };
			memcpy(a_data + pos, skip, kRecHeader);
			head += toEnd;
			pos = 0;
		}
		uint32_t hdr[2] = { a_size, a_type };
		memcpy(a_data + pos, hdr, kRecHeader);
		if (a_size) memcpy(a_data + pos + kRecHeader, a_payload, a_size);
		Barrier();
		a_ring.head = head + need;
		return true;
	}

	// Consumer side. Calls a_fn(type, payload, size) for every waiting record; returns how many.
	template <class F>
	inline size_t Drain(RingHeader& a_ring, const uint8_t* a_data, F&& a_fn, size_t a_max = SIZE_MAX)
	{
		size_t   n = 0;
		uint64_t cap = a_ring.capacity;
		while (n < a_max) {
			uint64_t tail = a_ring.tail, head = a_ring.head;
			if (tail == head) break;
			Barrier();
			uint32_t hdr[2];
			memcpy(hdr, a_data + tail % cap, kRecHeader);
			if (hdr[1] != kRecSkip) {
				a_fn(hdr[1], a_data + tail % cap + kRecHeader, hdr[0]);
				++n;
			}
			Barrier();
			a_ring.tail = tail + Align8(kRecHeader + hdr[0]);
		}
		return n;
	}

	inline bool PushCommand(RingHeader& a_ring, uint8_t* a_data, const std::string& a_line)
	{
		return Push(a_ring, a_data, kRecCommand, a_line.data(), static_cast<uint32_t>(a_line.size()));
	}
}
