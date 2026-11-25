// ShowcaseDirector - Automated deterministic GTA:SA 1.0 US benchmark & cinema capture ASI
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

struct CVector {
	float x, y, z;
};

static inline CVector LerpV(const CVector &a, const CVector &b, float t) {
	return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

static inline float SmoothStep(float t) {
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

// Catmull-Rom spline over 4 control points (t in 0..1 between p1 and p2)
static inline CVector CatmullRom(const CVector &p0, const CVector &p1, const CVector &p2, const CVector &p3, float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	auto eval = [&](float a, float b, float c, float d) {
		return 0.5f * ((2.0f * b) +
		               (-a + c) * t +
		               (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
		               (-a + 3.0f * b - 3.0f * c + d) * t3);
	};
	return {
		eval(p0.x, p1.x, p2.x, p3.x),
		eval(p0.y, p1.y, p2.y, p3.y),
		eval(p0.z, p1.z, p2.z, p3.z),
	};
}

// Evaluate multi-point smooth spline (N >= 2 points)
static CVector EvalPath(const CVector *pts, int count, float u) {
	if (count <= 0) return { 0, 0, 0 };
	if (count == 1) return pts[0];
	if (u <= 0.0f) return pts[0];
	if (u >= 1.0f) return pts[count - 1];
	float segF = u * (float)(count - 1);
	int seg = (int)segF;
	if (seg >= count - 1) seg = count - 2;
	float localT = segF - (float)seg;
	int i0 = seg > 0 ? seg - 1 : 0;
	int i1 = seg;
	int i2 = seg + 1;
	int i3 = (seg + 2 < count) ? seg + 2 : count - 1;
	return CatmullRom(pts[i0], pts[i1], pts[i2], pts[i3], localT);
}

struct SpawnVeh {
	int   model;
	float x, y, z, headingDeg;
};

struct SpawnPed {
	int   model;
	float x, y, z, headingDeg;
};

struct SceneDef {
	int         id;
	const char *name;
	float       captureDurationSec;
	int         hour;
	int         minute;
	int         weather;
	float       rainAmt;
	float       wetRoads;
	int         eyeCount;
	CVector     eyePts[5];
	int         lookCount;
	CVector     lookPts[5];
	CVector     anchorPos; // streaming center & spawn reference
	bool        heavyCrowd;
	bool        rainNightCars;
};

// 6 carefully framed Los Santos & Las Venturas showcase scenes
static const SceneDef kScenes[6] = {
	// Scene 0: Pershing Square / City Hall Plaza — Downtown LS Crowd & Traffic (ACT I Performance Bench A)
	{
		0, "downtown_pershing", 10.0f, 14, 30, 0, 0.0f, 0.0f,
		4, {
			{ 1479.5f, -1622.0f, 18.2f },
			{ 1480.0f, -1664.0f, 16.4f },
			{ 1498.0f, -1696.0f, 17.5f },
			{ 1528.0f, -1705.0f, 21.0f },
		},
		4, {
			{ 1481.0f, -1725.0f, 16.5f },
			{ 1481.0f, -1748.0f, 17.0f },
			{ 1481.0f, -1760.0f, 18.0f },
			{ 1478.0f, -1745.0f, 16.5f },
		},
		{ 1481.0f, -1690.0f, 14.0f },
		true, false
	},
	// Scene 1: Grove Street Cul-de-Sac — Iconic Hood + Heavy Crowd (ACT I Performance Bench B)
	{
		1, "grove_street", 10.0f, 16, 15, 1, 0.0f, 0.0f,
		4, {
			{ 2436.0f, -1669.0f, 17.2f },
			{ 2466.0f, -1667.0f, 15.6f },
			{ 2491.0f, -1677.0f, 15.4f },
			{ 2504.0f, -1662.0f, 18.2f },
		},
		4, {
			{ 2494.0f, -1668.0f, 14.2f },
			{ 2496.0f, -1669.0f, 14.0f },
			{ 2492.0f, -1682.0f, 14.0f },
			{ 2486.0f, -1670.0f, 14.0f },
		},
		{ 2490.0f, -1670.0f, 13.5f },
		true, false
	},
	// Scene 2: Golden Hour Rodeo / Downtown Architecture — Live Before/After Wipe (ACT II XeGTAO + SSGI + SMAA T2x + Fog)
	{
		2, "golden_downtown", 10.0f, 17, 40, 1, 0.0f, 0.0f,
		3, {
			{ 1435.0f, -1655.0f, 17.5f },
			{ 1475.0f, -1690.0f, 16.2f },
			{ 1518.0f, -1715.0f, 18.0f },
		},
		3, {
			{ 1485.0f, -1755.0f, 18.5f },
			{ 1485.0f, -1750.0f, 17.0f },
			{ 1465.0f, -1735.0f, 16.0f },
		},
		{ 1481.0f, -1715.0f, 14.0f },
		true, false
	},
	// Scene 3: Sunset Mulholland / Vinewood Overlook — Volumetric Aerial Fog + ACES + HDR Bloom
	{
		3, "sunset_skyline", 9.0f, 19, 25, 0, 0.0f, 0.0f,
		3, {
			{ 1385.0f, -825.0f, 82.0f },
			{ 1445.0f, -860.0f, 76.0f },
			{ 1505.0f, -905.0f, 70.0f },
		},
		3, {
			{ 1470.0f, -1380.0f, 42.0f },
			{ 1495.0f, -1420.0f, 38.0f },
			{ 1520.0f, -1460.0f, 35.0f },
		},
		{ 1450.0f, -950.0f, 65.0f },
		false, false
	},
	// Scene 4: Dusk Rainstorm Wet Avenue — World-Space SSR + Bilateral Blur + Rain Ripples + Screen Droplets
	{
		4, "rain_avenue_ssr", 10.0f, 20, 45, 8, 0.85f, 1.0f,
		3, {
			{ 1480.0f, -1586.0f, 15.1f },
			{ 1480.0f, -1635.0f, 14.9f },
			{ 1480.0f, -1690.0f, 15.3f },
		},
		3, {
			{ 1480.0f, -1745.0f, 15.8f },
			{ 1480.0f, -1760.0f, 15.5f },
			{ 1480.0f, -1775.0f, 16.0f },
		},
		{ 1480.0f, -1650.0f, 13.5f },
		false, true
	},
	// Scene 5: Las Venturas Strip Night Rainstorm — CLIMAX DROP (Neon SSR + HDR Bloom + Anamorphic Flares + SSGI + Ripples)
	{
		5, "lv_strip_climax", 13.0f, 23, 30, 8, 0.78f, 1.0f,
		4, {
			{ 2025.0f, 1020.0f, 12.6f },
			{ 2025.0f, 1110.0f, 12.8f },
			{ 2025.0f, 1210.0f, 13.5f },
			{ 2025.0f, 1315.0f, 15.5f },
		},
		4, {
			{ 2025.0f, 1260.0f, 16.0f },
			{ 2025.0f, 1350.0f, 17.0f },
			{ 2025.0f, 1440.0f, 18.0f },
			{ 2025.0f, 1540.0f, 20.0f },
		},
		{ 2025.0f, 1170.0f, 10.8f },
		false, true
	},
};

// ============================================================================
// Configuration & Globals
// ============================================================================
enum RunMode {
	MODE_SMOKE   = 0,
	MODE_BENCH   = 1,
	MODE_CAPTURE = 2,
};

struct JobConfig {
	RunMode mode = MODE_SMOKE;
	char    profile[64] = "md_ultra";
	char    outDir[MAX_PATH] = "C:\\Users\\root\\Desktop\\ModernDisplay\\scratch_showcase\\out";
	char    ffmpegPath[MAX_PATH] = "";
	std::vector<int> scenes;
	float   benchSec = 8.0f;
	int     captureFps = 60;
};

static JobConfig g_job;
static FILE     *g_log = nullptr;
static FILE     *g_csv = nullptr;

static void Logf(const char *fmt, ...) {
	if (!g_log) return;
	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(g_log, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

static void PatchMem(uintptr_t addr, const void *bytes, size_t len) {
	DWORD oldProt = 0;
	VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &oldProt);
	memcpy((void *)addr, bytes, len);
	VirtualProtect((void *)addr, len, oldProt, &oldProt);
}

static void WriteCall(uintptr_t callSite, void *dstFn, uintptr_t *origTargetOut) {
	uint8_t *p = (uint8_t *)callSite;
	if (p[0] == 0xE8) {
		int32_t oldRel = *(int32_t *)(p + 1);
		if (origTargetOut) *origTargetOut = (uintptr_t)(callSite + 5 + oldRel);
		int32_t newRel = (int32_t)((uintptr_t)dstFn - (callSite + 5));
		PatchMem(callSite + 1, &newRel, 4);
	}
}

// ============================================================================
// GTA:SA 1.0 US Native Functions
// ============================================================================
static auto fnCStreaming_RequestModel        = (void(__cdecl *)(int, int))0x4087E0;
static auto fnCStreaming_LoadAllRequestedModels = (void(__cdecl *)(bool))0x40EA10;
static auto fnCStreaming_LoadScene           = (void(__cdecl *)(const CVector *))0x40EB70;
static auto fnCStreaming_LoadSceneCollision  = (void(__cdecl *)(const CVector *))0x40ED80;
static auto fnCWorld_Add                     = (void(__cdecl *)(void *))0x563220;
static auto fnCWorld_Remove                  = (void(__cdecl *)(void *))0x563280;
static auto fnFindGroundZ                    = (float(__cdecl *)(float, float))0x569660;
static auto fnCWeather_ForceWeatherNow       = (void(__cdecl *)(short))0x72A4F0;
static auto fnCClock_SetGameClock            = (void(__cdecl *)(uint8_t, uint8_t, uint8_t))0x52D150;
static auto fnCCamera_SetPosFixed            = (void(__thiscall *)(void *, const CVector *, const CVector *))0x50BEC0;
static auto fnCCamera_TakeControlNoEntity    = (void(__thiscall *)(void *, const CVector *, short, int))0x50C7C0;
static auto fnCCamera_Fade                   = (void(__thiscall *)(void *, float, short))0x50AC20;
static auto fnCPlaceable_SetPosn             = (void(__thiscall *)(void *, float, float, float))0x420B80;
static auto fnCPlaceable_SetHeading          = (void(__thiscall *)(void *, float))0x43E0C0;
static auto fnCPed_OpNew                     = (void *(__cdecl *)(size_t))0x5E4720;
static auto fnCCivilianPed_Ctor              = (void *(__thiscall *)(void *, int, int))0x5DDB70;
static auto fnCVehicle_OpNew                 = (void *(__cdecl *)(size_t))0x6E2D50;
static auto fnCAutomobile_Ctor               = (void *(__thiscall *)(void *, int, uint8_t, uint8_t))0x6B0A90;

static void *TheCamera() { return (void *)0xB6F028; }
static void *FindPlayerPed() { return *(void **)0xB6F5F0; }

// ============================================================================
// Entity Manager (Deterministic Peds & Vehicles per Scene)
// ============================================================================
static std::vector<void *> g_spawnedEntities;

static void DestroyEntitySafe(void *ent) {
	if (!ent) return;
	__try {
		fnCWorld_Remove(ent);
		// Virtual destructor at vtable[0](1) deletes from pool cleanly
		typedef void(__thiscall *Dtor_t)(void *, int);
		void **vt = *(void ***)ent;
		if (vt && vt[0]) {
			((Dtor_t)vt[0])(ent, 1);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ClearSpawnedEntities() {
	for (void *e : g_spawnedEntities) {
		DestroyEntitySafe(e);
	}
	g_spawnedEntities.clear();
}

static void SpawnDeterministicSceneEntities(const SceneDef &sc) {
	ClearSpawnedEntities();

	// Request common vehicle & ped models cleanly
	static const int kCarModels[] = { 411, 560, 562, 415, 596, 420, 429, 402, 541, 451, 567, 536, 492, 412, 477, 506 };
	static const int kPedModels[] = { 105, 106, 107, 102, 103, 104, 21, 22, 23, 28, 29, 46, 47, 59, 60, 93, 98, 124, 170, 240 };

	for (int m : kCarModels) fnCStreaming_RequestModel(m, 2);
	for (int m : kPedModels) fnCStreaming_RequestModel(m, 2);
	fnCStreaming_LoadAllRequestedModels(false);

	// Deterministic LCG
	uint32_t rng = 0x1337C0DEu ^ (uint32_t)(sc.id * 2654435761u);
	auto nextRng = [&]() -> float {
		rng = rng * 1664525u + 1013904223u;
		return (float)((rng >> 8) & 0xFFFFu) / 65535.0f;
	};

	auto spawnCar = [&](int model, float x, float y, float headingDeg, bool lightsOn) {
		__try {
			float gz = fnFindGroundZ(x, y);
			if (gz < 2.0f || gz > 200.0f) gz = sc.anchorPos.z;
			void *v = fnCVehicle_OpNew(0x988);
			if (!v) return;
			fnCAutomobile_Ctor(v, model, 1, 1);
			fnCPlaceable_SetPosn(v, x, y, gz + 0.65f);
			fnCPlaceable_SetHeading(v, headingDeg * 0.0174532925f);
			*(uint8_t *)((uintptr_t)v + 0x428) = 4; // STATUS_ABANDONED
			if (lightsOn) {
				// Force headlights/taillights on via m_nOverrideLights (offset 0x5D4 = 2) and engine on flag
				*(uint8_t *)((uintptr_t)v + 0x428) = 0; // active lights
				*(uint8_t *)((uintptr_t)v + 0x42D) |= 0x10; // bEngineOn
				*(uint8_t *)((uintptr_t)v + 0x5D4) = 2; // FORCE_CAR_LIGHTS_ON
			}
			fnCWorld_Add(v);
			g_spawnedEntities.push_back(v);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Logf("WARN: Exception spawning car model %d", model);
		}
	};

	auto spawnPed = [&](int model, float x, float y, float headingDeg) {
		__try {
			float gz = fnFindGroundZ(x, y);
			if (gz < 2.0f || gz > 200.0f) gz = sc.anchorPos.z;
			void *p = fnCPed_OpNew(0x79C);
			if (!p) return;
			fnCCivilianPed_Ctor(p, 4, model);
			fnCPlaceable_SetPosn(p, x, y, gz + 1.0f);
			float rad = headingDeg * 0.0174532925f;
			fnCPlaceable_SetHeading(p, rad);
			*(float *)((uintptr_t)p + 0x558) = rad;
			*(float *)((uintptr_t)p + 0x55C) = rad;
			fnCWorld_Add(p);
			g_spawnedEntities.push_back(p);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Logf("WARN: Exception spawning ped model %d", model);
		}
	};

	if (sc.id == 0 || sc.id == 2) {
		// Pershing Square / City Hall: 16 cars parked neatly along the boulevard + 45 peds across the plaza
		for (int i = 0; i < 8; ++i) {
			float y = -1640.0f - i * 11.5f;
			spawnCar(kCarModels[i % 16], 1463.5f, y, 180.0f, false);
			spawnCar(kCarModels[(i + 8) % 16], 1497.5f, y, 0.0f, false);
		}
		int nPeds = (sc.id == 0) ? 48 : 26;
		for (int i = 0; i < nPeds; ++i) {
			float x = 1466.0f + nextRng() * 32.0f;
			float y = -1645.0f - nextRng() * 92.0f;
			float h = nextRng() * 360.0f;
			spawnPed(kPedModels[i % 20], x, y, h);
		}
	} else if (sc.id == 1) {
		// Grove Street Cul-de-sac: 14 classic hood cars + 44 homies & peds around the circle
		static const int kGroveCars[] = { 492, 567, 536, 412, 560, 411, 562, 415, 567, 492, 536, 412, 429, 541 };
		for (int i = 0; i < 14; ++i) {
			float ang = (float)i / 14.0f * 6.2831853f;
			float r = 13.5f + (i % 2) * 3.5f;
			float x = 2493.0f + cosf(ang) * r;
			float y = -1669.0f + sinf(ang) * r;
			spawnCar(kGroveCars[i], x, y, ang * 57.2958f + 90.0f, false);
		}
		for (int i = 0; i < 44; ++i) {
			float ang = nextRng() * 6.2831853f;
			float r = 2.0f + nextRng() * 19.0f;
			float x = 2486.0f + cosf(ang) * r - (i % 3 == 0 ? 18.0f : 0.0f);
			float y = -1668.0f + sinf(ang) * r;
			int model = (i % 2 == 0) ? (105 + (i % 3)) : kPedModels[i % 20];
			spawnPed(model, x, y, nextRng() * 360.0f);
		}
	} else if (sc.id == 4) {
		// Dusk Rain Avenue (Pershing Square approach): 14 sports cars with blazing headlights flanking wet avenue!
		for (int i = 0; i < 7; ++i) {
			float y = -1615.0f - i * 16.0f;
			spawnCar(kCarModels[i % 10], 1471.5f, y, 0.0f, true);
			spawnCar(kCarModels[(i + 5) % 10], 1488.5f, y - 6.0f, 180.0f, true);
		}
		for (int i = 0; i < 12; ++i) {
			float x = (i % 2 == 0) ? 1464.0f : 1495.5f;
			float y = -1620.0f - i * 10.0f;
			spawnPed(kPedModels[i % 20], x, y, (i % 2 == 0) ? 270.0f : 90.0f);
		}
	} else if (sc.id == 5) {
		// Las Venturas Strip Night Rainstorm: 18 sports cars lining The Strip with headlights/taillights blazing!
		for (int i = 0; i < 9; ++i) {
			float y = 1055.0f + i * 32.0f;
			spawnCar(kCarModels[i % 10], 2016.5f, y, 0.0f, true);
			spawnCar(kCarModels[(i + 3) % 10], 2033.5f, y + 14.0f, 180.0f, true);
		}
		for (int i = 0; i < 18; ++i) {
			float x = (i % 2 == 0) ? 2006.0f : 2044.0f;
			float y = 1060.0f + i * 16.0f;
			spawnPed(kPedModels[i % 20], x, y, nextRng() * 360.0f);
		}
	}

	Logf("Scene %d (%s): spawned %zu deterministic entities", sc.id, sc.name, g_spawnedEntities.size());
}

// ============================================================================
// Frame Capture Pipe (FFmpeg NVENC or BMP Smoke Dump)
// ============================================================================
static HANDLE   g_ffPipeWrite = nullptr;
static HANDLE   g_ffProc      = nullptr;
static uint32_t g_capW        = 0;
static uint32_t g_capH        = 0;
static bool     g_captureWantFrame = false;
static bool     g_smokeWantBmp     = false;
static char     g_smokeBmpPath[MAX_PATH] = {};
static std::vector<uint8_t> g_frameBuf;

static void StopFfmpegPipe() {
	if (g_ffPipeWrite) {
		CloseHandle(g_ffPipeWrite);
		g_ffPipeWrite = nullptr;
	}
	if (g_ffProc) {
		WaitForSingleObject(g_ffProc, 15000);
		CloseHandle(g_ffProc);
		g_ffProc = nullptr;
	}
}

static bool StartFfmpegPipe(const char *outMp4, uint32_t w, uint32_t h, int fps) {
	StopFfmpegPipe();
	SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
	HANDLE hRead = nullptr, hWrite = nullptr;
	if (!CreatePipe(&hRead, &hWrite, &sa, 16 * 1024 * 1024)) {
		Logf("ERROR: CreatePipe failed (%lu)", GetLastError());
		return false;
	}
	SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
		"\"%s\" -y -hide_banner -loglevel error "
		"-f rawvideo -pix_fmt bgra -s %ux%u -r %d -i pipe:0 "
		"-c:v h264_nvenc -preset p6 -rc constqp -qp 12 -pix_fmt yuv420p -movflags +faststart \"%s\"",
		g_job.ffmpegPath, w, h, fps, outMp4);

	STARTUPINFOA si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = hRead;
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

	PROCESS_INFORMATION pi = {};
	BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(hRead);
	if (!ok) {
		Logf("ERROR: CreateProcess ffmpeg failed (%lu): %s", GetLastError(), cmd);
		CloseHandle(hWrite);
		return false;
	}
	CloseHandle(pi.hThread);
	g_ffPipeWrite = hWrite;
	g_ffProc = pi.hProcess;
	g_capW = w;
	g_capH = h;
	Logf("FFmpeg pipe started: %ux%u @ %d -> %s", w, h, fps, outMp4);
	return true;
}

static void SaveBmp24(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t rowBytes = (w * 3 + 3) & ~3u;
	uint32_t imgSize = rowBytes * h;
	BITMAPFILEHEADER bfh = {};
	bfh.bfType = 0x4D42;
	bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	bfh.bfSize = bfh.bfOffBits + imgSize;
	BITMAPINFOHEADER bih = {};
	bih.biSize = sizeof(bih);
	bih.biWidth = (LONG)w;
	bih.biHeight = -(LONG)h; // top-down
	bih.biPlanes = 1;
	bih.biBitCount = 24;
	bih.biSizeImage = imgSize;
	fwrite(&bfh, 1, sizeof(bfh), f);
	fwrite(&bih, 1, sizeof(bih), f);
	std::vector<uint8_t> row(rowBytes, 0);
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *src = bgra + (size_t)y * w * 4;
		uint8_t *dst = row.data();
		for (uint32_t x = 0; x < w; ++x) {
			dst[x * 3 + 0] = src[x * 4 + 0];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 2];
		}
		fwrite(row.data(), 1, rowBytes, f);
	}
	fclose(f);
	Logf("Saved smoke BMP: %s (%ux%u)", path, w, h);
}

static void OnCapturedBgraFrame(const uint8_t *bgra, uint32_t w, uint32_t h) {
	if (g_smokeWantBmp) {
		g_smokeWantBmp = false;
		SaveBmp24(g_smokeBmpPath, bgra, w, h);
	}
	if (g_captureWantFrame && g_ffPipeWrite && w == g_capW && h == g_capH) {
		g_captureWantFrame = false;
		const uint8_t *p = bgra;
		size_t rem = (size_t)w * h * 4;
		while (rem > 0) {
			DWORD written = 0;
			DWORD chunk = rem > (4 * 1024 * 1024) ? (4 * 1024 * 1024) : (DWORD)rem;
			if (!WriteFile(g_ffPipeWrite, p, chunk, &written, nullptr) || written == 0) break;
			p += written;
			rem -= written;
		}
	}
}

// ============================================================================
// DXGI SwapChain::Present Hook (for ModernDisplay D3D11 Capture & Smoke)
// ============================================================================
typedef HRESULT(STDMETHODCALLTYPE *DxgiPresent_t)(IDXGISwapChain *, UINT, UINT);
static DxgiPresent_t g_origDxgiPresent = nullptr;
static void        **g_dxgiPresentSlot = nullptr;
static ID3D11Texture2D *g_stagingTex = nullptr;
static uint32_t      g_stagingW = 0, g_stagingH = 0;
static DXGI_FORMAT   g_stagingFmt = DXGI_FORMAT_UNKNOWN;
static bool          g_dxgiHookActive = false;

static volatile LONG g_dxgiFrames = 0;
static volatile LONG g_showRasterFrames = 0;

static HRESULT STDMETHODCALLTYPE hkDxgiPresent(IDXGISwapChain *sc, UINT syncInterval, UINT flags) {
	InterlockedIncrement(&g_dxgiFrames);
	if (g_smokeWantBmp || g_captureWantFrame) {
		ID3D11Texture2D *bb = nullptr;
		if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) && bb) {
			D3D11_TEXTURE2D_DESC bd = {};
			bb->GetDesc(&bd);
			ID3D11Device *dev = nullptr;
			bb->GetDevice(&dev);
			ID3D11DeviceContext *ctx = nullptr;
			if (dev) dev->GetImmediateContext(&ctx);
			if (dev && ctx) {
				if (!g_stagingTex || g_stagingW != bd.Width || g_stagingH != bd.Height || g_stagingFmt != bd.Format) {
					if (g_stagingTex) { g_stagingTex->Release(); g_stagingTex = nullptr; }
					D3D11_TEXTURE2D_DESC sd = bd;
					sd.Usage = D3D11_USAGE_STAGING;
					sd.BindFlags = 0;
					sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					sd.MiscFlags = 0;
					sd.SampleDesc.Count = 1;
					sd.SampleDesc.Quality = 0;
					if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_stagingTex))) {
						g_stagingW = bd.Width;
						g_stagingH = bd.Height;
						g_stagingFmt = bd.Format;
					}
				}
				if (g_stagingTex) {
					ctx->CopyResource(g_stagingTex, bb);
					D3D11_MAPPED_SUBRESOURCE mapped = {};
					if (SUCCEEDED(ctx->Map(g_stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
						g_frameBuf.resize((size_t)bd.Width * bd.Height * 4);
						if (bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
							for (uint32_t y = 0; y < bd.Height; ++y) {
								memcpy(g_frameBuf.data() + (size_t)y * bd.Width * 4,
								       (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch,
								       bd.Width * 4);
							}
						} else if (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
							for (uint32_t y = 0; y < bd.Height; ++y) {
								const uint8_t *s = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
								uint8_t *d = g_frameBuf.data() + (size_t)y * bd.Width * 4;
								for (uint32_t x = 0; x < bd.Width; ++x) {
									d[x * 4 + 0] = s[x * 4 + 2];
									d[x * 4 + 1] = s[x * 4 + 1];
									d[x * 4 + 2] = s[x * 4 + 0];
									d[x * 4 + 3] = 255;
								}
							}
						} else if (bd.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
							for (uint32_t y = 0; y < bd.Height; ++y) {
								const uint32_t *s = (const uint32_t *)((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
								uint8_t *d = g_frameBuf.data() + (size_t)y * bd.Width * 4;
								for (uint32_t x = 0; x < bd.Width; ++x) {
									uint32_t p = s[x];
									uint32_t r10 = (p) & 0x3FFu;
									uint32_t g10 = (p >> 10) & 0x3FFu;
									uint32_t b10 = (p >> 20) & 0x3FFu;
									d[x * 4 + 0] = (uint8_t)((b10 * 255u + 511u) / 1023u);
									d[x * 4 + 1] = (uint8_t)((g10 * 255u + 511u) / 1023u);
									d[x * 4 + 2] = (uint8_t)((r10 * 255u + 511u) / 1023u);
									d[x * 4 + 3] = 255;
								}
							}
						}
						ctx->Unmap(g_stagingTex, 0);
						OnCapturedBgraFrame(g_frameBuf.data(), bd.Width, bd.Height);
					}
				}
			}
			if (ctx) ctx->Release();
			if (dev) dev->Release();
			bb->Release();
		}
	}
	return g_origDxgiPresent(sc, syncInterval, flags);
}

static void InstallDxgiPresentHook() {
	if (g_dxgiHookActive) return;
	WNDCLASSA wc = {};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "MDShowcaseDummyDxgi";
	RegisterClassA(&wc);
	HWND hw = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
	if (!hw) return;

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 64;
	sd.BufferDesc.Height = 64;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hw;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	IDXGISwapChain *sc = nullptr;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
	                                            D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx)) && sc) {
		void **vt = *(void ***)sc;
		g_dxgiPresentSlot = &vt[8];
		g_origDxgiPresent = (DxgiPresent_t)vt[8];
		void *fn = (void *)&hkDxgiPresent;
		PatchMem((uintptr_t)g_dxgiPresentSlot, &fn, sizeof(fn));
		g_dxgiHookActive = true;
		Logf("DXGI SwapChain::Present vtable hook installed (orig=%p)", g_origDxgiPresent);
	}
	if (sc) sc->Release();
	if (ctx) ctx->Release();
	if (dev) dev->Release();
	DestroyWindow(hw);
	UnregisterClassA(wc.lpszClassName, wc.hInstance);
}

// ============================================================================
// Vanilla D3D9 Backbuffer Capture (when ModernDisplay.asi is NOT loaded)
// ============================================================================
static IDirect3DSurface9 *g_d3d9SysSurf = nullptr;
static uint32_t           g_d3d9SysW = 0, g_d3d9SysH = 0;

static void CaptureVanillaD3D9Backbuffer() {
	if (!g_smokeWantBmp && !g_captureWantFrame) return;
	IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)0xC97C28;
	if (!dev) return;
	IDirect3DSurface9 *bb = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
	D3DSURFACE_DESC desc = {};
	bb->GetDesc(&desc);
	if (!g_d3d9SysSurf || g_d3d9SysW != desc.Width || g_d3d9SysH != desc.Height) {
		if (g_d3d9SysSurf) { g_d3d9SysSurf->Release(); g_d3d9SysSurf = nullptr; }
		if (SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &g_d3d9SysSurf, nullptr))) {
			g_d3d9SysW = desc.Width;
			g_d3d9SysH = desc.Height;
		}
	}
	if (g_d3d9SysSurf && SUCCEEDED(dev->GetRenderTargetData(bb, g_d3d9SysSurf))) {
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(g_d3d9SysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			g_frameBuf.resize((size_t)desc.Width * desc.Height * 4);
			for (uint32_t y = 0; y < desc.Height; ++y) {
				memcpy(g_frameBuf.data() + (size_t)y * desc.Width * 4,
				       (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch,
				       desc.Width * 4);
			}
			g_d3d9SysSurf->UnlockRect();
			OnCapturedBgraFrame(g_frameBuf.data(), desc.Width, desc.Height);
		}
	}
	bb->Release();
}

