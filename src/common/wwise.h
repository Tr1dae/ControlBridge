// Wwise observer + camera-view finder, shared by both probes.
//
// Both games link Wwise and export AK::SoundEngine by decorated name (Control 1 from
// snd_rmdwin*_f.dll, Resonant from the exe), so these hooks are found by name, not by byte
// pattern, and survive game patches:
//   RenderAudio          once per game frame  -> cb::Tick() (the per-frame CSV row)
//   SetPosition          every emitter/listener transform -> table of game objects
//   SetMultiplePositions first position of multi-position emitters
//   Set/Add/RemoveDefaultListener(s), SetListeners -> which game object is the listener (camera)
//   UnregisterGameObj    marks objects gone
// Every detour records and then calls the original unchanged.
//
// Transform layouts are detected from the data (two unit vectors + a position):
//   AkTransform (float, Control 1)        {front, top, pos} or {pos, front, top}
//   AkWorldTransform (double pos, Resonant) {pos(d), front, top} or {front, top, pos(d)}
#pragma once

#include "probe.h"

namespace cb::wwise
{
	struct Obj
	{
		uint64_t id;
		uint64_t count, countMark;
		double   t;
		uint8_t  raw[48];
		bool     used, removed;
	};

	inline constexpr size_t kObjs = 1 << 13;
	inline Obj              g_objs[kObjs];
	inline size_t           g_used = 0;
	inline SRWLOCK          g_lock = SRWLOCK_INIT;
	inline bool             g_double = false;  // SetPosition takes AkWorldTransform (double positions)

	inline uint64_t g_defListeners[8] = {};
	inline uint32_t g_nDef = 0;
	inline uint64_t g_setLisEmitter = 0, g_setLis[8] = {};
	inline uint32_t g_nSetLis = 0;

	enum Kind
	{
		kSetPos,
		kMulti,
		kSetDef,
		kAddDef,
		kRemDef,
		kSetLis,
		kRender,
		kUnreg,
		kKinds
	};
	inline constexpr const char* kKindName[kKinds] = { "SetPosition", "SetMultiplePositions", "SetDefaultListeners",
		"AddDefaultListener", "RemoveDefaultListener", "SetListeners", "RenderAudio", "UnregisterGameObj" };
	inline constexpr const char* kPrefix[kKinds] = { "?SetPosition@SoundEngine@AK@@", "?SetMultiplePositions@SoundEngine@AK@@",
		"?SetDefaultListeners@SoundEngine@AK@@", "?AddDefaultListener@SoundEngine@AK@@", "?RemoveDefaultListener@SoundEngine@AK@@",
		"?SetListeners@SoundEngine@AK@@", "?RenderAudio@SoundEngine@AK@@", "?UnregisterGameObj@SoundEngine@AK@@" };
	inline std::atomic<uint64_t> g_calls[kKinds];
	inline uint64_t              g_callsMark[kKinds];
	inline std::atomic<uint32_t> g_renderTid{ 0 };

	inline constexpr uint64_t kAuto = ~0ull;
	inline std::atomic<uint64_t> g_tracked{ kAuto };  // emitter in the CSV: id, kAuto (nearest to listener) or 0

	// ---- transform decoding -------------------------------------------------------------------
	struct Xf
	{
		double pos[3];
		float  front[3], top[3];
	};

	inline bool Unit(const float* v)
	{
		float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
		return l > 0.9f && l < 1.1f;
	}

	// Returns the layout (0..3, see the header) or -1 if the bytes don't look like a transform.
	inline int Decode(const uint8_t* a_raw, bool a_double, Xf& a_out)
	{
		float f[9];
		if (!a_double) {
			memcpy(f, a_raw, 36);
			int lay = (Unit(f) && Unit(f + 3)) ? 0 : (Unit(f + 3) && Unit(f + 6)) ? 1 : -1;
			if (lay < 0) return -1;
			const float* p = lay == 0 ? f + 6 : f;
			const float* fr = lay == 0 ? f : f + 3;
			for (int i = 0; i < 3; ++i) {
				a_out.pos[i] = p[i];
				a_out.front[i] = fr[i];
				a_out.top[i] = fr[3 + i];
			}
			return lay;
		}
		memcpy(f, a_raw + 24, 24);
		if (Unit(f) && Unit(f + 3)) {
			memcpy(a_out.pos, a_raw, 24);
			memcpy(a_out.front, f, 12);
			memcpy(a_out.top, f + 3, 12);
			return 2;
		}
		memcpy(f, a_raw, 24);
		if (Unit(f) && Unit(f + 3)) {
			memcpy(a_out.pos, a_raw + 24, 24);
			memcpy(a_out.front, f, 12);
			memcpy(a_out.top, f + 3, 12);
			return 3;
		}
		return -1;
	}

