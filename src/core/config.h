// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>

enum class ReflexMode : int {
	Off   = 0,
	On    = 1,
	Boost = 2,
};

enum class ScalingMode : int {
	Stretch = 0,
	Aspect  = 1,
	Integer = 2,
};

struct DisplayConfig {
	bool        english         = false; // [Language] 0 = Russian, 1 = English (rewrites INI comments on launch)
	ReflexMode  reflex          = ReflexMode::Boost;
	bool        asyncPresent    = true;
	bool        directRenderTargets = false;
	bool        stateCache      = true;
	bool        smartManaged    = true;
	bool        compressedMipCache = false;
	bool        asyncScreenshot = true;
	int         fpsLimit        = 0;    // 0 = off, -1 = auto (Hz - 3 for VRR), >0 = exact FPS cap
	int         bgFpsLimit      = 30;   // 0 = off, >0 = FPS cap when unfocused / minimized
	int         aoQuality       = 0;    // 0 = off, 1 = low, 2 = medium, 3 = high (D3D11 Temporal XeGTAO)
	float       aoStrength      = 0.85f;
	int         aaQuality       = 0;    // 0 = off, 1 = SMAA 1x Med, 2 = SMAA 1x High, 3 = SMAA T2x (Temporal AA)
	int         ssrQuality      = 0;    // 0 = off, 1 = Med (water + rain wet roads), 2 = High (32-step SSR)
	float       giStrength      = 0.0f; // 0.00..1.00 Temporal Screen-Space Global Illumination (SSGI color bounce)
	bool        ssgiDisplayRes  = false;
	bool        rainFx          = false;// Animated rain ripples on SSR puddles + screen water droplets in rain
	bool        rainLensDrops   = true;
	bool        rainLighting    = false;
	int         wheelSpray      = 1;    // 0 = off, 1 = original, 2 = reduced spray in standing water
	float       bloomStrength   = 0.0f; // 0.00..1.00 3D-only Dual-Kawase HDR Bloom
	bool        bloomFlare      = false;// Subtle anamorphic lens flare on bright headlights/sun (3D-only)
	float       fogStrength     = 0.0f; // 0.00..1.00 Volumetric Aerial Perspective / Depth Fog (auto-off in interiors)
	int         tonemapMode     = 0;    // 0 = off, 1 = ACES Filmic, 2 = AgX Punchy
	float       clarity         = 0.0f; // 0.00..1.00 3D-only Local Contrast (Clarity)
	float       sharpness       = 0.0f;
	bool        depthCas        = true; // Restrict CAS / AO / LUT cleanly to 3D world (never touch 2D chat/HUD)
	float       vibrance        = 0.0f;
	int         anisotropy      = 0;    // 0, 2, 4, 8, 16 (forced D3D9 AF)
	bool        deband          = false;// Sky/fog debanding + triangular dithering
	bool        output10Bit     = true; // 10-bit R10G10B10A2_UNORM swapchain when supported
	char        lutName[128]    = {};   // Optional .cube 3D LUT file in ModernDisplay/luts
	float       renderScale     = 1.0f; // 1.00..2.00 SSAA internal supersampling + Catmull-Rom downscale
	ScalingMode scaling         = ScalingMode::Stretch;
	bool        gammaLut        = true;
	bool        overlay         = false;
	bool        forceWindowed   = false;
	bool        windowUiFix     = true;
	// Runtime-only live before/after split-screen wipe (not persisted in INI)
	int         compareMode     = 0;    // 0 = off, 1 = fixed split, 2 = smooth auto-sweep, 3 = mouse follow
	float       comparePos      = 0.50f;// 0.05..0.95 split position in fixed mode
};

struct DisplayTelemetry {
	float       fps           = 0.0f;
	float       frameTimeMs   = 0.0f;
	float       pcLatMs       = 0.0f;
	float       gpuMs         = 0.0f;
	float       osQueueMs     = 0.0f;
	bool        reflexOn      = false;
	bool        iFlip         = false;
	bool        vrr           = false;
	bool        tearing       = false;
	bool        asyncOn       = false;
	bool        realBB        = false;
	const char *lowLatBackend = "OFF";
	UINT        gameW         = 0;
	UINT        gameH         = 0;
	UINT        swapW         = 0;
	UINT        swapH         = 0;
	UINT        refreshHz     = 60;
};

extern DisplayConfig g_cfg;
extern volatile LONG g_cfgSeq;

inline const char *MdTr(const char *ru, const char *en) { return g_cfg.english ? en : ru; }

void ConfigLoad();
void ConfigSave();
void ConfigNotifyChanged();

const char *ReflexModeName(ReflexMode m);
const char *ScalingModeName(ScalingMode m);
void        FormatFpsLimit(int v, char *buf, size_t cap);