// ============================================================================
// Showcase State Machine
// ============================================================================
static uintptr_t g_origScriptsProcess = 0;
static uintptr_t g_origCameraProcess  = 0;
static uintptr_t g_origShowRaster     = 0;

static bool      g_hasModernDisplay   = false;
static bool      g_directorStarted    = false;
static int       g_curSceneListIdx    = -1;
static int       g_sceneWarmupFrames  = 0;
static int       g_sceneCapFrame      = 0;
static int       g_sceneTotalCapFrames = 0;
static bool      g_smokeCaptured      = false;
static LARGE_INTEGER g_qpcFreq        = {};
static LARGE_INTEGER g_sceneBenchStart = {};
static LARGE_INTEGER g_lastFrameQpc   = {};
static int       g_benchFrames        = 0;
static double    g_benchSumMs         = 0.0;

static CVector   g_curEye  = { 1480.0f, -1660.0f, 18.0f };
static CVector   g_curLook = { 1480.0f, -1740.0f, 16.0f };

static void ApplyEnvironment(const SceneDef &sc) {
	fnCClock_SetGameClock((uint8_t)sc.hour, (uint8_t)sc.minute, 0);
	*(uint32_t *)0xB7015C = 999999; // freeze clock progression
	fnCWeather_ForceWeatherNow((short)sc.weather);
	*(float *)0xC81324 = sc.rainAmt;  // CWeather::Rain
	*(float *)0xC81308 = sc.wetRoads; // CWeather::WetRoads
	*(float *)0xC81314 = sc.rainAmt > 0.0f ? 0.55f : 0.0f; // CloudCoverage
}

