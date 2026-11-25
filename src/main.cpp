// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "gfx/d3d9_proxy.h"

typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
typedef HMODULE(WINAPI *LoadLibraryA_t)(LPCSTR);
typedef FARPROC(WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef BOOL(WINAPI *ClipCursor_t)(const RECT *);
typedef BOOL(WINAPI *ScreenToClient_t)(HWND, LPPOINT);
typedef BOOL(WINAPI *ClientToScreen_t)(HWND, LPPOINT);
typedef LRESULT(WINAPI *DispatchMessageA_t)(const MSG *);
typedef LRESULT(WINAPI *DispatchMessageW_t)(const MSG *);

static bool IsWindows10OrNewer()
{
	typedef LONG(WINAPI *RtlGetVersion_t)(OSVERSIONINFOEXW *);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) return false;
	auto fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
	if (!fn) return false;
	OSVERSIONINFOEXW vi = {};
	vi.dwOSVersionInfoSize = sizeof(vi);
	return fn(&vi) == 0 && vi.dwMajorVersion >= 10;
}

static Direct3DCreate9_t   g_origDirect3DCreate9 = nullptr;
static Direct3DCreate9Ex_t g_fnDirect3DCreate9Ex = nullptr;
static LoadLibraryA_t      g_origLoadLibraryA = nullptr;
static GetProcAddress_t    g_origGetProcAddress = nullptr;
static SetCursorPos_t      g_origSetCursorPos = nullptr;
static ClipCursor_t        g_origClipCursor = nullptr;
static GetClientRect_t     g_origGetClientRect = nullptr;
static ScreenToClient_t    g_origScreenToClient = nullptr;
static ClientToScreen_t    g_origClientToScreen = nullptr;
static DispatchMessageA_t  g_origDispatchMessageA = nullptr;
static DispatchMessageW_t  g_origDispatchMessageW = nullptr;
static bool                g_d3d9Hooked = false;
static bool                g_d3d9Incompatible = false;

namespace {

struct ModRange {
	uintptr_t start;
	uintptr_t end;
	bool      isSystem;
};

constexpr LONG kMaxModRanges = 64;
ModRange       g_modRanges[kMaxModRanges] = {};
volatile LONG  g_modRangeCount = 0;
SRWLOCK        g_modRangeLock = SRWLOCK_INIT;
wchar_t        g_winDirW[MAX_PATH] = {};

DWORD SafeGetImageSize(HMODULE mod)
{
	__try {
		const BYTE *base = (const BYTE *)mod;
		auto *dos = (const IMAGE_DOS_HEADER *)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
		auto *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
		return nt->OptionalHeader.SizeOfImage;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

bool IsSystemModulePath(HMODULE mod)
{
	if (!mod) return false;
	if (mod == g_module) return true;
	wchar_t path[MAX_PATH] = {};
	if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;

	size_t winLen = wcslen(g_winDirW);
	if (winLen == 0 || _wcsnicmp(path, g_winDirW, winLen) != 0 ||
	    (path[winLen] != L'\\' && path[winLen] != L'/'))
		return false;

	// Treat d3dx9_* (if ever calling GetClientRect) as game/UI caller, everything else under C:\Windows as OS/driver.
	const wchar_t *baseName = wcsrchr(path, L'\\');
	baseName = baseName ? baseName + 1 : path;
	if (_wcsnicmp(baseName, L"d3dx9", 5) == 0)
		return false;
	return true;
}

bool IsSystemModuleCaller(void *retAddr)
{
	const uintptr_t addr = (uintptr_t)retAddr;
	LONG n = InterlockedCompareExchange(&g_modRangeCount, 0, 0);
	for (LONG i = 0; i < n; ++i) {
		if (addr >= g_modRanges[i].start && addr < g_modRanges[i].end)
			return g_modRanges[i].isSystem;
	}

	HMODULE mod = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)retAddr, &mod) || !mod) {
		return false; // Dynamic/JIT/mod trampoline code -> game caller
	}

