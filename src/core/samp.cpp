// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "samp.h"
#include "config.h"
#include "log.h"
#include "../gfx/async_screenshot.h"
#include "../gfx/d3d9_proxy.h"
#include "../gfx/lut_loader.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct SampOffsets {
	const char *name;
	DWORD entryPoint;
	DWORD netGame;
	DWORD chat;
	DWORD input;
	DWORD chatAddMessage;
	int   netStateOff;
	int   netConnectedVal;
};

const SampOffsets kVersions[] = {
	{ "0.3.7-R1",   0x31DF13, 0x21A0F8, 0x21A0E4, 0x21A0E8, 0x645A0, 0x3BD, 14 },
	{ "0.3.7-R2",   0x3195DD, 0x21A100, 0x21A0EC, 0x21A0F0, 0x64670, 0x3B9, 14 },
	{ "0.3.7-R3-1", 0x0CC4D0, 0x26E8DC, 0x26E8C8, 0x26E8CC, 0x679F0, 0x3CD,  5 },
	{ "0.3.7-R4",   0x0CBCB0, 0x26EA0C, 0x26E9F8, 0x26E9FC, 0x68130, 0x3CD,  5 },
	{ "0.3.7-R5-1", 0x0CBC90, 0x26EB94, 0x26EB80, 0x26EB84, 0x68170, 0x3CD,  5 },
	{ "0.3.DL-R1",  0x0FDB60, 0x2ACA24, 0x2ACA10, 0x2ACA14, 0x67BE0, 0x3CD,  5 },
};

typedef void(__cdecl *SampCmdProc_t)(const char *params);
typedef void(__thiscall *SampAddCmd_t)(void *pInput, const char *name, SampCmdProc_t fn);

BYTE              *g_base = nullptr;
const SampOffsets *g_ver = nullptr;
SampAddCmd_t       g_fnAddCmd = nullptr;
bool               g_mdHasSavedGfx = false;
DisplayConfig      g_mdSavedGfx = {};
bool               g_checked = false;
bool               g_cmdRegistered = false;
DWORD              g_startTick = 0;
DWORD              g_lastCheckTick = 0;
bool               g_greeted = false;
DWORD              g_connectedAt = 0;

bool SafeReadPtr(const void *addr, void **out)
{
	__try {
		*out = *(void *const *)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*out = nullptr;
		return false;
	}
}

bool SafeReadInt(const void *addr, int *out)
{
	__try {
		*out = *(const int *)addr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*out = 0;
		return false;
	}
}