static void BeginScene(int listIdx) {
	StopFfmpegPipe();
	if (listIdx >= (int)g_job.scenes.size()) {
		Logf("All %zu scenes complete. Exiting cleanly.", g_job.scenes.size());
		if (g_csv) { fclose(g_csv); g_csv = nullptr; }
		ClearSpawnedEntities();
		if (g_log) { fflush(g_log); fclose(g_log); g_log = nullptr; }
		ExitProcess(0);
	}

	g_curSceneListIdx = listIdx;
	int sid = g_job.scenes[listIdx];
	const SceneDef &sc = kScenes[sid];
	Logf("=== Starting Scene %d (%s) [mode=%d profile=%s] ===", sc.id, sc.name, (int)g_job.mode, g_job.profile);

	// Un-fade camera and suppress HUD
	fnCCamera_Fade(TheCamera(), 0.0f, 1);
	*(uint8_t *)0xBA6769 = 0; // bHudOn = 0
	*(uint32_t *)0xBA676C = 2; // m_nRadarMode = 2 (off)

	// Move player safely near scene streaming center (high & invisible) so streaming/LOD/shadows follow scene
	void *player = FindPlayerPed();
	if (player) {
		fnCPlaceable_SetPosn(player, sc.anchorPos.x, sc.anchorPos.y, sc.anchorPos.z + 55.0f);
		// Freeze player velocity & physics
		*(uint32_t *)((uintptr_t)player + 0x40) |= 0x02; // bUsesCollision off / physical flags
	}

	// Preload collision + high-LOD buildings/textures for this exact zone
	fnCStreaming_LoadSceneCollision(&sc.anchorPos);
	fnCStreaming_LoadScene(&sc.anchorPos);
	fnCStreaming_LoadAllRequestedModels(false);

	ApplyEnvironment(sc);
	SpawnDeterministicSceneEntities(sc);

	// Set initial camera frame
	g_curEye = EvalPath(sc.eyePts, sc.eyeCount, 0.0f);
	g_curLook = EvalPath(sc.lookPts, sc.lookCount, 0.0f);
	CVector zeroOff = { 0.0f, 0.0f, 0.0f };
	fnCCamera_SetPosFixed(TheCamera(), &g_curEye, &zeroOff);
	fnCCamera_TakeControlNoEntity(TheCamera(), &g_curLook, 2, 1);

	g_sceneWarmupFrames = 90; // Let streaming, reflections, temporal XeGTAO/SSGI/TAA history settle
	g_sceneCapFrame = 0;
	g_sceneTotalCapFrames = (int)(sc.captureDurationSec * (float)g_job.captureFps + 0.5f);
	g_smokeCaptured = false;
	g_benchFrames = 0;
	g_benchSumMs = 0.0;
	g_sceneBenchStart.QuadPart = 0;
	g_lastFrameQpc.QuadPart = 0;

	// Log actual D3D9 backbuffer dimensions for verification
	IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)0xC97C28;
	uint32_t rw = 1920, rh = 1080;
	if (dev) {
		IDirect3DSurface9 *bb = nullptr;
		if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			D3DSURFACE_DESC bd = {};
			bb->GetDesc(&bd);
			rw = bd.Width;
			rh = bd.Height;
			Logf("Active D3D9 BackBuffer: %ux%u fmt=%u msaa=%u", bd.Width, bd.Height, (unsigned)bd.Format, (unsigned)bd.MultiSampleType);
			bb->Release();
		}
	}

	if (g_job.mode == MODE_CAPTURE) {
		char mp4Path[MAX_PATH];
		snprintf(mp4Path, sizeof(mp4Path), "%s\\cap_%s_scene%d_%s.mp4", g_job.outDir, g_job.profile, sc.id, sc.name);
		// ModernDisplay always outputs at swapchain 1920x1080; Vanilla D3D9 outputs at rw x rh
		uint32_t cw = g_hasModernDisplay ? 1920 : rw;
		uint32_t ch = g_hasModernDisplay ? 1080 : rh;
		StartFfmpegPipe(mp4Path, cw, ch, g_job.captureFps);
	}
}