	const bool isSys = IsSystemModulePath(mod);
	const DWORD imgSize = SafeGetImageSize(mod);
	if (imgSize > 0) {
		const uintptr_t start = (uintptr_t)mod;
		const uintptr_t end   = start + imgSize;
		AcquireSRWLockExclusive(&g_modRangeLock);
		LONG cur = g_modRangeCount;
		bool exists = false;
		for (LONG i = 0; i < cur; ++i) {
			if (g_modRanges[i].start == start) { exists = true; break; }
		}
		if (!exists && cur < kMaxModRanges) {
			g_modRanges[cur] = { start, end, isSys };
			InterlockedIncrement(&g_modRangeCount);
		}
		ReleaseSRWLockExclusive(&g_modRangeLock);
	}
	return isSys;
}

} // namespace

static inline bool IsClientMouseMsg(UINT msg)
{
	return (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK) ||
	       (msg >= WM_XBUTTONDOWN && msg <= WM_XBUTTONDBLCLK);
}

static BOOL WINAPI hkGetClientRect(HWND hwnd, LPRECT lpRect)
{
	BOOL ok = g_origGetClientRect ? g_origGetClientRect(hwnd, lpRect) : FALSE;
	if (ok && lpRect && hwnd && hwnd == GetActiveGameWindow() && NeedsVirtualClientMapping() && !IsSystemModuleCaller(_ReturnAddress())) {
		UINT gw = 0, gh = 0;
		if (GetActiveGameSize(gw, gh)) {
			lpRect->left   = 0;
			lpRect->top    = 0;
			lpRect->right  = (LONG)gw;
			lpRect->bottom = (LONG)gh;
		}
	}
	return ok;
}

static BOOL WINAPI hkScreenToClient(HWND hwnd, LPPOINT lpPoint)
{
	BOOL ok = g_origScreenToClient ? g_origScreenToClient(hwnd, lpPoint) : FALSE;
	if (ok && lpPoint && hwnd && hwnd == GetActiveGameWindow() && NeedsVirtualClientMapping() && !IsSystemModuleCaller(_ReturnAddress()))
		MapPhysicalClientToGame(lpPoint->x, lpPoint->y);
	return ok;
}

static BOOL WINAPI hkClientToScreen(HWND hwnd, LPPOINT lpPoint)
{
	if (!g_origClientToScreen) return FALSE;
	if (lpPoint && hwnd && hwnd == GetActiveGameWindow() && NeedsVirtualClientMapping() && !IsSystemModuleCaller(_ReturnAddress())) {
		POINT pt = *lpPoint;
		MapGameToPhysicalClient(pt.x, pt.y);
		if (!g_origClientToScreen(hwnd, &pt)) return FALSE;
		*lpPoint = pt;
		return TRUE;
	}
	return g_origClientToScreen(hwnd, lpPoint);
}

static LRESULT WINAPI hkDispatchMessageA(const MSG *lpMsg)
{
	if (lpMsg && IsClientMouseMsg(lpMsg->message) && lpMsg->hwnd == GetActiveGameWindow() && NeedsVirtualClientMapping()) {
		MSG m = *lpMsg;
		LONG mx = (LONG)(SHORT)LOWORD(m.lParam);
		LONG my = (LONG)(SHORT)HIWORD(m.lParam);
		MapPhysicalClientToGame(mx, my);
		if (mx < -32768) mx = -32768; else if (mx > 32767) mx = 32767;
		if (my < -32768) my = -32768; else if (my > 32767) my = 32767;
		m.lParam = MAKELPARAM((WORD)(SHORT)mx, (WORD)(SHORT)my);
		return g_origDispatchMessageA ? g_origDispatchMessageA(&m) : 0;
	}
	return g_origDispatchMessageA ? g_origDispatchMessageA(lpMsg) : 0;
}