void ChatMsg(const char *msg)
{
	if (!g_base || !g_ver || !msg) return;
	void *chat = nullptr;
	if (!SafeReadPtr(g_base + g_ver->chat, &chat) || !chat) return;

	// SA-MP 0.3.7 CChat expects 8-bit Windows-1251 (CP1251), while our source literals are UTF-8.
	char cp1251[512];
	const char *outMsg = msg;
	bool hasNonAscii = false;
	for (const unsigned char *s = (const unsigned char *)msg; *s; ++s) {
		if (*s >= 0x80) { hasNonAscii = true; break; }
	}
	if (hasNonAscii) {
		wchar_t wide[512];
		int wn = MultiByteToWideChar(CP_UTF8, 0, msg, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));
		if (wn > 0) {
			int cn = WideCharToMultiByte(1251, 0, wide, wn, cp1251, (int)sizeof(cp1251), nullptr, nullptr);
			if (cn > 0) outMsg = cp1251;
		}
	}

	__try {
		auto addMsg = (void(__thiscall *)(void *, DWORD, const char *))(g_base + g_ver->chatAddMessage);
		addMsg(chat, 0xFFFFFFFF, outMsg);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void ChatPrintf(const char *fmt, ...)
{
	char buf[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ChatMsg(buf);
}

SampAddCmd_t FindAddCommandFn(BYTE *base)
{
	__try {
		auto *dos = (IMAGE_DOS_HEADER *)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
		auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

		// CInput::AddCommand signature across SA-MP 0.3.7 R1-R5 & 0.3.DL:
		// push edi; mov edi, [ecx+0x14DC]; cmp edi, 0x90
		static const BYTE kSig[] = { 0x57, 0x8B, 0xB9, 0xDC, 0x14, 0x00, 0x00, 0x81, 0xFF, 0x90, 0x00, 0x00, 0x00 };
		auto *sec = IMAGE_FIRST_SECTION(nt);
		for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
			if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
			DWORD len = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
			if (len < sizeof(kSig)) continue;
			const BYTE *p = base + sec->VirtualAddress;
			for (DWORD i = 0; i + sizeof(kSig) <= len; ++i) {
				if (memcmp(p + i, kSig, sizeof(kSig)) == 0)
					return (SampAddCmd_t)(p + i);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

void __cdecl CmdModernDisplay(const char *params)
{
	const char *p = params ? params : "";
	while (*p == ' ' || *p == '\t') ++p;

	if (!*p) {
		DisplayTelemetry t = GetDisplayTelemetry();
		char fpsStr[16];
		FormatFpsLimit(g_cfg.fpsLimit, fpsStr, sizeof(fpsStr));
		ChatPrintf("{FF6EC7}<" MD_NAME " v" MD_VERSION "> {FFFFFF}FPS: {A8FFB0}%.0f {FFFFFF}| Lat: {A8FFB0}%.1fms {FFFFFF}| iFlip: {A8FFB0}%s {FFFFFF}| %s: {FF6EC7}%s {FFFFFF}| Cap: {A8FFB0}%s",
			t.fps, t.pcLatMs, t.iFlip ? "ON" : "OFF",
			t.lowLatBackend ? t.lowLatBackend : "LowLat",
			ReflexModeName(g_cfg.reflex), fpsStr);
		static const char *kAoNames[4] = { "OFF", "LOW", "MED", "HIGH" };
		static const char *kAaNames[4] = { "OFF", "SMAA", "SMAA+", "T2x" };
		static const char *kSsrNames[3] = { "OFF", "MED", "HIGH" };
		static const char *kTmNames[3] = { "OFF", "ACES", "AgX" };
		int aoIdx = (g_cfg.aoQuality >= 0 && g_cfg.aoQuality <= 3) ? g_cfg.aoQuality : 0;
		int aaIdx = (g_cfg.aaQuality >= 0 && g_cfg.aaQuality <= 3) ? g_cfg.aaQuality : 0;
		int ssrIdx = (g_cfg.ssrQuality >= 0 && g_cfg.ssrQuality <= 2) ? g_cfg.ssrQuality : 0;
		int tmIdx = (g_cfg.tonemapMode >= 0 && g_cfg.tonemapMode <= 2) ? g_cfg.tonemapMode : 0;
		ChatPrintf("{CCCCCC}AO: {FFFFFF}%s(%.2f) {CCCCCC}| AA: {FFFFFF}%s {CCCCCC}| SSR: {FFFFFF}%s {CCCCCC}| GI: {FFFFFF}%.0f%% {CCCCCC}| RainFX: {FFFFFF}%s {CCCCCC}| Bloom: {FFFFFF}%.0f%% {CCCCCC}| Fog: {FFFFFF}%.0f%% {CCCCCC}| TM: {FFFFFF}%s",
			kAoNames[aoIdx], g_cfg.aoStrength, kAaNames[aaIdx], kSsrNames[ssrIdx],
			g_cfg.giStrength * 100.0f, g_cfg.rainFx ? "ON" : "OFF",
			g_cfg.bloomStrength * 100.0f, g_cfg.fogStrength * 100.0f, kTmNames[tmIdx]);
		ChatMsg("{CCCCCC}/md {FFFFFF}ao | aa <0-3> | ssr <0-2> | gi <0-100> | rainfx | bloom | fog | tonemap | clarity | lut | compare | fps | hud");
		return;
	}

	char cmd[32] = {}, arg[32] = {};
	sscanf_s(p, "%31s %31s", cmd, (unsigned)sizeof(cmd), arg, (unsigned)sizeof(arg));

	if (_stricmp(cmd, "fps") == 0 || _stricmp(cmd, "limit") == 0 || _stricmp(cmd, "cap") == 0) {
		if (!arg[0]) {
			g_cfg.fpsLimit = (g_cfg.fpsLimit == 0) ? -1 : 0;
		} else if (_stricmp(arg, "auto") == 0 || _stricmp(arg, "vrr") == 0) {
			g_cfg.fpsLimit = -1;
		} else if (_stricmp(arg, "off") == 0) {
			g_cfg.fpsLimit = 0;
		} else {
			int n = atoi(arg);
			if (n <= 0) g_cfg.fpsLimit = 0;
			else g_cfg.fpsLimit = (n < 20) ? 20 : ((n > 1000) ? 1000 : n);
		}
		ConfigNotifyChanged();
		ConfigSave();
		if (g_cfg.fpsLimit < 0) {
			DisplayTelemetry t = GetDisplayTelemetry();
			UINT hz = t.refreshHz ? t.refreshHz : 60;
			UINT eff = (hz > 40) ? (hz - 3) : 60;
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}FPS Limit: {A8FFB0}AUTO (%u FPS @ %uHz VRR)", eff, hz);
		} else if (g_cfg.fpsLimit == 0) {
			ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}FPS Limit: {A8FFB0}OFF (без лимита)");
		} else {
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}FPS Limit: {A8FFB0}%d FPS", g_cfg.fpsLimit);
		}
	} else if (_stricmp(cmd, "bgfps") == 0 || _stricmp(cmd, "bg") == 0) {
		if (!arg[0]) {
			g_cfg.bgFpsLimit = (g_cfg.bgFpsLimit > 0) ? 0 : 30;
		} else {
			int n = atoi(arg);
			g_cfg.bgFpsLimit = (n <= 0) ? 0 : ((n < 10) ? 10 : ((n > 240) ? 240 : n));
		}
		ConfigNotifyChanged();
		ConfigSave();
		if (g_cfg.bgFpsLimit == 0)
			ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}Background FPS Limit: {A8FFB0}OFF");
		else
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Background FPS Limit: {A8FFB0}%d FPS", g_cfg.bgFpsLimit);
	} else if (_stricmp(cmd, "sharp") == 0 || _stricmp(cmd, "cas") == 0) {
		if (!arg[0]) {
			g_cfg.sharpness = (g_cfg.sharpness > 0.01f) ? 0.0f : 0.35f;
		} else {
			float v = (float)atof(arg);
			if (v > 1.0f) v *= 0.01f;
			g_cfg.sharpness = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}CAS Sharpness: {A8FFB0}%.0f%%", g_cfg.sharpness * 100.0f);
	} else if (_stricmp(cmd, "vibrance") == 0 || _stricmp(cmd, "vib") == 0) {
		if (!arg[0]) {
			g_cfg.vibrance = (g_cfg.vibrance > 0.01f) ? 0.0f : 0.35f;
		} else {
			float v = (float)atof(arg);
			if (v > 1.0f || v < -1.0f) v *= 0.01f;
			g_cfg.vibrance = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Digital Vibrance: {A8FFB0}%+.0f%%", g_cfg.vibrance * 100.0f);
	} else if (_stricmp(cmd, "scale") == 0 || _stricmp(cmd, "scaling") == 0) {
		if (_stricmp(arg, "aspect") == 0)       g_cfg.scaling = ScalingMode::Aspect;
		else if (_stricmp(arg, "integer") == 0) g_cfg.scaling = ScalingMode::Integer;
		else if (_stricmp(arg, "stretch") == 0) g_cfg.scaling = ScalingMode::Stretch;
		else g_cfg.scaling = (ScalingMode)(((int)g_cfg.scaling + 1) % 3);
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Scaling: {A8FFB0}%s", ScalingModeName(g_cfg.scaling));
	} else if (_stricmp(cmd, "reflex") == 0 || _stricmp(cmd, "lowlat") == 0 || _stricmp(cmd, "ll") == 0) {
		if (_stricmp(arg, "off") == 0)        g_cfg.reflex = ReflexMode::Off;
		else if (_stricmp(arg, "on") == 0)    g_cfg.reflex = ReflexMode::On;
		else if (_stricmp(arg, "boost") == 0) g_cfg.reflex = ReflexMode::Boost;
		else g_cfg.reflex = (ReflexMode)(((int)g_cfg.reflex + 1) % 3);
		ConfigNotifyChanged();
		ConfigSave();
		DisplayTelemetry t = GetDisplayTelemetry();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}%s: {A8FFB0}%s",
			t.lowLatBackend ? t.lowLatBackend : "Low-Latency", ReflexModeName(g_cfg.reflex));
	} else if (_stricmp(cmd, "async") == 0) {
		g_cfg.asyncPresent = arg[0] ? (atoi(arg) != 0) : !g_cfg.asyncPresent;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Async Present: {A8FFB0}%s", g_cfg.asyncPresent ? "ON" : "OFF");
	} else if (_stricmp(cmd, "cache") == 0) {
		g_cfg.stateCache = arg[0] ? (atoi(arg) != 0) : !g_cfg.stateCache;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}SSE2 State Cache: {A8FFB0}%s", g_cfg.stateCache ? "ON" : "OFF");
	} else if (_stricmp(cmd, "shot") == 0 || _stricmp(cmd, "f8") == 0) {
		g_cfg.asyncScreenshot = arg[0] ? (atoi(arg) != 0) : !g_cfg.asyncScreenshot;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Async F8 Screenshot: {A8FFB0}%s", g_cfg.asyncScreenshot ? "ON" : "OFF");
	} else if (_stricmp(cmd, "gamma") == 0) {
		g_cfg.gammaLut = arg[0] ? (atoi(arg) != 0) : !g_cfg.gammaLut;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Gamma LUT: {A8FFB0}%s", g_cfg.gammaLut ? "ON" : "OFF");
	} else if (_stricmp(cmd, "ao") == 0 || _stricmp(cmd, "gtao") == 0) {
		if (arg[0] && (strchr(arg, '.') != nullptr || strchr(arg, ',') != nullptr)) {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float s = (float)atof(arg);
			g_cfg.aoStrength = (s < 0.1f) ? 0.1f : (s > 2.0f ? 2.0f : s);
			if (g_cfg.aoQuality == 0) g_cfg.aoQuality = 2;
			ConfigNotifyChanged();
			ConfigSave();
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}XeGTAO Strength: {A8FFB0}%.2f", g_cfg.aoStrength);
		} else {
			if (!arg[0]) g_cfg.aoQuality = (g_cfg.aoQuality + 1) % 4;
			else if (_stricmp(arg, "off") == 0) g_cfg.aoQuality = 0;
			else if (_stricmp(arg, "low") == 0) g_cfg.aoQuality = 1;
			else if (_stricmp(arg, "med") == 0 || _stricmp(arg, "medium") == 0) g_cfg.aoQuality = 2;
			else if (_stricmp(arg, "high") == 0) g_cfg.aoQuality = 3;
			else { int n = atoi(arg); g_cfg.aoQuality = (n < 0) ? 0 : (n > 3 ? 3 : n); }
			ConfigNotifyChanged();
			ConfigSave();
			static const char *kAoNames[4] = { "OFF", "LOW", "MEDIUM", "HIGH" };
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}D3D11 XeGTAO: {A8FFB0}%s (strength %.2f)", kAoNames[g_cfg.aoQuality], g_cfg.aoStrength);
		}
	} else if (_stricmp(cmd, "aostr") == 0 || _stricmp(cmd, "aostrength") == 0 || _stricmp(cmd, "strength") == 0) {
		if (!arg[0]) {
			g_cfg.aoStrength = (g_cfg.aoStrength >= 1.45f) ? 0.70f : (g_cfg.aoStrength + 0.25f);
		} else {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float s = (float)atof(arg);
			if (s > 5.0f) s *= 0.01f;
			g_cfg.aoStrength = (s < 0.1f) ? 0.1f : (s > 2.0f ? 2.0f : s);
		}
		if (g_cfg.aoQuality == 0) g_cfg.aoQuality = 2;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}XeGTAO Strength: {A8FFB0}%.2f", g_cfg.aoStrength);
	} else if (_stricmp(cmd, "aa") == 0 || _stricmp(cmd, "smaa") == 0) {
		if (!arg[0]) g_cfg.aaQuality = (g_cfg.aaQuality + 1) % 4;
		else if (_stricmp(arg, "off") == 0) g_cfg.aaQuality = 0;
		else if (_stricmp(arg, "med") == 0 || _stricmp(arg, "medium") == 0 || _stricmp(arg, "on") == 0) g_cfg.aaQuality = 1;
		else if (_stricmp(arg, "high") == 0) g_cfg.aaQuality = 2;
		else if (_stricmp(arg, "taa") == 0 || _stricmp(arg, "t2x") == 0 || _stricmp(arg, "ultra") == 0) g_cfg.aaQuality = 3;
		else { int n = atoi(arg); g_cfg.aaQuality = (n < 0) ? 0 : (n > 3 ? 3 : n); }
		ConfigNotifyChanged();
		ConfigSave();
		static const char *kAaNames[4] = { "OFF", "SMAA 1x MEDIUM", "SMAA 1x HIGH + DEPTH/AO", "SMAA T2x (TEMPORAL AA)" };
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}3D Anti-Aliasing: {A8FFB0}%s", kAaNames[g_cfg.aaQuality]);
	} else if (_stricmp(cmd, "taa") == 0 || _stricmp(cmd, "t2x") == 0) {
		bool enable = arg[0] ? (atoi(arg) != 0) : (g_cfg.aaQuality != 3);
		g_cfg.aaQuality = enable ? 3 : 2;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Temporal AA (SMAA T2x): {A8FFB0}%s", enable ? "ON" : "OFF (SMAA 1x High)");
	} else if (_stricmp(cmd, "ssr") == 0) {
		if (!arg[0]) g_cfg.ssrQuality = (g_cfg.ssrQuality + 1) % 3;
		else if (_stricmp(arg, "off") == 0) g_cfg.ssrQuality = 0;
		else if (_stricmp(arg, "med") == 0 || _stricmp(arg, "medium") == 0 || _stricmp(arg, "on") == 0) g_cfg.ssrQuality = 1;
		else if (_stricmp(arg, "high") == 0 || _stricmp(arg, "ultra") == 0) g_cfg.ssrQuality = 2;
		else { int n = atoi(arg); g_cfg.ssrQuality = (n < 0) ? 0 : (n > 2 ? 2 : n); }
		ConfigNotifyChanged();
		ConfigSave();
		static const char *kSsrNames[3] = { "OFF", "MEDIUM (Water + Rain Roads)", "HIGH (32-step SSR)" };
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Screen-Space Reflections: {A8FFB0}%s", kSsrNames[g_cfg.ssrQuality]);
	} else if (_stricmp(cmd, "gi") == 0 || _stricmp(cmd, "ssgi") == 0) {
		if (!arg[0]) {
			g_cfg.giStrength = (g_cfg.giStrength > 0.01f) ? 0.0f : 0.35f;
		} else if (_stricmp(arg, "off") == 0) {
			g_cfg.giStrength = 0.0f;
		} else {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float v = (float)atof(arg);
			if (v > 1.0f) v *= 0.01f;
			g_cfg.giStrength = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Temporal SSGI (Color Bounce): {A8FFB0}%.0f%%", g_cfg.giStrength * 100.0f);
	} else if (_stricmp(cmd, "rainfx") == 0 || _stricmp(cmd, "drops") == 0 || _stricmp(cmd, "rain") == 0) {
		g_cfg.rainFx = arg[0] ? (atoi(arg) != 0) : !g_cfg.rainFx;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Rain Ripples & Screen Droplets: {A8FFB0}%s", g_cfg.rainFx ? "ON" : "OFF");
	} else if (_stricmp(cmd, "bloom") == 0) {
		if (!arg[0]) {
			g_cfg.bloomStrength = (g_cfg.bloomStrength > 0.01f) ? 0.0f : 0.30f;
		} else if (_stricmp(arg, "off") == 0) {
			g_cfg.bloomStrength = 0.0f;
		} else {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float v = (float)atof(arg);
			if (v > 1.0f) v *= 0.01f;
			g_cfg.bloomStrength = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}3D HDR Bloom: {A8FFB0}%.0f%% {CCCCCC}(Flare: %s)", g_cfg.bloomStrength * 100.0f, g_cfg.bloomFlare ? "ON" : "OFF");
	} else if (_stricmp(cmd, "flare") == 0 || _stricmp(cmd, "flares") == 0) {
		g_cfg.bloomFlare = arg[0] ? (atoi(arg) != 0) : !g_cfg.bloomFlare;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Anamorphic Flares: {A8FFB0}%s", g_cfg.bloomFlare ? "ON" : "OFF");
	} else if (_stricmp(cmd, "fog") == 0 || _stricmp(cmd, "aerial") == 0) {
		if (!arg[0]) {
			g_cfg.fogStrength = (g_cfg.fogStrength > 0.01f) ? 0.0f : 0.25f;
		} else if (_stricmp(arg, "off") == 0) {
			g_cfg.fogStrength = 0.0f;
		} else {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float v = (float)atof(arg);
			if (v > 1.0f) v *= 0.01f;
			g_cfg.fogStrength = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Volumetric Aerial Fog: {A8FFB0}%.0f%%", g_cfg.fogStrength * 100.0f);
	} else if (_stricmp(cmd, "tonemap") == 0 || _stricmp(cmd, "tm") == 0) {
		if (!arg[0]) g_cfg.tonemapMode = (g_cfg.tonemapMode + 1) % 3;
		else if (_stricmp(arg, "off") == 0) g_cfg.tonemapMode = 0;
		else if (_stricmp(arg, "aces") == 0 || _stricmp(arg, "on") == 0) g_cfg.tonemapMode = 1;
		else if (_stricmp(arg, "agx") == 0) g_cfg.tonemapMode = 2;
		else { int n = atoi(arg); g_cfg.tonemapMode = (n < 0) ? 0 : (n > 2 ? 2 : n); }
		ConfigNotifyChanged();
		ConfigSave();
		static const char *kTmNames[3] = { "OFF", "ACES Filmic", "AgX Punchy" };
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Tonemapping: {A8FFB0}%s", kTmNames[g_cfg.tonemapMode]);
	} else if (_stricmp(cmd, "clarity") == 0) {
		if (!arg[0]) {
			g_cfg.clarity = (g_cfg.clarity > 0.01f) ? 0.0f : 0.25f;
		} else if (_stricmp(arg, "off") == 0) {
			g_cfg.clarity = 0.0f;
		} else {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float v = (float)atof(arg);
			if (v > 1.0f) v *= 0.01f;
			g_cfg.clarity = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}3D Clarity (Local Contrast): {A8FFB0}%.0f%%", g_cfg.clarity * 100.0f);
	} else if (_stricmp(cmd, "af") == 0 || _stricmp(cmd, "anisotropy") == 0) {
		if (!arg[0]) {
			g_cfg.anisotropy = (g_cfg.anisotropy == 0) ? 16 : 0;
		} else {
			int af = atoi(arg);
			if (af <= 0) g_cfg.anisotropy = 0;
			else if (af <= 2) g_cfg.anisotropy = 2;
			else if (af <= 4) g_cfg.anisotropy = 4;
			else if (af <= 8) g_cfg.anisotropy = 8;
			else g_cfg.anisotropy = 16;
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Anisotropic Filtering: {A8FFB0}%dx", g_cfg.anisotropy);
	} else if (_stricmp(cmd, "deband") == 0) {
		g_cfg.deband = arg[0] ? (atoi(arg) != 0) : !g_cfg.deband;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Sky/Fog Debanding: {A8FFB0}%s", g_cfg.deband ? "ON" : "OFF");
	} else if (_stricmp(cmd, "lut") == 0) {
		if (!arg[0] || _stricmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
			g_cfg.lutName[0] = 0;
			ConfigNotifyChanged();
			ConfigSave();
			ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}3D LUT: {A8FFB0}OFF");
		} else if (_stricmp(arg, "list") == 0) {
			auto list = ListAvailableCubeLuts();
			if (list.empty()) {
				ChatMsg("{FF6EC7}<" MD_NAME "> {CCCCCC}Нет .cube файлов в папке ModernDisplay\\luts\\");
			} else {
				std::string joined;
				for (size_t i = 0; i < list.size() && joined.size() < 200; ++i) {
					if (i > 0) joined += ", ";
					joined += list[i];
				}
				ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}LUTs: {A8FFB0}%s", joined.c_str());
			}
		} else {
			strncpy_s(g_cfg.lutName, arg, _TRUNCATE);
			ConfigNotifyChanged();
			ConfigSave();
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}3D LUT: {A8FFB0}%s", g_cfg.lutName);
		}
	} else if (_stricmp(cmd, "ssaa") == 0 || _stricmp(cmd, "res") == 0) {
		if (!arg[0]) {
			g_cfg.renderScale = (g_cfg.renderScale > 1.05f) ? 1.0f : 1.5f;
		} else {
			float sc = (float)atof(arg);
			if (sc > 10.0f) sc *= 0.01f;
			g_cfg.renderScale = (sc < 1.0f) ? 1.0f : (sc > 2.0f ? 2.0f : sc);
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}SSAA Render Scale: {A8FFB0}%.2fx", g_cfg.renderScale);
	} else if (_stricmp(cmd, "managed") == 0 || _stricmp(cmd, "smart") == 0) {
		g_cfg.smartManaged = arg[0] ? (atoi(arg) != 0) : !g_cfg.smartManaged;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Smart Managed Textures: {A8FFB0}%s", g_cfg.smartManaged ? "ON" : "OFF");
	} else if (_stricmp(cmd, "depthcas") == 0 || _stricmp(cmd, "2dfix") == 0) {
		g_cfg.depthCas = arg[0] ? (atoi(arg) != 0) : !g_cfg.depthCas;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}2D UI Protection (CAS/AO/LUT): {A8FFB0}%s", g_cfg.depthCas ? "ON" : "OFF");
	} else if (_stricmp(cmd, "10bit") == 0 || _stricmp(cmd, "10b") == 0) {
		g_cfg.output10Bit = arg[0] ? (atoi(arg) != 0) : !g_cfg.output10Bit;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}10-bit Color Output: {A8FFB0}%s", g_cfg.output10Bit ? "ON (10-bit)" : "OFF (8-bit)");
	} else if (_stricmp(cmd, "uifix") == 0 || _stricmp(cmd, "mousefix") == 0) {
		g_cfg.windowUiFix = arg[0] ? (atoi(arg) != 0) : !g_cfg.windowUiFix;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Window/4:3 UI & Mouse Fix: {A8FFB0}%s", g_cfg.windowUiFix ? "ON" : "OFF");
	} else if (_stricmp(cmd, "windowed") == 0 || _stricmp(cmd, "win") == 0) {
		g_cfg.forceWindowed = arg[0] ? (atoi(arg) != 0) : !g_cfg.forceWindowed;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Startup Windowed: {A8FFB0}%s {CCCCCC}(переключить прямо сейчас: Alt+Enter)", g_cfg.forceWindowed ? "ON" : "OFF");
	} else if (_stricmp(cmd, "compare") == 0 || _stricmp(cmd, "split") == 0 || _stricmp(cmd, "wipe") == 0) {
		if (!arg[0]) {
			g_cfg.compareMode = (g_cfg.compareMode + 1) % 4;
		} else if (_stricmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
			g_cfg.compareMode = 0;
		} else if (_stricmp(arg, "auto") == 0 || _stricmp(arg, "sweep") == 0) {
			g_cfg.compareMode = 2;
		} else if (_stricmp(arg, "mouse") == 0 || _stricmp(arg, "cursor") == 0) {
			g_cfg.compareMode = 3;
		} else if (_stricmp(arg, "on") == 0 || _stricmp(arg, "fixed") == 0) {
			g_cfg.compareMode = 1;
		} else {
			for (char *c = arg; *c; ++c) if (*c == ',') *c = '.';
			float v = (float)atof(arg);
			if (v > 1.0f) v *= 0.01f;
			g_cfg.comparePos = (v < 0.05f) ? 0.05f : (v > 0.95f ? 0.95f : v);
			g_cfg.compareMode = 1;
		}
		ConfigNotifyChanged();
		if (g_cfg.compareMode == 0) {
			ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}Before/After Split: {A8FFB0}OFF");
		} else if (g_cfg.compareMode == 1) {
			ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Before/After Split: {A8FFB0}FIXED %.0f%% {CCCCCC}(Alt+Left/Right сдвиг, Alt+F9 режим)", g_cfg.comparePos * 100.0f);
		} else if (g_cfg.compareMode == 2) {
			ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}Before/After Split: {A8FFB0}AUTO-SWEEP {CCCCCC}(Alt+F9 режим)");
		} else {
			ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}Before/After Split: {A8FFB0}MOUSE FOLLOW {CCCCCC}(Alt+F9 режим)");
		}
	} else if (_stricmp(cmd, "off") == 0) {
		const bool anyGfxOn =
			g_cfg.aoQuality > 0 || g_cfg.aaQuality > 0 || g_cfg.ssrQuality > 0 ||
			g_cfg.giStrength > 0.001f || g_cfg.rainFx || g_cfg.bloomStrength > 0.001f ||
			g_cfg.bloomFlare || g_cfg.fogStrength > 0.001f || g_cfg.tonemapMode > 0 ||
			g_cfg.clarity > 0.001f || g_cfg.sharpness > 0.001f || (g_cfg.vibrance > 0.001f || g_cfg.vibrance < -0.001f) ||
			g_cfg.anisotropy > 0 || g_cfg.deband || g_cfg.renderScale > 1.01f || g_cfg.lutName[0] != 0;
		if (anyGfxOn) {
			g_mdSavedGfx = g_cfg;
			g_mdHasSavedGfx = true;
		}
		g_cfg.aoQuality     = 0;
		g_cfg.aaQuality     = 0;
		g_cfg.ssrQuality    = 0;
		g_cfg.giStrength    = 0.0f;
		g_cfg.rainFx        = false;
		g_cfg.bloomStrength = 0.0f;
		g_cfg.bloomFlare    = false;
		g_cfg.fogStrength   = 0.0f;
		g_cfg.tonemapMode   = 0;
		g_cfg.clarity       = 0.0f;
		g_cfg.sharpness     = 0.0f;
		g_cfg.vibrance      = 0.0f;
		g_cfg.anisotropy    = 0;
		g_cfg.deband        = false;
		g_cfg.renderScale   = 1.0f;
		g_cfg.lutName[0]    = 0;
		g_cfg.compareMode   = 0;
		ConfigNotifyChanged();
		ConfigSave();
		ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}Вся графика: {FF9090}OFF {CCCCCC}(только оптимизации FPS; вернуть: /md on)");
	} else if (_stricmp(cmd, "on") == 0) {
		if (g_mdHasSavedGfx) {
			g_cfg.aoQuality     = g_mdSavedGfx.aoQuality;
			g_cfg.aoStrength    = g_mdSavedGfx.aoStrength;
			g_cfg.aaQuality     = g_mdSavedGfx.aaQuality;
			g_cfg.ssrQuality    = g_mdSavedGfx.ssrQuality;
			g_cfg.giStrength    = g_mdSavedGfx.giStrength;
			g_cfg.rainFx        = g_mdSavedGfx.rainFx;
			g_cfg.bloomStrength = g_mdSavedGfx.bloomStrength;
			g_cfg.bloomFlare    = g_mdSavedGfx.bloomFlare;
			g_cfg.fogStrength   = g_mdSavedGfx.fogStrength;
			g_cfg.tonemapMode   = g_mdSavedGfx.tonemapMode;
			g_cfg.clarity       = g_mdSavedGfx.clarity;
			g_cfg.sharpness     = g_mdSavedGfx.sharpness;
			g_cfg.vibrance      = g_mdSavedGfx.vibrance;
			g_cfg.anisotropy    = g_mdSavedGfx.anisotropy;
			g_cfg.deband        = g_mdSavedGfx.deband;
			g_cfg.renderScale   = g_mdSavedGfx.renderScale;
			strncpy_s(g_cfg.lutName, g_mdSavedGfx.lutName, _TRUNCATE);
		} else {
			g_cfg.aoQuality     = 2;
			g_cfg.aoStrength    = 0.85f;
			g_cfg.aaQuality     = 3;
			g_cfg.ssrQuality    = 1;
			g_cfg.giStrength    = 0.35f;
			g_cfg.rainFx        = true;
			g_cfg.bloomStrength = 0.28f;
			g_cfg.bloomFlare    = true;
			g_cfg.fogStrength   = 0.18f;
			g_cfg.tonemapMode   = 1;
			g_cfg.clarity       = 0.03f;
			g_cfg.anisotropy    = 16;
			g_cfg.deband        = true;
		}
		ConfigNotifyChanged();
		ConfigSave();
		ChatMsg("{FF6EC7}<" MD_NAME "> {FFFFFF}Вся графика: {A8FFB0}ON {CCCCCC}(выключить: /md off)");
	} else if (_stricmp(cmd, "hud") == 0 || _stricmp(cmd, "overlay") == 0) {
		g_cfg.overlay = arg[0] ? (atoi(arg) != 0) : !g_cfg.overlay;
		ConfigNotifyChanged();
		ConfigSave();
		ChatPrintf("{FF6EC7}<" MD_NAME "> {FFFFFF}Telemetry HUD: {A8FFB0}%s", g_cfg.overlay ? "ON" : "OFF");
	} else {
		ChatMsg("{FF6EC7}<" MD_NAME "> {CCCCCC}Команды: /md on | off | ao | aa | ssr | gi | bloom | fog | tonemap | ssaa | fps | hud");
	}
}