static void __cdecl hkScriptsProcess() {
	// Keep frame limiter off, keep foreground flag on, skip main menu & cutscenes
	*(uint8_t *)0xBA6794 = 0; // m_bFrameLimiter = 0
	*(uint8_t *)0xC1704C = 0; // RsGlobal frameLimiter = 0
	*(uint32_t *)0x8D621C = 1; // ForegroundApp = 1
	*(uint8_t *)0xBA6769 = 0; // HUD off
	*(uint32_t *)0xBA676C = 2; // Radar off

	void *player = FindPlayerPed();
	if (!player) {
		// Let game finish initial spawn before taking over
		((void(__cdecl *)())g_origScriptsProcess)();
		return;
	}

	// Suppress main.scm once player exists so intro cutscene / missions never interfere!
	// Also abort any active cutscene just in case
	*(uint8_t *)0xB5F851 = 0; // CCutsceneMgr::ms_running = 0
	*(uint8_t *)0xB5F852 = 0;

	if (!g_directorStarted) {
		g_directorStarted = true;
		QueryPerformanceFrequency(&g_qpcFreq);
		g_hasModernDisplay = (GetModuleHandleA("ModernDisplay.asi") != nullptr);
		Logf("Director active! Player=%p ModernDisplay=%d RsGlobal=%dx%d",
		     player, g_hasModernDisplay ? 1 : 0, *(int *)0xC17044, *(int *)0xC17048);

		if (g_job.mode == MODE_BENCH) {
			char csvPath[MAX_PATH];
			snprintf(csvPath, sizeof(csvPath), "%s\\bench_%s.csv", g_job.outDir, g_job.profile);
			g_csv = fopen(csvPath, "w");
			if (g_csv) {
				fprintf(g_csv, "scene_id,scene_name,elapsed_s,frametime_ms\n");
				fflush(g_csv);
			}
		}
		BeginScene(0);
	}
}