static LRESULT WINAPI hkDispatchMessageW(const MSG *lpMsg)
{
	if (lpMsg && IsClientMouseMsg(lpMsg->message) && lpMsg->hwnd == GetActiveGameWindow() && NeedsVirtualClientMapping()) {
		MSG m = *lpMsg;
		LONG mx = (LONG)(SHORT)LOWORD(m.lParam);
		LONG my = (LONG)(SHORT)HIWORD(m.lParam);
		MapPhysicalClientToGame(mx, my);
		if (mx < -32768) mx = -32768; else if (mx > 32767) mx = 32767;
		if (my < -32768) my = -32768; else if (my > 32767) my = 32767;
		m.lParam = MAKELPARAM((WORD)(SHORT)mx, (WORD)(SHORT)my);
		return g_origDispatchMessageW ? g_origDispatchMessageW(&m) : 0;
	}
	return g_origDispatchMessageW ? g_origDispatchMessageW(lpMsg) : 0;
}

static BOOL WINAPI hkSetCursorPos(int x, int y)
{
	HWND gw = GetActiveGameWindow();
	if (gw && (IsGameWindowDeactivating() || IsIconic(gw) || GetForegroundWindow() != gw))
		return TRUE;
	return g_origSetCursorPos ? g_origSetCursorPos(x, y) : TRUE;
}

static BOOL WINAPI hkClipCursor(const RECT *lpRect)
{
	HWND gw = GetActiveGameWindow();
	if (lpRect && gw) {
		bool degenerate = (lpRect->right - lpRect->left <= 4) || (lpRect->bottom - lpRect->top <= 4);
		if (degenerate || IsGameWindowDeactivating() || IsIconic(gw) || GetForegroundWindow() != gw)
			return g_origClipCursor ? g_origClipCursor(nullptr) : TRUE;
	}
	return g_origClipCursor ? g_origClipCursor(lpRect) : TRUE;
}

static DWORD g_origGameProcess = 0;
static DWORD g_origRenderEffects = 0;
static DWORD g_origWheelSpray = 0;
static DWORD g_origClumpRender = 0;
static void *__cdecl hkEntityClumpRender(void *clump)
{
	auto render = reinterpret_cast<RenderRwObject>(g_origClumpRender);
	if (g_cfg.rainFx && g_cfg.ssrQuality) {
		const BYTE *player = *reinterpret_cast<const BYTE *const *>(0xB6F5F0);
		if (player && *reinterpret_cast<void *const *>(player + 0x18) == clump)
			return RenderPlayerClump(clump, render);
	}
	return render(clump);
}

static DWORD g_origWeaponAtomicRender = 0;
static void *__cdecl hkWeaponAtomicRender(void *atomic)
{
	auto render = reinterpret_cast<RenderRwObject>(g_origWeaponAtomicRender);
	const BYTE *player = *reinterpret_cast<const BYTE *const *>(0xB6F5F0);
	const void *owner = *reinterpret_cast<void *const *>(static_cast<const BYTE *>(atomic) + 0x3C);
	if (player && owner && *reinterpret_cast<void *const *>(player + 0x4F4) == owner)
		return RenderEquippedAtomic(atomic, render);
	return render(atomic);
}

static DWORD g_origRainHaze = 0;
static DWORD g_origRainSplash = 0;

static void __cdecl hkGameProcess()
{
	NotifyInputSample();
	((void(__cdecl *)())g_origGameProcess)();
}

static void __cdecl hkRenderEffects()
{
	((void(__cdecl *)())g_origRenderEffects)();
	NotifyBefore2dStuff();
}

struct WheelPosition {
	float x, y, z;
};

// GTA SA 1.0 US, FxPrtMult_c and the position overload of FxSystem_c::AddParticle.
struct RainParticleMult {
	float red, green, blue, alpha, size, rotation, life;
};
using AddRainParticle = void(__thiscall *)(void *, WheelPosition *, WheelPosition *, float,
                                          RainParticleMult *, float, float, float, unsigned char);