	// ---- object table (callers hold g_lock) -----------------------------------------------------
	inline Obj* Slot(uint64_t a_id, bool a_create)
	{
		size_t h = static_cast<size_t>((a_id * 0x9E3779B97F4A7C15ull) >> 51);
		for (size_t i = 0; i < 64; ++i) {
			Obj& o = g_objs[(h + i) & (kObjs - 1)];
			if (o.used && o.id == a_id) return &o;
			if (!o.used) {
				if (!a_create) return nullptr;
				o = {};
				o.used = true;
				o.id = a_id;
				++g_used;
				return &o;
			}
		}
		return nullptr;
	}

	// Drops objects not seen for a minute (linear probing: rebuild instead of deleting in place).
	inline void Compact()
	{
		std::vector<Obj> keep;
		double           now = Now();
		AcquireSRWLockExclusive(&g_lock);
		for (auto& o : g_objs) {
			if (o.used && !o.removed && now - o.t < 60.0) keep.push_back(o);
		}
		memset(g_objs, 0, sizeof(g_objs));
		g_used = 0;
		for (auto& k : keep) {
			if (Obj* o = Slot(k.id, true)) *o = k;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	inline void Record(uint64_t a_id, const void* a_xf)
	{
		uint8_t raw[48] = {};
		if (!a_xf || !SafeCopy(raw, a_xf, g_double ? 48 : 36)) return;
		double t = Now();
		AcquireSRWLockExclusive(&g_lock);
		if (Obj* o = Slot(a_id, true)) {
			memcpy(o->raw, raw, sizeof(raw));
			++o->count;
			o->t = t;
			o->removed = false;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	inline bool Get(uint64_t a_id, Xf& a_out, double* a_t = nullptr)
	{
		AcquireSRWLockShared(&g_lock);
		Obj* o = Slot(a_id, false);
		bool ok = o && Decode(o->raw, g_double, a_out) >= 0;
		if (o && a_t) *a_t = o->t;
		ReleaseSRWLockShared(&g_lock);
		return ok;
	}

	inline uint64_t ListenerId()
	{
		AcquireSRWLockShared(&g_lock);
		uint64_t id = g_nDef ? g_defListeners[0] : (g_nSetLis ? g_setLis[0] : 0);
		ReleaseSRWLockShared(&g_lock);
		return id;
	}

	inline bool IsListener(uint64_t a_id)
	{
		for (uint32_t i = 0; i < g_nDef; ++i) {
			if (g_defListeners[i] == a_id) return true;
		}
		for (uint32_t i = 0; i < g_nSetLis; ++i) {
			if (g_setLis[i] == a_id) return true;
		}
		return false;
	}

	inline double Dist(const double* a, const double* b)
	{
		double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	struct Near
	{
		uint64_t id;
		double   dist;
		uint64_t count;
		Xf       xf;
	};

	// Emitters updated in the last a_maxAge seconds, nearest to a_ref first.
	inline std::vector<Near> Nearest(const double* a_ref, size_t a_n, double a_maxAge, double a_minDist = 0.05)
	{
		std::vector<Near> v;
		double            now = Now();
		AcquireSRWLockShared(&g_lock);
		for (auto& o : g_objs) {
			Xf x;
			if (!o.used || o.removed || now - o.t > a_maxAge || IsListener(o.id) || Decode(o.raw, g_double, x) < 0) continue;
			double dist = Dist(x.pos, a_ref);
			if (dist < a_minDist) continue;  // emitters riding on the camera/listener itself
			v.push_back({ o.id, dist, o.count, x });
		}
		ReleaseSRWLockShared(&g_lock);
		std::sort(v.begin(), v.end(), [](const Near& a, const Near& b) { return a.dist < b.dist; });
		if (v.size() > a_n) v.resize(a_n);
		return v;
	}

	// ---- detours ---------------------------------------------------------------------------------
	// Generic 6-argument pass-through: extra arguments are harmless on x64 (the callee ignores
	// registers and stack slots it doesn't declare), and every hooked function takes integers or
	// pointers only.
	using Fn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	inline constexpr int kOverloads = 4;
	inline Fn            g_orig[kKinds][kOverloads] = {};

	inline void SetIds(uint64_t* a_dst, uint32_t& a_n, const uint64_t* a_src, uint32_t a_count)
	{
		uint64_t tmp[8] = {};
		uint32_t n = std::min<uint32_t>(a_count, 8);
		if (n && (!a_src || !SafeCopy(tmp, a_src, n * 8))) return;
		AcquireSRWLockExclusive(&g_lock);
		memcpy(a_dst, tmp, sizeof(tmp));
		a_n = n;
		ReleaseSRWLockExclusive(&g_lock);
	}

	template <int K, int N>
	uintptr_t Detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
	{
		g_calls[K].fetch_add(1, std::memory_order_relaxed);
		if constexpr (K == kSetPos) {
			Record(a, reinterpret_cast<const void*>(b));
		} else if constexpr (K == kMulti) {
			if ((c & 0xFFFF) != 0) Record(a, reinterpret_cast<const void*>(b));  // element 0; transforms come first in every element type
		} else if constexpr (K == kSetDef) {
			SetIds(g_defListeners, g_nDef, reinterpret_cast<const uint64_t*>(a), static_cast<uint32_t>(b));
		} else if constexpr (K == kAddDef) {
			AcquireSRWLockExclusive(&g_lock);
			bool have = false;
			for (uint32_t i = 0; i < g_nDef; ++i) have |= g_defListeners[i] == a;
			if (!have && g_nDef < 8) g_defListeners[g_nDef++] = a;
			ReleaseSRWLockExclusive(&g_lock);
		} else if constexpr (K == kRemDef) {
			AcquireSRWLockExclusive(&g_lock);
			for (uint32_t i = 0; i < g_nDef; ++i) {
				if (g_defListeners[i] == a) g_defListeners[i] = g_defListeners[--g_nDef];
			}
			ReleaseSRWLockExclusive(&g_lock);
		} else if constexpr (K == kSetLis) {
			g_setLisEmitter = a;
			SetIds(g_setLis, g_nSetLis, reinterpret_cast<const uint64_t*>(b), static_cast<uint32_t>(c));
		} else if constexpr (K == kRender) {
			g_renderTid = GetCurrentThreadId();
			cb::Tick();
		} else if constexpr (K == kUnreg) {
			AcquireSRWLockExclusive(&g_lock);
			if (Obj* o = Slot(a, false)) o->removed = true;
			ReleaseSRWLockExclusive(&g_lock);
		}
		return g_orig[K][N](a, b, c, d, e, f);
	}

#define CB_WW_ROW(K) { reinterpret_cast<void*>(&Detour<K, 0>), reinterpret_cast<void*>(&Detour<K, 1>), reinterpret_cast<void*>(&Detour<K, 2>), reinterpret_cast<void*>(&Detour<K, 3>) }
	inline void* const kDetours[kKinds][kOverloads] = { CB_WW_ROW(kSetPos), CB_WW_ROW(kMulti), CB_WW_ROW(kSetDef), CB_WW_ROW(kAddDef),
		CB_WW_ROW(kRemDef), CB_WW_ROW(kSetLis), CB_WW_ROW(kRender), CB_WW_ROW(kUnreg) };
#undef CB_WW_ROW

	// ---- camera views (m::PerspectiveView) ----------------------------------------------------------
	// Control 1's exports give the layout: m::View::getViewToWorld = +0x70 (Matrix4x3<double>),
	// m::PerspectiveView::getFov = +0x2E0. Resonant keeps the class (RTTI) but the offsets are
	// unconfirmed: 'views' looks for the listener's position inside every live instance.
	namespace views
	{
		inline HMODULE                g_mod = nullptr;
		inline std::vector<uintptr_t> g_vts;
		inline std::atomic<uintptr_t> g_view{ 0 };
		inline uintptr_t              g_viewVt = 0;
		inline uint32_t               g_matOff = 0x70;  // Control 1; Resonant sets 0x68

		// FOV from the view-to-clip matrix that follows view-to-world (+0x60 after it in m::View).
		inline float FovY(uintptr_t a_view)
		{
			double p5 = 0;
			if (!Read(a_view + g_matOff + 0x60 + 5 * 8, p5) || p5 == 0.0 || !std::isfinite(p5)) return 0.0f;
			return static_cast<float>(2.0 * std::atan(1.0 / std::fabs(p5)));
		}

		// Lists live m::PerspectiveView objects whose position (view-to-world row 3) is near the
		// listener, dumps the closest to view_<n>.bin, finds what points at them (globals show as
		// module+rva) and tracks the closest in the CSV (c0..c11, fov).
		inline void Find(size_t a_max)
		{
			if (g_vts.empty() && g_mod) g_vts = FindVtables(g_mod, "PerspectiveView@m");
			Xf   lis{};
			bool haveLis = Get(ListenerId(), lis);
			Log("views: %zu vtable(s) of m::PerspectiveView, view-to-world at +%x; listener %s", g_vts.size(), g_matOff, haveLis ? "known" : "unknown");
			if (!haveLis) return;
			struct Hit
			{
				uintptr_t inst, vt;
				double    dist;
			};
			std::vector<Hit> hits;
			size_t           total = 0;
			for (uintptr_t vt : g_vts) {
				auto inst = ScanHeap(vt, a_max);
				total += inst.size();
				for (uintptr_t p : inst) {
					double pos[3];
					if (SafeRead(pos, p + g_matOff + 0x48, 24) && std::isfinite(pos[0])) hits.push_back({ p, vt, Dist(pos, lis.pos) });
				}
			}
			std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.dist < b.dist; });
			Log("views: %zu instance(s); nearest to the listener:", total);
			for (size_t i = 0; i < hits.size() && i < 8; ++i) {
				double m[12];
				SafeRead(m, hits[i].inst + g_matOff, sizeof(m));
				Log("  [%zu] %p %.2f m  pos %.3f %.3f %.3f  fwd %.3f %.3f %.3f  up %.3f %.3f %.3f  fovY %.1f deg", i, reinterpret_cast<void*>(hits[i].inst),
					hits[i].dist, m[9], m[10], m[11], m[6], m[7], m[8], m[3], m[4], m[5], FovY(hits[i].inst) * 57.29578f);
				if (i < 4) {
					uint8_t b[0x400] = {};
					SafeRead(b, hits[i].inst, sizeof(b));
					wchar_t name[64];
					swprintf(name, 64, L"%ls_view_%zu.bin", g_tag.c_str(), i);
					if (FILE* f = _wfopen(Path(name).c_str(), L"wb")) {
						fwrite(b, 1, sizeof(b), f);
						fclose(f);
					}
					for (auto& h : ScanQwords(hits[i].inst, 1, 16, true)) Log("      pointed to from %s", Rva(h.addr).c_str());
				}
			}
			if (!hits.empty() && hits[0].dist < 5.0) {
				g_viewVt = hits[0].vt;
				g_view = hits[0].inst;
				Log("views: tracking [0] in CSV c0..c11 / fov");
			}
		}

		inline void Fill(Row& a_r)
		{
			uintptr_t p = g_view.load();
			if (!p) return;
			uintptr_t vt = 0;
			if (!Read(p, vt) || vt != g_viewVt) {
				g_view = 0;
				Log("view %p: gone (vtable changed), tracking stopped", reinterpret_cast<void*>(p));
				return;
			}
			if (SafeRead(a_r.cam, p + g_matOff, sizeof(a_r.cam))) {
				a_r.fov = FovY(p);
				a_r.flags |= kRowCam;
			}
		}
	}

	// ---- autofind: where does the game keep the player's position? ----------------------------------
	// 'autofind [emitter id | listener] [box]':
	//   1. snapshot every double/float triple within `box` m of a reference object (default: nearest
	//      emitter that isn't on the camera, i.e. something on the player)
	//   2. wait until the reference moved > 3 m and stood still for 0.75 s
	//   3. keep candidates that moved by the same delta (25 cm per axis), so bone offsets don't matter
	//   4. repeat once more, then log the survivors and watch the first four (CSV w0..w11)
	namespace autofind
	{
		enum State
		{
			kIdle,
			kWaitMove,
			kWaitStop
		};
		inline State             g_state = kIdle;
		inline uint64_t          g_ref = 0;
		inline double            g_from[3], g_last[3], g_still = 0;
		inline int               g_round = 0;
		inline std::vector<Cand> g_c;

		// Chimes tell the tester what to do without looking at the log: asterisk = "walk now",
		// default beep = "round done, walk again", exclamation = "finished".
		inline void Chime(UINT a_type) { MessageBeep(a_type); }


		inline bool RefPos(double* a_out)
		{
			Xf x;
			if (!Get(g_ref, x)) return false;
			memcpy(a_out, x.pos, 24);
			return true;
		}

		inline void Finish(const char* a_why)
		{
			Log("autofind: %s, %zu candidate(s)", a_why, g_c.size());
			std::vector<double>  v;
			std::vector<uint8_t> ok;
			ReadMany(g_c, v, ok);
			for (size_t i = 0; i < g_c.size() && i < 32; ++i) {
				Log("  [%zu] %p %s now %.4f %.4f %.4f", i, reinterpret_cast<void*>(g_c[i].addr), g_c[i].dbl ? "d" : "f", v[i * 3], v[i * 3 + 1],
					v[i * 3 + 2]);
			}
			AcquireSRWLockExclusive(&g_watchLock);
			for (int w = 0; w < 3; ++w) g_watch[w] = w < static_cast<int>(g_c.size()) ? Watch{ g_c[w].addr, g_c[w].dbl } : Watch{};
			ReleaseSRWLockExclusive(&g_watchLock);
			if (!g_c.empty()) Log("autofind: watching the first %zu in CSV w0..w8; 'cands' lists them all", std::min<size_t>(3, g_c.size()));
			g_cands = g_c;
			g_state = kIdle;
			// For the double hits: dump the memory around them and list what points into that memory
			// (the start of the owning object/array is usually just below the position).
			int dumped = 0;
			for (size_t i = 0; i < g_c.size() && dumped < 4; ++i) {
				if (!g_c[i].dbl) continue;
				uintptr_t a = g_c[i].addr;
				uint8_t   b[0x400] = {};
				SafeRead(b, a - 0x200, sizeof(b));
				wchar_t name[64];
				swprintf(name, 64, L"%ls_player_%d.bin", g_tag.c_str(), dumped);
				if (FILE* f = _wfopen(Path(name).c_str(), L"wb")) {
					fwrite(b, 1, sizeof(b), f);
					fclose(f);
				}
				Log("autofind: [%zu] %p dumped -0x200..+0x200 to %ls; pointers into it:", i, reinterpret_cast<void*>(a), name);
				for (auto& h : ScanQwords(a - 0x1000, 0x1040, 24, true)) {
					Log("      %s holds %p (= position %+lld)", Rva(h.addr).c_str(), reinterpret_cast<void*>(h.value),
						static_cast<long long>(h.value) - static_cast<long long>(a));
				}
				++dumped;
			}
			Chime(MB_ICONEXCLAMATION);
		}

		inline void Filter(const double* a_now)
		{
			double d[3] = { a_now[0] - g_from[0], a_now[1] - g_from[1], a_now[2] - g_from[2] };
			std::vector<double>  v;
			std::vector<uint8_t> ok;
			ReadMany(g_c, v, ok);
			std::vector<Cand> keep;
			for (size_t i = 0; i < g_c.size(); ++i) {
				const double* n = &v[i * 3];
				const double* f = g_c[i].first;
				if (ok[i] && std::fabs(n[0] - f[0] - d[0]) < 0.25 && std::fabs(n[1] - f[1] - d[1]) < 0.25 && std::fabs(n[2] - f[2] - d[2]) < 0.25) {
					Cand c = g_c[i];
					memcpy(c.first, n, 24);
					keep.push_back(c);
				}
			}
			Log("autofind round %d: reference moved %.2f %.2f %.2f; %zu of %zu candidates moved the same way", g_round + 1, d[0], d[1], d[2],
				keep.size(), g_c.size());
			g_c.swap(keep);
			memcpy(g_from, a_now, 24);
			if (++g_round >= 2 || g_c.size() <= 8) {
				Finish(g_c.empty() ? "nothing followed the reference" : "done");
			} else {
				g_state = kWaitMove;
				Log("autofind: move the player again (> 3 m) and stop");
				Chime(MB_OK);
			}
		}

		// a_side: half-width of the box in X and Z; a_up: half-height in Y (emitters ride on bones above
		// the feet, so the box is tall and narrow).
		inline void Start(uint64_t a_ref, double a_side, double a_up)
		{
			g_ref = a_ref;
			if (!RefPos(g_from)) {
				Log("autofind: reference %llu has no position", static_cast<unsigned long long>(a_ref));
				return;
			}
			double t0 = Now(), tol[3] = { a_side, a_up, a_side };
			g_c = FindTriple(g_from, tol, true, 4000000);
			size_t nd = g_c.size();
			auto   f = FindTriple(g_from, tol, false, 1000000);
			g_c.insert(g_c.end(), f.begin(), f.end());
			Log("autofind: reference %llu at %.3f %.3f %.3f, box +-%.2f/%.2f m: %zu double + %zu float candidates%s (%.1f s). "
				"Now move the player > 3 m and stop.",
				static_cast<unsigned long long>(a_ref), g_from[0], g_from[1], g_from[2], a_side, a_up, nd, f.size(),
				nd >= 4000000 || f.size() >= 1000000 ? " (CAPPED: shrink the box)" : "", Now() - t0);
			g_round = 0;
			g_state = kWaitMove;
			Chime(MB_ICONASTERISK);
		}

		inline void Tick()
		{
			if (g_state == kIdle) return;
			double p[3];
			if (!RefPos(p)) return;
			if (g_state == kWaitMove) {
				if (Dist(p, g_from) > 3.0) {
					g_state = kWaitStop;
					memcpy(g_last, p, 24);
					g_still = Now();
				}
			} else if (Dist(p, g_last) > 0.02) {
				memcpy(g_last, p, 24);
				g_still = Now();
			} else if (Now() - g_still > 0.75) {
				Filter(p);
			}
		}
	}

	// ---- auto recon (AutoRecon=1 in controlbridge.ini) ---------------------------------------------
	// Once per session: when the listener has existed for 10 s and stood still for 3 s, run 'views'
	// and 'autofind' on their own, so a test only needs the player to walk and stop twice.
	namespace autorecon
	{
		inline bool   g_on = false, g_done = false;
		inline double g_firstSeen = 0, g_still = 0, g_last[3] = {};

		inline void Tick()
		{
			autofind::Tick();
			if (!g_on || g_done) return;
			Xf lis;
			if (!Get(ListenerId(), lis)) return;
			double now = Now();
			if (!g_firstSeen) g_firstSeen = now;
			if (Dist(lis.pos, g_last) > 0.05) {
				memcpy(g_last, lis.pos, 24);
				g_still = now;
				return;
			}
			if (now - g_firstSeen < 10.0 || now - g_still < 3.0) return;
			g_done = true;
			Log("auto recon: player standing still, searching camera views and the player's position");
			views::Find(1024);
			auto n = Nearest(lis.pos, 1, 0.5);
			if (n.empty()) {
				Log("auto recon: no emitter near the camera, autofind skipped");
				return;
			}
			autofind::Start(n[0].id, 1.0, 2.5);
		}
	}

	// ---- glue into the probe core ------------------------------------------------------------------
	inline void FillCommon(Row& a_r)
	{
		Xf     lis;
		double t;
		if (Get(ListenerId(), lis, &t)) {
			a_r.flags |= kRowListener;
			memcpy(a_r.lisPos, lis.pos, sizeof(lis.pos));
			memcpy(a_r.lisFront, lis.front, sizeof(lis.front));
			memcpy(a_r.lisTop, lis.top, sizeof(lis.top));
			uint64_t id = g_tracked.load();
			Xf       e;
			if (id == kAuto) {
				auto n = Nearest(lis.pos, 1, 0.5);
				if (!n.empty()) {
					id = n[0].id;
					e = n[0].xf;
				} else {
					id = 0;
				}
			} else if (id && !Get(id, e)) {
				id = 0;
			}
			if (id) {
				a_r.flags |= kRowEmitter;
				a_r.emitterId = id;
				memcpy(a_r.emitterPos, e.pos, sizeof(e.pos));
			}
		}
		views::Fill(a_r);
	}

	inline void SummaryCommon()
	{
		static double s_last = Now();
		double        now = Now(), dt = std::max(1e-3, now - s_last);
		s_last = now;
		char line[512];
		int  p = snprintf(line, sizeof(line), "wwise/s:");
		for (int k = 0; k < kKinds; ++k) {
			uint64_t c = g_calls[k].load();
			p += snprintf(line + p, sizeof(line) - p, " %s %.0f", kKindName[k], (c - g_callsMark[k]) / dt);
			g_callsMark[k] = c;
		}
		Log("%s; render thread %u; objects %zu", line, g_renderTid.load(), g_used);
		uint64_t lid = ListenerId();
		Xf       lis;
		if (Get(lid, lis)) {
			AcquireSRWLockShared(&g_lock);
			int lay = Slot(lid, false) ? Decode(Slot(lid, false)->raw, g_double, lis) : -1;
			ReleaseSRWLockShared(&g_lock);
			Log("listener %llu (layout %d): pos %.3f %.3f %.3f  front %.3f %.3f %.3f  top %.3f %.3f %.3f",
				static_cast<unsigned long long>(lid), lay, lis.pos[0], lis.pos[1], lis.pos[2], lis.front[0], lis.front[1], lis.front[2],
				lis.top[0], lis.top[1], lis.top[2]);
			for (auto& n : Nearest(lis.pos, 6, 1.0)) {
				Log("  emitter %-20llu %7.2f m  pos %.3f %.3f %.3f  updates %llu", static_cast<unsigned long long>(n.id), n.dist, n.xf.pos[0],
					n.xf.pos[1], n.xf.pos[2], static_cast<unsigned long long>(n.count));
			}
		} else {
			Log("listener: none yet (default %u, set %u)", g_nDef, g_nSetLis);
		}
		if (g_used > kObjs * 3 / 4) Compact();
	}

	inline bool CommandCommon(int argc, char** argv)
	{
		const char* c = argv[0];
		if (!_stricmp(c, "emitters")) {  // emitters [n]: nearest to the listener
			Xf lis;
			if (!Get(ListenerId(), lis)) {
				Log("no listener yet");
				return true;
			}
			for (auto& n : Nearest(lis.pos, argc > 1 ? atoi(argv[1]) : 20, 2.0)) {
				Log("  %-20llu %7.2f m  pos %.3f %.3f %.3f  front %.2f %.2f %.2f  updates %llu", static_cast<unsigned long long>(n.id), n.dist,
					n.xf.pos[0], n.xf.pos[1], n.xf.pos[2], n.xf.front[0], n.xf.front[1], n.xf.front[2], static_cast<unsigned long long>(n.count));
			}
		} else if (!_stricmp(c, "track") && argc > 1) {  // track <id> | auto | off
			g_tracked = !_stricmp(argv[1], "auto") ? kAuto : !_stricmp(argv[1], "off") ? 0 : strtoull(argv[1], nullptr, 10);
			Log("tracking emitter %s", argv[1]);
		} else if (!_stricmp(c, "findnear")) {  // findnear [emitter|listener|<id>] [tol]: 'find' at a Wwise object's position
			uint64_t id = ListenerId();
			if (argc > 1 && !_stricmp(argv[1], "emitter")) {
				Xf lis;
				auto n = Get(ListenerId(), lis) ? Nearest(lis.pos, 1, 0.5) : std::vector<Near>{};
				id = g_tracked.load() != kAuto && g_tracked.load() ? g_tracked.load() : (n.empty() ? 0 : n[0].id);
			} else if (argc > 1 && _stricmp(argv[1], "listener")) {
				id = strtoull(argv[1], nullptr, 10);
			}
			Xf x;
			if (!id || !Get(id, x)) {
				Log("findnear: object %llu unknown", static_cast<unsigned long long>(id));
				return true;
			}
			Log("findnear: object %llu", static_cast<unsigned long long>(id));
			FindAndLog(x.pos, argc > 2 ? atof(argv[2]) : 0.05, 4096);
		} else if (!_stricmp(c, "autofind")) {  // autofind [emitter id | listener] [side m] [up m]
			uint64_t ref = 0;
			Xf       lis;
			if (argc > 1 && !_stricmp(argv[1], "listener")) {
				ref = ListenerId();
			} else if (argc > 1 && strtoull(argv[1], nullptr, 10)) {
				ref = strtoull(argv[1], nullptr, 10);
			} else if (Get(ListenerId(), lis)) {
				auto n = Nearest(lis.pos, 1, 0.5);
				if (!n.empty()) ref = n[0].id;
			}
			if (!ref) {
				Log("autofind: no reference object yet");
				return true;
			}
			autofind::Start(ref, argc > 2 ? atof(argv[2]) : 1.0, argc > 3 ? atof(argv[3]) : 2.5);
		} else if (!_stricmp(c, "views")) {  // views [max]
			views::Find(argc > 1 ? strtoull(argv[1], nullptr, 10) : 32);
		} else if (!_stricmp(c, "view") && argc > 1) {  // view <addr>|off [matOff]
			if (!_stricmp(argv[1], "off")) {
				views::g_view = 0;
			} else {
				uintptr_t p = ParseAddr(argv[1]);
				if (argc > 2) views::g_matOff = strtoul(argv[2], nullptr, 16);
				Read(p, views::g_viewVt);
				views::g_view = p;
				Log("tracking view %p ('%s') view-to-world +%x", reinterpret_cast<void*>(p), ClassOf(p).c_str(), views::g_matOff);
			}
		} else {
			return false;
		}
		return true;
	}

	// Hooks every overload of the observed AK::SoundEngine functions exported by a_mod.
	inline void Install(HMODULE a_mod)
	{
		int used[kKinds] = {};
		ForEachExport(a_mod, [&](const char* a_name, void*) {
			if (!strncmp(a_name, kPrefix[kSetPos], strlen(kPrefix[kSetPos])) && strstr(a_name, "AkWorldTransform")) g_double = true;
		});
		Log("wwise: %s positions", g_double ? "double (AkWorldTransform)" : "float (AkTransform)");
		ForEachExport(a_mod, [&](const char* a_name, void* a_addr) {
			for (int k = 0; k < kKinds; ++k) {
				if (strncmp(a_name, kPrefix[k], strlen(kPrefix[k]))) continue;
				if (used[k] >= kOverloads) {
					Log("wwise: too many overloads of %s, %s not hooked", kKindName[k], a_name);
					continue;
				}
				if (Hook(a_addr, kDetours[k][used[k]], reinterpret_cast<void**>(&g_orig[k][used[k]]), a_name)) ++used[k];
			}
		});
		g_game.fillCommon = FillCommon;
		g_game.summaryCommon = SummaryCommon;
		g_game.commandCommon = CommandCommon;
		g_game.workerTick = autorecon::Tick;
		autorecon::g_on = IniInt(L"AutoRecon", 0) != 0;
		Log("auto recon %s", autorecon::g_on ? "on: stand still after loading, then walk > 3 m and stop, twice" : "off");
	}
}