static void __fastcall hkCameraProcess(void *thisCam, void *) {
	if (g_directorStarted && g_curSceneListIdx >= 0 && g_curSceneListIdx < (int)g_job.scenes.size()) {
		const SceneDef &sc = kScenes[g_job.scenes[g_curSceneListIdx]];
		ApplyEnvironment(sc);

		// Keep player safely parked above scene
		void *player = FindPlayerPed();
		if (player) {
			fnCPlaceable_SetPosn(player, sc.anchorPos.x, sc.anchorPos.y, sc.anchorPos.z + 55.0f);
			// Zero player velocity so they don't fall
			float *vel = (float *)((uintptr_t)player + 0x44);
			vel[0] = vel[1] = vel[2] = 0.0f;
		}

		float u = 0.0f;
		if (g_sceneWarmupFrames <= 0) {
			if (g_job.mode == MODE_CAPTURE) {
				u = (float)g_sceneCapFrame / (float)(g_sceneTotalCapFrames > 1 ? g_sceneTotalCapFrames - 1 : 1);
			} else if (g_job.mode == MODE_BENCH) {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				if (g_sceneBenchStart.QuadPart == 0) g_sceneBenchStart = now;
				double elapsed = (double)(now.QuadPart - g_sceneBenchStart.QuadPart) / (double)g_qpcFreq.QuadPart;
				u = (float)(elapsed / (double)g_job.benchSec);
			} else { // SMOKE
				u = 0.38f;
			}
			if (u < 0.0f) u = 0.0f;
			if (u > 1.0f) u = 1.0f;
		}

		g_curEye = EvalPath(sc.eyePts, sc.eyeCount, u);
		g_curLook = EvalPath(sc.lookPts, sc.lookCount, u);
		CVector zeroOff = { 0.0f, 0.0f, 0.0f };
		fnCCamera_SetPosFixed(thisCam, &g_curEye, &zeroOff);
		fnCCamera_TakeControlNoEntity(thisCam, &g_curLook, 2, 1);

		// Force smooth 60fps simulation step during deterministic capture so rain ripples / animations match
		if (g_job.mode == MODE_CAPTURE) {
			*(float *)0xB7CB5C = 50.0f / 60.0f; // CTimer::ms_fTimeStep (50.0 = 1 sec in RW units)
		}
	}

	((void(__thiscall *)(void *))g_origCameraProcess)(thisCam);
}