struct RainParticleBudget {
	DWORD lastTick = 0;
	float credits = 1.0f;

	bool Take(float perSecond, float burst)
	{
		const DWORD now = GetTickCount();
		if (now != lastTick) {
			const float replenished = credits + (now - lastTick) * (perSecond * .001f);
			credits = replenished < burst ? replenished : burst;
			lastTick = now;
		}
		if (credits < 1.0f) return false;
		credits -= 1.0f;
		return true;
	}
};

static void __fastcall hkRainHaze(void *fx, void *, WheelPosition *pos, WheelPosition *velocity,
                                 float age, RainParticleMult *mult, float rotation,
                                 float brightness, float localTime, unsigned char flags)
{
	auto emit = reinterpret_cast<AddRainParticle>(g_origRainHaze);
	if (!g_cfg.rainFx || !mult || !pos) {
		emit(fx, pos, velocity, age, mult, rotation, brightness, localTime, flags);
		return;
	}
	// Same camera origin used by CWeather::AddRain when it places the haze.
	const BYTE *matrix = *reinterpret_cast<const BYTE *const *>(0xB6F03C);
	const auto *camera = reinterpret_cast<const WheelPosition *>(matrix ? matrix + 0x30 : (const BYTE *)0xB6F02C);
	const float dx = pos->x - camera->x, dy = pos->y - camera->y;
	const float distance2 = dx * dx + dy * dy;
	if (distance2 <= 144.0f || mult->alpha <= 0.0f) return;
	static RainParticleBudget budget;
	if (!budget.Take(12.0f, 1.0f)) return;
	float fade = (distance2 - 144.0f) / (400.0f - 144.0f);
	if (fade > 1.0f) fade = 1.0f;
	auto mist = *mult;
	mist.alpha *= fade * .16f;
	mist.size *= .55f;
	emit(fx, pos, velocity, age, &mist, rotation, brightness, localTime, flags);
}

static void __fastcall hkRainSplash(void *fx, void *, WheelPosition *pos, WheelPosition *velocity,
                                   float age, RainParticleMult *mult, float rotation,
                                   float brightness, float localTime, unsigned char flags)
{
	auto emit = reinterpret_cast<AddRainParticle>(g_origRainSplash);
	if (!g_cfg.rainFx || !mult) {
		emit(fx, pos, velocity, age, mult, rotation, brightness, localTime, flags);
		return;
	}
	static RainParticleBudget budget;
	if (!budget.Take(160.0f, 10.0f)) return;
	auto splash = *mult;
	splash.alpha *= .65f;
	splash.size *= .75f;
	emit(fx, pos, velocity, age, &splash, rotation, brightness, localTime, flags);
}

static void __fastcall hkWheelSpray(void *fx, void *, void *vehicle, WheelPosition position,
                                   unsigned char spinning, unsigned char inWater, float brightness)
{
	const int mode = g_cfg.wheelSpray;
	if (mode == 0) {
		static bool logged = false;
		if (!logged) { logged = true; Log("Graphics: wheel spray suppressed"); }
		return;
	}
	if (mode == 2 && inWater) {
		struct VehicleSpray {
			void *vehicle;
			DWORD lastBurst;
			unsigned count;
		};
		static VehicleSpray recent[256] = {};
		auto &entry = recent[(reinterpret_cast<uintptr_t>(vehicle) >> 4) & 255u];
		const DWORD now = GetTickCount();
		if (entry.vehicle != vehicle || now - entry.lastBurst >= 33) {
			entry = { vehicle, now, 0 };
		}
		if (entry.count >= 2) {
			static bool logged = false;
			if (!logged) { logged = true; Log("Graphics: water spray rate limit active"); }
			return;
		}
		++entry.count;
	}
	using AddWheelSpray = void(__thiscall *)(void *, void *, WheelPosition, unsigned char, unsigned char, float);
	((AddWheelSpray)g_origWheelSpray)(fx, vehicle, position, spinning, inWater, brightness);
}

