// Control Bridge shared-memory protocol: CONTROL Resonant (host, the window you look at) <->
// Control (guest, hidden, drives Jesse).
//
// One mapping, Local\ControlBridge_v1, made by whichever side starts first. Per-frame state lives
// in latest-value slots under a seqlock (Slot<T>); streams (commands, later collision tiles) go
// through single-producer single-consumer byte rings (Ring). This header is the only definition of
// the layout; change it and bump kVersion.
//
// Coordinates: positions are world doubles in each game's OWN frame, tagged by which side wrote
// them. Conversion between the two worlds lives in coords.h, so the protocol doesn't change when
// the mapping is measured or re-anchored.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cb::proto
{
	inline constexpr uint32_t kMagic = 0x47524243;  // "CBRG"
	inline constexpr uint32_t kVersion = 1;
	inline constexpr wchar_t  kMappingName[] = L"Local\\ControlBridge_v1";
	inline constexpr uint64_t kHeartbeatTimeoutMs = 1500;  // older than this: that side is gone

	// A rigid transform as Northlight keeps it: double position, float rotation rows.
	struct Xform
	{
		double pos[3];
		float  rot[9];  // rows: right, up, forward (each game's convention, see coords.h)
	};
	static_assert(sizeof(Xform) == 64);

	struct View
	{
		Xform xf;
		float fovY;  // radians, vertical
		float aspect;
	};

	// ---- per-frame state ----------------------------------------------------------------------
	enum HostFlags : uint32_t
	{
		kHostInGame = 1u << 0,       // a save is loaded and the player exists
		kHostMenu = 1u << 1,         // a menu owns input: the guest drops held input
		kHostLoading = 1u << 2,      // loading / streaming stall
		kHostPuppet = 1u << 3,       // the host is applying GuestState to its player
		kHostWorldChanged = 1u << 4  // the host changed level: anchor and collision are stale
	};

	struct HostState  // host -> guest
	{
		uint32_t flags;
		uint32_t worldId;        // changes on level/region change
		uint64_t frame;
		float    dt;
		uint32_t teleportSeq;    // guest moves Jesse to teleport (host frame) when this changes
		Xform    teleport;
		Xform    player;         // the host player's own transform (the puppet), host frame
		View     camera;         // host camera, host frame
	};

	enum PadFlags : uint32_t
	{
		kPadActive = 1u << 0,  // the guest reports this pad as XInput controller 0
	};

	struct PadState  // host -> guest: the real controller, read on the host (it has focus)
	{
		uint32_t flags;
		uint32_t packet;
		uint16_t buttons;  // XINPUT_GAMEPAD_* bits
		uint8_t  lt, rt;
		int16_t  lx, ly, rx, ry;
	};

	enum GuestFlags : uint32_t
	{
		kGuestInWorld = 1u << 0,    // Jesse exists and the transform is valid
		kGuestFocusFaked = 1u << 1, // focus hooks engaged (runs in the background)
		kGuestAnchored = 1u << 2,   // Jesse stands in the streamed host world
	};

	struct GuestState  // guest -> host
	{
		uint32_t flags;
		uint32_t teleportAck;  // last HostState::teleportSeq applied
		uint64_t frame;
		int64_t  qpc;          // QueryPerformanceCounter at sampling
		Xform    jesse;        // guest frame
		float    vel[3];       // m/s, guest frame
		float    height;       // capsule height, m
		View     camera;       // guest camera, guest frame
	};

	// ---- seqlock slot ---------------------------------------------------------------------------
	// One writer. seq is odd while a write is in progress; readers retry until they see the same
	// even value before and after copying.
	template <class T>
	struct alignas(64) Slot
	{
		volatile uint32_t seq;
		uint32_t          pad;
		T                 data;
	};

	// ---- SPSC byte ring ---------------------------------------------------------------------------
	// Records are {u32 size, u32 type, payload}, padded to 8 bytes. head/tail are free-running byte
	// counters (the position is counter % capacity); a record never wraps: a type-0 skip record
	// fills the rest of the buffer instead.
	struct RingHeader
	{
		alignas(64) volatile uint64_t head;  // producer
		alignas(64) volatile uint64_t tail;  // consumer
		uint64_t capacity;                   // bytes, power of two
	};

	enum RecordType : uint32_t
	{
		kRecSkip = 0,
		kRecCommand = 1,  // UTF-8 text command line
		// Phase 2: collision tiles
	};

	// ---- layout ----------------------------------------------------------------------------------
	struct Header
	{
		uint32_t magic;
		uint32_t version;
		uint32_t hostPid;
		uint32_t guestPid;
		volatile uint64_t hostHeartbeatMs;   // GetTickCount64() at the host's last frame
		volatile uint64_t guestHeartbeatMs;  // GetTickCount64() at the guest's last publish
	};

	inline constexpr uint64_t kCmdRingBytes = 64 << 10;
	inline constexpr uint64_t kBulkRingBytes = 64ull << 20;  // Phase 2 collision

	struct Shared
	{
		alignas(64) Header header;
		Slot<HostState>  host;
		Slot<PadState>   pad;
		Slot<GuestState> guest;
		RingHeader       toGuestCmd;   // tools/host -> guest commands
		RingHeader       toHostCmd;    // tools/guest -> host commands
		RingHeader       toGuestBulk;  // streamer -> guest (collision)
		alignas(64) uint8_t toGuestCmdData[kCmdRingBytes];
		alignas(64) uint8_t toHostCmdData[kCmdRingBytes];
		alignas(64) uint8_t toGuestBulkData[kBulkRingBytes];
	};
}