static void __cdecl hkShowRaster(void *camera) {
	InterlockedIncrement(&g_showRasterFrames);
	if (g_directorStarted && g_curSceneListIdx >= 0 && g_curSceneListIdx < (int)g_job.scenes.size()) {
		const SceneDef &sc = kScenes[g_job.scenes[g_curSceneListIdx]];

		if (g_sceneWarmupFrames > 0) {
			--g_sceneWarmupFrames;
		} else {
			if (g_job.mode == MODE_SMOKE && !g_smokeCaptured) {
				snprintf(g_smokeBmpPath, sizeof(g_smokeBmpPath), "%s\\smoke_%s_scene%d_%s.bmp",
				         g_job.outDir, g_job.profile, sc.id, sc.name);
				g_smokeWantBmp = true;
				g_smokeCaptured = true;
			} else if (g_job.mode == MODE_CAPTURE) {
				g_captureWantFrame = true;
			}
		}

		// For Vanilla D3D9, capture backbuffer right BEFORE Present (RsCameraShowRaster)
		if (!g_hasModernDisplay && (g_smokeWantBmp || g_captureWantFrame)) {
			CaptureVanillaD3D9Backbuffer();
		}
	}

	((void(__cdecl *)(void *))g_origShowRaster)(camera);

	// Post-Present timing & scene progression
	if (!g_directorStarted || g_curSceneListIdx < 0 || g_curSceneListIdx >= (int)g_job.scenes.size())
		return;

	const SceneDef &sc = kScenes[g_job.scenes[g_curSceneListIdx]];
	if (g_sceneWarmupFrames > 0) return;

	if (g_job.mode == MODE_SMOKE) {
		if (g_smokeCaptured && !g_smokeWantBmp) {
			BeginScene(g_curSceneListIdx + 1);
		}
	} else if (g_job.mode == MODE_CAPTURE) {
		++g_sceneCapFrame;
		if (g_sceneCapFrame % 120 == 0) {
			Logf("Capture Scene %d: %d / %d frames", sc.id, g_sceneCapFrame, g_sceneTotalCapFrames);
		}
		if (g_sceneCapFrame >= g_sceneTotalCapFrames) {
			BeginScene(g_curSceneListIdx + 1);
		}
	} else if (g_job.mode == MODE_BENCH) {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (g_lastFrameQpc.QuadPart != 0) {
			double ftMs = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
			double elapsed = (double)(now.QuadPart - g_sceneBenchStart.QuadPart) / (double)g_qpcFreq.QuadPart;
			if (g_csv) {
				fprintf(g_csv, "%d,%s,%.5f,%.4f\n", sc.id, sc.name, elapsed, ftMs);
			}
			++g_benchFrames;
			g_benchSumMs += ftMs;
			if (elapsed >= (double)g_job.benchSec) {
				double avgFps = (g_benchSumMs > 0.0) ? ((double)g_benchFrames * 1000.0 / g_benchSumMs) : 0.0;
				Logf("BENCH Scene %d (%s): frames=%d avgFps=%.2f avgMs=%.3f",
				     sc.id, sc.name, g_benchFrames, avgFps, g_benchFrames ? g_benchSumMs / g_benchFrames : 0.0);
				if (g_csv) fflush(g_csv);
				BeginScene(g_curSceneListIdx + 1);
				return;
			}
		} else {
			g_sceneBenchStart = now;
		}
		g_lastFrameQpc = now;
	}
}