static bool IsSystemD3D9(HMODULE mod, wchar_t *pathOut, DWORD cap)
{
	if (!mod || !GetModuleFileNameW(mod, pathOut, cap)) return false;

	wchar_t sysDir[MAX_PATH] = {}, wowDir[MAX_PATH] = {};
	GetSystemDirectoryW(sysDir, MAX_PATH);
	GetSystemWow64DirectoryW(wowDir, MAX_PATH);

	auto startsWithI = [](const wchar_t *s, const wchar_t *prefix) -> bool {
		size_t n = wcslen(prefix);
		return n > 0 && _wcsnicmp(s, prefix, n) == 0 && (s[n] == L'\\' || s[n] == L'/');
	};
	return startsWithI(pathOut, sysDir) || startsWithI(pathOut, wowDir);
}

static IDirect3D9 *WINAPI hkDirect3DCreate9(UINT sdkVersion)
{
	if (g_fnDirect3DCreate9Ex && !g_d3d9Incompatible) {
		IDirect3D9Ex *d3d9Ex = nullptr;
		HRESULT hr = g_fnDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
		if (SUCCEEDED(hr) && d3d9Ex) {
			Log("Hook: Direct3DCreate9(%u) upgraded to IDirect3D9Ex", sdkVersion);
			return CreateProxyDirect3D9(d3d9Ex, g_origDirect3DCreate9, sdkVersion);
		}
		Log("Hook: Direct3DCreate9Ex failed (0x%08lX) - falling back to native D3D9", hr);
	}
	return g_origDirect3DCreate9 ? g_origDirect3DCreate9(sdkVersion) : nullptr;
}

static void TryHookD3D9(HMODULE mod)
{
	if (g_d3d9Hooked || g_d3d9Incompatible || !mod) return;

	wchar_t path[MAX_PATH] = {};
	if (!IsSystemD3D9(mod, path, MAX_PATH)) {
		g_d3d9Incompatible = true;
		char utf8[MAX_PATH * 2] = {};
		WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
		Log("Compatibility: non-system d3d9.dll detected (%s) - bridge disabled", utf8);
		return;
	}

	auto fn9   = (Direct3DCreate9_t)GetProcAddress(mod, "Direct3DCreate9");
	auto fn9Ex = (Direct3DCreate9Ex_t)GetProcAddress(mod, "Direct3DCreate9Ex");
	if (!fn9 || !fn9Ex) {
		g_d3d9Incompatible = true;
		Log("Compatibility: Direct3DCreate9Ex unavailable in d3d9.dll");
		return;
	}
	g_fnDirect3DCreate9Ex = fn9Ex;

	void *tramp = nullptr;
	if (HookHotpatchFn((void *)fn9, (void *)hkDirect3DCreate9, &tramp)) {
		g_origDirect3DCreate9 = (Direct3DCreate9_t)tramp;
		g_d3d9Hooked = true;
		Log("Hook: system d3d9!Direct3DCreate9 hooked");
	} else {
		g_origDirect3DCreate9 = fn9;
		g_d3d9Hooked = true;
		Log("Hook: using GetProcAddress hook for Direct3DCreate9");
	}
}

static HMODULE WINAPI hkLoadLibraryA(LPCSTR name)
{
	HMODULE mod = g_origLoadLibraryA(name);
	if (mod && name && !g_d3d9Hooked && !g_d3d9Incompatible) {
		const char *base = strrchr(name, '\\');
		base = base ? base + 1 : name;
		if (_stricmp(base, "d3d9.dll") == 0 || _stricmp(base, "d3d9") == 0)
			TryHookD3D9(mod);
	}
	return mod;
}