void TryRegisterCommand()
{
	if (g_cmdRegistered || !g_base || !g_ver || !g_fnAddCmd) return;
	void *pInput = nullptr;
	if (!SafeReadPtr(g_base + g_ver->input, &pInput) || !pInput) return;

	// Sanity check m_nCommandCount at +0x14DC before calling CInput::AddCommand
	int cmdCount = 0;
	if (!SafeReadInt((BYTE *)pInput + 0x14DC, &cmdCount) || cmdCount < 1 || cmdCount >= 140)
		return;

	__try {
		g_fnAddCmd(pInput, "md", CmdModernDisplay);
		g_cmdRegistered = true;
		Log("SA-MP: /md command registered");
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_fnAddCmd = nullptr;
	}
}

void IdentifySamp()
{
	if (g_checked) return;
	DWORD now = GetTickCount();
	if (!g_startTick) g_startTick = now;
	if (g_lastCheckTick && now - g_lastCheckTick < 500) return;
	g_lastCheckTick = now;

	HMODULE mod = GetModuleHandleA("samp.dll");
	if (!mod) {
		if (now - g_startTick > 30000) g_checked = true;
		return;
	}
	g_checked = true;

	BYTE *base = (BYTE *)mod;
	auto *dos = (IMAGE_DOS_HEADER *)base;
	auto *nt  = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	DWORD ep  = nt->OptionalHeader.AddressOfEntryPoint;

	for (const auto &v : kVersions) {
		if (v.entryPoint == ep) {
			g_base = base;
			g_ver = &v;
			g_fnAddCmd = FindAddCommandFn(base);
			Log("SA-MP %s detected (cmdHook=%d)", v.name, g_fnAddCmd ? 1 : 0);
			return;
		}
	}
}

} // namespace