// ============================================================================
// Job Config Loader & Startup Patches
// ============================================================================
static void LoadJobConfig() {
	g_job.scenes = { 0, 1, 2, 3, 4, 5 };
	FILE *f = fopen("showcase_job.ini", "r");
	if (!f) return;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = line;
		char *v = eq + 1;
		size_t vl = strlen(v);
		while (vl > 0 && (v[vl - 1] == '\r' || v[vl - 1] == '\n' || v[vl - 1] == ' ')) v[--vl] = 0;
		if (_stricmp(k, "mode") == 0) {
			if (_stricmp(v, "bench") == 0) g_job.mode = MODE_BENCH;
			else if (_stricmp(v, "capture") == 0) g_job.mode = MODE_CAPTURE;
			else g_job.mode = MODE_SMOKE;
		} else if (_stricmp(k, "profile") == 0) {
			strncpy(g_job.profile, v, sizeof(g_job.profile) - 1);
		} else if (_stricmp(k, "out_dir") == 0) {
			strncpy(g_job.outDir, v, sizeof(g_job.outDir) - 1);
		} else if (_stricmp(k, "ffmpeg") == 0) {
			strncpy(g_job.ffmpegPath, v, sizeof(g_job.ffmpegPath) - 1);
		} else if (_stricmp(k, "bench_sec") == 0) {
			g_job.benchSec = (float)atof(v);
			if (g_job.benchSec < 2.0f) g_job.benchSec = 2.0f;
		} else if (_stricmp(k, "capture_fps") == 0) {
			g_job.captureFps = atoi(v);
			if (g_job.captureFps < 30) g_job.captureFps = 60;
		} else if (_stricmp(k, "scenes") == 0) {
			g_job.scenes.clear();
			char *tok = strtok(v, ",");
			while (tok) {
				int sid = atoi(tok);
				if (sid >= 0 && sid < 6) g_job.scenes.push_back(sid);
				tok = strtok(nullptr, ",");
			}
			if (g_job.scenes.empty()) g_job.scenes = { 0 };
		}
	}
	fclose(f);
}