static FARPROC WINAPI hkGetProcAddress(HMODULE mod, LPCSTR proc)
{
	if (!g_d3d9Incompatible && proc && ((uintptr_t)proc >> 16) != 0 && strcmp(proc, "Direct3DCreate9") == 0) {
		TryHookD3D9(mod);
		if (g_d3d9Hooked && !g_d3d9Incompatible)
			return (FARPROC)hkDirect3DCreate9;
	}
	return g_origGetProcAddress(mod, proc);
}

static void HookGtaInputMarker()
{
	auto tryHookCall = [](DWORD site, DWORD expectedTarget, void *detour, DWORD *origOut, const char *label) {
		MEMORY_BASIC_INFORMATION mbi = {};
		if (!VirtualQuery((void *)(uintptr_t)site, &mbi, sizeof(mbi)) ||
		    mbi.State != MEM_COMMIT ||
		    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return;
		BYTE *p = (BYTE *)(uintptr_t)site;
		if (p[0] == 0xE8) {
			int32_t rel = 0;
			memcpy(&rel, p + 1, 4);
			if ((DWORD)(site + 5 + rel) == expectedTarget) {
				if (HookCallSite(site, detour, origOut))
					Log("%s hooked at 0x%08lX", label, site);
			}
		}
	};

	// GTA:SA 1.0 US: CGame::Process call in Idle (0x53E981 -> 0x53BEE0), right before CPad::UpdatePads.
	tryHookCall(0x53E981, 0x53BEE0, (void *)hkGameProcess, &g_origGameProcess, "Reflex: input marker");
	// GTA:SA 1.0 US: RenderEffects + CPostEffects::Render call in Idle (0x53EAD3 -> 0x53E170).
	// samp.dll overwrites 0x53EB12 (Render2dStuff), so hooking 0x53EAD3 and running NotifyBefore2dStuff()
	// right after it returns guarantees we capture the complete 3D color + INTZ depth buffer BEFORE
	// both SA-MP (chat/dialogs/nametags) and GTA SA (HUD/radar) draw any 2D UI.
	tryHookCall(0x53EAD3, 0x53E170, (void *)hkRenderEffects, &g_origRenderEffects, "Graphics: 3D/2D boundary");
	tryHookCall(0x6D2DA8, 0x49FB30, (void *)hkWheelSpray, &g_origWheelSpray, "Graphics: wheel spray in water");
	tryHookCall(0x6D2FE7, 0x49FB30, (void *)hkWheelSpray, &g_origWheelSpray, "Graphics: wheel spray on roads");
	tryHookCall(0x53439C, 0x749B20, (void *)hkEntityClumpRender, &g_origClumpRender, "Rain: player reflection");
	// RenderWeaponCB submits the positioned atomic through this call in both GTA and SilentPatch.
	tryHookCall(0x7336D7, 0x7491C0, (void *)hkWeaponAtomicRender, &g_origWeaponAtomicRender, "Rain: equipped weapon");
	tryHookCall(0x72AF59, 0x4AA440, (void *)hkRainHaze, &g_origRainHaze, "Rain: camera haze");
	tryHookCall(0x72AD55, 0x4AA440, (void *)hkRainSplash, &g_origRainSplash, "Rain: surface splashes");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		g_module = inst;
		GetModuleFileNameA(inst, g_dir, MAX_PATH);
		if (char *s = strrchr(g_dir, '\\')) *s = 0;
		GetModuleFileNameW(inst, g_dirW, MAX_PATH);
		if (wchar_t *s = wcsrchr(g_dirW, L'\\')) *s = 0;

		LogInit();
		Log(MD_NAME " v" MD_VERSION " - (c) 2026 " MD_AUTHOR " (" MD_AUTHOR_URL ")");
		ConfigLoad();

		if (!IsWindows10OrNewer()) {
			g_d3d9Incompatible = true;
			Log("Compatibility: Windows 10 or newer is required for DXGI Flip Model - staying passive");
			return TRUE;
		}

		// Check game executable directory (works even if the .asi is inside a scripts/ subfolder).
		wchar_t exeDirW[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exeDirW, MAX_PATH);
		if (wchar_t *s = wcsrchr(exeDirW, L'\\')) *s = 0;
		wchar_t localD3D9[MAX_PATH];
		swprintf_s(localD3D9, L"%ls\\d3d9.dll", exeDirW);
		if (GetFileAttributesW(localD3D9) != INVALID_FILE_ATTRIBUTES) {
			g_d3d9Incompatible = true;
			Log("Compatibility: local d3d9.dll found in game folder (DXVK/ENB/ReShade) - staying passive");
			return TRUE;
		}

		GetWindowsDirectoryW(g_winDirW, MAX_PATH);
		HMODULE exe = GetModuleHandleA(nullptr);
		PatchIat(exe, "kernel32.dll", "LoadLibraryA", (void *)hkLoadLibraryA, (void **)&g_origLoadLibraryA);
		PatchIat(exe, "kernel32.dll", "GetProcAddress", (void *)hkGetProcAddress, (void **)&g_origGetProcAddress);
		if (HMODULE u32 = GetModuleHandleA("user32.dll")) {
			void *fn = (void *)GetProcAddress(u32, "SetCursorPos");
			void *tramp = nullptr;
			if (fn && HookHotpatchFn(fn, (void *)hkSetCursorPos, &tramp)) {
				g_origSetCursorPos = (SetCursorPos_t)tramp;
				SetRawCursorPosFn(g_origSetCursorPos);
			} else if (PatchIat(exe, "user32.dll", "SetCursorPos", (void *)hkSetCursorPos, (void **)&g_origSetCursorPos)) {
				SetRawCursorPosFn(g_origSetCursorPos);
			}
			void *fnClip = (void *)GetProcAddress(u32, "ClipCursor");
			void *trampClip = nullptr;
			if (fnClip && HookHotpatchFn(fnClip, (void *)hkClipCursor, &trampClip))
				g_origClipCursor = (ClipCursor_t)trampClip;
			else
				PatchIat(exe, "user32.dll", "ClipCursor", (void *)hkClipCursor, (void **)&g_origClipCursor);

			void *fnGcr = (void *)GetProcAddress(u32, "GetClientRect");
			void *trampGcr = nullptr;
			if (fnGcr && HookHotpatchFn(fnGcr, (void *)hkGetClientRect, &trampGcr)) {
				g_origGetClientRect = (GetClientRect_t)trampGcr;
				SetRawGetClientRectFn(g_origGetClientRect);
			}
			void *fnStc = (void *)GetProcAddress(u32, "ScreenToClient");
			void *trampStc = nullptr;
			if (fnStc && HookHotpatchFn(fnStc, (void *)hkScreenToClient, &trampStc))
				g_origScreenToClient = (ScreenToClient_t)trampStc;

			void *fnCts = (void *)GetProcAddress(u32, "ClientToScreen");
			void *trampCts = nullptr;
			if (fnCts && HookHotpatchFn(fnCts, (void *)hkClientToScreen, &trampCts))
				g_origClientToScreen = (ClientToScreen_t)trampCts;

			void *fnDma = (void *)GetProcAddress(u32, "DispatchMessageA");
			void *trampDma = nullptr;
			if (fnDma && HookHotpatchFn(fnDma, (void *)hkDispatchMessageA, &trampDma))
				g_origDispatchMessageA = (DispatchMessageA_t)trampDma;

			void *fnDmw = (void *)GetProcAddress(u32, "DispatchMessageW");
			void *trampDmw = nullptr;
			if (fnDmw && HookHotpatchFn(fnDmw, (void *)hkDispatchMessageW, &trampDmw))
				g_origDispatchMessageW = (DispatchMessageW_t)trampDmw;
		}
		if (HMODULE existing = GetModuleHandleA("d3d9.dll"))
			TryHookD3D9(existing);

		HookGtaInputMarker();
	}
	return TRUE;
}