void SampTick()
{
	// Fast-path throttle once SA-MP detection/command registration/greeting has settled
	// so we never pay per-frame SEH __try/__except overhead at 1000+ FPS.
	static DWORD s_lastStatePoll = 0;
	DWORD now = GetTickCount();
	if (g_checked && (!g_ver || (g_cmdRegistered && g_greeted && (now - s_lastStatePoll) < 500)))
		return;
	s_lastStatePoll = now;

	IdentifySamp();
	if (!g_base || !g_ver) return;

	TryInstallAsyncScreenshotHook();

	if (!g_cmdRegistered)
		TryRegisterCommand();

	void *netRaw = nullptr;
	int netState = 0;
	bool connected = SafeReadPtr(g_base + g_ver->netGame, &netRaw) && netRaw &&
	                 SafeReadInt((BYTE *)netRaw + g_ver->netStateOff, &netState) &&
	                 netState == g_ver->netConnectedVal;
	if (!connected) {
		g_greeted = false;
		g_connectedAt = 0;
		return;
	}
	if (g_greeted) return;

	if (!g_connectedAt) g_connectedAt = now;
	if (now - g_connectedAt < 3000) return;

	void *chat = nullptr;
	if (!SafeReadPtr(g_base + g_ver->chat, &chat) || !chat) return;

	g_greeted = true;
	ChatMsg("{FF6EC7}<" MD_NAME " v" MD_VERSION "> {FFFFFF}by {FF6EC7}" MD_AUTHOR " {FFFFFF}loaded! {CCCCCC}(/md)");
}