static DWORD g_mainTid = 0;

static void LogMainThreadEip() {
	if (!g_mainTid) return;
	HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainTid);
	if (!ht) return;
	if (SuspendThread(ht) != (DWORD)-1) {
		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (GetThreadContext(ht, &ctx)) {
			HMODULE hm = nullptr;
			char modName[128] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                       (LPCSTR)ctx.Eip, &hm) && hm) {
				GetModuleFileNameA(hm, modName, sizeof(modName));
			}
			uint32_t stk[96] = {};
			__try {
				memcpy(stk, (void *)ctx.Esp, sizeof(stk));
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			char gtaFrames[512] = "";
			for (int k = 0; k < 96; ++k) {
				if (stk[k] >= 0x00401000 && stk[k] <= 0x00858000) {
					char tmp[24];
					snprintf(tmp, sizeof(tmp), " 0x%08X", stk[k]);
					if (strlen(gtaFrames) + strlen(tmp) + 1 < sizeof(gtaFrames)) strcat(gtaFrames, tmp);
				}
			}
			Logf("MainThread EIP=0x%08X (%s) ESP=0x%08X gtaStack=[%s]",
			     (unsigned)ctx.Eip, modName, (unsigned)ctx.Esp, gtaFrames);
		}
		ResumeThread(ht);
	}
	CloseHandle(ht);
}

static DWORD WINAPI InitThread(LPVOID) {
	CreateDirectoryA(g_job.outDir, nullptr);
	char logPath[MAX_PATH];
	snprintf(logPath, sizeof(logPath), "%s\\director_%s.log", g_job.outDir, g_job.profile);
	g_log = fopen(logPath, "w");
	Logf("ShowcaseDirector init: mode=%d profile=%s scenes=%zu", (int)g_job.mode, g_job.profile, g_job.scenes.size());

	// 1. Never pause on unfocus (ForegroundApp always stays 1)
	uint8_t oneByte = 1;
	PatchMem(0x747FB6, &oneByte, 1);
	PatchMem(0x74805A, &oneByte, 1);

	// 2. Disable built-in 14ms Frame Limiter spin-loop completely (fair unlocked FPS in BOTH DX9 and MD!)
	uint8_t nops2[2] = { 0x90, 0x90 };
	PatchMem(0x53E94D, nops2, 2); // NOP jb 0x53E930

	// 3. Disable HUD & After-Fade HUD completely for clean cinema frames
	uint8_t retOp = 0xC3;
	PatchMem(0x58FAE0, &retOp, 1); // CHud::Draw
	PatchMem(0x58D490, &retOp, 1); // CHud::DrawAfterFade

	if (g_job.mode != MODE_BENCH) {
		InstallDxgiPresentHook();
	}

	// 4. Hook CTheScripts::Process, TheCamera.Process, and RsCameraShowRaster
	WriteCall(0x53BFC7, (void *)&hkScriptsProcess, &g_origScriptsProcess);
	WriteCall(0x53C104, (void *)&hkCameraProcess,  &g_origCameraProcess);
	WriteCall(0x53EC01, (void *)&hkShowRaster,     &g_origShowRaster);
	Logf("Hooks installed: Scripts=0x%08X Cam=0x%08X ShowRaster=0x%08X",
	     (unsigned)g_origScriptsProcess, (unsigned)g_origCameraProcess, (unsigned)g_origShowRaster);

	// Monitor boot state and cleanly advance menu -> New Game once gGameState reaches 7
	for (int i = 0; i < 40; ++i) {
		int gs = *(volatile int *)0xC8D4C0;
		uint8_t menuAct = *(volatile uint8_t *)0xBA67A4;
		uint8_t menuStart = *(volatile uint8_t *)0xBA67A8;
		uint32_t fg = *(volatile uint32_t *)0x8D621C;
		uint32_t c920ec = *(volatile uint32_t *)0xC920EC;
		*(uint32_t *)0xBA6814 = 1; // MSAA off
		*(uint8_t *)0xBA6794 = 0;  // FrameLimiter off
		*(uint32_t *)0x8D621C = 1; // ForegroundApp = 1
		if (gs == 7 && menuAct) {
			// Trigger New Game cleanly in FrontEndMenuManager!
			*(uint8_t *)0xBA67A4 = 0; // m_bMenuActive = 0 -> WinMain case 7 transitions cleanly to gGameState = 8!
			*(uint8_t *)0xBA6831 = 1; // m_bWantToRestart = 1
			*(uint8_t *)0xBA67A5 = 0; // m_bWantToLoad = 0
		}
		if (i % 4 == 0) {
			Logf("Boot[%d]: gGameState=%d menuAct=%u menuStart=%u fg=%u c920ec=%u dxgi=%ld sr=%ld player=%p",
			     i, gs, menuAct, menuStart, fg, c920ec, g_dxgiFrames, g_showRasterFrames, FindPlayerPed());
			if (i == 12 || i == 20) LogMainThreadEip();
		}
		if (g_directorStarted) break;
		Sleep(250);
	}
	return 0;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hMod);
		g_mainTid = GetCurrentThreadId();
		LoadJobConfig();
		CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
	}
	return TRUE;
}
