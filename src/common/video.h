// Skips intro videos (SkipIntros=1). Both games play videos through Bink (bink2w64.dll): Control
// with BinkOpen(name, flags), Resonant with BinkOpenWithOptions(name, options, flags), reading the
// files through their own packfile callbacks.
//
// Test 17: refusing to open Control's intro (data_pc/textures/videos/intro/startvideos.tex, a
// video *texture*) crashed its renderer (access violation in d3d_rmdwin10_f.dll) a few seconds
// later. So the video is opened normally and then sought to its last frame (BinkGoto; HBINK starts
// {Width, Height, Frames, FrameNum}): every handle and texture stays valid, and the intro is over at
// once. Every open is logged with its name, so the skip list (SkipVideos=comma,separated,substrings
// in controlbridge.ini; case-insensitive) can be tuned.
#pragma once

#include <cctype>

#include "probe.h"

namespace cb::video
{
	using OpenFn = void* (*)(const char*, uintptr_t, uintptr_t);  // both overloads: name first, integers/pointers after
	using GotoFn = void (*)(void*, uint32_t, int32_t);
	inline OpenFn                   g_openOrig = nullptr, g_openOptOrig = nullptr;
	inline GotoFn                   g_goto = nullptr;
	inline bool                     g_skip = false;
	inline std::vector<std::string> g_patterns;
	inline std::atomic<uint32_t>    g_opened{ 0 }, g_skipped{ 0 };

	inline std::string Lower(std::string a_s)
	{
		for (char& c : a_s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		return a_s;
	}

	inline bool Decide(const char* a_name, const char* a_api)
	{
		char buf[260] = {};
		if (a_name) SafeRead(buf, reinterpret_cast<uintptr_t>(a_name), sizeof(buf) - 1);
		std::string n = Lower(buf);
		bool        skip = false;
		if (g_skip) {
			for (auto& p : g_patterns) {
				if (!p.empty() && n.find(p) != std::string::npos) skip = true;
			}
		}
		g_opened.fetch_add(1);
		if (skip) g_skipped.fetch_add(1);
		Log("video: %s(\"%s\") at %.1f s: %s", a_api, buf, Now(), skip ? "skipping to the end" : "played");
		return skip;
	}

	// Seeks a freshly opened video to its last frame.
	inline void ToEnd(void* a_bink)
	{
		uint32_t frames = 0;
		if (!a_bink || !g_goto || !Read(reinterpret_cast<uintptr_t>(a_bink) + 8, frames) || frames < 2) return;
		__try {
			g_goto(a_bink, frames, 0);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Log("video: BinkGoto failed");
			return;
		}
		uint32_t now = 0;
		Read(reinterpret_cast<uintptr_t>(a_bink) + 12, now);
		Log("video: sought to frame %u of %u", now, frames);
	}

	inline void* OpenDetour(const char* a_name, uintptr_t a_b, uintptr_t a_c)
	{
		bool  skip = Decide(a_name, "BinkOpen");
		void* h = g_openOrig(a_name, a_b, a_c);
		if (skip) ToEnd(h);
		return h;
	}

	inline void* OpenOptDetour(const char* a_name, uintptr_t a_b, uintptr_t a_c)
	{
		bool  skip = Decide(a_name, "BinkOpenWithOptions");
		void* h = g_openOptOrig(a_name, a_b, a_c);
		if (skip) ToEnd(h);
		return h;
	}

	inline void Install()
	{
		g_skip = IniInt(L"SkipIntros", 0) != 0;
		std::wstring w = IniString(L"SkipVideos", L"logo,intro,legal,splash,studio,remedy,505,annapurna,epilepsy,warning,nvidia,amd,dolby,trailer");
		std::string  s;
		for (wchar_t ch : w) s += static_cast<char>(ch < 128 ? ch : '?');  // the patterns are ASCII
		g_patterns.clear();
		for (size_t a = 0; a <= s.size();) {
			size_t b = s.find(',', a);
			if (b == std::string::npos) b = s.size();
			std::string p = Lower(s.substr(a, b - a));
			while (!p.empty() && p.front() == ' ') p.erase(p.begin());
			while (!p.empty() && p.back() == ' ') p.pop_back();
			if (!p.empty()) g_patterns.push_back(p);
			a = b + 1;
		}
		Log("video: intro skip %s (%zu patterns)", g_skip ? "on" : "off (videos still logged)", g_patterns.size());
		HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
		if (!bink) {
			Log("video: bink2w64.dll not loaded");
			return;
		}
		g_goto = reinterpret_cast<GotoFn>(GetProcAddress(bink, "BinkGoto"));
		Hook(reinterpret_cast<void*>(GetProcAddress(bink, "BinkOpen")), reinterpret_cast<void*>(&OpenDetour), reinterpret_cast<void**>(&g_openOrig),
			"bink2w64!BinkOpen");
		Hook(reinterpret_cast<void*>(GetProcAddress(bink, "BinkOpenWithOptions")), reinterpret_cast<void*>(&OpenOptDetour),
			reinterpret_cast<void**>(&g_openOptOrig), "bink2w64!BinkOpenWithOptions");
	}
}
